#include "syncn/dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --------------------------------------------------------- channel pick --- */

/* Frames to watch before committing. 150 is three seconds at 20 ms — long
 * enough that someone has probably spoken, short enough that the first words of
 * a call are not lost while deciding. */
#define DECIDE_FRAMES 150

/* How much louder one channel must be before it is called the live one. A dead
 * channel measured -66 dBFS against speech at -16, so 12 dB is a wide margin
 * that still refuses to guess when both are genuinely in use. */
#define DECIDE_MARGIN_DB 12.0

struct syncn_chanpick {
    syncn_channel_mode requested;
    syncn_channel_mode decided;
    bool     settled;
    unsigned observed;
    double   energy_left, energy_right;
};

const char *syncn_channel_mode_name(syncn_channel_mode m)
{
    switch (m) {
    case SYNCN_CH_LEFT:  return "left";
    case SYNCN_CH_RIGHT: return "right";
    case SYNCN_CH_MIX:   return "mix";
    case SYNCN_CH_AUTO:
    default:             return "auto";
    }
}

syncn_channel_mode syncn_channel_mode_from_name(const char *name)
{
    if (!name)                          return SYNCN_CH_AUTO;
    if (strcasecmp(name, "left") == 0)  return SYNCN_CH_LEFT;
    if (strcasecmp(name, "right") == 0) return SYNCN_CH_RIGHT;
    if (strcasecmp(name, "mix") == 0)   return SYNCN_CH_MIX;
    return SYNCN_CH_AUTO;
}

syncn_chanpick *syncn_chanpick_create(syncn_channel_mode mode)
{
    syncn_chanpick *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->requested = mode;
    p->decided   = (mode == SYNCN_CH_AUTO) ? SYNCN_CH_LEFT : mode;
    p->settled   = (mode != SYNCN_CH_AUTO);
    return p;
}

void syncn_chanpick_destroy(syncn_chanpick *p) { free(p); }

syncn_channel_mode syncn_chanpick_decided(const syncn_chanpick *p)
{
    return p ? p->decided : SYNCN_CH_MIX;
}

bool syncn_chanpick_is_settled(const syncn_chanpick *p)
{
    return p ? p->settled : false;
}

static double to_db(double energy, size_t n)
{
    if (n == 0 || energy <= 0.0)
        return -100.0;
    const double rms = sqrt(energy / (double)n);
    return 20.0 * log10(rms / 32768.0);
}

void syncn_chanpick_run(syncn_chanpick *p, const int16_t *stereo, size_t frames,
                        int16_t *mono_out)
{
    if (p->requested == SYNCN_CH_AUTO && !p->settled) {
        for (size_t i = 0; i < frames; i++) {
            const double l = stereo[i * 2];
            const double r = stereo[i * 2 + 1];
            p->energy_left  += l * l;
            p->energy_right += r * r;
        }
        p->observed++;

        if (p->observed >= DECIDE_FRAMES) {
            const size_t n = (size_t)p->observed * frames;
            const double ldb = to_db(p->energy_left, n);
            const double rdb = to_db(p->energy_right, n);

            if (ldb - rdb > DECIDE_MARGIN_DB) {
                p->decided = SYNCN_CH_LEFT;
            } else if (rdb - ldb > DECIDE_MARGIN_DB) {
                p->decided = SYNCN_CH_RIGHT;
            } else {
                /* Both carry signal, or neither does. Mixing is the honest
                 * choice when there is no evidence for one over the other. */
                p->decided = SYNCN_CH_MIX;
            }
            p->settled = true;
        }
    }

    /* While deciding, use the provisional choice rather than dropping audio.
     * Losing the first three seconds of a door call to indecision would be a
     * worse bug than the one this fixes. */
    const syncn_channel_mode use = p->decided;

    for (size_t i = 0; i < frames; i++) {
        switch (use) {
        case SYNCN_CH_RIGHT:
            mono_out[i] = stereo[i * 2 + 1];
            break;
        case SYNCN_CH_MIX:
            mono_out[i] = (int16_t)(((int)stereo[i * 2] + stereo[i * 2 + 1]) / 2);
            break;
        case SYNCN_CH_LEFT:
        case SYNCN_CH_AUTO:
        default:
            mono_out[i] = stereo[i * 2];
            break;
        }
    }
}

/* ------------------------------------------------------------- high pass --- */

void syncn_biquad_highpass(syncn_biquad *f, float cutoff_hz, float rate, float q)
{
    const float w0    = 2.0f * (float)M_PI * cutoff_hz / rate;
    const float cosw0 = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);

    const float a0 =  1.0f + alpha;
    f->b0 =  (1.0f + cosw0) / 2.0f / a0;
    f->b1 = -(1.0f + cosw0) / a0;
    f->b2 =  (1.0f + cosw0) / 2.0f / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = ( 1.0f - alpha) / a0;

    syncn_biquad_reset(f);
}

