/*
 * The return canceller.
 *
 * The door's microphone hears the panel talker -- through its own speaker
 * installed, straight through the air on the bench -- and sends them back
 * 100-300 ms later at -26 to -32 dBFS, which is what a visitor arrives at.
 * No threshold on the door's level can tell the two apart, and comparing
 * levels failed on the bench because the panel hears the visitor too.
 *
 * What does tell them apart: the return is a delayed, filtered copy of
 * exactly what we sent, and a visitor is not. So a second canceller,
 * reference = our uplink, cleans the door's stream. These tests are the
 * argument that it does: a synthetic return is taken down by better than
 * 15 dB once converged, and an uncorrelated visitor comes through whole.
 */
#include "test.h"

#include "syncn/aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE  8000
#define N     160
#define SECS  12
#define FRAMES (SECS * RATE / N)

static unsigned g_seed = 12345;
static float white(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((float)((g_seed >> 16) & 0x7fff) / 16383.5f) - 1.0f;
}

/*
 * Speech-like: a gliding pitch with its harmonics under a 4 Hz syllable
 * envelope. Voiced, not noise -- a denoiser treats stationary noise as
 * noise, so a noise-burst "talker" would test the denoiser, not the
 * canceller. Two voices differ in pitch and glide so they are uncorrelated.
 */
typedef struct { double phase; double t; float f0; float glide; } voice;

static void talk(voice *v, float *out, size_t n, float amp)
{
    for (size_t i = 0; i < n; i++) {
        const double t   = v->t + (double)i / RATE;
        const double f0  = v->f0 * (1.0 + 0.15 * sin(2 * M_PI * v->glide * t));
        v->phase += 2 * M_PI * f0 / RATE;
        float x = 0.0f;
        for (int h = 1; h <= 12; h++)
            x += (float)(sin(h * v->phase) / (1.0 + 0.4 * (h - 1)));
        const float env = 0.55f + 0.45f * (float)sin(2 * M_PI * 4.0 * t);
        out[i] = x * amp * env * 0.25f;
    }
    v->t += (double)n / RATE;
}

static double dbfs(const int16_t *f, size_t n)
{
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)f[i] * f[i];
    return 10.0 * log10(e / n / (32768.0 * 32768.0) + 1e-20);
}

