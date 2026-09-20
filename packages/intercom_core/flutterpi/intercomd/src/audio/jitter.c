#include "syncn/jitter.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct syncn_jitter {
    pthread_mutex_t lock;
    uint8_t  slot[SYNCN_JITTER_SLOTS][SYNCN_AUDIO_FRAME_BYTES];
    unsigned head, tail, count;
    unsigned target;
    bool     priming;

    /* Lowest depth seen in the current observation window. A buffer whose floor
     * sits above the target is holding latency it is not using. */
    unsigned window_min;
    unsigned window_ticks;

    uint64_t received, underruns, dropped, trimmed;
};

syncn_jitter *syncn_jitter_create(int target_ms)
{
    syncn_jitter *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;

    pthread_mutex_init(&j->lock, NULL);

    j->target = (unsigned)(target_ms / (int)SYNCN_AUDIO_FRAME_MS);
    if (j->target < 2)
        j->target = 2;
    if (j->target > SYNCN_JITTER_SLOTS / 2)
        j->target = SYNCN_JITTER_SLOTS / 2;

    j->priming    = true;
    j->window_min = UINT_MAX;
    return j;
}

void syncn_jitter_destroy(syncn_jitter *j)
{
    if (!j)
        return;
    pthread_mutex_destroy(&j->lock);
    free(j);
}

void syncn_jitter_push(syncn_jitter *j, const uint8_t *frame)
{
    pthread_mutex_lock(&j->lock);
    j->received++;

    if (j->count == SYNCN_JITTER_SLOTS) {
        /* Full. Drop the OLDEST, not the newest: in a live call stale audio is
         * worthless, and keeping it would only add delay. */
        j->tail = (j->tail + 1) % SYNCN_JITTER_SLOTS;
        j->count--;
        j->dropped++;
    }

    memcpy(j->slot[j->head], frame, SYNCN_AUDIO_FRAME_BYTES);
    j->head = (j->head + 1) % SYNCN_JITTER_SLOTS;
    j->count++;

    if (j->priming && j->count >= j->target)
        j->priming = false;

    pthread_mutex_unlock(&j->lock);
}

bool syncn_jitter_pop(syncn_jitter *j, uint8_t *out)
{
    bool got = false;
    pthread_mutex_lock(&j->lock);

    if (j->priming || j->count == 0) {
        if (!j->priming)
            j->underruns++;
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    memcpy(out, j->slot[j->tail], SYNCN_AUDIO_FRAME_BYTES);
    j->tail = (j->tail + 1) % SYNCN_JITTER_SLOTS;
    j->count--;
    got = true;

    if (j->count == 0) {
        /* Drained. Re-accumulate before resuming rather than playing each
         * frame the instant it lands, which would underrun on the next hiccup. */
        j->priming      = true;
        j->window_ticks = 0;
        j->window_min   = UINT_MAX;
        pthread_mutex_unlock(&j->lock);
        return got;
    }

    /*
     * Drain latency the buffer is not using.
     *
     * Measured AFTER the pop, deliberately: what matters is how many frames are
     * still queued once this tick has taken its own, which is the delay the far
     * end's voice is actually carrying. Measuring before the pop reads one
     * frame high, so a perfectly healthy buffer looks permanently over-deep and
     * gets trimmed — discarding live audio on every good call.
     *
     * The test is not "is it deep right now" but "has it stayed deep". A buffer
     * that reaches the target sometimes is absorbing real jitter and must be
     * left alone; one whose FLOOR sits above the target across five seconds is
     * carrying dead delay. The window minimum tells those apart; an
     * instantaneous threshold cannot.
     *
     * One frame per window keeps the correction inaudible: 20 ms, which during
     * speech is a slightly shortened gap rather than a click.
     */
    if (j->count < j->window_min)
        j->window_min = j->count;

    if (++j->window_ticks >= SYNCN_JITTER_DRAIN_WINDOW) {
        if (j->window_min > j->target) {
            j->tail = (j->tail + 1) % SYNCN_JITTER_SLOTS;
            j->count--;
            j->trimmed++;
        }
        j->window_ticks = 0;
        j->window_min   = UINT_MAX;
    }

    /* Fast path for a large burst. Unwinding half a second of delay one frame
     * per five seconds is too slow to sit through. */
    if (j->count > j->target * 4) {
        j->tail = (j->tail + 1) % SYNCN_JITTER_SLOTS;
        j->count--;
        j->trimmed++;
    }

    pthread_mutex_unlock(&j->lock);
    return got;
}

void syncn_jitter_get_stats(syncn_jitter *j, syncn_jitter_stats *out)
{
    pthread_mutex_lock(&j->lock);
    out->received  = j->received;
    out->underruns = j->underruns;
    out->dropped   = j->dropped;
    out->trimmed   = j->trimmed;
    out->depth     = j->count;
    out->target    = j->target;
    pthread_mutex_unlock(&j->lock);
}