void syncn_biquad_lowpass(syncn_biquad *f, float cutoff_hz, float rate, float q)
{
    const float w0    = 2.0f * (float)M_PI * cutoff_hz / rate;
    const float cosw0 = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);

    const float a0 =  1.0f + alpha;
    f->b0 = (1.0f - cosw0) / 2.0f / a0;
    f->b1 = (1.0f - cosw0) / a0;
    f->b2 = (1.0f - cosw0) / 2.0f / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = ( 1.0f - alpha) / a0;
    syncn_biquad_reset(f);
}

void syncn_biquad_highshelf(syncn_biquad *f, float corner_hz, float rate, float gain_db)
{
    const float A     = powf(10.0f, gain_db / 40.0f);
    const float w0    = 2.0f * (float)M_PI * corner_hz / rate;
    const float cosw0 = cosf(w0);
    const float alpha = sinf(w0) / 2.0f * sqrtf(2.0f);   /* S = 1 */
    const float sa    = 2.0f * sqrtf(A) * alpha;

    const float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + sa;
    f->b0 =  A * ((A + 1.0f) + (A - 1.0f) * cosw0 + sa) / a0;
    f->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0) / a0;
    f->b2 =  A * ((A + 1.0f) + (A - 1.0f) * cosw0 - sa) / a0;
    f->a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0) / a0;
    f->a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - sa) / a0;
    syncn_biquad_reset(f);
}

void syncn_biquad_reset(syncn_biquad *f)
{
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

void syncn_biquad_run(syncn_biquad *f, int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const float x = (float)samples[i];
        float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
                - f->a1 * f->y1 - f->a2 * f->y2;

        f->x2 = f->x1; f->x1 = x;
        f->y2 = f->y1; f->y1 = y;

        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        samples[i] = (int16_t)y;
    }
}

/* ---------------------------------------------------------------- levels --- */

float syncn_rms_dbfs(const int16_t *samples, size_t count)
{
    if (count == 0)
        return -100.0f;

    double sum = 0.0;
    for (size_t i = 0; i < count; i++)
        sum += (double)samples[i] * (double)samples[i];

    const double rms = sqrt(sum / (double)count);
    if (rms < 1e-6)
        return -100.0f;

    const double db = 20.0 * log10(rms / 32768.0);
    return db < -100.0 ? -100.0f : (float)db;
}


/* ------------------------------------------------------------- limiter --- */

void syncn_limiter_init(syncn_limiter *l, float ceiling_dbfs, float release_ms, float rate)
{
    l->ceiling  = powf(10.0f, ceiling_dbfs / 20.0f) * 32767.0f;
    l->envelope = 0.0f;
    l->limited  = 0;
    /* Exponential decay to 1/e over release_ms. */
    const float samples = release_ms * rate / 1000.0f;
    l->release = samples > 1.0f ? expf(-1.0f / samples) : 0.0f;
}

void syncn_limiter_run(syncn_limiter *l, int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const float x  = (float)samples[i];
        const float ax = fabsf(x);

        /* Instant attack: the envelope jumps to any peak at once. Then it
         * decays, so the gain reduction eases off between syllables rather
         * than snapping back and forth inside one. */
        if (ax > l->envelope)
            l->envelope = ax;
        else
            l->envelope *= l->release;

        if (l->envelope > l->ceiling) {
            const float g = l->ceiling / l->envelope;
            samples[i] = (int16_t)(x * g);
            l->limited++;
        }
    }
}

void syncn_limiter_run_gain(syncn_limiter *l, int16_t *samples, size_t count,
                            float gain)
{
    for (size_t i = 0; i < count; i++) {
        float x  = (float)samples[i] * gain;      /* unclamped: may exceed int16 */
        const float ax = fabsf(x);

        if (ax > l->envelope)
            l->envelope = ax;
        else
            l->envelope *= l->release;

        if (l->envelope > l->ceiling) {
            x *= l->ceiling / l->envelope;
            l->limited++;
        }

        /* By now |x| <= ceiling <= 32767, so this clamp is a formality. */
        if (x > 32767.0f)  x = 32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        samples[i] = (int16_t)x;
    }
}


/* ---------------------------------------------------------- half-duplex gate --- */

static double gate_dbfs_to_energy(float dbfs)
{
    const double amp = pow(10.0, dbfs / 20.0) * 32768.0;
    return amp * amp;
}

