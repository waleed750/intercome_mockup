// SPDX-License-Identifier: MIT
//
// flutter-pi native implementation of "syncn_intercom/audio" (MethodChannel)
// and "syncn_intercom/audio_uplink" (EventChannel).
//
// Mirrors AudioPipelineHandler.kt: 8kHz mono 16-bit PCM <-> A-law, 160-byte
// (20ms) frames. `start({captureEnabled})` brings up a GStreamer playback
// pipeline for the downlink (A-law -> speaker) and, if captureEnabled, a
// capture pipeline for the uplink (mic -> A-law -> events on the
// audio_uplink EventChannel). `setMuted(bool)` mirrors the Android behavior:
// it mutes the *outgoing mic* only (via a `volume` element ahead of the
// encoder, matching Android's "still send frames, but silence" approach),
// not the speaker. `playDownlink(Uint8List)` pushes one A-law frame into the
// playback appsrc. `stop()` tears both pipelines down.
//
// AEC/NS/AGC parity with the Android path's AcousticEchoCanceler/
// NoiseSuppressor/AutomaticGainControl: implemented via GstElements
// `webrtcechoprobe` (tapped off the playback pipeline, downstream of
// alawdec, as the far-end reference) and `webrtcdsp` (in the capture
// pipeline, ahead of the encoder). Both live in gst-plugins-bad and pair up
// automatically process-wide -- they don't need to share a GstBin/GstBus,
// which is why this works fine across our two separate playback/capture
// GstPipelines. Guarded by gst_element_available() so a panel image without
// gst-plugins-bad still gets working (just non-echo-cancelled) audio instead
// of every call failing to start.
//
// Headset routing: this board (rk809 codec) exposes no kernel jack-detect
// input device (confirmed via /proc/bus/input/devices), so the panel can't
// tell in software whether a headset is plugged in. `Playback Path`
// defaults to SPK (speaker only) and can be switched to headset-only (HP +
// Hands Free Mic) via the `headsetMode` start() arg / `setHeadsetMode`
// method, driven by a manual toggle in the app -- see
// AudioPipeline.setHeadsetMode in the Dart layer.
//
// Was SPK_HP (speaker + headphone always both on) until confirmed
// on-device 2026-09-09 that SPK_HP produced quiet/unclear call audio on
// this rk817_codec driver, while plain SPK alone was loud and clear on
// the same hardware/enclosure -- this panel has no headphone jack in
// active use, so there's no benefit to keeping HP live simultaneously,
// only the apparent cost of split/attenuated output.
//
// Diagnostic mode (SYNCN_INTERCOM_AUDIO_DIAG=1): the original per-buffer
// logging in handle_play_downlink/on_new_capture_sample stopped after the
// first 30 buffers (~600ms at 20ms/frame) -- nowhere near long enough to
// catch a whine/noise that starts or persists later into a call. This env
// var extends logging to a bounded ~20s window (SYNCN_DIAG_MAX_BUFFERS)
// per pipeline start, rate-limited to 1-in-N buffers within that window so
// the extra fopen/fprintf/fclose per logged buffer (see
// syncn_intercom_debug_log, which blocks on real file I/O on this same
// GStreamer streaming thread) doesn't itself perturb the audio it's
// measuring. Off by default -- every call still gets the original
// unconditional first-30-buffers logging regardless of this flag, this
// only ADDS the extended window on top when explicitly enabled.
//
// Computes A-law-domain stats (min/max/distinct-byte-count over the
// buffer) rather than decoding to PCM here -- PCM decode is alawdec's job
// downstream of appsrc; duplicating that decode in this diagnostic path
// would risk drifting from what the real pipeline does. distinct-byte-
// count is a cheap, decode-free proxy for "is this buffer constant/near-
// silent" (true digital silence is a single repeated byte; real encoded
// speech is not) -- good enough to tell "nothing being received" apart
// from "something is being received, decoded PCM quality unknown", which
// is the actual open question per the fix plan: whether the whine
// originates in the encoded bytes arriving over the network (this layer)
// or gets introduced later during decode/DSP/playback (a separate,
// PCM-level check would be needed downstream in alawdec/webrtcdsp/alsasink
// to fully rule that in or out -- not done here).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "syncn_intercom_audio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "syncn_intercom_debug.h"
#include "syncn_intercom_gst_util.h"
#include "util/logging.h"

