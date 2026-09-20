/*
 * The uplink limiter.
 *
 * The claim it exists to make good: raise the gain hard and speech gets louder
 * by the full amount, while no peak ever crosses the ceiling and quiet speech
 * is not touched. Each of those is a separate check, because each can fail on
 * its own and each would be a different bug.
 */
#include "test.h"

#include "syncn/dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 8000
#define N    (RATE * 2)

/* Speech-shaped: a voiced tone with a syllabic envelope and a high crest
 * factor, at a given RMS level. Real speech peaks 12-15 dB above its RMS and
 * this does the same, which is what makes the test meaningful. */
static void speech(int16_t *out, int n, double rms_dbfs, unsigned seed)
{
    double st = 0;
    const double target = pow(10.0, rms_dbfs / 20.0) * 32768.0;
    double *raw = malloc(sizeof *raw * (size_t)n);
    double acc = 0;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        const double white = ((double)((seed >> 16) & 0x7fff) / 16383.5) - 1.0;
        st = 0.8 * st + 0.2 * white;
        const double s   = (double)i / RATE;
        const double env = 0.5 + 0.5 * sin(2 * M_PI * 3.3 * s);
        const double burst = (fmod(s, 0.6) < 0.35) ? 1.0 : 0.05;   /* syllables */
        raw[i] = (0.7 * sin(2 * M_PI * 180 * s) + 0.3 * st) * env * env * burst;
        acc += raw[i] * raw[i];
    }
    const double scale = target / sqrt(acc / n);
    for (int i = 0; i < n; i++) {
        double v = raw[i] * scale;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
    free(raw);
}

static double peak_dbfs(const int16_t *s, int n)
{
    int p = 0;
    for (int i = 0; i < n; i++) if (abs(s[i]) > p) p = abs(s[i]);
    return p ? 20 * log10(p / 32768.0) : -96;
}

