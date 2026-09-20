#include "syncn/ringer.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- the sound --- */

/*
 * Ding-dong. The two notes of a door chime: E5 then C5, a major third apart,
 * each struck and left to decay. Nothing about this needs to be clever; it
 * needs to be unmistakably a doorbell from across a hallway.
 */
#define DING_HZ   659.25f
#define DONG_HZ   523.25f
#define DING_MS   350
#define DONG_MS   500
#define GAP_MS    1900          /* silence before the next cycle */

static size_t strike(int16_t *out, size_t cap, float hz, unsigned ms, float amp)
{
    const size_t n = (size_t)SYNCN_RING_RATE * ms / 1000;
    if (n > cap)
        return 0;

    /* Decays to about a tenth over the note, with a 5 ms attack so the
     * onset is a strike rather than a click. */
    const float decay  = logf(10.0f) / (float)n;
    const size_t attack = SYNCN_RING_RATE * 5 / 1000;

    for (size_t i = 0; i < n; i++) {
        float env = expf(-decay * (float)i);
        if (i < attack)
            env *= (float)i / (float)attack;
        /* A touch of the octave gives it a bell's edge rather than a pure
         * sine's hum. */
        const float t = (float)i / SYNCN_RING_RATE;
        const float v = (sinf(2.0f * (float)M_PI * hz * t)
                       + 0.25f * sinf(2.0f * (float)M_PI * hz * 2.0f * t)) * env * amp;
        out[i] = (int16_t)(v * 32767.0f);
    }
    return n;
}

size_t syncn_ringer_synth(int16_t *out, size_t cap, float gain_db)
{
    /* -6 dBFS peak at 0 dB: loud enough to be heard down a hallway, and well
     * clear of the rail on a speaker fed through a charge pump. */
    const float amp = 0.4f * powf(10.0f, gain_db / 20.0f);
    size_t n = 0;

    n += strike(out + n, cap - n, DING_HZ, DING_MS, amp);
    n += strike(out + n, cap - n, DONG_HZ, DONG_MS, amp);

    const size_t gap = (size_t)SYNCN_RING_RATE * GAP_MS / 1000;
    if (n + gap <= cap) {
        memset(out + n, 0, gap * sizeof *out);
        n += gap;
    }
    return n;
}

