#include "test.h"
#include "syncn/alaw.h"
#include "syncn/dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME 160  /* 20 ms at 8 kHz */

/* The panel's microphone as measured on the bench: speech and a 60 Hz mains
 * hum on the left channel, nothing on the right. Amplitudes are the real ones —
 * hum 670, speech peaking near 5000. */
static void make_panel_mic(int16_t *stereo, size_t frames, double *phase_hum,
                           double *phase_voice, bool speaking)
{
    for (size_t i = 0; i < frames; i++) {
        double s = 670.0 * sin(*phase_hum);
        if (speaking) {
            /* Speech is not a sine, but a few harmonics in the telephony band
             * are enough to test what the filter keeps and what it removes. */
            s += 3000.0 * sin(*phase_voice)
               + 1500.0 * sin(*phase_voice * 2.0)
               +  800.0 * sin(*phase_voice * 3.0);
        }
        *phase_hum   += 2.0 * M_PI * 60.0  / 8000.0;
        *phase_voice += 2.0 * M_PI * 400.0 / 8000.0;

        if (s >  32767.0) s =  32767.0;
        if (s < -32768.0) s = -32768.0;

        stereo[i * 2]     = (int16_t)s;
        stereo[i * 2 + 1] = (int16_t)(rand() % 43 - 21); /* dead: peak 21 */
    }
}

static void tone(int16_t *out, size_t n, double hz, double amp, double *phase)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (int16_t)(amp * sin(*phase));
        *phase += 2.0 * M_PI * hz / 8000.0;
    }
}

