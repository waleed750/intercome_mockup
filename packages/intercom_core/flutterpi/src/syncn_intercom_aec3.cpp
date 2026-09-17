// SPDX-License-Identifier: MIT
//
// Implementation of syncn_intercom_aec3.h. See that header for the full
// rationale (why AEC3 is called directly instead of through GStreamer's
// webrtcdsp/webrtcechoprobe).

#include "syncn_intercom_aec3.h"

#include <cstring>
#include <memory>

#include <api/scoped_refptr.h>
#include <modules/audio_processing/include/audio_processing.h>

// No custom EchoControlFactory is needed. AudioProcessingImpl itself
// constructs an EchoCanceller3 (AEC3) internally whenever
// Config::echo_canceller.enabled is true and mobile_mode is false (both set
// below) and no factory was provided via SetEchoControlFactory --
// confirmed by reading audio_processing_impl.cc directly:
//
//   if (echo_control_factory_) { ... } else {
//     EchoCanceller3Config config = ...;
//     submodules_.echo_controller =
//         std::make_unique<EchoCanceller3>(config, ...);
//   }
//
// A custom factory was tried first, but EchoCanceller3's own class
// declaration (modules/audio_processing/aec3/echo_canceller3.h) is NOT a
// publicly installed header in this library (only
// api/audio/echo_canceller3_config.h is) -- confirmed via a local `meson
// install` dry run. Relying on the library's own default construction path
// avoids depending on an internal header entirely.

struct syncn_aec3 {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream_config;
    int num_channels;
    double capture_gain_multiplier;
};

extern "C" struct syncn_aec3 *syncn_aec3_create(
    int sample_rate_hz,
    int num_channels,
    int estimated_render_delay_ms,
    bool noise_suppression_enabled,
    double capture_gain_multiplier
) {
    if (sample_rate_hz != 8000 || num_channels != 1) {
        // The wrapper (and the wire format it serves) is 8kHz mono only --
        // see the header comment. Not worth generalizing until something
        // actually needs it.
        return nullptr;
    }

    webrtc::AudioProcessingBuilder builder;

    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false;  // required for AEC3, see comment above
    config.noise_suppression.enabled = noise_suppression_enabled;
    // gain_controller1 deliberately left disabled (default: false). See
    // syncn_aec3_create's header comment for the measurement showing
    // compression_gain_db has no effect in this library build's
    // kFixedDigital mode -- gain is applied manually in
    // syncn_aec3_process_capture() instead, after AEC3 has already
    // cancelled the echo.
    config.high_pass_filter.enabled = true;

    webrtc::AudioProcessing *raw = builder.Create();
    if (raw == nullptr) {
        return nullptr;
    }

    rtc::scoped_refptr<webrtc::AudioProcessing> apm(raw);
    apm->ApplyConfig(config);

    struct syncn_aec3 *aec = new (std::nothrow) struct syncn_aec3();
    if (aec == nullptr) {
        return nullptr;
    }
    aec->apm = apm;
    aec->stream_config = webrtc::StreamConfig(sample_rate_hz, static_cast<size_t>(num_channels));
    aec->num_channels = num_channels;
    aec->capture_gain_multiplier = capture_gain_multiplier;

    if (estimated_render_delay_ms >= 0) {
        aec->apm->set_stream_delay_ms(estimated_render_delay_ms);
    }

    return aec;
}

extern "C" void syncn_aec3_destroy(struct syncn_aec3 *aec) {
    if (aec == nullptr) {
        return;
    }
    delete aec;
}

// WebRTC APM requires exactly 10ms/80-sample chunks (kChunkSizeMs). Our wire
// frames are 20ms/160 samples. Splits each 20ms buffer into two 10ms
// sub-frames and feeds them one at a time -- no state carried between calls
// beyond what's already inside the APM instance itself, since 160 samples at
// 8kHz mono always divides evenly into two 80-sample halves (no partial
// leftover to buffer across calls).
static constexpr size_t k10msSamplesAt8kHz = 80;

extern "C" bool syncn_aec3_process_render(struct syncn_aec3 *aec, const int16_t *samples, size_t num_samples_20ms) {
    if (aec == nullptr || samples == nullptr) {
        return false;
    }
    if (num_samples_20ms != 2 * k10msSamplesAt8kHz) {
        return false;
    }

    for (int half = 0; half < 2; half++) {
        const int16_t *chunk = samples + (half * k10msSamplesAt8kHz);
        // ProcessReverseStream's int16 overload explicitly documents that
        // src and dest may share the same memory -- pass chunk for both
        // since we don't need (or want to mutate) the render output here,
        // only to feed it as the echo reference.
        int ret = aec->apm->ProcessReverseStream(
            chunk,
            aec->stream_config,
            aec->stream_config,
            const_cast<int16_t *>(chunk)
        );
        if (ret != webrtc::AudioProcessing::kNoError) {
            return false;
        }
    }
    return true;
}

extern "C" bool syncn_aec3_process_capture(struct syncn_aec3 *aec, const int16_t *samples, int16_t *out, size_t num_samples_20ms, int delay_ms_hint) {
    if (aec == nullptr || samples == nullptr || out == nullptr) {
        return false;
    }
    if (num_samples_20ms != 2 * k10msSamplesAt8kHz) {
        return false;
    }

    if (delay_ms_hint >= 0) {
        aec->apm->set_stream_delay_ms(delay_ms_hint);
    }

    for (int half = 0; half < 2; half++) {
        const int16_t *in_chunk = samples + (half * k10msSamplesAt8kHz);
        int16_t *out_chunk = out + (half * k10msSamplesAt8kHz);
        // ProcessStream's int16 overload also documents src/dest may alias
        // -- callers of this wrapper are free to pass the same pointer for
        // samples and out for in-place processing.
        int ret = aec->apm->ProcessStream(
            in_chunk,
            aec->stream_config,
            aec->stream_config,
            out_chunk
        );
        if (ret != webrtc::AudioProcessing::kNoError) {
            return false;
        }
    }

    // Manual makeup gain, applied AFTER echo cancellation -- see
    // syncn_aec3_create's header comment for why this replaces AEC3's own
    // (measured-inert) gain_controller1. Verified via a standalone harness
    // not to clip a realistic speech-envelope signal at this codebase's
    // chosen multiplier; still clamp defensively per-sample since real mic
    // input is not guaranteed to match that synthetic envelope exactly.
    if (aec->capture_gain_multiplier != 1.0) {
        for (size_t i = 0; i < num_samples_20ms; i++) {
            double scaled = static_cast<double>(out[i]) * aec->capture_gain_multiplier;
            if (scaled > 32767.0) {
                out[i] = 32767;
            } else if (scaled < -32768.0) {
                out[i] = -32768;
            } else {
                out[i] = static_cast<int16_t>(scaled);
            }
        }
    }

    return true;
}
