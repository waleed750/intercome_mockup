// SPDX-License-Identifier: MIT
//
// C wrapper around webrtc-audio-processing's AudioProcessing/EchoCanceller3,
// called directly instead of routing through GStreamer's webrtcdsp/
// webrtcechoprobe elements.
//
// WHY THIS EXISTS (2026-09-17): webrtcdsp and webrtcechoprobe lived in two
// SEPARATE GstPipelines here (capture and playback are built and torn down
// independently -- see start_locked() in syncn_intercom_audio.c). GStreamer's
// own webrtcdsp documentation states plainly: "The probe can only be used
// within the same top level GstPipeline." Linking the probe by name across
// pipeline boundaries (as the old code did via
// g_object_set(dsp, "probe", "syncn_echoprobe", NULL)) let the property set
// succeed and the code log "AEC engaged", but no cancellation ever actually
// ran -- confirmed via a GStreamer maintainer giving identical advice to
// someone with the same two-pipeline ARM-board setup on Discourse, and via
// the Collabora writeup by the element's author explaining the probe
// converts timestamps to *running time* plus *pipeline latency* -- both
// per-pipeline values that cannot align across two separate clocks.
//
// Merging capture and playback into one GstPipeline was considered (and is
// what GStreamer's docs prescribe) but tested on-device with the OLD 0.3
// webrtc-audio-processing library that Debian 11 ships: AEC engaged (no
// runaway howling) but quality was poor -- full sentences destroyed,
// disturbing noise, usable only close to the mic. AEC3 (this file) is a
// materially better algorithm, purpose-built for variable/unknown delay --
// exactly this hardware's situation (mic and speaker centimetres apart,
// buffered ~280ms playback path: 80ms jitter queue + 200ms ALSA
// buffer-time) -- and calling it directly gives full control over frame
// timing instead of relying on GStreamer pipeline latency, which was never
// reliable here to begin with.
//
// Frame size: WebRTC APM requires EXACTLY 10ms chunks (80 samples at 8kHz,
// per AudioProcessing::kChunkSizeMs). The wire protocol's frames are 20ms/
// 160 bytes A-law. This wrapper takes 20ms PCM buffers at the call sites
// (matching audiobuffersplit's existing output-buffer-duration-fraction=1/50
// framing) and splits/processes them as two 10ms sub-frames internally, so
// callers don't have to know about the 10ms requirement.
//
// Threading: an instance of `struct syncn_aec3` is NOT internally
// thread-safe. The real webrtc::AudioProcessing IS documented thread-safe
// between its capture-side and render-side calls specifically (that's the
// whole point -- one thread feeds ProcessReverseStream from playback,
// another feeds ProcessStream from capture, concurrently), so ProcessRender
// and ProcessCapture below may be called concurrently from the playback and
// capture threads respectively without an external lock. Do not add one --
// see handle_play_downlink's existing comment on why holding self->lock
// across this kind of work starves the other direction.
#ifndef _SYNCN_INTERCOM_AEC3_H
#define _SYNCN_INTERCOM_AEC3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct syncn_aec3;

// sample_rate_hz must be 8000 (the only rate this wrapper/the call format
// uses); num_channels must be 1. estimated_render_delay_ms is a starting
// point only -- AEC3 estimates and refines the real delay continuously, but
// give it something close to the truth (our known ~280ms: 80ms jitter queue
// + 200ms ALSA buffer-time) rather than 0, which is what AEC3 assumes if you
// never call syncn_aec3_set_stream_delay_ms at all.
//
// noise_suppression_enabled mirrors the setting this codebase already
// measured and kept from the old webrtcdsp config (noise-suppression=false
// -- see capture_desc's history in syncn_intercom_audio.c: NS=moderate was
// measured destroying ~80% of an already-weak signal).
//
// capture_gain_multiplier (2026-09-17): AEC3's own gain_controller1 is
// DELIBERATELY left disabled here (see .cpp) -- config.gain_controller1
// .compression_gain_db was tested across the full 0-18 dB range in this
// exact library build via a standalone harness (fed a real built
// AudioProcessing instance synthetic tones, not guessed) and measured ZERO
// effect on output amplitude every time (steady ~3.1x gain regardless of
// the value set). Whatever internal path this build's kFixedDigital mode
// is supposed to use, it isn't reachable through that documented field, and
// I was not willing to ship a second guess after getting the equivalent
// target_level_dbfs field wrong first. A plain linear multiplier applied
// in C, AFTER AEC3's ProcessStream returns and BEFORE A-law encoding, is
// simple, predictable, and was verified (same harness) not to clip at 4.0x
// even against a synthetic speech-envelope signal with realistic
// 5x-average peaks (peaked at only 15% of full scale at 6.0x). Pass 1.0
// for no additional gain. Values above ~6-8x should be re-verified before
// use -- this codebase has hit real distortion/whine at aggressive gains
// before (capvol=5.0, capvol=1.8x on this same mic), though those were
// applied to the UNCANCELLED signal; gain after AEC3 cancellation is
// expected to be safer since it's not also amplifying uncancelled echo,
// but that expectation itself is untested on real hardware as of this
// writing.
//
// Returns NULL on failure (logs nothing itself -- caller logs via
// syncn_intercom_debug_log, matching this codebase's existing convention).
struct syncn_aec3 *syncn_aec3_create(
    int sample_rate_hz,
    int num_channels,
    int estimated_render_delay_ms,
    bool noise_suppression_enabled,
    double capture_gain_multiplier
);

void syncn_aec3_destroy(struct syncn_aec3 *aec);

// Feeds one 20ms PCM chunk of RENDER (far-end / playback / downlink) audio
// as the echo reference -- call this with the SAME audio that is about to
// reach the speaker, ideally just before/as it is pushed to appsrc. This is
// the direct replacement for webrtcechoprobe's role.
//
// samples must point to exactly num_samples_20ms int16_t samples (160 at
// 8kHz mono for a 20ms frame). Splits internally into two 10ms sub-frames.
// Returns false if the APM call fails (logs nothing; see above).
bool syncn_aec3_process_render(struct syncn_aec3 *aec, const int16_t *samples, size_t num_samples_20ms);

// Feeds one 20ms PCM chunk of CAPTURE (near-end / mic / uplink) audio,
// applies echo cancellation, then multiplies by capture_gain_multiplier
// (see syncn_aec3_create's comment) and clamps to int16 range, writing the
// result into `out` (caller-allocated, same size as `samples` -- may alias
// `samples` for in-place processing, matching AudioProcessing::
// ProcessStream's documented support for in-place use). This is the direct
// replacement for webrtcdsp's role on the capture side.
//
// delay_ms_hint, if >= 0, is passed to set_stream_delay_ms() before
// processing (see AudioProcessing's own documented usage pattern: call
// set_stream_delay_ms once per capture frame). Pass -1 to skip updating it
// and rely on AEC3's internal estimate alone.
//
// Returns false if the APM call fails.
bool syncn_aec3_process_capture(struct syncn_aec3 *aec, const int16_t *samples, int16_t *out, size_t num_samples_20ms, int delay_ms_hint);

#ifdef __cplusplus
}
#endif

#endif  // _SYNCN_INTERCOM_AEC3_H
