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
// on flutter-pi's shared "resource uploading" EGL context, then pushed into a
// flutter-pi `struct texture`.
//
// NOTE: mock_door is a development stand-in for the real door unit -- its
// encoder settings (ffmpeg libx264, baseline profile, byte-stream Annex-B)
// are a reasonable proxy but not a guarantee the real hardware encoder
// matches exactly. Re-verify against real door unit traffic if frames don't
// decode.

#include "syncn_intercom_video.h"

#include <string.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <GLES2/gl2.h>

#include "flutter-pi.h"
#include "gl_renderer.h"
#include "platformchannel.h"
#include "pluginregistry.h"
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

    GLuint gl_texture_name;
    int gl_texture_width;
    int gl_texture_height;
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

    if (self->running && gl_renderer_make_flutter_resource_uploading_context_current(self->gl_renderer) == 0) {
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
                .format = GL_RGBA8,
                .width = (size_t) width,
                .height = (size_t) height,
            },
            .destroy = on_texture_frame_destroy,
            .userdata = NULL,
        };
        texture_push_frame(self->texture, &frame);

        gl_renderer_clear_current(self->gl_renderer);
    }

    pthread_mutex_unlock(&self->lock);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void teardown_pipeline_locked(struct syncn_intercom_video *self) {
    if (self->pipeline != NULL) {
        gst_element_set_state(self->pipeline, GST_STATE_NULL);
        gst_object_unref(self->pipeline);
        self->pipeline = NULL;
        self->appsrc = NULL;
        self->appsink = NULL;
    }
    if (self->gl_texture_name != 0 && gl_renderer_make_flutter_resource_uploading_context_current(self->gl_renderer) == 0) {
        glDeleteTextures(1, &self->gl_texture_name);
        gl_renderer_clear_current(self->gl_renderer);
    }
    self->gl_texture_name = 0;
    self->gl_texture_width = 0;
    self->gl_texture_height = 0;
}

static int64_t handle_start(struct syncn_intercom_video *self) {
    pthread_mutex_lock(&self->lock);

    if (self->running) {
        int64_t id = texture_get_id(self->texture);
        pthread_mutex_unlock(&self->lock);
        return id;
    }

    if (!gst_is_initialized()) {
        GError *error = NULL;
        if (!gst_init_check(NULL, NULL, &error)) {
            LOG_ERROR("syncn_intercom_video: gst_init_check failed: %s\n", error != NULL ? error->message : "unknown error");
            if (error != NULL) g_error_free(error);
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    if (self->texture == NULL) {
        self->texture = texture_new(self->texture_registry);
        if (self->texture == NULL) {
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true block=false "
        "caps=video/x-h264,stream-format=byte-stream,alignment=nal ! "
        "h264parse config-interval=-1 ! avdec_h264 ! videoconvert ! "
        "video/x-raw,format=RGBA ! "
        "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false",
        &error
    );
    if (pipeline == NULL) {
        LOG_ERROR("syncn_intercom_video: failed to build pipeline: %s\n", error != NULL ? error->message : "unknown error");
        if (error != NULL) g_error_free(error);
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
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), self);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("syncn_intercom_video: failed to start pipeline\n");
        gst_object_unref(appsrc);
        gst_object_unref(appsink);
        gst_object_unref(pipeline);
        pthread_mutex_unlock(&self->lock);
        return -1;
    }

    self->pipeline = pipeline;
    self->appsrc = appsrc;
    self->appsink = appsink;
    self->running = true;

    int64_t id = texture_get_id(self->texture);
    pthread_mutex_unlock(&self->lock);
    return id;
}

static void handle_submit(struct syncn_intercom_video *self, const uint8_t *data, size_t size) {
    pthread_mutex_lock(&self->lock);
    if (!self->running || self->appsrc == NULL || size == 0) {
        pthread_mutex_unlock(&self->lock);
        return;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    gst_buffer_fill(buffer, 0, data, size);

    GstFlowReturn ret;
    g_signal_emit_by_name(self->appsrc, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);

    pthread_mutex_unlock(&self->lock);
}

static void handle_stop(struct syncn_intercom_video *self) {
    pthread_mutex_lock(&self->lock);
    self->running = false;
    teardown_pipeline_locked(self);
    pthread_mutex_unlock(&self->lock);
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
    pthread_mutex_destroy(&self->lock);
    instance = NULL;
    free(self);
}

FLUTTERPI_PLUGIN("syncn_intercom_video", syncn_intercom_video, syncn_intercom_video_init, syncn_intercom_video_deinit)
