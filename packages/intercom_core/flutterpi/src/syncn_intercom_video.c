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
// (appsrc -> h264parse -> mppvideodec, Rockchip's hardware decoder, scaled
// to the caller's screen size and output as NV12). NV12->RGB conversion is
// done ourselves via a small GLES2 shader (not GStreamer's videoconvert,
// which benchmarked ~18x slower than decode itself on this board -- see the
// struct's yuv_program field comment) into an FBO-backed GL_TEXTURE_2D, on
// a dedicated EGL context shared with flutter-pi's main GL context, then
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
#include <time.h>

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

    // NV12->RGBA GPU conversion resources (2026-08-24): GStreamer's
    // videoconvert element produces the RGBA appsink needs, but measured
    // ~72s of the ~76s spent processing a small test capture -- vs. ~4s for
    // mppvideodec's decode+scale alone (isolated via gst-launch-1.0 file
    // benchmarks off-device from the app). Decode is fast; that one
    // conversion element is pathologically slow on this board's GStreamer
    // build, likely a per-frame caps/pool renegotiation rather than genuine
    // per-pixel cost. Fix: mppvideodec now outputs NV12 directly (no
    // videoconvert in the pipeline at all), and this file does the
    // NV12->RGB conversion itself via a tiny GLES2 shader into an FBO,
    // reusing the GPU we already touch for texture upload every frame
    // anyway. flutter-pi's own EGL context is GLES2-only (confirmed via its
    // gl_renderer.c: EGL_CONTEXT_CLIENT_VERSION=2), so this uses
    // GL_LUMINANCE/GL_LUMINANCE_ALPHA (core GLES2, no extension needed) for
    // the Y/UV planes rather than GLES3's GL_R8/GL_RG8.
    GLuint yuv_program;
    GLint yuv_attr_position;
    GLint yuv_attr_texcoord;
    GLint yuv_uniform_y_texture;
    GLint yuv_uniform_uv_texture;
    GLuint yuv_quad_vbo;
    GLuint y_texture_name;
    GLuint uv_texture_name;
    int yuv_texture_width;
    int yuv_texture_height;
    GLuint fbo;
    GLuint fbo_texture_name; // which gl_texture_name the FBO is currently bound to
    bool yuv_gl_resources_ready;

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

static double ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000.0 + (now.tv_nsec - start->tv_nsec) / 1e6;
}

static const char *k_yuv_vertex_shader_src =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

// BT.601 limited-range NV12->RGB, the conventional default for H.264 camera
// streams at this resolution class. GL_LUMINANCE sampling returns the byte
// in .r (==.g==.b); GL_LUMINANCE_ALPHA returns the two interleaved UV bytes
// as .r (U) and .a (V).
static const char *k_yuv_fragment_shader_src =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_y_texture;\n"
    "uniform sampler2D u_uv_texture;\n"
    "void main() {\n"
    "    float y = texture2D(u_y_texture, v_texcoord).r;\n"
    "    vec4 uv_sample = texture2D(u_uv_texture, v_texcoord);\n"
    "    float u = uv_sample.r - 0.5;\n"
    "    float v = uv_sample.a - 0.5;\n"
    "    float y_adj = y - 0.0625;\n"
    "    float r = 1.164 * y_adj + 1.596 * v;\n"
    "    float g = 1.164 * y_adj - 0.391 * u - 0.813 * v;\n"
    "    float b = 1.164 * y_adj + 2.018 * u;\n"
    "    gl_FragColor = vec4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG_ERROR("syncn_intercom_video: shader compile failed: %s\n", log);
        syncn_intercom_debug_log("video", "shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Lazily builds the YUV->RGB shader program, a full-screen-quad VBO, the Y/UV
// texture objects, and an FBO -- called with the dedicated EGL context
// already current. Mirrors ensure_egl_context_locked's lazy-init pattern
// (see that function's comment): avoid allocating GPU resources until video
// actually starts.
static bool ensure_yuv_gl_resources_locked(struct syncn_intercom_video *self) {
    if (self->yuv_gl_resources_ready) {
        return true;
    }

    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, k_yuv_vertex_shader_src);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, k_yuv_fragment_shader_src);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) glDeleteShader(vertex_shader);
        if (fragment_shader != 0) glDeleteShader(fragment_shader);
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOG_ERROR("syncn_intercom_video: shader link failed: %s\n", log);
        syncn_intercom_debug_log("video", "shader link failed: %s", log);
        glDeleteProgram(program);
        return false;
    }

    self->yuv_program = program;
    self->yuv_attr_position = glGetAttribLocation(program, "a_position");
    self->yuv_attr_texcoord = glGetAttribLocation(program, "a_texcoord");
    self->yuv_uniform_y_texture = glGetUniformLocation(program, "u_y_texture");
    self->yuv_uniform_uv_texture = glGetUniformLocation(program, "u_uv_texture");

    // Full-screen quad as a triangle strip: (x, y, u, v) per vertex.
    // No v-flip here (2026-08-24 correction): an earlier version flipped v
    // assuming a general FBO-rendering convention mismatch, but confirmed
    // wrong on-device -- the result came out upside down. Direct 1:1
    // mapping (NDC bottom -> v=0, NDC top -> v=1) is correct for this
    // straightforward sample-and-write pass.
    const GLfloat quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &self->yuv_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, self->yuv_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &self->y_texture_name);
    glBindTexture(GL_TEXTURE_2D, self->y_texture_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &self->uv_texture_name);
    glBindTexture(GL_TEXTURE_2D, self->uv_texture_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &self->fbo);

    self->yuv_gl_resources_ready = true;
    return true;
}

