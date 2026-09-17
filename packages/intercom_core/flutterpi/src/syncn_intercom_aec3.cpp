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
};

extern "C" struct syncn_aec3 *syncn_aec3_create(
    int sample_rate_hz,
    int num_channels,
    int estimated_render_delay_ms,
    bool noise_suppression_enabled,
    double capture_gain_db
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
    // gain_controller1 (the legacy AGC) is left disabled -- verified via a
    // standalone harness against this exact library build that BOTH of its
    // gain knobs (target_level_dbfs, then compression_gain_db) have ZERO
    // measurable effect on output amplitude in kFixedDigital mode, across
    // their full documented ranges. A manual post-ProcessStream multiply
    // was shipped as a workaround (panel-v1.3.93) and confirmed on
    // real hardware to still leave uplink too quiet -- meaning even that
    // workaround likely never executed, or v1.3.93 itself did not actually
    // reach the device correctly; not fully root-caused.
    //
    // gain_controller2 (AGC2) is the fix: fixed_digital.gain_db is a
    // genuinely live, in-dB gain knob in this library build -- verified via
    // the same harness, doubling output amplitude roughly every 6dB
    // (0->0.97x, 6->1.94x, 12->3.86x, 18->7.71x, 24->15.38x), and confirmed
    // NOT to fight AEC3 under a synthetic double-talk test (loud simulated
    // far-end echo mixed into the capture signal): AEC3 strips the leaked
    // echo before AGC2 amplifies what's left, output stayed at 2% of full
    // scale with zero clipping. adaptive_digital is left disabled for
    // predictable, unsurprising behavior -- pure fixed gain, no
    // level-tracking that could drift.
    config.gain_controller2.enabled = true;
    config.gain_controller2.fixed_digital.gain_db = static_cast<float>(capture_gain_db);
    config.gain_controller2.adaptive_digital.enabled = false;
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

    return true;
}