void suite_dsp(void)
{
    int16_t stereo[FRAME * 2], mono[FRAME];

    SUITE("dsp: auto picks the live channel");
    {
        /* The bug this exists for: ALSA's mono downmix averages the live
         * channel with a dead one and throws away 6 dB before anything else
         * touches the signal. */
        syncn_chanpick *p = syncn_chanpick_create(SYNCN_CH_AUTO);
        double ph = 0, pv = 0;

        for (int f = 0; f < 200; f++) {
            make_panel_mic(stereo, FRAME, &ph, &pv, f % 3 == 0);
            syncn_chanpick_run(p, stereo, FRAME, mono);
        }

        CHECK(syncn_chanpick_is_settled(p));
        CHECK_EQ_INT(syncn_chanpick_decided(p), SYNCN_CH_LEFT);
        syncn_chanpick_destroy(p);
    }

    SUITE("dsp: auto picks right when the wiring is the other way round");
    {
        /* Another panel may be wired the other way. Measuring rather than
         * assuming is what makes this work on hardware nobody has seen. */
        syncn_chanpick *p = syncn_chanpick_create(SYNCN_CH_AUTO);
        double ph = 0, pv = 0;

        for (int f = 0; f < 200; f++) {
            make_panel_mic(stereo, FRAME, &ph, &pv, true);
            for (int i = 0; i < FRAME; i++) {  /* swap the channels over */
                const int16_t t = stereo[i * 2];
                stereo[i * 2] = stereo[i * 2 + 1];
                stereo[i * 2 + 1] = t;
            }
            syncn_chanpick_run(p, stereo, FRAME, mono);
        }
        CHECK_EQ_INT(syncn_chanpick_decided(p), SYNCN_CH_RIGHT);
        syncn_chanpick_destroy(p);
    }

    SUITE("dsp: auto mixes when both channels are genuinely live");
    {
        /* A real stereo microphone must not have half of it discarded. */
        syncn_chanpick *p = syncn_chanpick_create(SYNCN_CH_AUTO);
        double ph = 0;

        for (int f = 0; f < 200; f++) {
            int16_t m[FRAME];
            tone(m, FRAME, 400, 4000, &ph);
            for (int i = 0; i < FRAME; i++) {
                stereo[i * 2]     = m[i];
                stereo[i * 2 + 1] = (int16_t)(m[i] * 0.8); /* within the margin */
            }
            syncn_chanpick_run(p, stereo, FRAME, mono);
        }
        CHECK_EQ_INT(syncn_chanpick_decided(p), SYNCN_CH_MIX);
        syncn_chanpick_destroy(p);
    }

    SUITE("dsp: choosing the live channel recovers the 6 dB that mixing loses");
    {
        syncn_chanpick *left = syncn_chanpick_create(SYNCN_CH_LEFT);
        syncn_chanpick *mix  = syncn_chanpick_create(SYNCN_CH_MIX);
        double ph = 0, pv = 0;
        int16_t as_left[FRAME], as_mix[FRAME];

        make_panel_mic(stereo, FRAME, &ph, &pv, true);
        syncn_chanpick_run(left, stereo, FRAME, as_left);
        syncn_chanpick_run(mix,  stereo, FRAME, as_mix);

        const float db_left = syncn_rms_dbfs(as_left, FRAME);
        const float db_mix  = syncn_rms_dbfs(as_mix, FRAME);

        /* Averaging with a dead channel is exactly a halving: -6 dB. */
        CHECK(db_left - db_mix > 5.0f);
        CHECK(db_left - db_mix < 7.0f);

        syncn_chanpick_destroy(left);
        syncn_chanpick_destroy(mix);
    }

    SUITE("dsp: the high-pass removes mains hum and keeps speech");
    {
        syncn_biquad f;
        double ph;
        int16_t buf[FRAME * 25];

        /* 60 Hz, the measured hum. */
        syncn_biquad_highpass(&f, 150.0f, 8000.0f, 0.707f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 60, 4000, &ph);
        const float hum_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f, buf, sizeof buf / sizeof buf[0]);
        /* Skip the settling transient before measuring. */
        const float hum_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        /* 400 Hz, well inside the telephony band the door actually carries. */
        syncn_biquad_highpass(&f, 150.0f, 8000.0f, 0.707f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 400, 4000, &ph);
        const float voice_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f, buf, sizeof buf / sizeof buf[0]);
        const float voice_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        printf("        60 Hz: %.1f -> %.1f dBFS (%.1f dB cut)\n",
               (double)hum_before, (double)hum_after,
               (double)(hum_before - hum_after));
        printf("       400 Hz: %.1f -> %.1f dBFS (%.1f dB cut)\n",
               (double)voice_before, (double)voice_after,
               (double)(voice_before - voice_after));

        CHECK(hum_before - hum_after > 12.0f);   /* hum substantially gone   */
        CHECK(voice_before - voice_after < 2.0f); /* speech barely touched   */
    }

    SUITE("dsp: two low-pass sections take the hiss above the voice band");
    {
        /* Hiss at 3.8 kHz, the top of what 8 kHz A-law carries; 1 kHz for
         * the voice. Two Butterworth sections at 3.4 kHz. */
        syncn_biquad f1, f2;
        int16_t buf[FRAME * 25];
        double ph;

        syncn_biquad_lowpass(&f1, 3400.0f, 8000.0f, 0.707f);
        syncn_biquad_lowpass(&f2, 3400.0f, 8000.0f, 0.707f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 3800, 4000, &ph);
        const float hiss_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f1, buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f2, buf, sizeof buf / sizeof buf[0]);
        const float hiss_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        syncn_biquad_lowpass(&f1, 3400.0f, 8000.0f, 0.707f);
        syncn_biquad_lowpass(&f2, 3400.0f, 8000.0f, 0.707f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 1000, 4000, &ph);
        const float voice_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f1, buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f2, buf, sizeof buf / sizeof buf[0]);
        const float voice_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        printf("      3800 Hz: %.1f -> %.1f dBFS (%.1f dB cut)\n",
               (double)hiss_before, (double)hiss_after, (double)(hiss_before - hiss_after));
        printf("      1000 Hz: %.1f -> %.1f dBFS (%.1f dB cut)\n",
               (double)voice_before, (double)voice_after, (double)(voice_before - voice_after));

        CHECK(hiss_before - hiss_after > 10.0f);
        CHECK(voice_before - voice_after < 1.0f);
    }

    SUITE("dsp: the presence shelf lifts the consonants and leaves the vowels");
    {
        syncn_biquad f;
        int16_t buf[FRAME * 25];
        double ph;

        syncn_biquad_highshelf(&f, 2000.0f, 8000.0f, 3.0f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 3200, 4000, &ph);
        const float hi_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f, buf, sizeof buf / sizeof buf[0]);
        const float hi_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        syncn_biquad_highshelf(&f, 2000.0f, 8000.0f, 3.0f);
        ph = 0;
        tone(buf, sizeof buf / sizeof buf[0], 500, 4000, &ph);
        const float lo_before = syncn_rms_dbfs(buf, sizeof buf / sizeof buf[0]);
        syncn_biquad_run(&f, buf, sizeof buf / sizeof buf[0]);
        const float lo_after = syncn_rms_dbfs(buf + FRAME * 5, FRAME * 20);

        printf("      3200 Hz: %.1f -> %.1f dBFS (%+.1f dB)\n",
               (double)hi_before, (double)hi_after, (double)(hi_after - hi_before));
        printf("       500 Hz: %.1f -> %.1f dBFS (%+.1f dB)\n",
               (double)lo_before, (double)lo_after, (double)(lo_after - lo_before));

        CHECK(hi_after - hi_before > 2.0f && hi_after - hi_before < 3.5f);
        CHECK(fabsf(lo_after - lo_before) < 0.5f);
    }

    SUITE("dsp: the full capture chain rescues speech from the hum");
    {
        /*
         * The measured situation, end to end. With ALSA's mono downmix the
         * speech sits roughly level with the hum, so noise suppression cannot
         * tell them apart and removes both — which is why the door heard
         * nothing while the panel transmitted 50 frames a second.
         */
        double ph = 0, pv = 0;
        int16_t chain[FRAME * 25], mixed[FRAME * 25];
        syncn_chanpick *left = syncn_chanpick_create(SYNCN_CH_LEFT);
        syncn_chanpick *mix  = syncn_chanpick_create(SYNCN_CH_MIX);
        syncn_biquad f;
        syncn_biquad_highpass(&f, 150.0f, 8000.0f, 0.707f);

        for (int blk = 0; blk < 25; blk++) {
            make_panel_mic(stereo, FRAME, &ph, &pv, true);
            syncn_chanpick_run(left, stereo, FRAME, chain + blk * FRAME);
            syncn_chanpick_run(mix,  stereo, FRAME, mixed + blk * FRAME);
        }
        syncn_biquad_run(&f, chain, sizeof chain / sizeof chain[0]);

        /* Hum only, through both paths, for the noise floor. */
        int16_t hum_chain[FRAME * 25], hum_mixed[FRAME * 25];
        ph = 0; pv = 0;
        syncn_chanpick *l2 = syncn_chanpick_create(SYNCN_CH_LEFT);
        syncn_chanpick *m2 = syncn_chanpick_create(SYNCN_CH_MIX);
        syncn_biquad f2;
        syncn_biquad_highpass(&f2, 150.0f, 8000.0f, 0.707f);
        for (int blk = 0; blk < 25; blk++) {
            make_panel_mic(stereo, FRAME, &ph, &pv, false);
            syncn_chanpick_run(l2, stereo, FRAME, hum_chain + blk * FRAME);
            syncn_chanpick_run(m2, stereo, FRAME, hum_mixed + blk * FRAME);
        }
        syncn_biquad_run(&f2, hum_chain, sizeof hum_chain / sizeof hum_chain[0]);

        const float snr_before = syncn_rms_dbfs(mixed + FRAME * 5, FRAME * 20)
                               - syncn_rms_dbfs(hum_mixed + FRAME * 5, FRAME * 20);
        const float snr_after  = syncn_rms_dbfs(chain + FRAME * 5, FRAME * 20)
                               - syncn_rms_dbfs(hum_chain + FRAME * 5, FRAME * 20);

        printf("        speech over hum: %.1f dB -> %.1f dB\n",
               (double)snr_before, (double)snr_after);

        CHECK(snr_after > snr_before + 10.0f);
        CHECK(snr_after > 20.0f);  /* comfortably separable */

        syncn_chanpick_destroy(left);
        syncn_chanpick_destroy(mix);
        syncn_chanpick_destroy(l2);
        syncn_chanpick_destroy(m2);
    }

    SUITE("dsp: level metering");
    {
        int16_t silence[FRAME];
        memset(silence, 0, sizeof silence);
        CHECK(syncn_rms_dbfs(silence, FRAME) <= -100.0f);

        int16_t full[FRAME];
        for (int i = 0; i < FRAME; i++)
            full[i] = (i % 2) ? 32767 : -32768;
        CHECK(syncn_rms_dbfs(full, FRAME) > -0.5f);
    }
}
