/*
 * SyncN door video for flutter-pi.
 *
 * The daemon decodes the door's H.264 in hardware, converts it to BGRA, and
 * offers each frame over its control socket as a DMA-BUF descriptor. This
 * plugin is the other end: it imports each buffer once as an EGL image, hands
 * every frame to Flutter as an external texture, and tells the daemon when
 * Flutter has finished with a buffer so it can be written again.
 *
 * Three rules from docs/12 are kept here, and each has a reason:
 *
 *   frame_done for every frame taken     the pool has three buffers; one not
 *                                        returned is a third of the pipeline
 *   import once per BUFFER, not per id   ids never repeat, memory does; the
 *                                        cache is keyed on the DMA-BUF's inode
 *   use the stride the daemon sends      a guessed pitch shears the picture
 *
 * Threads. The socket is on flutter-pi's platform event loop, so parsing,
 * importing and pushing happen on the platform thread. Flutter releases a
 * frame from its raster thread; the destroy callback does nothing there but
 * post a task back to the platform thread, which sends frame_done. Retired
 * imports (after a resolution change) are freed on that same task, once
 * Flutter has demonstrably moved on to a newer frame -- deleting a GL texture
 * the raster thread may still be sampling is the one thing this must not do.
 *
 * Method channel "syncn/video":
 *   start  -> {textureId: int64}   connect, subscribe, return the texture
 *   stop   -> null                  disconnect and drop everything
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "flutter-pi.h"
#include "gl_renderer.h"
#include "pluginregistry.h"
#include "platformchannel.h"
#include "texture_registry.h"
#include "util/logging.h"

#include "syncn_video_proto.h"

#define CHANNEL      "syncn/video"
#define DEFAULT_SOCK "/run/syncn/intercomd.sock"
#define SV_LINE_MAX     1024

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241   /* 'AR24': BGRA8888 little-endian */
#endif
#ifndef GL_RGBA8_OES
#define GL_RGBA8_OES 0x8058
#endif

/* One imported pool buffer: the EGL image and the GL texture bound to it. */
struct sv_import {
    EGLImageKHR image;
    GLuint      tex;
    GLenum      target;
};

/* One frame handed to Flutter. Lives until Flutter's destroy callback. */
struct sv_pushed {
    uint64_t id;
};

/* An import that was evicted while Flutter might still be sampling it. Freed
 * on the platform thread after a newer frame has been released. */
struct sv_retired {
    struct sv_import *imp;
    struct sv_retired *next;
};

struct sv_plugin {
    struct flutterpi *flutterpi;

    /* EGL, borrowed from flutter-pi's renderer; our own context. */
    EGLDisplay display;
    EGLContext context;
    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    bool external_target;

    /* The socket to the daemon, on the platform event loop. */
    int              sock;
    sd_event_source *source;
    char             in[SV_LINE_MAX * 2];
    size_t           in_len;

    struct texture *texture;
    sv_cache        cache;
    struct sv_retired *retired;

    /* A descriptor that arrived with a partial line. The daemon sends each
     * frame line and its descriptor in one sendmsg, and the kernel delivers
     * the descriptor with the first byte of that data -- so a short read gets
     * the descriptor before the newline. It belongs to the first line that
     * completes after it. */
    int pending_fd;

    uint64_t frames_pushed, frames_done, frames_dropped, imports_failed;
    uint32_t width, height;
    bool     warned_full;
};

static struct sv_plugin g;   /* one per process, like flutter-pi's own plugins */

/* ------------------------------------------------------------ the socket --- */

static bool sock_send_line(const char *line)
{
    if (g.sock < 0)
        return false;
    const size_t n = strlen(line);
    ssize_t w = send(g.sock, line, n, MSG_NOSIGNAL);
    return w == (ssize_t)n;
}

/* recvmsg with room for one descriptor. Mirrors syncn_fd_recv in the daemon,
 * inlined so this file has no dependency on the daemon's library. */
