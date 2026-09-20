/*
 * The neural denoiser, offline: hiss under a voice is taken down by more
 * than the voice is.
 *
 * Synthetic voiced signal (a gliding pitch with harmonics under a syllable
 * envelope) at -20 dBFS with white noise at -32: the 12 dB ratio this
 * microphone gives on a good day. The noise-only stretch must drop by a
 * clear margin; the voiced stretch must keep most of its level. Real speech
 * does better than a synthetic voice here, because that is what the network
 * was trained on; the numbers below are the floor, not the expectation.
 */
#include "test.h"

#include "syncn/denoise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 8000
#define N    160

static unsigned g_seed = 777;
static float white(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((float)((g_seed >> 16) & 0x7fff) / 16383.5f) - 1.0f;
}

static double dbfs(const int16_t *f, size_t n)
{
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)f[i] * f[i];
    return 10.0 * log10(e / n / (32768.0 * 32768.0) + 1e-20);
}

void suite_denoise(void)
{
    SUITE("denoise: the network is built in");
    CHECK(syncn_denoise_available());
    if (!syncn_denoise_available())
        return;

    SUITE("denoise: hiss between words goes, the voice stays");
    {
        syncn_denoise *d = syncn_denoise_create(RATE, 1.0f);
        CHECK(d != NULL);

        double phase = 0, t = 0;
        double noise_in = 0, noise_out = 0; int n_noise = 0;
        double voice_in = 0, voice_out = 0; int n_voice = 0;
        const float noise_amp = powf(10.0f, -32.0f / 20.0f) * 32767.0f;
        const float voice_amp = powf(10.0f, -20.0f / 20.0f) * 32767.0f;

        for (int f = 0; f < 600; f++) {           /* 12 seconds */
            int16_t frame[N];
            /* 0-3 s: noise only. 3-9 s: voice over noise. 9-12 s: noise only. */
            const int voiced = f >= 150 && f < 450;
            for (int i = 0; i < N; i++) {
                float v = white() * noise_amp;
                if (voiced) {
                    const double f0 = 140.0 * (1.0 + 0.12 * sin(2 * M_PI * 0.8 * t));
                    phase += 2 * M_PI * f0 / RATE;
                    float x = 0;
                    for (int h = 1; h <= 10; h++)
                        x += (float)(sin(h * phase) / (1.0 + 0.5 * (h - 1)));
                    const float env = 0.6f + 0.4f * (float)sin(2 * M_PI * 3.5 * t);
                    v += x * voice_amp * env * 0.35f;
                }
                t += 1.0 / RATE;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                frame[i] = (int16_t)v;
            }
            const double before = dbfs(frame, N);
            syncn_denoise_run(d, frame, N);
            const double after = dbfs(frame, N);

            /* Skip the first second (network settling) and the frame after
             * each transition (the one-frame delay). */
            if (f >= 50 && f < 148)  { noise_in += before; noise_out += after; n_noise++; }
            if (f >= 200 && f < 440) { voice_in += before; voice_out += after; n_voice++; }
            if (f >= 500 && f < 600) { noise_in += before; noise_out += after; n_noise++; }
        }
        noise_in /= n_noise; noise_out /= n_noise;
        voice_in /= n_voice; voice_out /= n_voice;
        printf("    noise-only: %.1f -> %.1f dBFS (%.1f dB down)\n", noise_in, noise_out, noise_in - noise_out);
        printf("    voice+noise: %.1f -> %.1f dBFS (%.1f dB down)\n", voice_in, voice_out, voice_in - voice_out);

        CHECK(noise_in - noise_out > 6.0);                        /* hiss taken down */
        CHECK(voice_in - voice_out < 6.0);                        /* voice kept      */
        CHECK((noise_in - noise_out) - (voice_in - voice_out) > 4.0); /* and by more */

        syncn_denoise_destroy(d);
    }

    SUITE("denoise: exactly one frame in, one frame out, every tick");
    {
        syncn_denoise *d = syncn_denoise_create(RATE, 1.0f);
        int16_t frame[N];
        for (int f = 0; f < 300; f++) {
            for (int i = 0; i < N; i++) frame[i] = (int16_t)(white() * 2000.0f);
            syncn_denoise_run(d, frame, N);
        }
        /* If the FIFOs drifted, run would have refused frames by now; the
         * check is that it still produces output at all. */
        for (int i = 0; i < N; i++) frame[i] = (int16_t)(sin(i * 0.5) * 8000.0);
        for (int f = 0; f < 5; f++) syncn_denoise_run(d, frame, N);
        CHECK(fabs(dbfs(frame, N)) < 90.0);
        syncn_denoise_destroy(d);
    }

    SUITE("denoise: wet 0 is a delay and nothing else");
    {
        syncn_denoise *d = syncn_denoise_create(RATE, 0.0f);
        double in_db = 0, out_db = 0;
        for (int f = 0; f < 200; f++) {
            int16_t frame[N];
            for (int i = 0; i < N; i++) frame[i] = (int16_t)(white() * 6000.0f);
            const double b = dbfs(frame, N);
            syncn_denoise_run(d, frame, N);
            if (f >= 20) { in_db += b; out_db += dbfs(frame, N); }
        }
        CHECK(fabs(in_db - out_db) / 180.0 < 0.5);
        syncn_denoise_destroy(d);
    }
}