// Safe to call unconditionally before destroy_egl_context_locked, from any
// teardown/error path -- no-ops if resources were never created, and makes
// the dedicated context current itself (mirrors teardown_pipeline_unlocked's
// own eglMakeCurrent-before-glDelete* pattern) rather than assuming the
// caller already has it current.
static void destroy_yuv_gl_resources_locked(struct syncn_intercom_video *self) {
    if (!self->yuv_gl_resources_ready) {
        return;
    }
    if (self->egl_context != EGL_NO_CONTEXT &&
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context)) {
        if (self->yuv_program != 0) glDeleteProgram(self->yuv_program);
        if (self->yuv_quad_vbo != 0) glDeleteBuffers(1, &self->yuv_quad_vbo);
        if (self->y_texture_name != 0) glDeleteTextures(1, &self->y_texture_name);
        if (self->uv_texture_name != 0) glDeleteTextures(1, &self->uv_texture_name);
        if (self->fbo != 0) glDeleteFramebuffers(1, &self->fbo);
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    self->yuv_program = 0;
    self->yuv_quad_vbo = 0;
    self->y_texture_name = 0;
    self->uv_texture_name = 0;
    self->fbo = 0;
    self->fbo_texture_name = 0;
    self->yuv_texture_width = 0;
    self->yuv_texture_height = 0;
    self->yuv_gl_resources_ready = false;
}