static ssize_t sock_recv_fd(int sock, void *buf, size_t len, int *fd_out)
{
    *fd_out = -1;

    struct iovec iov = { .iov_base = buf, .iov_len = len };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof ctl);

    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = &ctl;
    msg.msg_controllen = sizeof ctl;

    const ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n <= 0)
        return n;

    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(fd_out, CMSG_DATA(c), sizeof(int));
            break;
        }
    }
    return n;
}

/* -------------------------------------------------------------- imports --- */

static bool egl_current(bool on)
{
    const EGLBoolean ok = eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                         on ? g.context : EGL_NO_CONTEXT);
    if (ok == EGL_FALSE)
        LOG_ERROR("syncn_video: eglMakeCurrent(%s) failed: 0x%x\n", on ? "on" : "off", eglGetError());
    return ok == EGL_TRUE;
}

static struct sv_import *import_buffer(int fd, uint32_t w, uint32_t h, uint32_t stride)
{
    const EGLint attrs[] = {
        EGL_WIDTH,                     (EGLint)w,
        EGL_HEIGHT,                    (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT,      (EGLint)DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,     fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        /* The stride the daemon sent, in bytes. Not width*4. */
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)stride,
        EGL_NONE,
    };

    EGLImageKHR image = g.eglCreateImageKHR(g.display, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        LOG_ERROR("syncn_video: eglCreateImageKHR failed for %ux%u stride %u: 0x%x\n",
                  w, h, stride, eglGetError());
        return NULL;
    }

    struct sv_import *imp = calloc(1, sizeof *imp);
    if (!imp) {
        g.eglDestroyImageKHR(g.display, image);
        return NULL;
    }
    imp->image  = image;
    imp->target = g.external_target ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

    if (!egl_current(true)) {
        g.eglDestroyImageKHR(g.display, image);
        free(imp);
        return NULL;
    }

    glGenTextures(1, &imp->tex);
    glBindTexture(imp->target, imp->tex);
    g.glEGLImageTargetTexture2DOES(imp->target, image);
    const GLenum err = glGetError();
    glBindTexture(imp->target, 0);
    egl_current(false);

    if (err != GL_NO_ERROR) {
        LOG_ERROR("syncn_video: glEGLImageTargetTexture2DOES failed: 0x%x\n", err);
        egl_current(true);
        glDeleteTextures(1, &imp->tex);
        egl_current(false);
        g.eglDestroyImageKHR(g.display, image);
        free(imp);
        return NULL;
    }
    return imp;
}

/* Must run on the platform thread with no frame of Flutter's referencing it. */
static void free_import(struct sv_import *imp)
{
    if (!imp)
        return;
    if (egl_current(true)) {
        glDeleteTextures(1, &imp->tex);
        egl_current(false);
    }
    g.eglDestroyImageKHR(g.display, imp->image);
    free(imp);
}

/* Cache eviction hands the import here rather than freeing it: Flutter may be
 * mid-frame on that texture. It is freed once a newer frame comes back. */
static void retire_import(void *import, void *user)
{
    (void)user;
    struct sv_retired *r = calloc(1, sizeof *r);
    if (!r) {
        free_import(import);           /* better a stall than a leak */
        return;
    }
    r->imp    = import;
    r->next   = g.retired;
    g.retired = r;
}

static void free_retired(void)
{
    while (g.retired) {
        struct sv_retired *r = g.retired;
        g.retired = r->next;
        free_import(r->imp);
        free(r);
    }
}

/* --------------------------------------------------------- frame return --- */

/* Platform thread. Flutter has released the frame; the daemon may reuse the
 * buffer, and anything retired before this frame is now safe to free. */
static int on_frame_done_task(void *userdata)
{
    struct sv_pushed *p = userdata;

    char line[64];
    snprintf(line, sizeof line, "{\"cmd\":\"frame_done\",\"id\":%" PRIu64 "}\n", p->id);
    if (sock_send_line(line))
        g.frames_done++;

    free_retired();
    free(p);
    return 0;
}

