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
// NOT implemented in this first pass (tracked as a follow-up, not blocking
// the "no sound at all" bug this exists to fix): AEC/NS/AGC parity with the
// Android path's AcousticEchoCanceler/NoiseSuppressor/AutomaticGainControl.
// GStreamer's `webrtcdsp` element (gst-plugins-bad) is the natural fit if/when
// echo quality turns out to need it on real hardware.

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

// Issues the PLAYING state change and actually waits for the real outcome
// (see log_pipeline_bus_errors' comment for why the immediate return value
// alone isn't trustworthy). Returns true if the pipeline is confirmed
// playing (or still legitimately pending as ASYNC), false on a real failure
// -- logging bus errors either way so real hardware failures are visible.
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

static void set_alsa_voice_routing(void) {
    const char *playback_argv[] = { "amixer", "-c", "0", "sset", "Playback Path", "SPK_HP", NULL };
    const char *capture_argv[] = { "amixer", "-c", "0", "sset", "Capture MIC Path", "Main Mic", NULL };
    struct {
        const char *tag;
        const char **argv;
    } controls[] = {
        { "Playback Path -> SPK_HP", playback_argv },
        { "Capture MIC Path -> Main Mic", capture_argv },
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

static bool start_locked(struct syncn_intercom_audio *self, bool capture_enabled) {
    syncn_intercom_debug_log("audio", "start_locked called, capture_enabled=%d, self->running=%d", capture_enabled, self->running);
    if (self->running) {
        self->capture_enabled = capture_enabled;
        return true;
    }
    set_alsa_voice_routing();
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

    GError *error = NULL;
    GstElement *playback = gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "alawdec ! audioconvert ! audioresample ! alsasink device=hw:0,0 sync=false",
        &error
    );
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
    if (playback_appsrc == NULL || !start_and_confirm_playing("syncn_intercom_audio playback", playback)) {
        LOG_ERROR("syncn_intercom_audio: failed to start playback pipeline\n");
        if (playback_appsrc != NULL) gst_object_unref(playback_appsrc);
        syncn_gst_bounded_teardown("audio-playback", playback, 3);
        return false;
    }

    GstElement *capture = NULL;
    GstElement *capture_appsink = NULL;
    GstElement *capture_volume = NULL;
    if (capture_enabled) {
        capture = gst_parse_launch(
            "alsasrc device=hw:0,0 ! audioconvert ! audioresample ! "
            "audio/x-raw,rate=8000,channels=1,format=S16LE ! "
            "volume name=capvol ! alawenc ! "
            "appsink name=sink emit-signals=true sync=false max-buffers=4 drop=true",
            &error
        );
        if (capture == NULL) {
            LOG_ERROR(
                "syncn_intercom_audio: failed to build capture pipeline (mic will be unavailable): %s\n",
                error != NULL ? error->message : "unknown error"
            );
            if (error != NULL) g_error_free(error);
        } else {
            capture_appsink = gst_bin_get_by_name(GST_BIN(capture), "sink");
            capture_volume = gst_bin_get_by_name(GST_BIN(capture), "capvol");
            if (capture_appsink == NULL) {
                gst_object_unref(capture);
                capture = NULL;
            } else {
                g_signal_connect(capture_appsink, "new-sample", G_CALLBACK(on_new_capture_sample), self);
                if (!start_and_confirm_playing("syncn_intercom_audio capture", capture)) {
                    LOG_ERROR("syncn_intercom_audio: failed to start capture pipeline (mic will be unavailable)\n");
                    gst_object_unref(capture_appsink);
                    if (capture_volume != NULL) gst_object_unref(capture_volume);
                    syncn_gst_bounded_teardown("audio-capture", capture, 3);
                    capture = NULL;
                    capture_appsink = NULL;
                    capture_volume = NULL;
                }
            }
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
        if (object.std_arg.type == kStdMap) {
            for (size_t i = 0; i < object.std_arg.size; i++) {
                struct std_value *key = &object.std_arg.keys[i];
                if (key->type == kStdString && strcmp(key->string_value, "captureEnabled") == 0) {
                    struct std_value *value = &object.std_arg.values[i];
                    capture_enabled = STDVALUE_IS_BOOL(*value) ? STDVALUE_AS_BOOL(*value) : true;
                }
            }
        }

        pthread_mutex_lock(&self->lock);
        bool ok = start_locked(self, capture_enabled);
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
