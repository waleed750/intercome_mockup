/*
 * The half-duplex gate.
 *
 * On a real call the door's echo of the panel talker sat at -34.1 dBFS
 * against a -34 dBFS threshold, and the bare comparison that was there closed
 * the gate 24% of the time while the panel was talking. These tests are the
 * argument that the replacement cannot do that: a signal sitting on the
 * threshold picks a side and stays there, a gap between words does not reopen
 * it, and the edges are fades rather than steps.
 */
#include "test.h"

#include "syncn/dsp.h"
#include "syncn/alaw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 8000
#define N    160

static double energy_dbfs(double dbfs)
{
    const double a = pow(10.0, dbfs / 20.0) * 32768.0;
    return a * a;
}

static void fill(int16_t *f, int16_t v) { for (int i = 0; i < N; i++) f[i] = v; }

void suite_gate(void)
{
    SUITE("gate: quiet far end never closes it");
    {
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 150.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];
        int muted = 0;
        for (int i = 0; i < 500; i++) {
            fill(f, 10000);
            if (syncn_gate_run(&g, energy_dbfs(-40.0), f, N)) muted++;
        }
        CHECK_EQ_INT(muted, 0);
        CHECK_EQ_INT(g.closures, 0);
        CHECK_EQ_INT(f[N - 1], 10000);
    }

    SUITE("gate: a signal sitting exactly on the threshold does not flicker");
    {
        /* The failure that was on the panel. far wanders +-1.5 dB around the
         * close threshold, ten seconds of it. The old gate flipped on almost
         * every frame; this one may close once and must then stay put. */
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 150.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];
        unsigned seed = 3;
        for (int i = 0; i < 500; i++) {
            seed = seed * 1103515245u + 12345u;
            const double jitter = ((double)((seed >> 16) & 0xff) / 255.0) * 3.0 - 1.5;
            fill(f, 10000);
            syncn_gate_run(&g, energy_dbfs(-30.0 + jitter), f, N);
        }
        printf("        far hovering at threshold +-1.5 dB for 10 s: %llu closure(s)\n",
               (unsigned long long)g.closures);
        CHECK(g.closures <= 2);
    }

    SUITE("gate: hysteresis -- reopens only once the far end is clearly quiet");
    {
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 0.0f, 5.0f, 5.0f, 0.0f, RATE);   /* no hangover */
        int16_t f[N];

        fill(f, 10000);
        CHECK(syncn_gate_run(&g, energy_dbfs(-28.0), f, N));   /* closes    */
        /* Between the two thresholds: must stay closed. */
        for (int i = 0; i < 20; i++) {
            fill(f, 10000);
            CHECK(syncn_gate_run(&g, energy_dbfs(-32.0), f, N));
        }
        /* Below the open threshold: reopens. Give it the ramp. */
        bool open = false;
        for (int i = 0; i < 3; i++) {
            fill(f, 10000);
            if (!syncn_gate_run(&g, energy_dbfs(-36.0), f, N)) open = true;
        }
        CHECK(open);
        CHECK_EQ_INT(g.closures, 1);
    }

    SUITE("gate: hangover -- a gap between a visitor's words does not reopen it");
    {
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 150.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];

        fill(f, 10000);
        syncn_gate_run(&g, energy_dbfs(-20.0), f, N);          /* visitor talks */
        CHECK(g.closed);

        /* 100 ms of silence: shorter than the hangover, stays closed. */
        for (int i = 0; i < 5; i++) {
            fill(f, 10000);
            CHECK(syncn_gate_run(&g, energy_dbfs(-60.0), f, N));
        }
        /* Visitor resumes: re-arms without a second closure. */
        fill(f, 10000);
        syncn_gate_run(&g, energy_dbfs(-20.0), f, N);
        CHECK_EQ_INT(g.closures, 1);

        /* 300 ms of silence: past the hangover, reopens. */
        bool open = false;
        for (int i = 0; i < 15; i++) {
            fill(f, 10000);
            if (!syncn_gate_run(&g, energy_dbfs(-60.0), f, N)) open = true;
        }
        CHECK(open);
    }

    SUITE("gate: the edges are fades, not steps");
    {
        /* A hard switch from full to zero between two frames is a click. The
         * largest sample-to-sample jump across a closing edge must be small
         * next to the signal itself. 5 ms at 8 kHz is 40 samples, so a 10000
         * step spreads to 250 per sample. */
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 150.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];

        fill(f, 10000);
        syncn_gate_run(&g, energy_dbfs(-40.0), f, N);          /* open      */
        fill(f, 10000);
        syncn_gate_run(&g, energy_dbfs(-20.0), f, N);          /* closes    */

        int worst = abs(10000 - f[0]);
        for (int i = 1; i < N; i++) {
            const int d = abs(f[i] - f[i - 1]);
            if (d > worst) worst = d;
        }
        printf("        largest jump across the closing edge: %d (of 10000)\n", worst);
        CHECK(worst <= 300);
        CHECK_EQ_INT(f[N - 1], 0);                              /* fully shut */
    }
}