/* Raster thread. Do nothing here but hand off. */
static void on_frame_destroy(const struct texture_frame *frame, void *userdata)
{
    (void)frame;
    flutterpi_post_platform_task(on_frame_done_task, userdata);
}

/* ------------------------------------------------------------- a frame --- */

static void handle_frame(const sv_line *l, int fd)
{
    if (fd < 0) {
        LOG_ERROR("syncn_video: frame %" PRIu64 " arrived with no descriptor\n", l->id);
        g.frames_dropped++;
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        LOG_ERROR("syncn_video: fstat on frame descriptor: %s\n", strerror(errno));
        close(fd);
        g.frames_dropped++;
        return;
    }

    sv_cache_entry *e = sv_cache_find(&g.cache, st.st_dev, st.st_ino);
    if (e) {
        close(fd);                         /* a duplicate of one we hold */
        if (e->width != l->width || e->height != l->height || e->stride != l->stride) {
            /* Same memory, different geometry: the daemon rebuilt its pool
             * without a reset reaching us. Treat it as one. */
            LOG_DEBUG("syncn_video: geometry changed under a cached buffer; resetting\n");
            sv_cache_clear(&g.cache, retire_import, NULL);
            e = NULL;
            fd = -1;
        }
    }

    if (!e) {
        if (fd < 0) {
            g.frames_dropped++;            /* we closed it above; wait for the next */
            return;
        }
        e = sv_cache_insert(&g.cache, st.st_dev, st.st_ino, fd, l->width, l->height, l->stride);
        if (!e) {
            if (!g.warned_full) {
                g.warned_full = true;
                LOG_ERROR("syncn_video: more than %d distinct buffers; the daemon's pool is "
                          "deeper than this plugin caches\n", SV_CACHE_MAX);
            }
            close(fd);
            g.frames_dropped++;
            return;
        }
        e->import = import_buffer(fd, l->width, l->height, l->stride);
        if (!e->import) {
            g.imports_failed++;
            g.frames_dropped++;
            /* Leave the entry: retrying every frame would log every frame. */
            return;
        }
        g.width  = l->width;
        g.height = l->height;
        LOG_DEBUG("syncn_video: imported buffer ino %llu as %ux%u stride %u\n",
                  (unsigned long long)st.st_ino, l->width, l->height, l->stride);
    }

    if (!e->import) {
        g.frames_dropped++;
        return;                            /* an earlier import failed */
    }

    struct sv_pushed *p = calloc(1, sizeof *p);
    if (!p) {
        g.frames_dropped++;
        return;
    }
    p->id = l->id;

    const struct sv_import *imp = e->import;
    const struct texture_frame frame = {
        .gl = {
            .target = imp->target,
            .name   = imp->tex,
            .format = GL_RGBA8_OES,
            .width  = l->width,
            .height = l->height,
        },
        .destroy  = on_frame_destroy,
        .userdata = p,
    };

    const int ok = texture_push_frame(g.texture, &frame);
    if (ok != 0) {
        /* Flutter refused it. The daemon must still get its buffer back, or
         * the pool loses a slot for the rest of the call. */
        LOG_ERROR("syncn_video: texture_push_frame: %s\n", strerror(ok));
        on_frame_done_task(p);
        g.frames_dropped++;
        return;
    }
    g.frames_pushed++;
}

/* ------------------------------------------------------------ the loop --- */