/* Ramp the frame toward open (1) or closed (0). Shared by both gates. */
static bool gate_apply(syncn_gate *g, int16_t *frame, size_t count)
{
    const float target = g->closed ? 0.0f : 1.0f;
    const int   ramp   = g->closed ? g->release_samples : g->attack_samples;
    const float step   = 1.0f / (float)(ramp > 0 ? ramp : 1);

    for (size_t i = 0; i < count; i++) {
        if (g->level < target) {
            g->level += step;
            if (g->level > target) g->level = target;
        } else if (g->level > target) {
            g->level -= step;
            if (g->level < target) g->level = target;
        }

        float v = (float)frame[i] * g->level;

        /* As the gate closes, comfort noise comes up to fill it. */
        if (g->comfort_amp > 0.0f && g->level < 1.0f) {
            g->noise_seed = g->noise_seed * 1103515245u + 12345u;
            const float white = ((float)((g->noise_seed >> 16) & 0x7fff) / 16383.5f) - 1.0f;
            v += white * g->comfort_amp * (1.0f - g->level);
        }
        frame[i] = (int16_t)v;
    }
    /* The decision, not the level. With a slow release the level lags the
     * decision by the release time, and what the caller counts as "gated"
     * is when the gate decided to shut, not when the fade finished. */
    return g->closed;
}

void syncn_gate_init(syncn_gate *g, float close_dbfs, float hysteresis_db,
                     float hangover_ms, float attack_ms, float release_ms,
                     float comfort_dbfs, float rate)
{
    g->close_energy    = gate_dbfs_to_energy(close_dbfs);
    g->open_energy     = gate_dbfs_to_energy(close_dbfs - hysteresis_db);
    g->hangover_frames = (int)(hangover_ms / 20.0f + 0.5f);
    g->attack_samples  = (int)(attack_ms  * rate / 1000.0f + 0.5f);
    g->release_samples = (int)(release_ms * rate / 1000.0f + 0.5f);
    if (g->attack_samples  < 1) g->attack_samples  = 1;
    if (g->release_samples < 1) g->release_samples = 1;
    /* White noise has an RMS of about 0.58 of its peak; scale so the stated
     * dBFS is the RMS the far end measures. */
    g->comfort_amp = comfort_dbfs < 0.0f
                   ? powf(10.0f, comfort_dbfs / 20.0f) * 32767.0f / 0.577f
                   : 0.0f;
    g->noise_seed  = 0x5A17u;
    g->closed    = false;
    g->hang_left = 0;
    g->level     = 1.0f;
    g->closures  = 0;
}

static void gate_decide(syncn_gate *g, bool above_close, bool above_open)
{
    if (!g->closed) {
        if (above_close) {
            g->closed    = true;
            g->hang_left = g->hangover_frames;
            g->closures++;
        }
    } else {
        if (above_open) {
            g->hang_left = g->hangover_frames;   /* still talking: re-arm */
        } else if (g->hang_left > 0) {
            g->hang_left--;
        } else {
            g->closed = false;
        }
    }
}

bool syncn_gate_run(syncn_gate *g, double far_energy, int16_t *uplink, size_t count)
{
    gate_decide(g, far_energy > g->close_energy, far_energy > g->open_energy);
    return gate_apply(g, uplink, count);
}

bool syncn_gate_run_active(syncn_gate *g, bool far_talking, int16_t *uplink, size_t count)
{
    gate_decide(g, far_talking, far_talking);
    return gate_apply(g, uplink, count);
}

bool syncn_gate_run_yield(syncn_gate *g, int16_t *uplink, size_t count)
{
    g->closed    = false;
    g->hang_left = 0;
    return gate_apply(g, uplink, count);
}

void syncn_noise_gate_init(syncn_gate *g, float floor_dbfs, float hysteresis_db,
                           float hangover_ms, float attack_ms, float release_ms,
                           float comfort_dbfs, float rate)
{
    syncn_gate_init(g, floor_dbfs, hysteresis_db, hangover_ms,
                    attack_ms, release_ms, comfort_dbfs, rate);
    /* Reopen ABOVE the floor, not below it: the sense is inverted. */
    g->open_energy = gate_dbfs_to_energy(floor_dbfs + hysteresis_db);
}

bool syncn_noise_gate_run(syncn_gate *g, double own_energy, int16_t *frame, size_t count)
{
    if (!g->closed) {
        if (own_energy < g->close_energy) {
            /* Quiet. Hold on for the hangover first, so the tail of a word
             * is not cut -- only then shut. */
            if (g->hang_left > 0) {
                g->hang_left--;
            } else {
                g->closed = true;
                g->closures++;
            }
        } else {
            g->hang_left = g->hangover_frames;   /* speech: re-arm the hold */
        }
    } else {
        if (own_energy > g->open_energy) {
            g->closed    = false;
            g->hang_left = g->hangover_frames;
        }
    }
    return gate_apply(g, frame, count);
}