void suite_noise_gate(void)
{
    SUITE("noise gate: speech passes, the pause after it is muted");
    {
        /* This panel: room floor -36 on the uplink after gain, speech -20.
         * Floor at -32, reopen at -28. */
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 300.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];

        /* A word: passes untouched. */
        for (int i = 0; i < 25; i++) {
            fill(f, 3000);
            CHECK(!syncn_noise_gate_run(&g, energy_dbfs(-20.0), f, N));
            CHECK_EQ_INT(f[N - 1], 3000);
        }
        /* Silence: holds 300 ms (15 frames), then shuts. */
        int open_frames = 0, muted = 0;
        for (int i = 0; i < 60; i++) {
            fill(f, 500);
            if (syncn_noise_gate_run(&g, energy_dbfs(-36.0), f, N)) muted++;
            else open_frames++;
        }
        printf("        after the word: %d frames held open, then %d muted\n",
               open_frames, muted);
        CHECK(open_frames >= 15 && open_frames <= 17);
        CHECK(muted >= 40);
        CHECK_EQ_INT(f[N - 1], 0);
    }

    SUITE("noise gate: the quiet start of the next word is not lost");
    {
        /* Closed on the room floor. A word arrives. It must be open within
         * one frame plus the ramp -- 20 ms of a word is a consonant. */
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 300.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];
        for (int i = 0; i < 40; i++) { fill(f, 500); syncn_noise_gate_run(&g, energy_dbfs(-36.0), f, N); }
        CHECK(g.closed);

        fill(f, 3000);
        syncn_noise_gate_run(&g, energy_dbfs(-20.0), f, N);
        CHECK(!g.closed);
        CHECK_EQ_INT(f[N - 1], 3000);              /* ramp done inside the frame */
    }

    SUITE("noise gate: room noise sitting on the floor does not flicker");
    {
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 300.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];
        unsigned seed = 9;
        for (int i = 0; i < 500; i++) {
            seed = seed * 1103515245u + 12345u;
            const double jitter = ((double)((seed >> 16) & 0xff) / 255.0) * 3.0 - 1.5;
            fill(f, 500);
            syncn_noise_gate_run(&g, energy_dbfs(-32.0 + jitter), f, N);
        }
        printf("        noise hovering on the floor +-1.5 dB for 10 s: %llu closure(s)\n",
               (unsigned long long)g.closures);
        CHECK(g.closures <= 2);
    }
}


void suite_gate_release(void)
{
    static const double E20 = 0;   /* silence the unused warning below */
    (void)E20;

    SUITE("gate: a slow release fades the tail out instead of cutting it");
    {
        /* After the visitor stops, the old gate held full open for the
         * hangover and then dropped in 5 ms -- the floor was heard at full
         * level, then cut. With a 400 ms release the level must fall
         * smoothly: each successive frame quieter than the last, and never a
         * step of more than a few percent between samples. */
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 100.0f, 5.0f, 400.0f, 0.0f, RATE);
        int16_t f[N];

        for (int i = 0; i < 10; i++) { fill(f, 8000); syncn_noise_gate_run(&g, energy_dbfs(-20.0), f, N); }

        int last = 8000, monotonic = 1, worst_step = 0;
        for (int i = 0; i < 30; i++) {                  /* 600 ms of floor */
            fill(f, 8000);
            syncn_noise_gate_run(&g, energy_dbfs(-40.0), f, N);
            if (f[N - 1] > last) monotonic = 0;
            for (int k = 1; k < N; k++) {
                const int d = abs(f[k] - f[k - 1]);
                if (d > worst_step) worst_step = d;
            }
            last = f[N - 1];
        }
        printf("        600 ms after speech stops: level %d of 8000, worst step %d\n",
               last, worst_step);
        CHECK(monotonic);
        CHECK(worst_step <= 10);            /* 8000 over 3200 samples = 2.5 */
        CHECK(last < 200);                  /* and it does get there */
    }

    SUITE("gate: a quiet word arriving mid-release reopens at full speed");
    {
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 100.0f, 5.0f, 400.0f, 0.0f, RATE);
        int16_t f[N];
        for (int i = 0; i < 10; i++) { fill(f, 8000); syncn_noise_gate_run(&g, energy_dbfs(-20.0), f, N); }
        for (int i = 0; i < 12; i++) { fill(f, 8000); syncn_noise_gate_run(&g, energy_dbfs(-40.0), f, N); }
        CHECK(g.level < 0.7f);              /* part way down the release   */

        fill(f, 8000);
        syncn_noise_gate_run(&g, energy_dbfs(-24.0), f, N);   /* a word */
        CHECK(!g.closed);
        CHECK_EQ_INT(f[N - 1], 8000);       /* attack completes in-frame  */
    }

    SUITE("gate: closed, it sends comfort noise rather than digital silence");
    {
        /* This door reads a long run of pure 0xD5 as an idle line and stops
         * sending audio. So a closed gate must produce something -- quiet,
         * but not zero. -55 dBFS is inaudible next to speech and is what
         * telephony has sent into silence since the 1990s. */
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 0.0f, 5.0f, 5.0f, -55.0f, RATE);
        int16_t f[N];
        for (int i = 0; i < 10; i++) { fill(f, 0); syncn_noise_gate_run(&g, energy_dbfs(-60.0), f, N); }
        CHECK(g.closed);

        fill(f, 0);
        syncn_noise_gate_run(&g, energy_dbfs(-60.0), f, N);
        const double lvl = syncn_rms_dbfs(f, N);
        int nonzero = 0;
        for (int i = 0; i < N; i++) if (f[i] != 0) nonzero++;
        printf("        closed gate output: %.1f dBFS, %d of %d samples non-zero\n",
               lvl, nonzero, N);
        CHECK(lvl > -60.0 && lvl < -50.0);
        CHECK(nonzero > N / 2);

        /* And through A-law it is not the silence byte. */
        uint8_t enc[N];
        syncn_alaw_encode(f, N, enc);
        int d5 = 0;
        for (int i = 0; i < N; i++) if (enc[i] == 0xD5) d5++;
        CHECK(d5 < N / 2);
    }

    SUITE("gate: with no comfort noise, closed is exactly silent");
    {
        syncn_gate g;
        syncn_noise_gate_init(&g, -32.0f, 4.0f, 0.0f, 5.0f, 5.0f, 0.0f, RATE);
        int16_t f[N];
        for (int i = 0; i < 10; i++) { fill(f, 5000); syncn_noise_gate_run(&g, energy_dbfs(-60.0), f, N); }
        fill(f, 5000);
        syncn_noise_gate_run(&g, energy_dbfs(-60.0), f, N);
        CHECK_EQ_INT(f[N - 1], 0);
    }
}