static void gain(int16_t *s, int n, double db)
{
    const double g = pow(10.0, db / 20.0);
    for (int i = 0; i < n; i++) {
        double v = s[i] * g;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

void suite_limiter(void)
{
    static int16_t buf[N];

    SUITE("limiter: below the ceiling it changes nothing");
    {
        speech(buf, N, -30.0, 7);
        int16_t copy[N];
        memcpy(copy, buf, sizeof copy);

        syncn_limiter l;
        syncn_limiter_init(&l, -1.0f, 200.0f, RATE);
        syncn_limiter_run(&l, buf, N);

        CHECK_EQ_MEM(buf, copy, sizeof buf);
        CHECK_EQ_INT(l.limited, 0);
    }

    SUITE("limiter: no peak crosses the ceiling, whatever the gain");
    {
        for (double g = 6; g <= 30; g += 6) {
            speech(buf, N, -36.0, 11);          /* this panel's raw talker */
            gain(buf, N, g);
            syncn_limiter l;
            syncn_limiter_init(&l, -1.0f, 200.0f, RATE);
            syncn_limiter_run(&l, buf, N);
            const double pk = peak_dbfs(buf, N);
            CHECK(pk <= -0.9);                  /* ceiling is -1 dBFS */
        }
    }

    SUITE("limiter: loudness rises by the full gain until the peaks hit the ceiling");
    {
        /* The whole point. -36 dBFS speech with +20 dB should come out near
         * -16 dBFS RMS -- not held back to leave room for peaks the limiter
         * is now catching. Some loss is expected right at the top from peaks
         * being pulled down; it must be small. */
        speech(buf, N, -36.0, 11);
        const double before = syncn_rms_dbfs(buf, N);

        gain(buf, N, 20.0);
        syncn_limiter l;
        syncn_limiter_init(&l, -1.0f, 200.0f, RATE);
        syncn_limiter_run(&l, buf, N);

        const double after = syncn_rms_dbfs(buf, N);
        const double lift  = after - before;
        printf("        -36 dBFS speech, +20 dB gain: %.1f -> %.1f dBFS rms, "
               "peak %.1f, %llu samples limited\n",
               before, after, peak_dbfs(buf, N), (unsigned long long)l.limited);
        CHECK(lift >= 17.0);                    /* most of the 20 dB kept */
        CHECK(peak_dbfs(buf, N) <= -0.9);
    }

    SUITE("limiter: it is a limiter, not a clipper");
    {
        /* Clipping flattens the top of the waveform, which is what makes the
         * uplink 'splatter'. A limiter scales the whole waveform down instead.
         * So a pure sine well above the ceiling must come out still a sine:
         * its 3rd harmonic stays far below the fundamental. Under clipping it
         * would be within ~10 dB. */
        for (int i = 0; i < N; i++)
            buf[i] = (int16_t)(32000.0 * sin(2 * M_PI * 300 * i / (double)RATE));
        syncn_limiter l;
        syncn_limiter_init(&l, -12.0f, 200.0f, RATE);   /* 11 dB over */
        syncn_limiter_run(&l, buf, N);

        /* Goertzel at 300 and 900 Hz. */
        double e1 = 0, e3 = 0;
        for (int h = 1; h <= 3; h += 2) {
            const double w = 2 * M_PI * (300.0 * h) / RATE;
            double s1 = 0, s2 = 0;
            const double c = 2 * cos(w);
            for (int i = RATE / 2; i < N; i++) {      /* skip the attack */
                const double s0 = buf[i] + c * s1 - s2;
                s2 = s1; s1 = s0;
            }
            const double mag = sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
            if (h == 1) e1 = mag; else e3 = mag;
        }
        const double h3 = 20 * log10(e3 / e1);
        printf("        3rd harmonic after 11 dB of limiting: %.1f dB\n", h3);
        CHECK(h3 < -30.0);
    }

    SUITE("limiter: release lets the gain recover between syllables");
    {
        /* A loud burst, then quiet. The quiet part must come back to full
         * gain within the release time, or the word after a loud one is
         * swallowed. */
        memset(buf, 0, sizeof buf);
        for (int i = 0; i < RATE / 4; i++)                        /* 250 ms loud */
            buf[i] = (int16_t)(30000.0 * sin(2 * M_PI * 300 * i / (double)RATE));
        for (int i = RATE; i < N; i++)                            /* quiet later */
            buf[i] = (int16_t)(3000.0 * sin(2 * M_PI * 300 * i / (double)RATE));
        const double quiet_before = syncn_rms_dbfs(buf + RATE + RATE / 2, RATE / 4);

        syncn_limiter l;
        syncn_limiter_init(&l, -12.0f, 200.0f, RATE);
        syncn_limiter_run(&l, buf, N);

        const double quiet_after = syncn_rms_dbfs(buf + RATE + RATE / 2, RATE / 4);
        printf("        quiet passage 750 ms after a loud burst: %.1f -> %.1f dBFS\n",
               quiet_before, quiet_after);
        CHECK(fabs(quiet_after - quiet_before) < 0.5);
    }
}

void suite_limiter_gain(void)
{
    static int16_t buf[N];

    SUITE("limiter: gain and limiting in one pass never saturates");
    {
        /* The bug that shipped: gain clamped to int16, THEN the limiter. On
         * this panel's talker at +20 that produced 34,000 saturated samples in
         * one call and was heard as harshness. One pass in float: the limiter
         * sees the true peak, and nothing reaches the rail. */
        for (double g = 12; g <= 36; g += 6) {
            speech(buf, N, -36.0, 11);
            syncn_limiter l;
            syncn_limiter_init(&l, -1.0f, 200.0f, RATE);
            syncn_limiter_run_gain(&l, buf, N, (float)pow(10.0, g / 20.0));

            int at_rail = 0;
            for (int i = 0; i < N; i++)
                if (buf[i] >= 32700 || buf[i] <= -32700) at_rail++;

            const double pk = peak_dbfs(buf, N);
            if (g == 24 || g == 36)
                printf("        +%.0f dB: rms %.1f dBFS, peak %.1f, %d at the rail, "
                       "%llu limited\n", g, syncn_rms_dbfs(buf, N), pk, at_rail,
                       (unsigned long long)l.limited);
            CHECK_EQ_INT(at_rail, 0);
            CHECK(pk <= -0.9);
        }
    }

    SUITE("limiter: one pass and two pass agree when nothing is limited");
    {
        speech(buf, N, -36.0, 11);
        int16_t two[N];
        memcpy(two, buf, sizeof two);

        syncn_limiter a, b;
        syncn_limiter_init(&a, -1.0f, 200.0f, RATE);
        syncn_limiter_init(&b, -1.0f, 200.0f, RATE);

        const float g6 = (float)pow(10.0, 6.0 / 20.0);   /* +6 dB, well clear */
        syncn_limiter_run_gain(&a, buf, N, g6);
        for (int i = 0; i < N; i++)
            two[i] = (int16_t)((float)two[i] * g6);
        syncn_limiter_run(&b, two, N);

        int diff = 0;
        for (int i = 0; i < N; i++)
            if (abs(buf[i] - two[i]) > 1) diff++;       /* rounding only */
        CHECK_EQ_INT(diff, 0);
    }
}