static void handle_line(char *line, int fd)
{
    const sv_line l = sv_parse_line(line);

    switch (l.kind) {
    case SV_LINE_FRAME:
        handle_frame(&l, fd);
        return;

    case SV_LINE_RESET:
        /* Every cached import describes memory of the wrong size now. */
        sv_cache_clear(&g.cache, retire_import, NULL);
        g.warned_full = false;
        break;

    case SV_LINE_STOPPED:
        LOG_DEBUG("syncn_video: call ended; %" PRIu64 " pushed, %" PRIu64 " returned, "
                  "%" PRIu64 " dropped\n", g.frames_pushed, g.frames_done, g.frames_dropped);
        break;

    case SV_LINE_OTHER:
        break;
    }

    /* A descriptor on a non-frame line is not ours to keep. */
    if (fd >= 0)
        close(fd);
}

static void disconnect(void);

static int on_socket_readable(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    (void)s; (void)userdata;

    if (revents & (EPOLLHUP | EPOLLERR)) {
        LOG_ERROR("syncn_video: daemon socket closed\n");
        disconnect();
        return 0;
    }

    for (;;) {
        const size_t room = sizeof g.in - g.in_len - 1;
        if (room == 0) {
            LOG_ERROR("syncn_video: over-long line from the daemon; resynchronising\n");
            g.in_len = 0;
            continue;
        }

        int got_fd = -1;
        const ssize_t n = sock_recv_fd(fd, g.in + g.in_len, room, &got_fd);
        if (n == 0) {
            disconnect();
            return 0;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            LOG_ERROR("syncn_video: recvmsg: %s\n", strerror(errno));
            disconnect();
            return 0;
        }
        g.in_len += (size_t)n;
        g.in[g.in_len] = '\0';

        /*
         * A descriptor belongs to the line it arrived with. The daemon sends
         * each frame line in its own sendmsg, so a descriptor pairs with the
         * first line to complete at or after the read that carried it; lines
         * after that in the same buffer carry none.
         */
        if (got_fd >= 0) {
            if (g.pending_fd >= 0) {
                /* Two descriptors with no line between them: the first has no
                 * line to belong to. Do not leak it. */
                close(g.pending_fd);
            }
            g.pending_fd = got_fd;
        }

        char *start = g.in;
        for (;;) {
            char *nl = memchr(start, '\n', g.in_len - (size_t)(start - g.in));
            if (!nl)
                break;
            *nl = '\0';
            if (*start) {
                handle_line(start, g.pending_fd);   /* takes ownership, or -1 */
                g.pending_fd = -1;
            }
            start = nl + 1;
        }

        const size_t consumed = (size_t)(start - g.in);
        if (consumed) {
            memmove(g.in, start, g.in_len - consumed);
            g.in_len -= consumed;
        }
    }
    return 0;
}

static bool connect_daemon(void)
{
    const char *path = getenv("SYNCN_IPC_PATH");
    if (!path || !*path)
        path = DEFAULT_SOCK;

    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) {
        LOG_ERROR("syncn_video: socket: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0 && errno != EINPROGRESS) {
        LOG_ERROR("syncn_video: connect %s: %s\n", path, strerror(errno));
        close(s);
        return false;
    }
    g.sock = s;
    g.in_len = 0;
    g.pending_fd = -1;

    if (!sock_send_line("{\"cmd\":\"subscribe_video\"}\n")) {
        LOG_ERROR("syncn_video: could not subscribe: %s\n", strerror(errno));
        close(s);
        g.sock = -1;
        return false;
    }

    const int ok = flutterpi_sd_event_add_io(&g.source, s, EPOLLIN, on_socket_readable, NULL);
    if (ok != 0) {
        LOG_ERROR("syncn_video: could not watch the socket: %s\n", strerror(-ok));
        close(s);
        g.sock = -1;
        return false;
    }
    LOG_DEBUG("syncn_video: connected to %s\n", path);
    return true;
}

static void disconnect(void)
{
    if (g.source) {
        sd_event_source_unref(g.source);
        g.source = NULL;
    }
    if (g.sock >= 0) {
        close(g.sock);
        g.sock = -1;
    }
    if (g.pending_fd >= 0) {
        close(g.pending_fd);
        g.pending_fd = -1;
    }
    g.in_len = 0;
    /* The daemon reclaims its pool on our disconnect; our imports hold their
     * own references to the memory, so retiring them is safe. */
    sv_cache_clear(&g.cache, retire_import, NULL);
}

