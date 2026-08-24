// SPDX-License-Identifier: MIT
//
// flutter-pi native implementation of the "syncn_intercom/video" MethodChannel.
//
// Mirrors the Android implementation (VideoDecoderHandler.kt): `start()` creates
// a Flutter external texture and returns its id, `submit(Uint8List)` pushes one
// H.264 NAL unit (Annex-B byte-stream, one NAL per submit() call -- confirmed
// against mock_door/video_stream.py's `_read_ffmpeg_stdout`, which splits
// ffmpeg's Annex-B output on start codes and sends each NAL individually via
// `encode_frame(VIDEO, nalu)`; h264parse reassembles NALs into access units),
// `stop()` tears everything down. Decoding is done by a GStreamer pipeline
// (appsrc -> h264parse -> avdec_h264 -> videoconvert), and each decoded RGBA
// frame is uploaded into a plain GL_TEXTURE_2D via glTexImage2D/glTexSubImage2D
// on a dedicated EGL context shared with flutter-pi's main GL context, then
// pushed into a flutter-pi `struct texture`.
//
// NOTE: mock_door is a development stand-in for the real door unit -- its
// encoder settings (ffmpeg libx264, baseline profile, byte-stream Annex-B)
// are a reasonable proxy but not a guarantee the real hardware encoder
// matches exactly. Re-verify against real door unit traffic if frames don't
// decode.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "syncn_intercom_video.h"

#include <errno.h>
#include <string.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "flutter-pi.h"
#include "gl_renderer.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "syncn_intercom_debug.h"
#include "syncn_intercom_gst_util.h"
#include "texture_registry.h"
#include "util/logging.h"

struct syncn_intercom_video {
    struct flutterpi *flutterpi;
    struct gl_renderer *gl_renderer;
    struct texture_registry *texture_registry;

    pthread_mutex_t lock;
    bool running;

    struct texture *texture;
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *appsink;

    // Lazily-created dedicated EGL context. flutter-pi's own "resource
    // uploading" EGL context is shared with the Flutter engine's internal use
    // (e.g. decoding image assets); a GStreamer streaming thread making that
    // shared context current concurrently with the engine fails with
    // EGL_BAD_ACCESS ("context already current on another thread"), confirmed
    // on-device via journald. A separate context shares GL objects with the
    // main context so uploaded textures are still visible to Flutter, but it
    // must not be created at plugin init: on this panel's vendor GPU stack,
    // an idle, always-created native context was implicated in steady RSS
    // growth. Create it on first video start and destroy it on stop.
    EGLDisplay egl_display;
    EGLContext egl_context;

    GLuint gl_texture_name;
    int gl_texture_width;
    int gl_texture_height;

    int submit_count;
    int frame_count;
};

// Only one intercom call (and therefore one video stream) is ever active at a
// time -- see CallController, which tears down/rebuilds per call -- so a
// process-wide singleton is sufficient and avoids plumbing instance handles
// through the platform channel.
static struct syncn_intercom_video *instance;

static void on_texture_frame_destroy(const struct texture_frame *frame, void *userdata) {
    (void) frame;
    (void) userdata;
    // The GL texture object is owned by `instance` and reused across frames
    // (we glTexSubImage2D into it rather than allocating a new one each
    // time), so there's nothing to free here.
}