void suite_return(void)
{
    SUITE("return: the door's echo of our uplink is taken out, a visitor is not");
    {
        /* The path: 200 ms delay, -14 dB, a little colouration. */
        const int    delay = RATE / 5;
        const float  gain  = powf(10.0f, -14.0f / 20.0f);
        static float sent[SECS * RATE + N];
        memset(sent, 0, sizeof sent);

        syncn_aec_tuning tune;
        syncn_aec_default_tuning(&tune);
        tune.denoise                 = true;    /* the door's floor, too */
        tune.noise_suppress_db       = -10;
        tune.echo_suppress_db        = -25;
        tune.echo_suppress_active_db = -15;
        syncn_aec *ret = syncn_aec_create_tuned(SYNCN_AEC_SPEEX, RATE, N, 600, 0, &tune);
        CHECK(ret != NULL);
        CHECK(syncn_aec_active(ret) == SYNCN_AEC_SPEEX);

        double raw_return = 0, clean_return = 0; int n_return = 0;
        double raw_visit  = 0, clean_visit  = 0; int n_visit  = 0;
        /* What the linear filter alone reports, and how often it clears
         * the 8 dB the engine mutes the speaker on. */
        double lin_return = 0, lin_visit = 0; int hit_return = 0, hit_visit = 0;
        /* And the whole removal, which is what the engine judges. */
        int tot_return = 0, tot_visit = 0;
        static double lin_r[4000], lin_v[4000]; int n_lin_r = 0, n_lin_v = 0;

        float lpf = 0.0f;
        voice panel   = { 0, 0, 130.0f, 0.7f };
        voice visitor = { 0, 0, 190.0f, 1.1f };
        for (int f = 0; f < FRAMES; f++) {
            const double t = (double)f * N / RATE;
            const int    s = f * N;

            /* Seconds 0-8: the panel talks. 8-12: the panel is quiet and a
             * visitor talks at the door. */
            float up[N];
            if (t < 8.0) talk(&panel, up, N, 0.35f); else memset(up, 0, sizeof up);
            float vis[N];
            if (t >= 8.0) talk(&visitor, vis, N, 0.5f); else memset(vis, 0, sizeof vis);
            for (int i = 0; i < N; i++) sent[s + i] = up[i];

            int16_t far[N];
            for (int i = 0; i < N; i++) {
                const int j = s + i - delay;
                float r = j >= 0 ? sent[j] : 0.0f;
                lpf += 0.6f * (r - lpf);                /* the door's speaker and mic */
                float x = (lpf * gain + vis[i]) * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                far[i] = (int16_t)x;
            }

            int16_t up16[N];
            for (int i = 0; i < N; i++) {
                float x = up[i] * 32767.0f;
                if (x > 32767.0f) x = 32767.0f;
                if (x < -32768.0f) x = -32768.0f;
                up16[i] = (int16_t)x;
            }

            const double before = dbfs(far, N);
            /* As the engine does it: clean this tick's far end with what we
             * sent on previous ticks, then record what we send now. */
            syncn_aec_capture(ret, far, N);
            const double after = dbfs(far, N);
            syncn_aec_play(ret, up16, N);

            const double lin = syncn_aec_last_cancel_db(ret);
            if (t >= 4.0 && t < 8.0) {
                raw_return += before; clean_return += after; n_return++;
                lin_return += lin; hit_return += lin >= 8.0;
                if (before > -37.0) lin_r[n_lin_r++] = lin;
                tot_return += before > -37.0 && before - after >= 8.0;
            }
            if (t >= 9.0) {
                raw_visit += before; clean_visit += after; n_visit++;
                lin_visit += lin; hit_visit += lin >= 8.0;
                if (before > -37.0) lin_v[n_lin_v++] = lin;
                /* As the engine judges it: only frames the downlink gate
                 * would play (above -37 dBFS raw). What the denoiser takes
                 * off a pause between the visitor's words is not echo, and
                 * the engine never looks at it. */
                tot_visit += before > -37.0 && before - after >= 8.0;
            }
        }
        raw_return /= n_return; clean_return /= n_return; lin_return /= n_return;
        raw_visit  /= n_visit;  clean_visit  /= n_visit;  lin_visit  /= n_visit;
        printf("    return: %.1f -> %.1f dBFS (%.1f dB down); visitor: %.1f -> %.1f dBFS\n",
               raw_return, clean_return, raw_return - clean_return, raw_visit, clean_visit);
        printf("    linear filter alone: return %.1f dB (%d of %d frames >= 8), "
               "visitor %.1f dB (%d of %d frames >= 8)\n",
               lin_return, hit_return, n_return, lin_visit, hit_visit, n_visit);
        printf("    whole removal >= 8 dB: return %d of %d frames, visitor %d of %d\n",
               tot_return, n_return, tot_visit, n_visit);
        for (int th = 2; th <= 6; th++) {
            int r = 0, v = 0;
            for (int i = 0; i < n_lin_r; i++) r += lin_r[i] >= th;
            for (int i = 0; i < n_lin_v; i++) v += lin_v[i] >= th;
            printf("    linear >= %d dB: return %d of %d, visitor %d of %d\n", th, r, n_lin_r, v, n_lin_v);
        }

        CHECK(raw_return > -46.0 && raw_return < -20.0);   /* the synthetic return is realistic */
        CHECK(raw_return - clean_return > 15.0);            /* and it is gone */
        CHECK(raw_visit - clean_visit < 3.0);               /* the visitor is not */
        /* The engine keeps a frame off the speaker when the canceller took
         * 8 dB out of it: that has to catch the return and spare the
         * visitor, or the visitor is muted at the panel. The linear share
         * is reported for the log (ret= in the stats line) and has to stay
         * clean on the visitor too. */
        {
            /* At the shipped bar, on the frames the engine judges (loud
             * enough to be played): every return frame caught, no visitor
             * frame. The whole removal is printed for comparison; it is
             * not what the engine judges, because the denoiser alone
             * clears 8 dB on some loud visitor frames (see above). */
            int r = 0, v = 0;
            for (int i = 0; i < n_lin_r; i++) r += lin_r[i] >= 3.0;
            for (int i = 0; i < n_lin_v; i++) v += lin_v[i] >= 3.0;
            CHECK(n_lin_r > 50);
            CHECK(r == n_lin_r);
            CHECK(v == 0);
        }
        CHECK(lin_return > 8.0);
        CHECK(lin_visit < 2.0);

        syncn_aec_destroy(ret);
    }

    SUITE("return: with nothing sent, the door's stream passes through untouched");
    {
        syncn_aec *ret = syncn_aec_create(SYNCN_AEC_SPEEX, RATE, N, 600, 0);
        int16_t silence[N]; memset(silence, 0, sizeof silence);
        double raw = 0, clean = 0;
        for (int f = 0; f < 200; f++) {
            int16_t far[N];
            for (int i = 0; i < N; i++) far[i] = (int16_t)(white() * 3000.0f);
            raw += dbfs(far, N);
            syncn_aec_capture(ret, far, N);
            clean += dbfs(far, N);
            syncn_aec_play(ret, silence, N);
        }
        printf("    idle: %.1f -> %.1f dBFS\n", raw / 200.0, clean / 200.0);
        /* The preprocessor has about 1 dB of fixed insertion loss even with
         * nothing to suppress. Anything beyond that would be it acting on a
         * reference it does not have. */
        CHECK(fabs(raw - clean) / 200.0 < 2.0);
        syncn_aec_destroy(ret);
    }
}