// Called on a GStreamer streaming thread (not the flutter-pi platform
// thread), once per decoded frame.
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer userdata) {
    struct syncn_intercom_video *self = userdata;

    // Diagnostic timing (2026-08-24): both avdec_h264 and mppvideodec
    // independently settled at the identical ~300ms/frame rate, and NALs are
    // confirmed arriving from the network much faster than that -- so the
    // bottleneck is somewhere in this function's own path (pull/map/EGL/
    // upload), not in decode itself. t_pull_start brackets
    // gst_app_sink_pull_sample() specifically: if THAT call is what's slow,
    // the pacing is coming from upstream (queue/decoder handing us samples
    // slowly); if it returns fast and the delay shows up in the later
    // stages instead, it's the EGL context switch or GL upload.
    struct timespec t_pull_start;
    clock_gettime(CLOCK_MONOTONIC, &t_pull_start);

    GstSample *sample = gst_app_sink_pull_sample(sink);
    double pull_ms = ms_since(&t_pull_start);
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

    // Real internal pipeline latency for this specific frame: PTS is the
    // running-time this buffer was stamped with when appsrc received it
    // (do-timestamp=true), and current running-time is "now" in the same
    // clock domain -- the difference is exactly how long the buffer has
    // been in the pipeline (queue dwell + decode), independent of wall-clock
    // drift. This is what pull_sample/GL-upload timing could NOT show: that
    // only covers work after the decoder already finished.
    double pipeline_latency_ms = -1.0;
    GstClockTime buf_pts = GST_BUFFER_PTS(buffer);
    if (self->pipeline != NULL && GST_CLOCK_TIME_IS_VALID(buf_pts)) {
        GstClockTime running_time = gst_element_get_current_running_time(self->pipeline);
        if (GST_CLOCK_TIME_IS_VALID(running_time) && running_time >= buf_pts) {
            pipeline_latency_ms = (double) (running_time - buf_pts) / 1e6;
        }
    }

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
            "on_new_sample #%d: running=%d, decoded %dx%d, buffer size=%zu, pull_sample took %.1fms, pipeline latency (queue dwell + decode) %.1fms",
            self->frame_count,
            self->running,
            width,
            height,
            map.size,
            pull_ms,
            pipeline_latency_ms
        );
    }

    if (!self->running) {
        if (should_log) syncn_intercom_debug_log("video", "on_new_sample #%d: dropped, self->running is false", self->frame_count);
        pthread_mutex_unlock(&self->lock);
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    struct timespec t_egl_start;
    clock_gettime(CLOCK_MONOTONIC, &t_egl_start);
    int gl_context_ret = eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context) ? 0 : EIO;
    double egl_ms = ms_since(&t_egl_start);
    if (should_log) syncn_intercom_debug_log("video", "on_new_sample #%d: eglMakeCurrent(own context) -> %d (eglGetError=0x%x), took %.1fms", self->frame_count, gl_context_ret, eglGetError(), egl_ms);

    if (gl_context_ret == 0 && ensure_yuv_gl_resources_locked(self)) {
        struct timespec t_upload_start;
        clock_gettime(CLOCK_MONOTONIC, &t_upload_start);

        // Upstream is now NV12 (2 planes: full-res Y, then half-res
        // interleaved UV) -- see this struct's comment for why videoconvert
        // was removed. Same stride-aware upload approach as before (GLES2
        // has no GL_UNPACK_ROW_LENGTH without an extension, so fall back to
        // row-by-row when a plane isn't tightly packed).
        int y_width = width;
        int y_height = height;
        int y_stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
        const uint8_t *y_plane = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 0);

        int uv_width = GST_VIDEO_INFO_COMP_WIDTH(&info, 1);
        int uv_height = GST_VIDEO_INFO_COMP_HEIGHT(&info, 1);
        int uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 1);
        const uint8_t *uv_plane = map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 1);

        bool size_changed = (y_width != self->yuv_texture_width || y_height != self->yuv_texture_height);

        glBindTexture(GL_TEXTURE_2D, self->y_texture_name);
        if (size_changed) {
            if (y_stride == y_width) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, y_width, y_height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_plane);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, y_width, y_height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
                for (int y = 0; y < y_height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, y_width, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_plane + y * y_stride);
                }
            }
        } else {
            if (y_stride == y_width) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, y_width, y_height, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_plane);
            } else {
                for (int y = 0; y < y_height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, y_width, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_plane + y * y_stride);
                }
            }
        }

        glBindTexture(GL_TEXTURE_2D, self->uv_texture_name);
        if (size_changed) {
            if (uv_stride == uv_width * 2) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, uv_width, uv_height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_plane);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, uv_width, uv_height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
                for (int y = 0; y < uv_height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, uv_width, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_plane + y * uv_stride);
                }
            }
        } else {
            if (uv_stride == uv_width * 2) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_plane);
            } else {
                for (int y = 0; y < uv_height; y++) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, uv_width, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_plane + y * uv_stride);
                }
            }
        }

        if (size_changed) {
            self->yuv_texture_width = y_width;
            self->yuv_texture_height = y_height;
        }

        // Output (RGBA) texture: same reused gl_texture_name as before, just
        // now filled by rendering through the shader instead of a direct
        // pixel upload.
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
        if (width != self->gl_texture_width || height != self->gl_texture_height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            self->gl_texture_width = width;
            self->gl_texture_height = height;
        }

        // (Re)attach the FBO's color target whenever the output texture
        // object or its size changed -- glFramebufferTexture2D must be
        // re-issued after a glTexImage2D resize, not just on first bind.
        glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
        if (self->fbo_texture_name != self->gl_texture_name || size_changed) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->gl_texture_name, 0);
            self->fbo_texture_name = self->gl_texture_name;
        }

        glViewport(0, 0, width, height);
        glUseProgram(self->yuv_program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, self->y_texture_name);
        glUniform1i(self->yuv_uniform_y_texture, 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, self->uv_texture_name);
        glUniform1i(self->yuv_uniform_uv_texture, 1);

        glBindBuffer(GL_ARRAY_BUFFER, self->yuv_quad_vbo);
        glEnableVertexAttribArray((GLuint) self->yuv_attr_position);
        glVertexAttribPointer((GLuint) self->yuv_attr_position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *) 0);
        glEnableVertexAttribArray((GLuint) self->yuv_attr_texcoord);
        glVertexAttribPointer((GLuint) self->yuv_attr_texcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *) (2 * sizeof(GLfloat)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray((GLuint) self->yuv_attr_position);
        glDisableVertexAttribArray((GLuint) self->yuv_attr_texcoord);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

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
        double upload_ms = ms_since(&t_upload_start);
        struct timespec t_push_start;
        clock_gettime(CLOCK_MONOTONIC, &t_push_start);
        int push_ret = texture_push_frame(self->texture, &frame);
        double push_ms = ms_since(&t_push_start);
        if (should_log) {
            syncn_intercom_debug_log(
                "video",
                "on_new_sample #%d: NV12->RGB shader gl_texture_name=%u (took %.1fms), texture_push_frame -> %d (took %.1fms), TOTAL since pull_sample returned: %.1fms",
                self->frame_count,
                self->gl_texture_name,
                upload_ms,
                push_ret,
                push_ms,
                ms_since(&t_egl_start)
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

// Fits the door's known 16:9 source aspect inside a screen_w x screen_h
// bounding box (never upscaling past the source, never distorting aspect),
// rounded down to even numbers since YUV/H.264 formats require it. Screens
// vary per panel model (4-inch vs 10-inch builds use different physical
// resolutions -- see panel_home_4inch.dart / panel_home_10inch.dart), so
// this is computed per-call from whatever Dart reports at start() time
// rather than a value hardcoded for one specific panel.
static void compute_decode_target(int screen_w, int screen_h, int *out_w, int *out_h) {
    const int src_w = 1280, src_h = 720;
    if (screen_w <= 0 || screen_h <= 0) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    int candidate_w = screen_w;
    int candidate_h = (int) ((int64_t) candidate_w * src_h / src_w);
    if (candidate_h > screen_h) {
        candidate_h = screen_h;
        candidate_w = (int) ((int64_t) candidate_h * src_w / src_h);
    }
    if (candidate_w > src_w || candidate_h > src_h) {
        candidate_w = src_w;
        candidate_h = src_h;
    }
    *out_w = candidate_w & ~1;
    *out_h = candidate_h & ~1;
}

static int64_t handle_start(struct syncn_intercom_video *self, int screen_w, int screen_h) {
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
            destroy_yuv_gl_resources_locked(self);
            destroy_egl_context_locked(self);
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    if (self->texture == NULL) {
        self->texture = texture_new(self->texture_registry);
        if (self->texture == NULL) {
            destroy_yuv_gl_resources_locked(self);
            destroy_egl_context_locked(self);
            pthread_mutex_unlock(&self->lock);
            return -1;
        }
    }

    GError *error = NULL;
    // Findings so far (2026-08-24): bumping the leaky queue to 5s (a one-shot
    // diagnostic, now reverted to 800ms below) made lag WORSE (grew to a
    // full 5s, matching the cap exactly) and framerate WORSE too -- proving
    // decode genuinely cannot sustain the incoming rate in real time, and
    // that the queue was correctly protecting against a real backlog, not
    // manufacturing one. Our own pull_sample+GL-upload timing (~2.5-5ms)
    // only measures work AFTER the decoder has already produced a frame --
    // it says nothing about how long decode itself takes, which happens
    // entirely before on_new_sample's callback fires. mppvideodec's caps
    // negotiation was also confirmed forcing it to output RGBA directly
    // (buffer size matched 1280*720*4 exactly), meaning any NV12->RGBA
    // colorspace conversion happens *inside* the decoder call, not in the
    // separate videoconvert element -- if that conversion isn't going
    // through the SoC's RGA hardware block, it could be a slow per-pixel
    // software path scaling with full 1280x720 resolution when the actual
    // screen is much smaller. mppvideodec's width/height properties let it
    // decode-and-scale in one hardware pass instead of full-res decode +
    // separate convert -- compute_decode_target() sizes that per-call to
    // whatever screen Dart reports, instead of a value hardcoded for one
    // specific panel model.
    int decode_w, decode_h;
    compute_decode_target(screen_w, screen_h, &decode_w, &decode_h);
    syncn_intercom_debug_log(
        "video",
        "handle_start: screen=%dx%d -> decode target=%dx%d (0x0 means native 1280x720, no scaling)",
        screen_w,
        screen_h,
        decode_w,
        decode_h
    );

    // The leaky queue moved from before h264parse to after it (2026-08-24):
    // sitting before h264parse, it was leaking raw individual NALs -- a drop
    // could take out some but not all of a single access unit's NALs,
    // corrupting that frame's decode directly (not just breaking the
    // reference chain for later frames). h264parse's output caps are pinned
    // to alignment=au below (mppvideodec's own sink pad already requires
    // au alignment, so this just makes explicit what negotiation would have
    // forced anyway) -- so the queue now only ever sees, and only ever
    // drops, complete parsed frames. A drop still means the decoder has to
    // wait for the next IDR to fully recover (fundamental to H.264 P-frame
    // referencing, no way around that without touching the encoder), but it
    // no longer manufactures blocky/smeared corruption from partial NALs --
    // worst case is a clean skip/freeze instead.
    char pipeline_desc[512];
    if (decode_w > 0 && decode_h > 0) {
        snprintf(
            pipeline_desc,
            sizeof(pipeline_desc),
            "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
            "h264parse config-interval=-1 ! video/x-h264,alignment=au ! "
            "queue name=inq max-size-buffers=0 max-size-bytes=0 max-size-time=800000000 leaky=downstream ! "
            // ignore-error stays at its default (true): setting it false was
            // a one-shot diagnostic (2026-08-24) to check whether mppvideodec
            // was silently swallowing real decode errors, but the actual
            // root cause of the corruption turned out to be the Dart-side
            // unbounded submit() queue (see CallController's
            // _maxVideoSubmitsInFlight) forcing the native pipeline to drop
            // over half of all frames -- ignore-error=false was never
            // conclusively tested (build issues) and is unsafe to ship
            // regardless, since a single bad frame can kill the whole
            // pipeline with it set false.
            // No videoconvert / RGBA caps here anymore -- benchmarked
            // off-device with gst-launch-1.0 against a real captured NAL
            // stream (2026-08-24): decode+scale alone took ~4s for the test
            // file, decode+scale+NV12 (no videoconvert) also ~4s, but
            // decode+scale+videoconvert-to-RGBA took ~76s. The conversion
            // element itself is the entire cost (~18x), not decode. NV12
            // output is converted to RGB ourselves in on_new_sample via a
            // GLES2 shader instead.
            "mppvideodec qos=false width=%d height=%d ! video/x-raw,format=NV12 ! "
            "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false",
            decode_w,
            decode_h
        );
    } else {
        snprintf(
            pipeline_desc,
            sizeof(pipeline_desc),
            "appsrc name=src is-live=true format=time do-timestamp=true block=false ! "
            "h264parse config-interval=-1 ! video/x-h264,alignment=au ! "
            "queue name=inq max-size-buffers=0 max-size-bytes=0 max-size-time=800000000 leaky=downstream ! "
            "mppvideodec qos=false ! video/x-raw,format=NV12 ! "
            "appsink name=sink emit-signals=true max-buffers=1 drop=true sync=false"
        );
    }
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &error);
    if (pipeline == NULL) {
        LOG_ERROR("syncn_intercom_video: failed to build pipeline: %s\n", error != NULL ? error->message : "unknown error");
        if (error != NULL) g_error_free(error);
        destroy_yuv_gl_resources_locked(self);
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
        destroy_yuv_gl_resources_locked(self);
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
            destroy_yuv_gl_resources_locked(self);
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
    // See the same fix/rationale on frame_count's should_log in
    // on_new_sample() -- a hard cutoff at 30 made it impossible to see NAL
    // arrival timing during any test longer than a couple seconds.
    bool should_log = self->submit_count <= 30 || self->submit_count % 15 == 0;
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
        destroy_yuv_gl_resources_locked(self);
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
        int screen_w = 0, screen_h = 0;
        if (object.std_arg.type == kStdMap) {
            for (size_t i = 0; i < object.std_arg.size; i++) {
                struct std_value *key = &object.std_arg.keys[i];
                if (key->type != kStdString) continue;
                struct std_value *value = &object.std_arg.values[i];
                if (strcmp(key->string_value, "screenWidth") == 0) {
                    screen_w = STDVALUE_IS_INT(*value) ? (int) STDVALUE_AS_INT(*value) : 0;
                } else if (strcmp(key->string_value, "screenHeight") == 0) {
                    screen_h = STDVALUE_IS_INT(*value) ? (int) STDVALUE_AS_INT(*value) : 0;
                }
            }
        }
        int64_t id = handle_start(self, screen_w, screen_h);
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
    destroy_yuv_gl_resources_locked(self);
    destroy_egl_context_locked(self);
    pthread_mutex_destroy(&self->lock);
    instance = NULL;
    free(self);
}

FLUTTERPI_PLUGIN("syncn_intercom_video", syncn_intercom_video, syncn_intercom_video_init, syncn_intercom_video_deinit)