/* ------------------------------------------------------- method channel --- */

static int on_start(FlutterPlatformMessageResponseHandle *handle)
{
    if (g.sock < 0 && !connect_daemon())
        return platch_respond_error_std(handle, "unavailable",
                                        "could not connect to the intercom daemon", NULL);

    if (!g.texture) {
        g.texture = texture_new(flutterpi_get_texture_registry(g.flutterpi));
        if (!g.texture)
            return platch_respond_error_std(handle, "unavailable",
                                            "could not register a texture", NULL);
    }

    return platch_respond_success_std(handle,
        &STDMAP1(STDSTRING("textureId"), STDINT64(texture_get_id(g.texture))));
}

static int on_stop(FlutterPlatformMessageResponseHandle *handle)
{
    disconnect();
    free_retired();
    return platch_respond_success_std(handle, &STDNULL);
}

static int on_receive(char *channel, struct platch_obj *object,
                      FlutterPlatformMessageResponseHandle *handle)
{
    (void)channel;
    const char *method = object->method;
    if (strcmp(method, "start") == 0)
        return on_start(handle);
    if (strcmp(method, "stop") == 0)
        return on_stop(handle);
    return platch_respond_not_implemented(handle);
}

/* ------------------------------------------------------------- lifecycle --- */

static enum plugin_init_result syncn_video_init(struct flutterpi *flutterpi, void **userdata_out)
{
    memset(&g, 0, sizeof g);
    g.flutterpi  = flutterpi;
    g.sock       = -1;
    g.pending_fd = -1;
    sv_cache_init(&g.cache);

    struct gl_renderer *r = flutterpi_get_gl_renderer(flutterpi);
    if (!r) {
        LOG_ERROR("syncn_video: no GL renderer; video needs EGL\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }
    if (!gl_renderer_supports_egl_extension(r, "EGL_EXT_image_dma_buf_import")) {
        LOG_ERROR("syncn_video: EGL lacks EGL_EXT_image_dma_buf_import\n");
        return PLUGIN_INIT_RESULT_NOT_APPLICABLE;
    }
    g.external_target = gl_renderer_supports_gl_extension(r, "GL_OES_EGL_image_external");

    g.display = gl_renderer_get_egl_display(r);
    g.context = gl_renderer_create_context(r);
    if (g.display == EGL_NO_DISPLAY || g.context == EGL_NO_CONTEXT) {
        LOG_ERROR("syncn_video: could not get an EGL display and context\n");
        return PLUGIN_INIT_RESULT_ERROR;
    }

    g.eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC)  gl_renderer_get_proc_address(r, "eglCreateImageKHR");
    g.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) gl_renderer_get_proc_address(r, "eglDestroyImageKHR");
    g.glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) gl_renderer_get_proc_address(r, "glEGLImageTargetTexture2DOES");
    if (!g.eglCreateImageKHR || !g.eglDestroyImageKHR || !g.glEGLImageTargetTexture2DOES) {
        LOG_ERROR("syncn_video: EGL image procedures unavailable\n");
        return PLUGIN_INIT_RESULT_ERROR;
    }

    const int ok = plugin_registry_set_receiver_locked(CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0)
        return PLUGIN_INIT_RESULT_ERROR;

    *userdata_out = &g;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

static void syncn_video_deinit(struct flutterpi *flutterpi, void *userdata)
{
    (void)userdata;
    disconnect();
    free_retired();
    if (g.texture) {
        texture_destroy(g.texture);
        g.texture = NULL;
    }
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), CHANNEL);
}

FLUTTERPI_PLUGIN("syncn video", syncn_video, syncn_video_init, syncn_video_deinit)
