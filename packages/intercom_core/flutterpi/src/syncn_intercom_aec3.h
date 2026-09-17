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
// noise_suppression_enabled / agc_target_level_dbfs mirror the settings this
// codebase already measured and kept from the old webrtcdsp config
// (noise-suppression=false, gain-control=true fixed-digital
// target-level-dbfs=3) -- see capture_desc_aec's history in
// syncn_intercom_audio.c for why those specific values. Pass through rather
// than hardcoding here so a future tuning change doesn't need to touch this
// file.
//
// Returns NULL on failure (logs nothing itself -- caller logs via
// syncn_intercom_debug_log, matching this codebase's existing convention).
struct syncn_aec3 *syncn_aec3_create(
    int sample_rate_hz,
    int num_channels,
    int estimated_render_delay_ms,
    bool noise_suppression_enabled,
    int agc_target_level_dbfs
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

// Feeds one 20ms PCM chunk of CAPTURE (near-end / mic / uplink) audio and
// writes the echo-cancelled result into `out` (caller-allocated, same size
// as `samples` -- may alias `samples` for in-place processing, matching
// AudioProcessing::ProcessStream's documented support for in-place use).
// This is the direct replacement for webrtcdsp's role on the capture side.
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
