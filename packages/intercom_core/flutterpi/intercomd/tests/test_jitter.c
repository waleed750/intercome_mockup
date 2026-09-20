#include "test.h"
#include "syncn/alaw.h"
#include "syncn/jitter.h"

#include <stdlib.h>
#include <string.h>

/* Frames carry their sequence number in the first byte so ordering and
 * discards can be checked, not just counted. */
static void make_frame(uint8_t *f, uint8_t seq)
{
    syncn_alaw_fill_silence(f, SYNCN_AUDIO_FRAME_BYTES);
    f[0] = seq;
}

static unsigned depth_of(syncn_jitter *j)
{
    syncn_jitter_stats s;
    syncn_jitter_get_stats(j, &s);
    return s.depth;
}

static uint64_t trimmed_of(syncn_jitter *j)
{
    syncn_jitter_stats s;
    syncn_jitter_get_stats(j, &s);
    return s.trimmed;
}

void suite_jitter(void)
{
    uint8_t f[SYNCN_AUDIO_FRAME_BYTES], out[SYNCN_AUDIO_FRAME_BYTES];

    SUITE("jitter: primes before playing");
    {
        /* Playing the first frame the instant it lands leaves nothing to
         * absorb jitter with, and the buffer underruns on the next hiccup. */
        syncn_jitter *j = syncn_jitter_create(60); /* 3 frames */
        make_frame(f, 1);
        syncn_jitter_push(j, f);
        CHECK(!syncn_jitter_pop(j, out));

        make_frame(f, 2);
        syncn_jitter_push(j, f);
        CHECK(!syncn_jitter_pop(j, out));

        make_frame(f, 3);
        syncn_jitter_push(j, f);
        CHECK(syncn_jitter_pop(j, out));
        CHECK_EQ_INT(out[0], 1); /* oldest first */
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: steady state holds the target and trims nothing");
    {
        /* One in, one out — the normal case. Trimming here would discard live
         * audio for no reason. */
        syncn_jitter *j = syncn_jitter_create(60);
        for (int i = 0; i < 3; i++) { make_frame(f, (uint8_t)i); syncn_jitter_push(j, f); }

        for (int i = 3; i < 2000; i++) {
            make_frame(f, (uint8_t)i);
            syncn_jitter_push(j, f);
            CHECK(syncn_jitter_pop(j, out));
        }

        syncn_jitter_stats s;
        syncn_jitter_get_stats(j, &s);
        CHECK_EQ_INT(s.trimmed, 0);
        CHECK_EQ_INT(s.underruns, 0);
        CHECK_EQ_INT(s.depth, 3);
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: a persistent excess is drained away");
    {
        /*
         * The bug this module exists for. A burst at call setup left the buffer
         * at 20 frames — 400 ms of one-way delay — and it stayed there for the
         * whole call, because one frame in and one frame out never shrinks it.
         */
        syncn_jitter *j = syncn_jitter_create(60); /* target 3 */

        for (int i = 0; i < 20; i++) { make_frame(f, (uint8_t)i); syncn_jitter_push(j, f); }
        CHECK_EQ_INT(depth_of(j), 20);

        /* Now run in perfect lockstep, which is what the real call did. */
        for (int i = 0; i < 4000; i++) {
            make_frame(f, (uint8_t)i);
            syncn_jitter_push(j, f);
            syncn_jitter_pop(j, out);
        }

        const unsigned settled = depth_of(j);
        CHECK(settled <= 4);
        CHECK(trimmed_of(j) > 0);
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: a large burst unwinds quickly, not over minutes");
    {
        /* Waiting five seconds per frame to unwind half a second of delay is
         * too slow to sit through, so a big excess has a fast path. */
        syncn_jitter *j = syncn_jitter_create(60);
        for (int i = 0; i < 40; i++) { make_frame(f, (uint8_t)i); syncn_jitter_push(j, f); }

        for (int i = 0; i < 100; i++) { /* two seconds */
            make_frame(f, (uint8_t)i);
            syncn_jitter_push(j, f);
            syncn_jitter_pop(j, out);
        }
        CHECK(depth_of(j) <= 13); /* target*4 + 1 */
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: real jitter is absorbed, not trimmed away");
    {
        /*
         * The case the previous fix would have got wrong. This buffer dips to
         * the target regularly — it is doing its job. An instantaneous depth
         * threshold would see the peaks and trim, causing dropouts on exactly
         * the connection that needs the buffering most.
         */
        syncn_jitter *j = syncn_jitter_create(60);
        for (int i = 0; i < 3; i++) { make_frame(f, (uint8_t)i); syncn_jitter_push(j, f); }

        int seq = 3;
        for (int cycle = 0; cycle < 40; cycle++) {
            /* A clump of five arrives at once ... */
            for (int i = 0; i < 5; i++) { make_frame(f, (uint8_t)seq++); syncn_jitter_push(j, f); }
            /* ... then five ticks with nothing new, draining back down. */
            for (int i = 0; i < 5; i++)
                syncn_jitter_pop(j, out);
        }

        syncn_jitter_stats s;
        syncn_jitter_get_stats(j, &s);
        CHECK_EQ_INT(s.trimmed, 0);
        CHECK_EQ_INT(s.underruns, 0);
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: an empty buffer underruns rather than repeating audio");
    {
        syncn_jitter *j = syncn_jitter_create(60);
        for (int i = 0; i < 3; i++) { make_frame(f, (uint8_t)i); syncn_jitter_push(j, f); }
        for (int i = 0; i < 3; i++)
            CHECK(syncn_jitter_pop(j, out));

        /* Drained. It must re-prime rather than hand out stale frames. */
        CHECK(!syncn_jitter_pop(j, out));
        make_frame(f, 99);
        syncn_jitter_push(j, f);
        CHECK(!syncn_jitter_pop(j, out)); /* still priming */
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: overflow discards the oldest, keeping the newest audio");
    {
        syncn_jitter *j = syncn_jitter_create(60);
        for (int i = 0; i < SYNCN_JITTER_SLOTS + 10; i++) {
            make_frame(f, (uint8_t)i);
            syncn_jitter_push(j, f);
        }

        syncn_jitter_stats s;
        syncn_jitter_get_stats(j, &s);
        CHECK_EQ_INT(s.dropped, 10);
        CHECK_EQ_INT(s.depth, SYNCN_JITTER_SLOTS);

        /* What survives must be recent audio, not the stale head. A buffer this
         * far over target also trips the burst trim, so the exact frame is not
         * fixed — what matters is that the discarded ones are the old ones. */
        CHECK(syncn_jitter_pop(j, out));
        CHECK(out[0] >= 10);
        syncn_jitter_destroy(j);
    }

    SUITE("jitter: target is clamped to something sane");
    {
        syncn_jitter *j = syncn_jitter_create(0);
        syncn_jitter_stats s;
        syncn_jitter_get_stats(j, &s);
        CHECK(s.target >= 2);
        syncn_jitter_destroy(j);

        j = syncn_jitter_create(100000);
        syncn_jitter_get_stats(j, &s);
        CHECK(s.target <= SYNCN_JITTER_SLOTS / 2);
        syncn_jitter_destroy(j);
    }
}
