/*
 * Downlink jitter buffer.
 *
 * Absorbs network jitter on the door's audio without accumulating latency it
 * does not need. Those two goals pull against each other, which is why this is
 * its own module with its own tests rather than a few lines inside the audio
 * thread: the first version held 400 ms of dead delay for an entire call and
 * nobody could tell from the code that it would.
 *
 * The network thread pushes; the audio thread pops once per 20 ms tick.
 */
#ifndef SYNCN_JITTER_H
#define SYNCN_JITTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syncn/alaw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_JITTER_SLOTS 64  /* 1.28 s ceiling */

/*
 * How long to watch before deciding the buffer is permanently too deep.
 * 250 pops is five seconds: long enough that ordinary jitter has had room to
 * drain the buffer at least once, short enough that nobody sits through a
 * minute of needless delay.
 */
#define SYNCN_JITTER_DRAIN_WINDOW 250

typedef struct {
    uint64_t received;
    uint64_t underruns; /* nothing to play when a frame was due          */
    uint64_t dropped;   /* buffer full; the far end is ahead of us       */
    uint64_t trimmed;   /* discarded to pull latency back down           */
    unsigned depth;     /* frames held right now                         */
    unsigned target;
} syncn_jitter_stats;

typedef struct syncn_jitter syncn_jitter;

/* `target_ms` is the depth to accumulate before playback starts. Clamped to at
 * least two frames and at most half the ring. */
syncn_jitter *syncn_jitter_create(int target_ms);
void          syncn_jitter_destroy(syncn_jitter *j);

/* Frames must be SYNCN_AUDIO_FRAME_BYTES. Safe from any thread. */
void syncn_jitter_push(syncn_jitter *j, const uint8_t *frame);

/* Returns false if nothing is available, leaving `out` untouched. */
bool syncn_jitter_pop(syncn_jitter *j, uint8_t *out);

void syncn_jitter_get_stats(syncn_jitter *j, syncn_jitter_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_JITTER_H */
