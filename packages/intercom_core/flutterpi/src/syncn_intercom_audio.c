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
// defaults to SPK_HP (speaker + headphone, both always on) and can be
// switched to headset-only (HP + Hands Free Mic) via the `headsetMode`
// start() arg / `setHeadsetMode` method, driven by a manual toggle in the
// app -- see AudioPipeline.setHeadsetMode in the Dart layer.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "syncn_intercom_audio.h"

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
    bool should_log = self->capture_count <= 30;
    pthread_mutex_unlock(&self->lock);

    if (should_log) {
        syncn_intercom_debug_log("audio", "on_new_capture_sample #%d: listening=%d, size=%zu", self->capture_count, listening, map.size);
    }

    if (listening && map.size > 0) {
        struct std_value event = {
            .type = kStdUInt8Array,
            .size = map.size,
            .uint8array = map.data,
        };
        int send_ret = platch_send_success_event_std(SYNCN_INTERCOM_AUDIO_EVENT_CHANNEL, &event);
        if (should_log) {
            syncn_intercom_debug_log("audio", "on_new_capture_sample #%d: platch_send_success_event_std -> %d", self->capture_count, send_ret);
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
    const char *playback_path = headset_mode ? "HP" : "SPK_HP";
    const char *capture_mic_path = headset_mode ? "Hands Free Mic" : "Main Mic";
    const char *playback_argv[] = { "amixer", "-c", "0", "sset", "Playback Path", playback_path, NULL };
    const char *capture_argv[] = { "amixer", "-c", "0", "sset", "Capture MIC Path", capture_mic_path, NULL };
    struct {
        const char *tag;
        const char **argv;
    } controls[] = {
        { headset_mode ? "Playback Path -> HP" : "Playback Path -> SPK_HP", playback_argv },
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
    // - sync=true + a ~80ms jitter queue: with sync=false, network jitter
    //   went straight to the DAC as underruns (pops/crackle). The queue
    //   absorbs jitter at the cost of a little added latency.
    // - volume=1.0: analog gain belongs to the ALSA mixer (see
    //   set_alsa_voice_routing / boot-time tuning); attenuating in software
    //   here just burned headroom and resolution.
    static const char *playback_desc_aec =
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "alawdec ! audioconvert ! audioresample quality=10 ! volume name=playvol volume=1.0 ! tee name=t ! "
        "queue min-threshold-time=80000000 max-size-time=400000000 ! "
        "alsasink device=default sync=true buffer-time=200000 latency-time=20000 "
        "t. ! queue leaky=downstream max-size-buffers=1 ! webrtcechoprobe name=syncn_echoprobe ! fakesink sync=false async=false";
    static const char *playback_desc_plain =
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "alawdec ! audioconvert ! audioresample quality=10 ! volume name=playvol volume=1.0 ! "
        "queue min-threshold-time=80000000 max-size-time=400000000 ! "
        "alsasink device=default sync=true buffer-time=200000 latency-time=20000";

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
    if (playback_appsrc == NULL || !start_pipeline_with_retry("syncn_intercom_audio playback", playback, 3, 150)) {
        if (aec_available) {
            // The echo probe/tee branch is the one new failure mode here (e.g.
            // fakesink negotiation); retry once without it before giving up
            // entirely -- a call with no echo cancellation beats no call.
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
    // - noise-suppression-level=high: hands-free wall panel in a live room,
    //   not a handset near the mouth -- aggressive NS is the right default.
    // - extended-filter=true: longer echo tail coverage; speaker and mic sit
    //   centimeters apart in the same enclosure, so the echo path is strong.
    static const char *capture_desc_aec =
        "alsasrc device=default ! audioconvert ! audioresample quality=10 ! "
        "audio/x-raw,rate=8000,channels=1,format=S16LE ! "
        "webrtcdsp name=dsp echo-cancel=true noise-suppression=true gain-control=true "
        "high-pass-filter=true noise-suppression-level=high extended-filter=true ! "
        "volume name=capvol ! alawenc ! "
        "appsink name=sink emit-signals=true sync=false max-buffers=4 drop=true";
    static const char *capture_desc_plain =
        "alsasrc device=default ! audioconvert ! audioresample quality=10 ! "
        "audio/x-raw,rate=8000,channels=1,format=S16LE ! "
        "volume name=capvol ! alawenc ! "
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
    pthread_mutex_lock(&self->lock);
    self->playback_count++;
    bool should_log = self->playback_count <= 30;
    if (self->running && self->playback_appsrc != NULL && size > 0) {
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
        gst_buffer_fill(buffer, 0, data, size);
        GstFlowReturn ret;
        g_signal_emit_by_name(self->playback_appsrc, "push-buffer", buffer, &ret);
        gst_buffer_unref(buffer);
        if (should_log) {
            syncn_intercom_debug_log("audio", "handle_play_downlink #%d: pushed %zu bytes, ret=%d", self->playback_count, size, ret);
        }
    } else if (should_log) {
        syncn_intercom_debug_log(
            "audio",
            "handle_play_downlink #%d: DROPPED (running=%d, playback_appsrc=%p, size=%zu)",
            self->playback_count,
            self->running,
            (void *) self->playback_appsrc,
            size
        );
    }
    pthread_mutex_unlock(&self->lock);
}

static void handle_set_muted(struct syncn_intercom_audio *self, bool muted) {
    pthread_mutex_lock(&self->lock);
    if (self->capture_volume != NULL) {
        g_object_set(self->capture_volume, "mute", (gboolean) muted, NULL);
    }
    pthread_mutex_unlock(&self->lock);
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

        pthread_mutex_lock(&self->lock);
        bool ok = start_locked(self, capture_enabled, headset_mode);
        pthread_mutex_unlock(&self->lock);

        if (ok) {
            platch_respond_success_std(message->response_handle, NULL);
        } else {
            platch_respond_error_std(message->response_handle, "audio_start_failed", "Could not start the GStreamer audio pipeline.", NULL);
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