// Called on a GStreamer streaming thread (not the flutter-pi platform
// thread), once per decoded frame.
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer userdata) {
    struct syncn_intercom_video *self = userdata;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (buffer == NULL || caps == NULL) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    int width = GST_VIDEO_INFO_WIDTH(&info);
    int height = GST_VIDEO_INFO_HEIGHT(&info);

    pthread_mutex_lock(&self->lock);

    self->frame_count++;
    // Logging every frame forever would spam the log file, but capping it at
    // the first 30 frames ever (the old behavior) made it impossible to see
    // frame timing during any test more than a few seconds after pipeline
    // start -- confirmed on-device 2026-08-24: this looked exactly like a
    // silently-stalled pipeline (no new log lines) when the video was
    // actually still running fine, just past frame 30. Log the first 30
    // always (covers startup), then every 15th frame after that, so a long
    // soak test still shows periodic real timing data.
    bool should_log = self->frame_count <= 30 || self->frame_count % 15 == 0;
    if (should_log) {
        syncn_intercom_debug_log(
            "video",
            "on_new_sample #%d: running=%d, decoded %dx%d, buffer size=%zu",
            self->frame_count,
            self->running,
            width,
            height,
            map.size
        );
    }

    if (!self->running) {
        if (should_log) syncn_intercom_debug_log("video", "on_new_sample #%d: dropped, self->running is false", self->frame_count);
        pthread_mutex_unlock(&self->lock);
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    int gl_context_ret = eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context) ? 0 : EIO;
    if (should_log) syncn_intercom_debug_log("video", "on_new_sample #%d: eglMakeCurrent(own context) -> %d (eglGetError=0x%x)", self->frame_count, gl_context_ret, eglGetError());

    if (gl_context_ret == 0) {
        if (self->gl_texture_name == 0) {
            glGenTextures(1, &self->gl_texture_name);
            glBindTexture(GL_TEXTURE_2D, self->gl_texture_name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, self->gl_texture_name);
        }

        // videoconvert's output stride can be padded; GST_VIDEO_INFO gives us
        // the real plane stride, but glTexImage2D assumes tightly packed rows,
        // so upload row-by-row when stride != width * 4 rather than assuming
        // GL_UNPACK_ROW_LENGTH is available (it isn't in GLES2 without an
        // extension).
        int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
        const uint8_t *plane = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 0);

        if (width != self->gl_texture_width || height != self->gl_texture_height) {
            if (stride == width * 4) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, plane);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                for (int y = 0; y < height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, plane + y * stride);
                }
            }
            self->gl_texture_width = width;
            self->gl_texture_height = height;
        } else {
            if (stride == width * 4) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, plane);
            } else {
                for (int y = 0; y < height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, plane + y * stride);
                }
            }
        }

        struct texture_frame frame = {
            .gl = {
                .target = GL_TEXTURE_2D,
                .name = self->gl_texture_name,
                .format = GL_RGBA8_OES,
                .width = (size_t) width,
                .height = (size_t) height,
            },
            .destroy = on_texture_frame_destroy,
            .userdata = NULL,
        };
        int push_ret = texture_push_frame(self->texture, &frame);
        if (should_log) {
            syncn_intercom_debug_log(
                "video",
                "on_new_sample #%d: uploaded gl_texture_name=%u, texture_push_frame -> %d",
                self->frame_count,
                self->gl_texture_name,
                push_ret
            );
        }

        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else if (should_log) {
        syncn_intercom_debug_log(
            "video",
            "on_new_sample #%d: SKIPPED upload -- eglMakeCurrent(own context) failed (%d)",
            self->frame_count,
            gl_context_ret
        );
    }

    pthread_mutex_unlock(&self->lock);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Splitting teardown into a quick locked part (just detach the pipeline/texture
// from `self` so on_new_sample's `self->running` check sees it's gone) and a
// separately-called unlocked part (the actual blocking GStreamer/GL calls) is
// required to avoid a deadlock: gst_element_set_state(..., GST_STATE_NULL) blocks
// until the pipeline's streaming thread (which is what calls on_new_sample) has
// settled, but on_new_sample itself tries to acquire `self->lock` at its very
// start -- if we called gst_element_set_state while still holding that same
// lock, the streaming thread could never acquire it to finish, and
// gst_element_set_state would then block forever waiting on a thread that's
// blocked waiting on us. Confirmed on-device: this exact pattern froze the
// whole panel (not just the intercom) on call decline/close, since flutter-pi's
// platform thread -- shared with all UI/input handling -- was the one stuck
// making this blocking call while holding the lock.
static void teardown_pipeline_locked(struct syncn_intercom_video *self, GstElement **pipeline_out, GLuint *gl_texture_name_out) {
    *pipeline_out = self->pipeline;
    self->pipeline = NULL;
    self->appsrc = NULL;
    self->appsink = NULL;
    *gl_texture_name_out = self->gl_texture_name;
    self->gl_texture_name = 0;
    self->gl_texture_width = 0;
    self->gl_texture_height = 0;
}

// gst_element_set_state()'s own return value is not a reliable success/failure
// signal for a live appsrc/appsink pipeline like this one -- it very commonly
// returns ASYNC (or NO_PREROLL, since is-live sources skip normal preroll)
// even when the pipeline is about to fail for a real reason (missing decoder,
// no audio device, negotiation failure, etc). GStreamer reports that failure
// asynchronously via a GST_MESSAGE_ERROR on the pipeline's bus instead, which
// nothing was checking before this -- so real, actionable errors on real
// hardware were being silently swallowed. This drains and logs them.
static void log_pipeline_bus_errors(const char *tag, GstElement *pipeline) {
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)) != NULL) {
        GError *err = NULL;
        gchar *debug = NULL;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &debug);
            LOG_ERROR("%s: GStreamer ERROR from %s: %s (%s)\n", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
            syncn_intercom_debug_log("video", "%s: GStreamer ERROR from %s: %s (%s)", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
        } else {
            gst_message_parse_warning(msg, &err, &debug);
            LOG_ERROR("%s: GStreamer WARNING from %s: %s (%s)\n", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
            syncn_intercom_debug_log("video", "%s: GStreamer WARNING from %s: %s (%s)", tag, GST_OBJECT_NAME(msg->src), err->message, debug != NULL ? debug : "no debug info");
        }
        g_clear_error(&err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

static bool ensure_egl_context_locked(struct syncn_intercom_video *self) {
    if (self->egl_context != EGL_NO_CONTEXT) {
        return true;
    }

    self->egl_display = gl_renderer_get_egl_display(self->gl_renderer);
    self->egl_context = gl_renderer_create_context(self->gl_renderer);
    if (self->egl_display == EGL_NO_DISPLAY || self->egl_context == EGL_NO_CONTEXT) {
        LOG_ERROR("syncn_intercom_video: could not create a dedicated EGL context.\n");
        self->egl_display = EGL_NO_DISPLAY;
        self->egl_context = EGL_NO_CONTEXT;
        return false;
    }
    return true;
}

static void destroy_egl_context_locked(struct syncn_intercom_video *self) {
    if (self->egl_context == EGL_NO_CONTEXT) {
        return;
    }

    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(self->egl_display, self->egl_context);
    self->egl_context = EGL_NO_CONTEXT;
    self->egl_display = EGL_NO_DISPLAY;
}

static bool teardown_pipeline_unlocked(struct syncn_intercom_video *self, GstElement *pipeline, GLuint gl_texture_name) {
    bool settled = syncn_gst_bounded_teardown("video", pipeline, 3);
    if (settled && gl_texture_name != 0 && eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context)) {
        glDeleteTextures(1, &gl_texture_name);
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else if (!settled && gl_texture_name != 0) {
        syncn_intercom_debug_log("video", "teardown_pipeline_unlocked: skipped GL texture cleanup after abandoned pipeline");
    }
    return settled;
}

static int64_t handle_start(struct syncn_intercom_video *self) {
    pthread_mutex_lock(&self->lock);

    syncn_intercom_debug_log("video", "handle_start called, self->running=%d", self->running);

    if (self->running) {
        int64_t id = texture_get_id(self->texture);
        syncn_intercom_debug_log("video", "handle_start: already running, returning existing texture id=%lld", (long long) id);
        pthread_mutex_unlock(&self->lock);
        return id;
    }

    self->submit_count = 0;
    self->frame_count = 0;

    if (!ensure_egl_context_locked(self)) {
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    if (!gst_is_initialized()) {
        GError *error = NULL;
        if (!gst_init_check(NULL, NULL, &error)) {
            LOG_ERROR("syncn_intercom_video: gst_init_check failed: %s\n", error != NULL ? error->message : "unknown error");
            if (error != NULL) g_error_free(error);
            destroy_egl_context_locked(self);
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    if (self->texture == NULL) {
        self->texture = texture_new(self->texture_registry);
        if (self->texture == NULL) {
            destroy_egl_context_locked(self);
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    GError *error = NULL;
    // Root cause found on-device 2026-08-24: this board's CPU was already
    // running near-saturated (~78% user, load avg 3.7+) and avdec_h264
    // (software decode) could not sustain real-time decode on top of that,
    // so it chronically fell behind the incoming stream. The leaky queue
    // below (added as an earlier band-aid) was firing constantly as a
    // result, and because submit() pushes one NAL at a time (not a whole
    // frame), dropping mid-frame NALs corrupts that access unit AND breaks
    // the H.264 reference chain for every subsequent P-frame until the next
    // IDR -- producing exactly the observed "blocky/stale regions, smearing,
    // sudden clean recovery" pattern. Queue tuning alone can't fix a
    // sustained throughput deficit, only mask jitter.
    //
    // This board has a Rockchip MPP hardware video decoder available
    // (confirmed via `gst-inspect-1.0 | grep mpp`: rockchipmpp's
    // mppvideodec), which offloads H.264 decode to a dedicated hardware
    // block instead of the CPU -- switching to it should let decode keep up
    // in real time on its own, making the leaky queue a rarely-used safety
    // net again instead of the everyday path.
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
        "queue name=inq max-size-buffers=0 max-size-bytes=0 max-size-time=450000000 leaky=downstream ! "
        "h264parse config-interval=-1 ! mppvideodec ! videoconvert ! "
        "video/x-raw,format=RGBA ! "
        "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false",
        &error
    );
    if (pipeline == NULL) {
        LOG_ERROR("syncn_intercom_video: failed to build pipeline: %s\n", error != NULL ? error->message : "unknown error");
        if (error != NULL) g_error_free(error);
        destroy_egl_context_locked(self);
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (appsrc == NULL || appsink == NULL) {
        LOG_ERROR("syncn_intercom_video: pipeline missing appsrc/appsink\n");
        gst_object_unref(pipeline);
        if (appsrc != NULL) gst_object_unref(appsrc);
        if (appsink != NULL) gst_object_unref(appsink);
        destroy_egl_context_locked(self);
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    // Caps embedded in the gst_parse_launch() string only set the pad
    // template's default caps -- on this device's GStreamer version that
    // was NOT reliably propagating to the live GstAppSrc, leaving appsrc
    // unable to negotiate with h264parse ("not-linked" bus error, appsrc
    // stuck at ASYNC/PAUSED instead of reaching PLAYING, confirmed
    // on-device via /tmp/syncn_intercom_debug.log). Setting caps explicitly
    // via the C API is the fix.
    GstCaps *appsrc_caps = gst_caps_new_simple(
        "video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "nal",
        NULL
    );
    gst_app_src_set_caps(GST_APP_SRC(appsrc), appsrc_caps);
    gst_caps_unref(appsrc_caps);
    gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), self);

    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    syncn_intercom_debug_log("video", "handle_start: gst_element_set_state(PLAYING) -> %d", state_ret);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("syncn_intercom_video: failed to start pipeline\n");
        log_pipeline_bus_errors("syncn_intercom_video", pipeline);
        gst_object_unref(appsrc);
        gst_object_unref(appsink);
        if (syncn_gst_bounded_teardown("video", pipeline, 3)) {
            destroy_egl_context_locked(self);
        }
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    // Non-blocking start: do NOT call gst_element_get_state() here.
    // That call blocks the flutter-pi platform thread for up to 3s while
    // the pipeline negotiates -- and while the platform thread is blocked,
    // it cannot process the submit() method-channel calls that deliver
    // incoming NAL frames to appsrc (confirmed deadlock-by-timeout on
    // real hardware, always exactly 3s, matching the reported delay).
    // A live appsrc pipeline reaches PLAYING without needing buffers, and
    // real failures surface asynchronously via the bus log below.
    log_pipeline_bus_errors("syncn_intercom_video", pipeline);

    self->pipeline = pipeline;
    self->appsrc = appsrc;
    self->appsink = appsink;
    self->running = true;

    int64_t id = texture_get_id(self->texture);
    syncn_intercom_debug_log("video", "handle_start: SUCCESS, texture id=%lld", (long long) id);
    pthread_mutex_unlock(&self->lock);
    return id;
}

static void handle_submit(struct syncn_intercom_video *self, const uint8_t *data, size_t size) {
    pthread_mutex_lock(&self->lock);
    self->submit_count++;
    bool should_log = self->submit_count <= 30;
    if (!self->running || self->appsrc == NULL || size == 0) {
        if (should_log) {
            syncn_intercom_debug_log(
                "video",
                "handle_submit #%d: DROPPED (running=%d, appsrc=%p, size=%zu)",
                self->submit_count,
                self->running,
                (void *) self->appsrc,
                size
            );
        }
        pthread_mutex_unlock(&self->lock);
        return;
    }

    // One-shot raw capture of the real door unit's NAL stream, to replay
    // through a standalone gst-launch-1.0 pipeline on-device (bypassing this
    // plugin's own appsrc/C code entirely) and determine whether GStreamer
    // itself can decode this exact data, independent of anything this file
    // does. Concatenated as-is (each submit() payload is expected to already
    // carry its own Annex-B start code), capped so it doesn't grow unbounded.
    if (self->submit_count <= 200) {
        FILE *raw = fopen("/tmp/syncn_video_raw.h264", "ab");
        if (raw != NULL) {
            fwrite(data, 1, size, raw);
            fclose(raw);
        }
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    gst_buffer_fill(buffer, 0, data, size);

    GstFlowReturn ret;
    g_signal_emit_by_name(self->appsrc, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);

    if (should_log) {
        // Bus errors (e.g. a real decode failure in h264parse/avdec_h264) are
        // only ever checked at handle_start time otherwise -- if the pipeline
        // starts fine but then fails once real data flows through it, nothing
        // would ever see that. Poll it here too, for the same logging window.
        log_pipeline_bus_errors("syncn_intercom_video (during submit)", self->pipeline);
        syncn_intercom_debug_log("video", "handle_submit #%d: pushed %zu bytes, push-buffer signal returned ret=%d", self->submit_count, size, ret);
    }

    pthread_mutex_unlock(&self->lock);
}

static void handle_stop(struct syncn_intercom_video *self) {
    pthread_mutex_lock(&self->lock);
    syncn_intercom_debug_log(
        "video",
        "handle_stop called: total submit_count=%d, total frame_count=%d",
        self->submit_count,
        self->frame_count
    );
    self->running = false;
    GstElement *pipeline;
    GLuint gl_texture_name;
    teardown_pipeline_locked(self, &pipeline, &gl_texture_name);
    pthread_mutex_unlock(&self->lock);

    bool settled = teardown_pipeline_unlocked(self, pipeline, gl_texture_name);

    if (settled) {
        pthread_mutex_lock(&self->lock);
        destroy_egl_context_locked(self);
        pthread_mutex_unlock(&self->lock);
    } else {
        syncn_intercom_debug_log("video", "handle_stop: skipped EGL context cleanup after abandoned pipeline");
    }
}

static void on_platform_message(void *userdata, const FlutterPlatformMessage *message) {
    struct syncn_intercom_video *self = userdata;

    struct platch_obj object;
    int ok = platch_decode((uint8_t *) message->message, message->message_size, kStandardMethodCall, &object);
    if (ok != 0) {
        platch_respond_illegal_arg_std(message->response_handle, "Malformed method call.");
        return;
    }

    if (strcmp(object.method, "start") == 0) {
        int64_t id = handle_start(self);
        if (id < 0) {
            platch_respond_error_std(message->response_handle, "video_start_failed", "Could not start the GStreamer video pipeline.", NULL);
        } else {
            struct std_value result = STDINT64(id);
            platch_respond_success_std(message->response_handle, &result);
        }
    } else if (strcmp(object.method, "submit") == 0) {
        if (object.std_arg.type == kStdUInt8Array) {
            handle_submit(self, object.std_arg.uint8array, object.std_arg.size);
        }
        platch_respond_success_std(message->response_handle, NULL);
    } else if (strcmp(object.method, "stop") == 0) {
        handle_stop(self);
        platch_respond_success_std(message->response_handle, NULL);
    } else {
        platch_respond_not_implemented(message->response_handle);
    }

    platch_free_obj(&object);
}

enum plugin_init_result syncn_intercom_video_init(struct flutterpi *flutterpi, void **userdata_out) {
    struct syncn_intercom_video *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    self->flutterpi = flutterpi;
    self->gl_renderer = flutterpi_get_gl_renderer(flutterpi);
    self->texture_registry = flutterpi_get_texture_registry(flutterpi);
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;
    pthread_mutex_init(&self->lock, NULL);

    if (self->gl_renderer == NULL || self->texture_registry == NULL) {
        LOG_ERROR("syncn_intercom_video: GL renderer / texture registry not available. Is EGL/GLES2 enabled?\n");
        free(self);
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }

    int ok = plugin_registry_set_receiver_v2_locked(
        flutterpi_get_plugin_registry(flutterpi),
        SYNCN_INTERCOM_VIDEO_CHANNEL,
        on_platform_message,
        self
    );
    if (ok != 0) {
        free(self);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    instance = self;
    *userdata_out = self;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void syncn_intercom_video_deinit(struct flutterpi *flutterpi, void *userdata) {
    struct syncn_intercom_video *self = userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), SYNCN_INTERCOM_VIDEO_CHANNEL);

    handle_stop(self);
    if (self->texture != NULL) {
        texture_destroy(self->texture);
    }
    destroy_egl_context_locked(self);
    pthread_mutex_destroy(&self->lock);
    instance = NULL;
    free(self);
}

FLUTTERPI_PLUGIN("syncn_intercom_video", syncn_intercom_video, syncn_intercom_video_init, syncn_intercom_video_deinit)