// See "Diagnostic mode" comment in the file header. -1 means "not yet
// read from the environment"; read lazily once on first use rather than
// at load time, since getenv() this early in plugin registration has
// bitten this codebase before (see set_alsa_voice_routing's PANEL_WIDTH
// usage, which is read at start_locked() time for the same reason -- env
// vars set by the launching systemd unit are guaranteed present by then).
static int g_diag_enabled = -1;
static bool diag_mode_enabled(void) {
    if (g_diag_enabled < 0) {
        const char *v = getenv("SYNCN_INTERCOM_AUDIO_DIAG");
        g_diag_enabled = (v != NULL && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return g_diag_enabled == 1;
}

// ~20s at 20ms/frame (160-byte A-law frames per the file header), per
// pipeline start -- matches the fix plan's suggested bounded-capture
// window. Diagnostic logging is additionally rate-limited to 1-in-10
// buffers within this window (see callers) to keep the extra blocking
// file I/O off the hot path as much as possible while still covering
// enough of a real call to catch a whine that starts after the original
// 30-buffer cutoff.
#define SYNCN_DIAG_MAX_BUFFERS 1000
#define SYNCN_DIAG_LOG_EVERY_N 10

// Raw call-audio capture (2026-09-10): opt-in dump of the actual A-law
// bytes flowing through a real call, in both directions, to disk --
// added after an extended on-device investigation into a "noisy/voice-
// cancelled" call-audio complaint that never reproduced in isolated
// single-ended testing (a local mic recording sounded clean every time)
// and was unaffected by toggling AEC/NS/gain settings, suggesting the
// live audio itself needed to be inspected directly rather than judged
// by ear or inferred from settings changes. Separate from
// SYNCN_INTERCOM_AUDIO_DIAG (verbose per-buffer *logging*, rate-limited
// and bounded) -- this instead writes the *actual bytes* so they can be
// pulled off-device and inspected (played back, viewed in a
// spectrogram, decoded from A-law and compared numerically) after a
// real call. Gated behind its own env var so it never runs by default;
// bounded to the same ~20s/1000-frame window as diag mode so it can't
// grow unbounded on a long call. Files are raw A-law bytes with no
// header (not a .wav) -- any tool decoding A-law (e.g. `sox -t al -r
// 8000 -c 1 in.al out.wav`) can convert them for playback/analysis.
static int g_raw_capture_enabled = -1;
static bool raw_capture_enabled(void) {
    if (g_raw_capture_enabled < 0) {
        const char *v = getenv("SYNCN_INTERCOM_AUDIO_RAW_CAPTURE");
        g_raw_capture_enabled = (v != NULL && strcmp(v, "1") == 0) ? 1 : 0;
    }
    return g_raw_capture_enabled == 1;
}

#define SYNCN_RAW_CAPTURE_DIR "/var/log/syncn-panel"
#define SYNCN_RAW_CAPTURE_UPLINK_PATH SYNCN_RAW_CAPTURE_DIR "/syncn_call_audio_uplink.al"
#define SYNCN_RAW_CAPTURE_DOWNLINK_PATH SYNCN_RAW_CAPTURE_DIR "/syncn_call_audio_downlink.al"

// Lazily opened on first write per pipeline start, truncating any
// previous capture -- these are debug artifacts meant to be pulled
// after ONE test call, not accumulated across calls/restarts.
static FILE *g_raw_capture_uplink_file = NULL;
static FILE *g_raw_capture_downlink_file = NULL;

static void raw_capture_write(FILE **file_slot, const char *path, const uint8_t *data, size_t size) {
    if (*file_slot == NULL) {
        *file_slot = fopen(path, "wb");
        if (*file_slot == NULL) {
            return;
        }
    }
    fwrite(data, 1, size, *file_slot);
    fflush(*file_slot);
}

static void raw_capture_close_all(void) {
    if (g_raw_capture_uplink_file != NULL) {
        fclose(g_raw_capture_uplink_file);
        g_raw_capture_uplink_file = NULL;
    }
    if (g_raw_capture_downlink_file != NULL) {
        fclose(g_raw_capture_downlink_file);
        g_raw_capture_downlink_file = NULL;
    }
}

// Cheap, decode-free A-law-domain buffer summary: min/max byte value and
// count of distinct byte values seen (capped at 256, obviously, but we
// only need to distinguish "1" i.e. perfectly constant from "more than
// 1"). Real encoded speech/noise varies buffer-to-buffer and byte-to-byte;
// true digital silence in A-law is a single repeated byte (0xD5). This
// does NOT decode to PCM -- see file header comment for why -- so it
// cannot report RMS/peak/clipping in PCM terms, only "does this look like
// silence in the encoded domain or not".
struct syncn_alaw_buffer_stats {
    uint8_t min_byte;
    uint8_t max_byte;
    int distinct_count;
    bool is_constant;
};

static struct syncn_alaw_buffer_stats syncn_alaw_stats(const uint8_t *data, size_t size) {
    struct syncn_alaw_buffer_stats stats = { .min_byte = 0, .max_byte = 0, .distinct_count = 0, .is_constant = true };
    if (size == 0) return stats;

    bool seen[256] = { false };
    stats.min_byte = data[0];
    stats.max_byte = data[0];
    for (size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        if (b < stats.min_byte) stats.min_byte = b;
        if (b > stats.max_byte) stats.max_byte = b;
        if (!seen[b]) {
            seen[b] = true;
            stats.distinct_count++;
        }
    }
    stats.is_constant = (stats.distinct_count == 1);
    return stats;
}

// Half-duplex uplink suppression (2026-09-13).
//
// Why this exists: the far end hears themselves echoed whenever this panel's
// mic is live, and that echo survives everything AEC can do to it. Confirmed
// on-device across two builds -- toggling echo-cancel changed nothing, and
// correcting the echo probe's placement (it had been on a dead-end tee stub,
// so webrtcdsp was fed a ~0ms delay for an echo that returned hundreds of ms
// later) did not stop it either.
//
// This codebase already recorded why, on a sibling panel sharing the same
// rk809/rk817 codec (see _hardwareMicAvailable in call_controller.dart): the
// capture path picks up "electrical crosstalk from the speaker/DAC on the
// shared audio codec rather than real acoustic sound, which AEC (built for
// acoustic echo) did not meaningfully cancel". That was only noticed there
// because the unit had no microphone at all. On a panel that does have one,
// the same crosstalk simply blends into real mic audio and looks like a badly
// tuned echo canceller. This panel additionally drives playvol at 1.5x (gated
// to PANEL_WIDTH=800), which scales the DAC output -- and any crosstalk
// riding on it -- accordingly.
//
// So: stop trying to cancel it, and stop transmitting it. While the far end
// is speaking, replace the uplink payload with A-law silence. Frames keep
// flowing at the same 20ms cadence -- the door gates its downlink on seeing
// continuous CC frames, so going quiet is not an option -- they just carry
// nothing to echo back.
//
// Trade-off, stated plainly: this makes the call half-duplex. The panel user
// cannot talk over the far end, and the first moments of their reply may be
// clipped. That is how most door intercoms behave, and it is what this door
// appears to expect.
//
// Tunables, all overridable at runtime so this can be A/B'd without a rebuild:
//   SYNCN_INTERCOM_AUDIO_HALF_DUPLEX=0   disable entirely (default: enabled)
//   SYNCN_INTERCOM_AUDIO_HD_THRESHOLD=N  mean |amplitude| that counts as
//                                        far-end speech (default 800; A-law
//                                        digital silence decodes to ~8)
//   SYNCN_INTERCOM_AUDIO_HD_HANGOVER=N   ms to keep suppressing after the
//                                        far end stops (default 300)
#define SYNCN_HD_DEFAULT_THRESHOLD 800
#define SYNCN_HD_DEFAULT_HANGOVER_MS 300

static int g_half_duplex_enabled = -1;
static int g_hd_threshold = -1;
static int g_hd_hangover_ms = -1;

static int syncn_env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    if (v == NULL) return fallback;
    char *end = NULL;
    long parsed = strtol(v, &end, 10);
    if (end == v || parsed < 0 || parsed > 100000) return fallback;
    return (int) parsed;
}

static bool half_duplex_enabled(void) {
    if (g_half_duplex_enabled < 0) {
        const char *v = getenv("SYNCN_INTERCOM_AUDIO_HALF_DUPLEX");
        g_half_duplex_enabled = (v != NULL && strcmp(v, "0") == 0) ? 0 : 1;
        g_hd_threshold = syncn_env_int("SYNCN_INTERCOM_AUDIO_HD_THRESHOLD", SYNCN_HD_DEFAULT_THRESHOLD);
        g_hd_hangover_ms = syncn_env_int("SYNCN_INTERCOM_AUDIO_HD_HANGOVER", SYNCN_HD_DEFAULT_HANGOVER_MS);
    }
    return g_half_duplex_enabled == 1;
}

// Standard ITU-T G.711 A-law expansion. Only used to measure level, never to
// re-encode -- alawdec downstream remains the single decoder of record for
// anything that actually gets played.
static inline int syncn_alaw_expand(uint8_t a) {
    a ^= 0x55;
    int sign = a & 0x80;
    int exponent = (a & 0x70) >> 4;
    int mantissa = a & 0x0F;
    int sample = (exponent == 0) ? ((mantissa << 4) + 8)
                                 : (((mantissa << 4) + 0x108) << (exponent - 1));
    return sign ? -sample : sample;
}

// Mean absolute amplitude of an A-law payload, in linear PCM units.
static int syncn_alaw_mean_level(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    int64_t total = 0;
    for (size_t i = 0; i < size; i++) {
        int s = syncn_alaw_expand(data[i]);
        total += (s < 0) ? -s : s;
    }
    return (int) (total / (int64_t) size);
}

// Pre-built A-law digital silence, substituted for the real uplink payload
// while the far end is speaking. Static so the capture callback never
// allocates on its hot path.
#define SYNCN_HD_SILENCE_MAX 512
static uint8_t g_alaw_silence[SYNCN_HD_SILENCE_MAX];
static bool g_alaw_silence_ready = false;

static const uint8_t *syncn_alaw_silence(size_t size) {
    if (!g_alaw_silence_ready) {
        memset(g_alaw_silence, 0xD5, sizeof(g_alaw_silence));
        g_alaw_silence_ready = true;
    }
    return (size <= SYNCN_HD_SILENCE_MAX) ? g_alaw_silence : NULL;
}

struct syncn_intercom_audio {
    struct flutterpi *flutterpi;

    pthread_mutex_t lock;
    bool running;
    bool capture_enabled;
    bool uplink_listening;
    bool headset_mode;

    GstElement *playback_pipeline;
    GstElement *playback_appsrc;

    GstElement *capture_pipeline;
    GstElement *capture_appsink;
    GstElement *capture_volume;

    int playback_count;
    int capture_count;

    // Half-duplex uplink suppression state (2026-09-13). Written by
    // handle_play_downlink on the platform thread, read by
    // on_new_capture_sample on the GStreamer capture thread. A naturally
    // aligned 64-bit scalar, so plain load/store is atomic on this board's
    // aarch64 -- no lock is taken for it, deliberately: the capture
    // callback must never block on the playback path (see the narrowed
    // lock scope in handle_play_downlink for why that mattered), and a
    // torn read here would at worst mis-gate a single 20ms frame.
    volatile gint64 last_downlink_voice_us;
};

// Split into a quick locked "detach" part and a separately-called unlocked
// "close" part for the same reason as the video plugin's teardown split (see
// its comment): gst_element_set_state(..., GST_STATE_NULL) blocks until the
// pipeline's streaming thread has settled, but that thread (on_new_capture_sample)
// tries to acquire `self->lock` itself -- calling it while still holding the
// lock deadlocks the platform thread against the streaming thread, freezing
// the whole panel (confirmed on-device, same failure mode as the video plugin).
static void teardown_locked(struct syncn_intercom_audio *self, GstElement **playback_out, GstElement **capture_out) {
    *playback_out = self->playback_pipeline;
    self->playback_pipeline = NULL;
    self->playback_appsrc = NULL;

    *capture_out = self->capture_pipeline;
    self->capture_pipeline = NULL;
    self->capture_appsink = NULL;
    self->capture_volume = NULL;

    self->running = false;
}

static void teardown_unlocked(GstElement *playback_pipeline, GstElement *capture_pipeline) {
    syncn_gst_bounded_teardown("audio-playback", playback_pipeline, 3);
    syncn_gst_bounded_teardown("audio-capture", capture_pipeline, 3);
}

// Called on a GStreamer streaming thread.
static GstFlowReturn on_new_capture_sample(GstAppSink *sink, gpointer userdata) {
    struct syncn_intercom_audio *self = userdata;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer == NULL || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    pthread_mutex_lock(&self->lock);
    bool listening = self->uplink_listening;
    self->capture_count++;
    int count = self->capture_count;
    pthread_mutex_unlock(&self->lock);

    bool should_log = count <= 30;
    bool diag_log = !should_log && diag_mode_enabled() && count <= SYNCN_DIAG_MAX_BUFFERS &&
        (count % SYNCN_DIAG_LOG_EVERY_N == 0);

    if (raw_capture_enabled() && count <= SYNCN_DIAG_MAX_BUFFERS && map.size > 0) {
        raw_capture_write(&g_raw_capture_uplink_file, SYNCN_RAW_CAPTURE_UPLINK_PATH, map.data, map.size);
    }

    if (should_log) {
        syncn_intercom_debug_log("audio", "on_new_capture_sample #%d: listening=%d, size=%zu", count, listening, map.size);
    } else if (diag_log) {
        struct syncn_alaw_buffer_stats stats = syncn_alaw_stats(map.data, map.size);
        syncn_intercom_debug_log(
            "audio-diag",
            "on_new_capture_sample #%d: listening=%d, size=%zu, byte range [0x%02x, 0x%02x], distinct=%d, constant=%d",
            count, listening, map.size, stats.min_byte, stats.max_byte, stats.distinct_count, stats.is_constant
        );
    }

    if (listening && map.size > 0) {
        // Half-duplex gate: while the far end is speaking, transmit A-law
        // silence instead of whatever the mic picked up. See the comment
        // block on half_duplex_enabled() -- what the mic picks up during
        // far-end speech is dominated by speaker/DAC crosstalk that AEC has
        // repeatedly proven unable to remove, and sending it back is what
        // makes the far end hear themselves. Frame cadence and size are
        // unchanged (the door gates its downlink on continuous CC frames),
        // only the payload is replaced.
        const uint8_t *payload = map.data;
        bool suppressed = false;
        if (half_duplex_enabled()) {
            gint64 since_us = g_get_monotonic_time() - self->last_downlink_voice_us;
            if (since_us < (gint64) g_hd_hangover_ms * 1000) {
                const uint8_t *silence = syncn_alaw_silence(map.size);
                if (silence != NULL) {
                    payload = silence;
                    suppressed = true;
                }
            }
        }

        struct std_value event = {
            .type = kStdUInt8Array,
            .size = map.size,
            .uint8array = (uint8_t *) payload,
        };
        int send_ret = platch_send_success_event_std(SYNCN_INTERCOM_AUDIO_EVENT_CHANNEL, &event);
        if (should_log) {
            syncn_intercom_debug_log("audio", "on_new_capture_sample #%d: platch_send_success_event_std -> %d, half_duplex_suppressed=%d", count, send_ret, suppressed);
        } else if (diag_log) {
            syncn_intercom_debug_log("audio-diag", "on_new_capture_sample #%d: platch_send_success_event_std -> %d, half_duplex_suppressed=%d", count, send_ret, suppressed);
        }
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// See syncn_intercom_video.c's identical helper for why this is needed:
// gst_element_set_state()'s immediate return value is not a reliable
// success/failure signal for a live pipeline -- real failures (no ALSA
// device, negotiation failure, etc) are reported asynchronously via the
// pipeline's bus instead, which nothing was checking before this.
static void log_pipeline_bus_errors(const char *tag, GstElement *pipeline) {
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)) != NULL) {
        GError *err = NULL;
        gchar *debug = NULL;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &debug);
            LOG_ERROR("%s: GStreamer ERROR from %s: %s (%s)\n", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
            syncn_intercom_debug_log("audio", "%s: GStreamer ERROR from %s: %s (%s)", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
        } else {
            gst_message_parse_warning(msg, &err, &debug);
            LOG_ERROR("%s: GStreamer WARNING from %s: %s (%s)\n", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
            syncn_intercom_debug_log("audio", "%s: GStreamer WARNING from %s: %s (%s)", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
        }
        g_clear_error(&err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

// Non-blocking start for playback pipelines: set_state + failure check only,
// no blocking gst_element_get_state() wait. The playback pipeline uses
// appsrc is-live=true, which reaches PLAYING without needing buffers, and
// real failures surface asynchronously via the bus log. The blocking 3s
// get_state() was deadlocking the flutter-pi platform thread -- while it
// blocked, no submit() method-channel calls could reach appsrc, creating a
// chicken-and-egg deadlock that added exactly 3s to every call setup (see
// syncn_intercom_video.c's identical fix).
static bool start_pipeline_non_blocking(const char *tag, GstElement *pipeline) {
    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    syncn_intercom_debug_log("audio", "%s: gst_element_set_state(PLAYING) -> %d (non-blocking)", tag, state_ret);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        log_pipeline_bus_errors(tag, pipeline);
        return false;
    }
    log_pipeline_bus_errors(tag, pipeline);
    return true;
}

// Retries a non-blocking pipeline start on failure, resetting to NULL and
// backing off briefly between attempts. Added after an on-device failure
// (2026-08-25): alsasink's exclusive `plughw:0,0` device open failed with
// "Device is being used by another application" on BOTH the AEC and no-AEC
// attempts back to back -- almost certainly the doorbell ringtone
// (`aplay -D default`, likely resolving to the same physical card/device)
// not having released the hardware yet at the exact moment a call is
// answered. That's a transient race with a process in a different package
// entirely (the Dart-side ring service) with no way to synchronize against
// it directly, so tolerate it here instead: a pipeline can't distinguish
// "this device is broken" from "this device is busy for another 100ms", and
// retrying costs nothing in the (common) non-failing case. Bounded to a few
// short attempts -- worst case adds under a second of latency to call setup
// in the failure path only, which beats a permanently silent call.
static bool start_pipeline_with_retry(const char *tag, GstElement *pipeline, int max_attempts, guint retry_delay_ms) {
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (start_pipeline_non_blocking(tag, pipeline)) {
            if (attempt > 1) {
                syncn_intercom_debug_log("audio", "%s: succeeded on retry attempt %d/%d", tag, attempt, max_attempts);
            }
            return true;
        }
        if (attempt == max_attempts) break;
        syncn_intercom_debug_log(
            "audio",
            "%s: start failed (attempt %d/%d), likely transient ALSA device contention -- resetting to NULL and retrying in %ums",
            tag,
            attempt,
            max_attempts,
            retry_delay_ms
        );
        gst_element_set_state(pipeline, GST_STATE_NULL);
        g_usleep(retry_delay_ms * 1000);
    }
    return false;
}

// Blocking start for capture pipelines: confirms PLAYING via get_state().
// Kept for capture only -- it's a separate, already-fast path (31ms in the
// measured log) and captures need the pipeline confirmed running before
// on_new_capture_sample can deliver useful uplink frames.
static bool start_and_confirm_playing(const char *tag, GstElement *pipeline) {
    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    syncn_intercom_debug_log("audio", "%s: gst_element_set_state(PLAYING) -> %d", tag, state_ret);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        log_pipeline_bus_errors(tag, pipeline);
        return false;
    }
    GstState final_state;
    GstStateChangeReturn wait_ret = gst_element_get_state(pipeline, &final_state, NULL, 3 * GST_SECOND);
    syncn_intercom_debug_log("audio", "%s: gst_element_get_state -> wait_ret=%d, final_state=%d", tag, wait_ret, final_state);
    log_pipeline_bus_errors(tag, pipeline);
    if (wait_ret == GST_STATE_CHANGE_FAILURE || (final_state != GST_STATE_PLAYING && wait_ret != GST_STATE_CHANGE_ASYNC)) {
        LOG_ERROR("%s: pipeline did not reach PLAYING (wait_ret=%d, final_state=%d)\n", tag, wait_ret, final_state);
        return false;
    }
    return true;
}

// `headset_mode` picks between the panel's built-in speaker+mic (default,
// hands-free wall-panel use) and a manually-selected wired headset. There's
// no hardware jack-detect on this board (see file header comment), so this
// is purely driven by the caller (app-side toggle) -- it is not, and cannot
// be, automatic.
static void set_alsa_voice_routing(bool headset_mode) {
    const char *playback_path = headset_mode ? "HP" : "SPK";
    const char *capture_mic_path = headset_mode ? "Hands Free Mic" : "Main Mic";
    const char *playback_argv[] = { "amixer", "-c", "0", "sset", "Playback Path", playback_path, NULL };
    const char *capture_argv[] = { "amixer", "-c", "0", "sset", "Capture MIC Path", capture_mic_path, NULL };
    struct {
        const char *tag;
        const char **argv;
    } controls[] = {
        { headset_mode ? "Playback Path -> HP" : "Playback Path -> SPK", playback_argv },
        { headset_mode ? "Capture MIC Path -> Hands Free Mic" : "Capture MIC Path -> Main Mic", capture_argv },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(controls); i++) {
        gchar *out = NULL;
        gchar *err = NULL;
        gint status = -1;
        GError *gerror = NULL;
        gboolean spawned = g_spawn_sync(
            NULL,
            (gchar **) controls[i].argv,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            &out,
            &err,
            &status,
            &gerror
        );
        if (!spawned) {
            syncn_intercom_debug_log(
                "audio",
                "set_alsa_voice_routing: %s: failed to launch amixer: %s (non-fatal)",
                controls[i].tag,
                gerror != NULL ? gerror->message : "unknown"
            );
            g_clear_error(&gerror);
        } else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            syncn_intercom_debug_log(
                "audio",
                "set_alsa_voice_routing: %s: amixer exited non-zero (control may not exist on this hardware) -- stderr: %s (non-fatal)",
                controls[i].tag,
                err != NULL ? err : ""
            );
        } else {
            syncn_intercom_debug_log("audio", "set_alsa_voice_routing: %s: OK", controls[i].tag);
        }
        g_free(out);
        g_free(err);
    }
}

// gst-plugins-bad may not be on every panel image; probing before we build
// the pipeline string lets us fall back to plain (non-echo-cancelled) audio
// instead of every call failing to start when it's missing.
static bool gst_element_available(const char *factory_name) {
    GstElementFactory *factory = gst_element_factory_find(factory_name);
    if (factory == NULL) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

static bool start_locked(struct syncn_intercom_audio *self, bool capture_enabled, bool headset_mode) {
    syncn_intercom_debug_log(
        "audio",
        "start_locked called, capture_enabled=%d, headset_mode=%d, self->running=%d",
        capture_enabled,
        headset_mode,
        self->running
    );
    if (self->running) {
        self->capture_enabled = capture_enabled;
        self->headset_mode = headset_mode;
        set_alsa_voice_routing(headset_mode);
        return true;
    }
    set_alsa_voice_routing(headset_mode);
    self->headset_mode = headset_mode;
    self->playback_count = 0;
    self->capture_count = 0;
    // Clear half-duplex state so a previous call's far-end speech can never
    // suppress the opening moments of a new one.
    self->last_downlink_voice_us = 0;
    if (raw_capture_enabled()) {
        // Fresh files per pipeline start -- these are meant to capture ONE
        // test call, not accumulate across repeated start/stop cycles.
        raw_capture_close_all();
    }

    if (!gst_is_initialized()) {
        GError *error = NULL;
        if (!gst_init_check(NULL, NULL, &error)) {
            LOG_ERROR("syncn_intercom_audio: gst_init_check failed: %s\n", error != NULL ? error->message : "unknown error");
            if (error != NULL) g_error_free(error);
            return false;
        }
    }

    bool aec_available = gst_element_available("webrtcechoprobe") && gst_element_available("webrtcdsp");
    syncn_intercom_debug_log("audio", "start_locked: AEC elements available=%d", aec_available);

    // Playback quality notes (parity with Android's AudioTrack path):
    // - plughw (not raw hw) lets ALSA's plug layer run the codec at its
    //   native rate instead of forcing the DAC into an 8kHz-derived mode.
    // - audioresample quality=10 upsamples the 8kHz narrowband stream with
    //   the best filter instead of the default (audible aliasing/harshness).
    // - sync=true + a jitter queue: with sync=false, network jitter went
    //   straight to the DAC as underruns (pops/crackle). The queue absorbs
    //   jitter at the cost of a little added latency.
    // - min-threshold-time=300ms (2026-09-10, up from 160ms, originally
    //   80ms): root-caused an intermittent mid-phrase audio cutoff
    //   complaint to network contention -- confirmed on-device that a
    //   second device sharing this panel's wired connection was
    //   degrading call audio, and cutoffs on longer continuous phrases
    //   persisted even after removing that device and after doubling the
    //   buffer to 160ms, and after fixing a separate, more severe
    //   mid-call TCP-reconnect bug (see
    //   docs/opencode-intercom-call-drop-investigation.md in
    //   syncn_smarthome_panel) that was NOT the cause of this specific
    //   symptom. Pushed further to 300ms as a blunt trade of more
    //   one-way audio latency (~300ms) for more absorption headroom
    //   against whatever network stalls remain -- no new diagnostic
    //   evidence pins down the exact stall duration causing this, so
    //   this is a pragmatic step, not a measured fix. If cutoffs persist
    //   even at 300ms, stop increasing this value further without first
    //   measuring real per-packet arrival timing during a long phrase
    //   (e.g. via SYNCN_INTERCOM_AUDIO_RAW_CAPTURE combined with network-
    //   level packet capture) to find the actual stall duration --
    //   guessing at ever-larger buffer sizes has diminishing returns and
    //   real latency cost.
    //   max-size-time scaled to match, so the larger threshold has
    //   headroom within the queue's own cap.
    // - alsasink buffer-time=500ms (2026-09-10, up from 200ms, unchanged
    //   since this pipeline's early history): found a real mismatch while
    //   investigating the persistent long-phrase cutoff -- the GStreamer
    //   jitter queue above was raised to a 300ms min-threshold-time, but
    //   the ALSA hardware ring buffer underneath it (buffer-time) was
    //   still only 200ms, meaning the queue could be holding more audio
    //   than the hardware sink even has room to buffer. Raised buffer-
    //   time to comfortably exceed the queue's threshold so this isn't a
    //   second, uncoordinated bottleneck. latency-time (ALSA period size)
    //   left at 20ms -- that controls interrupt/wakeup granularity within
    //   the buffer, not overall capacity, and hasn't been implicated by
    //   anything found so far.
    // - volume=1.0: analog gain belongs to the ALSA mixer (see
    //   set_alsa_voice_routing / boot-time tuning); attenuating in software
    //   here just burned headroom and resolution.
    // - echo probe placement (2026-09-13): THE AEC fix. The probe used to
    //   hang off a `tee` as a dead-end branch ending in
    //   `fakesink sync=false async=false`, with the real playback path
    //   (jitter queue -> alsasink) on the OTHER branch. That silently
    //   disabled echo cancellation entirely, for a mechanical reason
    //   straight out of gstwebrtcechoprobe.cpp: the probe learns the echo
    //   delay by querying the latency DOWNSTREAM OF ITS OWN SRCPAD --
    //
    //       if (gst_pad_query (btrans->srcpad, query)) {
    //         gst_query_parse_latency (query, NULL, &upstream_latency, NULL);
    //       }
    //       self->delay = upstream_latency / GST_MSECOND;
    //
    //   and that value is handed to the APM via apm->set_stream_delay_ms().
    //   On a tee stub, everything downstream of the probe is just a
    //   fakesink, so the query returned ~0ms -- the AEC was told the echo
    //   returns almost immediately, while the real speaker output sat
    //   behind the jitter queue plus the ALSA ring buffer. The filter
    //   searched the wrong time window and cancelled nothing, which is
    //   exactly the "far end hears himself" echo reported on-device, and
    //   why toggling echo-cancel on/off changed nothing: it was inert
    //   either way.
    //
    //   Upstream documents the correct topology plainly -- the probe goes
    //   IN-LINE in the real playback path, never on a side branch:
    //     far-end-src ! audio/x-raw,rate=48000 ! webrtcechoprobe ! pulsesink
    //   In-line placement is what makes the latency query traverse the
    //   genuine chain (queue + alsasink) and report a true delay.
    //
    //   Probe sits at 8kHz immediately after alawdec because upstream
    //   requires the probe and the DSP to run at the SAME sample rate, and
    //   the capture-side webrtcdsp is pinned to 8kHz. playvol is applied
    //   BEFORE the probe so the reference the AEC sees carries the same
    //   gain the speaker actually reproduces; resampling afterwards is
    //   amplitude-neutral.
    //
    // - jitter queue / ALSA buffer reverted to 80ms / 200ms (2026-09-13):
    //   these were inflated to 160ms then 300ms, and buffer-time 200ms ->
    //   500ms, while chasing this same cutoff. None of it helped, and it
    //   actively widened the echo path the AEC has to span (~280ms -> ~800ms).
    //   Back to the long-standing values now that the probe is positioned
    //   to report that delay honestly.
    static const char *playback_desc_aec =
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "alawdec ! audioconvert ! audio/x-raw,rate=8000,channels=1 ! "
        "volume name=playvol volume=1.0 ! "
        "webrtcechoprobe name=syncn_echoprobe ! "
        "audioconvert ! audioresample quality=10 ! "
        "queue min-threshold-time=80000000 max-size-time=400000000 ! "
        "alsasink device=plughw:0,0 sync=true buffer-time=200000 latency-time=20000";
    static const char *playback_desc_plain =
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "alawdec ! audioconvert ! audioresample quality=10 ! volume name=playvol volume=1.0 ! "
        "queue min-threshold-time=80000000 max-size-time=400000000 ! "
        "alsasink device=plughw:0,0 sync=true buffer-time=200000 latency-time=20000";

    GError *error = NULL;
    GstElement *playback = gst_parse_launch(aec_available ? playback_desc_aec : playback_desc_plain, &error);
    if (playback == NULL && aec_available) {
        // Parsing itself shouldn't fail if both elements probed OK, but fall
        // back defensively rather than taking the whole call down with it.
        LOG_ERROR(
            "syncn_intercom_audio: failed to build AEC playback pipeline, falling back without AEC: %s\n",
            error != NULL ? error->message : "unknown error"
        );
        if (error != NULL) g_error_free(error);
        error = NULL;
        aec_available = false;
        playback = gst_parse_launch(playback_desc_plain, &error);
    }
    if (playback == NULL) {
        LOG_ERROR("syncn_intercom_audio: failed to build playback pipeline: %s\n", error != NULL ? error->message : "unknown error");
        if (error != NULL) g_error_free(error);
        return false;
    }
    GstElement *playback_appsrc = gst_bin_get_by_name(GST_BIN(playback), "src");
    if (playback_appsrc != NULL) {
        // See syncn_intercom_video.c's identical fix for why: caps embedded in
        // the gst_parse_launch() string weren't reliably reaching the live
        // GstAppSrc on this device's GStreamer version, leaving it unable to
        // negotiate with alawdec (same ASYNC/PAUSED-stuck symptom as video).
        GstCaps *appsrc_caps = gst_caps_new_simple(
            "audio/x-alaw",
            "rate", G_TYPE_INT, 8000,
            "channels", G_TYPE_INT, 1,
            NULL
        );
        gst_app_src_set_caps(GST_APP_SRC(playback_appsrc), appsrc_caps);
        gst_caps_unref(appsrc_caps);
        gst_app_src_set_stream_type(GST_APP_SRC(playback_appsrc), GST_APP_STREAM_TYPE_STREAM);
    }
    // volume=1.0 in the pipeline strings above is the correct default: analog
    // gain belongs to the ALSA mixer (see set_alsa_voice_routing / boot-time
    // tuning). But confirmed on-device 2026-09-09 that the 800x1280 panel's
    // rk809/rk817 codec exposes NO gain/volume control at all -- amixer -c 0
    // scontrols on that board lists only enum path-selectors (Playback Path,
    // Capture MIC Path, etc.), nothing with a settable level -- so on that
    // specific panel there is no ALSA-side gain to rely on and call audio is
    // audibly much quieter than the ringtone (a plain WAV played via aplay,
    // unaffected by this). Boost playvol's gain in software, but ONLY on that
    // panel model (matched via the same PANEL_WIDTH env var syncnhome.service
    // already sets from /etc/syncn/panel-display.conf) -- every other panel
    // keeps the original 1.0 default so this doesn't reintroduce the
    // headroom/resolution cost the comment above warns about where a real
    // ALSA gain control does exist.
    GstElement *playback_volume_elem = gst_bin_get_by_name(GST_BIN(playback), "playvol");
    if (playback_volume_elem != NULL) {
        const char *panel_width = getenv("PANEL_WIDTH");
        if (panel_width != NULL && strcmp(panel_width, "800") == 0) {
            // 2.5 (confirmed on-device 2026-09-09) made call audio noisy --
            // amplifying an 8kHz A-law stream's quantization/noise floor
            // right along with the voice. 1.5 held as the safe value since.
            //
            // Retested 2026-09-14 at 8kHz (matching this real call format,
            // not the 16kHz bench format used for the bulk of that day's
            // testing -- see docs/panel-audio-gain-final-findings.md in
            // syncn_smarthome_panel): confirmed the same 8kHz noise-floor
            // sensitivity as 2026-09-09, including at moderate playback
            // boosts layered on an unchanged, previously-good mic
            // recording, suggesting some of that session's later noisy/far
            // verdicts may reflect ALSA/PulseAudio state drift from heavy
            // manual PulseAudio experimentation earlier in that same
            // session rather than the gain value itself -- unconfirmed,
            // needs a clean-reboot retest before pushing past 2.0 here.
            // 2.0 is a modest, real improvement over 1.5 that stays under
            // GStreamer's single `volume` element hard cap of 10.0 (values
            // above that are silently rejected, falling back to the
            // element's previous value -- confirmed on-device 2026-09-14).
            // A meaningfully louder boost (e.g. matching the 60x figure
            // reached in 16kHz bench testing that same day) would need a
            // SECOND chained `volume` element in the pipeline string above,
            // since a single element cannot express it -- not done here;
            // do that only after a clean, reboot-verified real-call test
            // at 8kHz confirms a specific higher value is actually safe.
            g_object_set(playback_volume_elem, "volume", 2.0, NULL);
            syncn_intercom_debug_log("audio", "start_locked: boosted playvol gain to 2.0 for PANEL_WIDTH=800");
        }
        gst_object_unref(playback_volume_elem);
    }

    if (playback_appsrc == NULL || !start_pipeline_with_retry("syncn_intercom_audio playback", playback, 3, 150)) {
        if (aec_available) {
            // The in-line echo probe is the one extra failure mode here (e.g.
            // the probe refusing the 8kHz caps it is pinned to, or not being
            // present at all on a panel image without gst-plugins-bad);
            // retry once without it before giving up entirely -- a call with
            // no echo cancellation beats no call.
            LOG_ERROR("syncn_intercom_audio: AEC playback pipeline failed to start, retrying without AEC\n");
            if (playback_appsrc != NULL) gst_object_unref(playback_appsrc);
            syncn_gst_bounded_teardown("audio-playback", playback, 3);
            aec_available = false;
            error = NULL;
            playback = gst_parse_launch(playback_desc_plain, &error);
            playback_appsrc = playback != NULL ? gst_bin_get_by_name(GST_BIN(playback), "src") : NULL;
            if (playback_appsrc != NULL) {
                GstCaps *appsrc_caps = gst_caps_new_simple("audio/x-alaw", "rate", G_TYPE_INT, 8000, "channels", G_TYPE_INT, 1, NULL);
                gst_app_src_set_caps(GST_APP_SRC(playback_appsrc), appsrc_caps);
                gst_caps_unref(appsrc_caps);
                gst_app_src_set_stream_type(GST_APP_SRC(playback_appsrc), GST_APP_STREAM_TYPE_STREAM);
            }
        }
        if (playback_appsrc == NULL || !start_pipeline_with_retry("syncn_intercom_audio playback (retry)", playback, 3, 150)) {
            LOG_ERROR("syncn_intercom_audio: failed to start playback pipeline\n");
            if (playback_appsrc != NULL) gst_object_unref(playback_appsrc);
            if (playback != NULL) syncn_gst_bounded_teardown("audio-playback", playback, 3);
            return false;
        }
    }

    // Retrieve the echo probe element from the playback pipeline so we can
    // hand it to webrtcdsp in the capture pipeline below. The probe property
    // is a GstElement pointer -- gst_parse_launch's `probe=syncn_echoprobe`
    // syntax can't resolve references across separate pipelines, so we must
    // set it via g_object_set after both pipelines exist.
    GstElement *echoprobe = aec_available
        ? gst_bin_get_by_name(GST_BIN(playback), "syncn_echoprobe")
        : NULL;
    if (aec_available && echoprobe == NULL) {
        LOG_ERROR("syncn_intercom_audio: AEC enabled but webrtcechoprobe element not found in playback pipeline\n");
        aec_available = false;
    }

    // Capture quality notes: plughw + quality=10 for the same reasons as
    // playback (capture at the codec's native rate, downsample well).
    // webrtcdsp tuning beyond the bare AEC/NS/AGC booleans:
    // - high-pass-filter strips DC offset and low-frequency rumble the wall
    //   mount picks up (matches Android's voice-processing chain).
    // - noise-suppression-level=moderate (reverted 2026-09-10): retried
    //   `high` isolated from the earlier failed gain-control=true
    //   combination (see git history) and confirmed on-device it made
    //   ZERO audible difference from `moderate` -- ruling out NS level
    //   entirely as a lever for the "noisy/unclear" complaint. The real
    //   cause was found the same day: a second device sharing this
    //   panel's wired network connection was degrading call audio quality
    //   (confirmed by the complaint clearing after disconnecting it);
    //   remaining intermittent mid-phrase audio cutoffs after that are
    //   being addressed via the playback jitter queue size below, not NS.
    //   Reverted to `moderate` since `high` provided no benefit to justify
    //   the config drift from the long-verified default.
    // - extended-filter=true: longer echo tail coverage; speaker and mic sit
    //   centimeters apart in the same enclosure, so the echo path is strong.
    // - echo-cancel=true (reverted 2026-09-10): tried disabling AEC for
    //   PANEL_WIDTH=800 after the client preferred a plain arecord capture
    //   over real-call audio in isolated A/B testing. Shipped as
    //   panel-v1.3.52/53 and confirmed on-device WORSE than the original
    //   echo-cancel=true baseline -- client reported it as still bad, and
    //   specifically still choppy/gappy on a real two-way call even after
    //   also reverting the capvol gain experiment shipped in the same
    //   build. The clean sound in isolated single-ended testing did not
    //   carry over to a real call with both sides talking; whatever AEC
    //   was doing there evidently mattered. Fully reverted back to
    //   echo-cancel=true for all panel sizes, matching pre-2026-09-10
    //   behavior. Do not re-attempt disabling AEC without isolating it
    //   from every other audio change in its own build AND testing on a
    //   real two-way call (not just a local recording) before shipping.
    // - audiobuffersplit output-buffer-duration-fraction=1/50 (2026-09-10):
    //   THE uplink frame-size fix. Without it, alawenc emits whatever
    //   buffer size arrives from upstream -- and webrtcdsp internally
    //   processes/emits 10ms chunks, so every uplink frame was 80 A-law
    //   bytes (10ms) instead of the 160 bytes (20ms) the door's CC-channel
    //   protocol expects. Confirmed on-device 2026-09-10 straight from the
    //   debug log: "on_new_capture_sample #1: size=80" on our uplink
    //   versus "handle_play_downlink #1: pushed 160 bytes" on the door's
    //   downlink -- we were sending half-length frames at twice the rate
    //   for this panel's entire history.
    //
    //   This exact element+property is lifted from the proven
    //   packages/intercom/linux/audio_pipeline_handler.cc implementation on
    //   `main`, whose own comment states it re-chunks into exact 20ms
    //   buffers "so alawenc always emits exactly kAlawFrameBytes (160) per
    //   buffer - matching the fixed-size reads AudioPipelineHandler.kt does
    //   from AudioRecord, which the door's protocol (CC channel, 160
    //   bytes/20ms) expects". That file's TODO(linux-aec) even specifies
    //   webrtcdsp belongs "between audioresample and audiobuffersplit" --
    //   which is exactly the ordering used here; the flutter-pi port added
    //   webrtcdsp but never carried audiobuffersplit across, dropping the
    //   framing guarantee.
    //
    //   Why this explains what nothing else could: every layer measured on
    //   THIS panel tested clean (gap-free raw downlink capture, no ALSA
    //   XRUN, steady 20ms native dispatch, and the cutoff surviving AEC
    //   off, NS levels, capvol values, jitter-queue 80/160/300ms, ALSA
    //   buffer-time 200/500ms, lock-scope narrowing and frame-dispatch
    //   deferral). The defect was never in what we receive or render -- it
    //   is in what we TRANSMIT, and the damage lands inside the door
    //   station, which is why it only ever reproduces with the mic live and
    //   why Tuya's own app is unaffected on this identical hardware.
    // Capture channel 0 explicitly instead of letting audioconvert average
    // the codec's two channels down to mono. Measured on real hardware
    // (192.168.100.203, 2026-09-15, same phrase and distance for each):
    //
    //   channel 0 alone:                     rms=1423
    //   channel 1 alone:                     rms= 178   <- dead, noise floor only
    //   both averaged (what we shipped):     rms= 662
    //
    // This board has ONE physical mic, wired to channel 0; channel 1 carries
    // nothing. `audioconvert`'s stereo->mono downmix averages the two, so
    // every call was mixing a live mic with silence and throwing away ~6 dB
    // of signal for free. Taking channel 0 alone roughly doubles the level
    // with no gain applied anywhere -- this is not a boost, it is the
    // removal of a loss, which is why it costs nothing in AEC terms (unlike
    // the compressor experiment reverted in a89abe0, which broke echo
    // cancellation during double-talk).
    //
    // The codec natively opens at 44100 Hz stereo S16LE (confirmed via
    // /proc/asound/card0/pcm0c/sub0/hw_params while a capture was live), so
    // the caps below ask for exactly that and let audioresample quality=10
    // do the 44.1k->8k conversion after the channel is selected. Keeping
    // plughw (rather than raw hw) means a panel whose codec reports a
    // different native format still negotiates instead of failing outright.
    //
    // Do not replace the deinterleave with `audioconvert ! audio/x-raw,
    // channels=1` -- that is precisely the averaging path this fixes.
    static const char *capture_desc_aec =
        "alsasrc device=plughw:0,0 ! audio/x-raw,rate=44100,channels=2,format=S16LE ! "
        "deinterleave name=capdi  capdi.src_0 ! queue ! "
        "audioconvert ! audioresample quality=10 ! "
        "audio/x-raw,rate=8000,channels=1,format=S16LE ! "
        "webrtcdsp name=dsp echo-cancel=true noise-suppression=true gain-control=false "
        "high-pass-filter=true noise-suppression-level=moderate extended-filter=true ! "
        "volume name=capvol ! audiobuffersplit output-buffer-duration-fraction=1/50 ! alawenc ! "
        "appsink name=sink emit-signals=true sync=false max-buffers=4 drop=true";
    static const char *capture_desc_plain =
        "alsasrc device=plughw:0,0 ! audio/x-raw,rate=44100,channels=2,format=S16LE ! "
        "deinterleave name=capdi  capdi.src_0 ! queue ! "
        "audioconvert ! audioresample quality=10 ! "
        "audio/x-raw,rate=8000,channels=1,format=S16LE ! "
        "volume name=capvol ! audiobuffersplit output-buffer-duration-fraction=1/50 ! alawenc ! "
        "appsink name=sink emit-signals=true sync=false max-buffers=4 drop=true";

    GstElement *capture = NULL;
    GstElement *capture_appsink = NULL;
    GstElement *capture_volume = NULL;
    if (capture_enabled) {
        for (int attempt = 0; attempt < 2 && capture == NULL; attempt++) {
            bool use_aec = aec_available && attempt == 0;
            error = NULL;
            capture = gst_parse_launch(use_aec ? capture_desc_aec : capture_desc_plain, &error);
            if (capture == NULL) {
                LOG_ERROR(
                    "syncn_intercom_audio: failed to build capture pipeline (aec=%d): %s\n",
                    use_aec,
                    error != NULL ? error->message : "unknown error"
                );
                if (error != NULL) g_error_free(error);
                continue;
            }
            capture_appsink = gst_bin_get_by_name(GST_BIN(capture), "sink");
            capture_volume = gst_bin_get_by_name(GST_BIN(capture), "capvol");
            if (capture_appsink == NULL) {
                gst_object_unref(capture);
                capture = NULL;
                continue;
            }
            g_signal_connect(capture_appsink, "new-sample", G_CALLBACK(on_new_capture_sample), self);
            // History of capvol boost attempts on this panel (PANEL_WIDTH=800):
            // 2026-09-09: 1.8x -> loud whine/buzz, drowned out speech, reverted
            // to 1.0. 2026-09-10: 1.2x, not isolated from an unrelated AEC
            // change in the same build, client wanted back to 1.0. Both were
            // real in-app call tests, but both also predate the standalone
            // bench-test investigation below.
            //
            // 2026-09-14 (docs/panel-audio-gain-final-findings.md in
            // syncn_smarthome_panel): a clean, isolated GStreamer bench
            // pipeline (alsasrc -> volume -> wavenc, no AEC, no app) at the
            // real 8kHz call format found 2.5x "clear, if distant-sounding"
            // -- explicitly confirmed by the client as a real improvement,
            // with values above (2.7-3.2x) tried and rejected as worse.
            // webrtcdsp's own AGC (gain-control=true) was ALSO tried
            // separately that session, multiple configurations, every one
            // worse than plain gain -- expected, since `amixer -c 0
            // contents` confirms this codec has zero real preamp/PGA
            // silicon, so AGC can't distinguish voice from the mic's
            // electrical noise floor any better than a plain volume
            // element can.
            //
            // 2026-09-15 findings, CONFIRMED ON A REAL CALL with the
            // channel-0 capture fix (see capture_desc_aec comment above) in
            // place -- read this before touching capvol again:
            //
            // At 2.5x: one direction at a time is clear (far end hears you
            // fine when only you are talking, and vice versa), but you still
            // have to stand close to the mic for a comfortable volume.
            //
            // At 5.0x (tried and REVERTED): loudness at a distance did not
            // meaingfully improve, AND double-talk (both parties speaking at
            // once) became noisy/unclear on BOTH ends -- the same double-
            // talk-breaks-first signature as the compressor experiment
            // reverted in a89abe0, except this time triggered by PLAIN
            // LINEAR GAIN ALONE, no compressor, no non-linearity anywhere in
            // the chain. This disproves the earlier working theory that
            // "linear gain is AEC-safe, only non-linear processing isn't" --
            // on this hardware/AEC combination, pushing capvol too high is
            // ALSO enough to degrade double-talk cancellation by itself.
            //
            // Conclusion: 2.5x is the ceiling that keeps double-talk clean.
            // The remaining "quiet, have to stand close" complaint is NOT
            // fixed by more capvol gain -- that avenue is closed. If
            // revisited, look at the call pipeline stage-by-stage (does
            // webrtcdsp's own internal processing attenuate the signal
            // somewhere the raw-capture bench tests never exercised?) rather
            // than raising this value again.
            if (capture_volume != NULL) {
                const char *panel_width_cap = getenv("PANEL_WIDTH");
                if (panel_width_cap != NULL && strcmp(panel_width_cap, "800") == 0) {
                    g_object_set(capture_volume, "volume", 2.5, NULL);
                    syncn_intercom_debug_log("audio", "start_locked: capvol=2.5 for PANEL_WIDTH=800 (5.0 tried and reverted -- broke double-talk, see comment above)");
                }
            }
            // Hand the playback pipeline's echo probe to webrtcdsp via
            // g_object_set -- gst_parse_launch can't resolve cross-pipeline
            // element references (the `probe=` syntax only works within the
            // same bin), which is why the probe property was silently unset
            // before an earlier fix attempt (webrtcdsp fell back to looking
            // for the default `webrtcechoprobe0` name, which doesn't exist).
            //
            // That earlier fix was itself still wrong (found 2026-08-24):
            // `probe` is a STRING property (the target element's *name*,
            // confirmed via `gst-inspect-1.0 webrtcdsp`: "String. Default:
            // webrtcechoprobe0"), not a GstElement* reference. Passing the
            // raw `echoprobe` object pointer here made g_object_set treat
            // that pointer's bits as a C string and read whatever garbage
            // memory followed it -- visible on-device as webrtcdsp's error
            // reporting nonsense probe names ("No echo probe with name 801
            // found", "No echo probe with name ??+& found"), which failed
            // gst_webrtc_dsp_start() every time, which crashed the whole
            // capture pipeline's PLAYING transition, which fell through to
            // this function's no-AEC retry path -- so AEC has been silently
            // disabled on every real call, letting the mic pick up the
            // speaker uncancelled (the actual cause of "hearing my own
            // voice" during a call, reported same day). Pass the probe's
            // name string instead -- it's the fixed literal from the
            // pipeline description above, not worth an allocating
            // gst_element_get_name() round-trip.
            if (use_aec && echoprobe != NULL) {
                GstElement *dsp = gst_bin_get_by_name(GST_BIN(capture), "dsp");
                if (dsp != NULL) {
                    g_object_set(dsp, "probe", "syncn_echoprobe", NULL);
                    syncn_intercom_debug_log("audio", "start_locked: set webrtcdsp probe -> syncn_echoprobe (AEC engaged)");
                    // SYNCN_INTERCOM_AUDIO_ECHO_CANCEL_OVERRIDE (2026-09-10,
                    // diagnostic): lets echo-cancel be flipped at runtime
                    // without a rebuild, e.g. "0" to disable it. A prior
                    // isolated AEC-off attempt was confirmed worse and
                    // reverted, but that build predated this same day's
                    // real fixes for the persistent mid-phrase cutoff (ALSA
                    // buffer-time/jitter-queue mismatch, capture/playback
                    // lock contention, audio/video frame-dispatch blocking
                    // in Dart) -- this override lets AEC-off be retried
                    // cleanly, isolated from those now-fixed confounds,
                    // without another build/patch-regen cycle. Falls back
                    // to the pipeline's own echo-cancel=true default (set
                    // in the gst_parse_launch string above) if unset.
                    const char *echo_cancel_override = getenv("SYNCN_INTERCOM_AUDIO_ECHO_CANCEL_OVERRIDE");
                    if (echo_cancel_override != NULL) {
                        gboolean echo_cancel = strcmp(echo_cancel_override, "0") != 0;
                        g_object_set(dsp, "echo-cancel", echo_cancel, NULL);
                        syncn_intercom_debug_log("audio", "start_locked: echo-cancel overridden to %d", echo_cancel);
                    }
                    gst_object_unref(dsp);
                } else {
                    LOG_ERROR("syncn_intercom_audio: webrtcdsp element not found in AEC capture pipeline\n");
                }
            }
            if (!start_and_confirm_playing("syncn_intercom_audio capture", capture)) {
                LOG_ERROR("syncn_intercom_audio: failed to start capture pipeline (aec=%d)\n", use_aec);
                gst_object_unref(capture_appsink);
                if (capture_volume != NULL) gst_object_unref(capture_volume);
                syncn_gst_bounded_teardown("audio-capture", capture, 3);
                capture = NULL;
                capture_appsink = NULL;
                capture_volume = NULL;
            } else {
                // Explicit success confirmation, added 2026-09-10 alongside
                // the raw-capture feature: the "AEC engaged" log above only
                // means the probe property was SET, not that the pipeline
                // actually reached PLAYING with it -- this line is the one
                // to grep for to know for certain, from a single real call,
                // whether AEC genuinely stayed active (aec=1 here) or this
                // attempt silently fell through to attempt 2's plain
                // capture (aec=0 here, from the retry loop's second pass).
                syncn_intercom_debug_log(
                    "audio",
                    "start_locked: capture pipeline confirmed PLAYING (aec=%d)%s",
                    use_aec,
                    use_aec ? " -- AEC genuinely active for this call" : " -- AEC NOT active for this call"
                );
            }
        }
        if (capture == NULL) {
            LOG_ERROR("syncn_intercom_audio: mic will be unavailable for this call\n");
        }
    }

    self->playback_pipeline = playback;
    self->playback_appsrc = playback_appsrc;
    self->capture_pipeline = capture;
    self->capture_appsink = capture_appsink;
    self->capture_volume = capture_volume;
    self->capture_enabled = capture_enabled && capture != NULL;
    self->running = true;
    syncn_intercom_debug_log("audio", "start_locked: SUCCESS, capture_enabled=%d (requested %d)", self->capture_enabled, capture_enabled);
    if (echoprobe != NULL) gst_object_unref(echoprobe);
    return true;
}

static void handle_play_downlink(struct syncn_intercom_audio *self, const uint8_t *data, size_t size) {
    // Narrowed lock scope (2026-09-10): this used to hold self->lock for
    // the entire function body, including the GStreamer push-buffer call
    // and all logging -- the SAME lock on_new_capture_sample (the mic
    // callback) also takes on every single captured buffer. With a real
    // two-way call (mic unmuted, both directions genuinely active),
    // capture-side lock contention could directly delay downlink audio
    // delivery here, since this function could not proceed until the
    // capture callback released the lock. Confirmed on-device 2026-09-10:
    // muting the mic (which does not stop capture, only silences its
    // output -- see handle_set_muted -- so capture callbacks and this
    // lock contention continued regardless) made playback audio
    // consistently clear, while unmuting caused audible cuts on longer
    // continuous speech -- consistent with lock contention scaling with
    // how much real capture activity was happening. Only the shared
    // self->running / self->playback_appsrc read needs the lock; the
    // actual push-buffer call and all logging below do not touch shared
    // state and are moved outside the critical section.
    pthread_mutex_lock(&self->lock);
    self->playback_count++;
    int count = self->playback_count;
    bool running = self->running;
    GstElement *playback_appsrc = self->playback_appsrc;
    pthread_mutex_unlock(&self->lock);

    bool should_log = count <= 30;
    // See "Diagnostic mode" file header comment.
    bool diag_log = !should_log && diag_mode_enabled() && count <= SYNCN_DIAG_MAX_BUFFERS &&
        (count % SYNCN_DIAG_LOG_EVERY_N == 0);
    if (raw_capture_enabled() && count <= SYNCN_DIAG_MAX_BUFFERS && size > 0) {
        raw_capture_write(&g_raw_capture_downlink_file, SYNCN_RAW_CAPTURE_DOWNLINK_PATH, data, size);
    }

    // Mark far-end speech for the half-duplex gate (see its comment block
    // above). Measured on the encoded A-law payload, before it reaches the
    // pipeline, so this is unaffected by anything downstream -- jitter
    // queue, resampling or the ALSA buffer. Cheap: one table-free expand per
    // byte over a 160-byte frame, outside the lock.
    if (half_duplex_enabled() && size > 0 &&
        syncn_alaw_mean_level(data, size) > g_hd_threshold) {
        self->last_downlink_voice_us = g_get_monotonic_time();
    }
    if (running && playback_appsrc != NULL && size > 0) {
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
        gst_buffer_fill(buffer, 0, data, size);
        GstFlowReturn ret;
        g_signal_emit_by_name(playback_appsrc, "push-buffer", buffer, &ret);
        gst_buffer_unref(buffer);
        if (should_log) {
            // Diagnostic only (2026-08-25): logs whether the raw A-law bytes
            // received actually vary (real audio) or are constant (digital
            // silence, A-law encodes silence as a constant 0xD5 byte) --
            // added after a "no audio" report survived a confirmed-working
            // pipeline/hardware/PulseAudio path, narrowing down whether the
            // problem is upstream (nothing real being received) or in this
            // pipeline (real data failing to render).
            uint8_t min_byte = data[0];
            uint8_t max_byte = data[0];
            for (size_t i = 1; i < size; i++) {
                if (data[i] < min_byte) min_byte = data[i];
                if (data[i] > max_byte) max_byte = data[i];
            }
            syncn_intercom_debug_log(
                "audio",
                "handle_play_downlink #%d: pushed %zu bytes, ret=%d, byte range [0x%02x, 0x%02x] (0xd5=A-law silence)",
                count,
                size,
                ret,
                min_byte,
                max_byte
            );
        } else if (diag_log) {
            struct syncn_alaw_buffer_stats stats = syncn_alaw_stats(data, size);
            syncn_intercom_debug_log(
                "audio-diag",
                "handle_play_downlink #%d: pushed %zu bytes, ret=%d, byte range [0x%02x, 0x%02x], distinct=%d, constant=%d",
                count, size, ret, stats.min_byte, stats.max_byte, stats.distinct_count, stats.is_constant
            );
        }
    } else if (should_log) {
        syncn_intercom_debug_log(
            "audio",
            "handle_play_downlink #%d: DROPPED (running=%d, playback_appsrc=%p, size=%zu)",
            count,
            running,
            (void *) playback_appsrc,
            size
        );
    } else if (diag_log) {
        syncn_intercom_debug_log(
            "audio-diag",
            "handle_play_downlink #%d: DROPPED (running=%d, playback_appsrc=%p, size=%zu)",
            count,
            running,
            (void *) playback_appsrc,
            size
        );
    }
}

static void handle_set_muted(struct syncn_intercom_audio *self, bool muted) {
    pthread_mutex_lock(&self->lock);
    if (self->capture_volume != NULL) {
        g_object_set(self->capture_volume, "mute", (gboolean) muted, NULL);
    }
    pthread_mutex_unlock(&self->lock);
}

// Args for start_thread_main. Heap-allocated per call, freed by the thread
// itself once it has sent its response.
struct start_thread_args {
    struct syncn_intercom_audio *self;
    bool capture_enabled;
    bool headset_mode;
    const FlutterPlatformMessageResponseHandle *response_handle;
};

// Runs start_locked() off the platform thread and replies once done.
//
// start_locked() can block for up to ~1s inside start_pipeline_with_retry's
// g_usleep() calls, retrying alsasink's exclusive plughw:0,0 open against a
// transient race with the doorbell ringtone player not having released the
// device yet (see that function's comment). on_method_channel_message runs
// on flutter-pi's platform thread -- the same thread driving the whole
// event loop, input handling, and DRM frame presentation -- so calling
// start_locked() there directly froze the entire app (touch, rendering,
// even the video call's own picture) for the retry's duration every time
// this race was hit (confirmed on-device 2026-09-07: "the whole app
// freezes with the video too" right after answering a call, exactly the
// span start_pipeline_with_retry's own comment already knew about but
// assumed was cheap enough not to matter). platch_respond_*_std/
// FlutterEngineSendPlatformMessageResponse is safe to call from any thread
// -- this file's own bus-watcher-style async event pattern elsewhere in
// this plugin family already relies on that same guarantee.
static void *start_thread_main(void *userdata) {
    struct start_thread_args *args = userdata;

    pthread_mutex_lock(&args->self->lock);
    bool ok = start_locked(args->self, args->capture_enabled, args->headset_mode);
    pthread_mutex_unlock(&args->self->lock);

    if (ok) {
        platch_respond_success_std(args->response_handle, NULL);
    } else {
        platch_respond_error_std(args->response_handle, "audio_start_failed", "Could not start the GStreamer audio pipeline.", NULL);
    }

    free(args);
    return NULL;
}

static void on_method_channel_message(void *userdata, const FlutterPlatformMessage *message) {
    struct syncn_intercom_audio *self = userdata;

    struct platch_obj object;
    if (platch_decode((uint8_t *) message->message, message->message_size, kStandardMethodCall, &object) != 0) {
        platch_respond_illegal_arg_std(message->response_handle, "Malformed method call.");
        return;
    }

    if (strcmp(object.method, "start") == 0) {
        bool capture_enabled = true;
        bool headset_mode = false;
        if (object.std_arg.type == kStdMap) {
            for (size_t i = 0; i < object.std_arg.size; i++) {
                struct std_value *key = &object.std_arg.keys[i];
                if (key->type != kStdString) continue;
                struct std_value *value = &object.std_arg.values[i];
                if (strcmp(key->string_value, "captureEnabled") == 0) {
                    capture_enabled = STDVALUE_IS_BOOL(*value) ? STDVALUE_AS_BOOL(*value) : true;
                } else if (strcmp(key->string_value, "headsetMode") == 0) {
                    headset_mode = STDVALUE_IS_BOOL(*value) ? STDVALUE_AS_BOOL(*value) : false;
                }
            }
        }

        struct start_thread_args *start_args = malloc(sizeof(struct start_thread_args));
        if (start_args == NULL) {
            platch_respond_error_std(message->response_handle, "audio_start_failed", "Out of memory.", NULL);
        } else {
            start_args->self = self;
            start_args->capture_enabled = capture_enabled;
            start_args->headset_mode = headset_mode;
            start_args->response_handle = message->response_handle;

            pthread_t start_thread;
            int thread_ret = pthread_create(&start_thread, NULL, start_thread_main, start_args);
            if (thread_ret != 0) {
                free(start_args);
                platch_respond_error_std(message->response_handle, "audio_start_failed", "Could not spawn start thread.", NULL);
            } else {
                pthread_detach(start_thread);
            }
        }
    } else if (strcmp(object.method, "playDownlink") == 0) {
        if (object.std_arg.type == kStdUInt8Array) {
            handle_play_downlink(self, object.std_arg.uint8array, object.std_arg.size);
        }
        platch_respond_success_std(message->response_handle, NULL);
    } else if (strcmp(object.method, "setMuted") == 0) {
        bool muted = STDVALUE_IS_BOOL(object.std_arg) ? STDVALUE_AS_BOOL(object.std_arg) : false;
        handle_set_muted(self, muted);
        platch_respond_success_std(message->response_handle, NULL);
    } else if (strcmp(object.method, "setHeadsetMode") == 0) {
        bool headset_mode = STDVALUE_IS_BOOL(object.std_arg) ? STDVALUE_AS_BOOL(object.std_arg) : false;
        pthread_mutex_lock(&self->lock);
        self->headset_mode = headset_mode;
        bool running = self->running;
        pthread_mutex_unlock(&self->lock);
        // Routing is a global ALSA mixer control (not per-pipeline), so this
        // is safe to apply even when called without holding the lock across
        // the amixer spawn -- matches set_alsa_voice_routing's existing
        // non-fatal, fire-and-forget error handling.
        if (running) {
            set_alsa_voice_routing(headset_mode);
        }
        platch_respond_success_std(message->response_handle, NULL);
    } else if (strcmp(object.method, "stop") == 0) {
        pthread_mutex_lock(&self->lock);
        GstElement *playback_pipeline, *capture_pipeline;
        teardown_locked(self, &playback_pipeline, &capture_pipeline);
        pthread_mutex_unlock(&self->lock);
        teardown_unlocked(playback_pipeline, capture_pipeline);
        platch_respond_success_std(message->response_handle, NULL);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }

    platch_free_obj(&object);
}

static void on_event_channel_message(void *userdata, const FlutterPlatformMessage *message) {
    struct syncn_intercom_audio *self = userdata;

    struct platch_obj object;
    if (platch_decode((uint8_t *) message->message, message->message_size, kStandardMethodCall, &object) != 0) {
        platch_respond_illegal_arg_std(message->response_handle, "Malformed method call.");
        return;
    }

    if (strcmp(object.method, "listen") == 0) {
        pthread_mutex_lock(&self->lock);
        self->uplink_listening = true;
        pthread_mutex_unlock(&self->lock);
        platch_respond_success_std(message->response_handle, NULL);
    } else if (strcmp(object.method, "cancel") == 0) {
        pthread_mutex_lock(&self->lock);
        self->uplink_listening = false;
        pthread_mutex_unlock(&self->lock);
        platch_respond_success_std(message->response_handle, NULL);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }

    platch_free_obj(&object);
}

enum plugin_init_result syncn_intercom_audio_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct syncn_intercom_audio *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    self->flutterpi = flutterpi;
    pthread_mutex_init(&self->lock, NULL);

    struct plugin_registry *registry = flutterpi_get_plugin_registry(flutterpi);

    int ok = plugin_registry_set_receiver_v2_locked(registry, SYNCN_INTERCOM_AUDIO_METHOD_CHANNEL, on_method_channel_message, self);
    if (ok != 0) {
        pthread_mutex_destroy(&self->lock);
        free(self);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    ok = plugin_registry_set_receiver_v2_locked(registry, SYNCN_INTERCOM_AUDIO_EVENT_CHANNEL, on_event_channel_message, self);
    if (ok != 0) {
        plugin_registry_remove_receiver_v2_locked(registry, SYNCN_INTERCOM_AUDIO_METHOD_CHANNEL);
        pthread_mutex_destroy(&self->lock);
        free(self);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = self;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void syncn_intercom_audio_deinit(struct flutterpi *flutterpi, void *userdata) {
    struct syncn_intercom_audio *self = userdata;
    struct plugin_registry *registry = flutterpi_get_plugin_registry(flutterpi);

    plugin_registry_remove_receiver_v2_locked(registry, SYNCN_INTERCOM_AUDIO_EVENT_CHANNEL);
    plugin_registry_remove_receiver_v2_locked(registry, SYNCN_INTERCOM_AUDIO_METHOD_CHANNEL);

    pthread_mutex_lock(&self->lock);
    GstElement *playback_pipeline, *capture_pipeline;
    teardown_locked(self, &playback_pipeline, &capture_pipeline);
    pthread_mutex_unlock(&self->lock);
    teardown_unlocked(playback_pipeline, capture_pipeline);

    pthread_mutex_destroy(&self->lock);
    free(self);
}

FLUTTERPI_PLUGIN("syncn_intercom_audio", syncn_intercom_audio, syncn_intercom_audio_init, syncn_intercom_audio_deinit)
