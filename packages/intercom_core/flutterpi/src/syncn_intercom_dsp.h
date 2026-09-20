// SPDX-License-Identifier: MIT
//
// Small DSP helpers ported from the other team's syncn-intercom
// (docs/14-audio-as-shipped.md in that repo): a telephony-band bandpass
// filter and an RMS noise gate with hangover. That repo runs these on raw
// ALSA in a standalone daemon; here they're plain PCM-in/PCM-out functions
// called from syncn_intercom_audio.c around the existing GStreamer/AEC3
// pipeline, so nothing about this file depends on GStreamer or AEC3 itself.
//
// Both filters operate on 8kHz mono S16LE PCM, matching this codebase's
// fixed wire format (see syncn_intercom_aec3.h) -- not generalized to other
// rates/channels since nothing here needs that.
#ifndef _SYNCN_INTERCOM_DSP_H
#define _SYNCN_INTERCOM_DSP_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Telephony-band bandpass (250-3400 Hz), ported from the other team's
// mic/downlink filter stage. Implemented as one first-order high-pass into
// one first-order low-pass RC filter in series -- not a sharp brick-wall
// filter, but that repo's own docs describe theirs the same way ("simple
// RC-style filters, not because a sharper crossover was needed"): this is
// about knocking down rumble/DC below 250Hz and roll-off above 3400Hz where
// this door's own capture/playback don't have useful content anyway, not
// audiophile filtering.
struct syncn_bandpass_state {
    double hp_prev_in;
    double hp_prev_out;
    double lp_prev_out;
    double hp_alpha;
    double lp_alpha;
    bool initialized;
};

static inline void syncn_bandpass_init(struct syncn_bandpass_state *state, int sample_rate_hz, double highpass_hz, double lowpass_hz) {
    double dt = 1.0 / (double) sample_rate_hz;
    // Standard first-order RC coefficient derivations.
    double hp_rc = 1.0 / (2.0 * M_PI * highpass_hz);
    state->hp_alpha = hp_rc / (hp_rc + dt);
    double lp_rc = 1.0 / (2.0 * M_PI * lowpass_hz);
    state->lp_alpha = dt / (lp_rc + dt);
    state->hp_prev_in = 0.0;
    state->hp_prev_out = 0.0;
    state->lp_prev_out = 0.0;
    state->initialized = true;
}

// In-place filter over `count` samples. Safe to call with `out == in`.
static inline void syncn_bandpass_process(struct syncn_bandpass_state *state, const int16_t *in, int16_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        double x = (double) in[i];

        // High-pass (removes DC/rumble below highpass_hz).
        double hp_out = state->hp_alpha * (state->hp_prev_out + x - state->hp_prev_in);
        state->hp_prev_in = x;
        state->hp_prev_out = hp_out;

        // Low-pass (rolls off above lowpass_hz), fed from the high-pass output.
        double lp_out = state->lp_prev_out + state->lp_alpha * (hp_out - state->lp_prev_out);
        state->lp_prev_out = lp_out;

        if (lp_out > 32767.0) lp_out = 32767.0;
        if (lp_out < -32768.0) lp_out = -32768.0;
        out[i] = (int16_t) lp_out;
    }
}

// RMS noise gate with hangover, ported from the other team's uplink/downlink
// noise floor gates (docs/14-audio-as-shipped.md: uplink_noise_floor_dbfs=-34,
// downlink_noise_floor_dbfs=-37). While the signal's RMS level stays below
// the floor for longer than the hangover, output is replaced with comfort
// noise (see syncn_comfort_noise_fill below) instead of hard silence -- that
// repo's docs note this door reads sustained hard digital silence as "line
// idle" and stops sending audio, a real protocol-compatibility concern, not
// just a cosmetic one.
struct syncn_noise_gate_state {
    double floor_linear;      // linear RMS threshold, converted from dBFS once
    int hangover_ms;
    int64_t below_floor_since_us;  // 0 if currently above floor
    bool gated;
};

static inline void syncn_noise_gate_init(struct syncn_noise_gate_state *state, double floor_dbfs, int hangover_ms) {
    state->floor_linear = 32767.0 * pow(10.0, floor_dbfs / 20.0);
    state->hangover_ms = hangover_ms;
    state->below_floor_since_us = 0;
    state->gated = false;
}

static inline double syncn_pcm_rms(const int16_t *samples, size_t count) {
    if (count == 0) return 0.0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        double s = (double) samples[i];
        sum_sq += s * s;
    }
    return sqrt(sum_sq / (double) count);
}

// Updates gate state from one frame's RMS and returns whether the frame
// should be gated (replaced with comfort noise). `now_us` is a monotonic
// clock reading (e.g. g_get_monotonic_time()) so this has no dependency on
// any particular timer source.
static inline bool syncn_noise_gate_update(struct syncn_noise_gate_state *state, double rms, int64_t now_us) {
    if (rms >= state->floor_linear) {
        state->below_floor_since_us = 0;
        state->gated = false;
        return false;
    }

    if (state->below_floor_since_us == 0) {
        state->below_floor_since_us = now_us;
    }
    int64_t below_for_us = now_us - state->below_floor_since_us;
    state->gated = below_for_us >= (int64_t) state->hangover_ms * 1000;
    return state->gated;
}

// Fills `out` with low-level comfort noise at `level_dbfs` (ported default:
// -55 dBFS) instead of true digital silence, for the reason given on
// syncn_noise_gate_state above. A simple seeded LCG is enough -- this isn't
// meant to sound like anything, only to keep the line from reading as idle.
static inline void syncn_comfort_noise_fill(int16_t *out, size_t count, double level_dbfs, uint32_t *rng_state) {
    double amplitude = 32767.0 * pow(10.0, level_dbfs / 20.0);
    for (size_t i = 0; i < count; i++) {
        *rng_state = (*rng_state) * 1103515245u + 12345u;
        double unit = ((double) (*rng_state >> 16 & 0x7FFF) / 16384.0) - 1.0;  // ~[-1, 1)
        out[i] = (int16_t) (unit * amplitude);
    }
}

#endif  // _SYNCN_INTERCOM_DSP_H