void suite_gate_active(void)
{
    SUITE("gate: driven by a decision, it ignores levels entirely");
    {
        /* The failure on the panel: door idle at -33, reopen at -34, stuck
         * shut for the rest of the call. Driven by the downlink gate's
         * decision instead, the level is irrelevant -- visitor stops, mic
         * comes back. */
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 400.0f, 5.0f, 100.0f, 0.0f, RATE);
        int16_t f[N];

        fill(f, 8000);
        CHECK(syncn_gate_run_active(&g, true, f, N));      /* visitor talks  */
        CHECK(g.closed);

        /* Visitor stops. 400 ms hangover = 20 frames, then it reopens, and
         * nothing about any level was consulted. */
        int reopened_at = -1;
        for (int i = 0; i < 40; i++) {
            fill(f, 8000);
            if (!syncn_gate_run_active(&g, false, f, N) && reopened_at < 0)
                reopened_at = i;
        }
        printf("        reopened %d frames after the visitor stopped (hangover 20)\n",
               reopened_at);
        CHECK(reopened_at >= 20 && reopened_at <= 26);
        CHECK(!g.closed);
        CHECK_EQ_INT(f[N - 1], 8000);
    }

    SUITE("gate: a brief dip in the visitor's sentence does not hand the mic back");
    {
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 400.0f, 5.0f, 100.0f, 0.0f, RATE);
        int16_t f[N];
        fill(f, 8000);
        syncn_gate_run_active(&g, true, f, N);

        /* 200 ms dip -- shorter than the hangover -- then talking resumes. */
        for (int i = 0; i < 10; i++) { fill(f, 8000); CHECK(syncn_gate_run_active(&g, false, f, N)); }
        fill(f, 8000);
        CHECK(syncn_gate_run_active(&g, true, f, N));
        CHECK_EQ_INT(g.closures, 1);                       /* no re-closure */
    }

    SUITE("gate: the near end taking the floor opens it at once, hangover and all");
    {
        /* The panel talker is clearly louder than the door's pickup of them.
         * Waiting out the hangover would cost the first 150 ms of every
         * phrase; yielding opens over the 5 ms attack instead. */
        syncn_gate g;
        syncn_gate_init(&g, -30.0f, 4.0f, 150.0f, 5.0f, 100.0f, 0.0f, RATE);
        int16_t f[N];
        fill(f, 8000);
        for (int i = 0; i < 10; i++) syncn_gate_run_active(&g, true, f, N);
        CHECK(g.closed);
        CHECK_EQ_INT(f[N - 1], 0);

        fill(f, 8000);
        CHECK(!syncn_gate_run_yield(&g, f, N));
        CHECK(!g.closed);
        CHECK_EQ_INT(g.hang_left, 0);
        CHECK_EQ_INT(f[N - 1], 8000);          /* fully open within the frame */
        CHECK(f[0] < 8000);                    /* but ramped, not stepped */

        /* And the visitor can take it back as before. */
        fill(f, 8000);
        CHECK(syncn_gate_run_active(&g, true, f, N));
        CHECK_EQ_INT(g.closures, 2);
    }
}
