/*
 * The parts of the flutter-pi video plugin that have logic in them, kept free
 * of flutter-pi, EGL and sockets so they can be tested on any machine.
 *
 *   - parsing the daemon's frame lines
 *   - the import cache, keyed on the buffer's identity rather than its id
 *
 * The plugin proper (syncn_video.c) is the glue around these: a socket on
 * flutter-pi's event loop, an EGL image per cached buffer, a texture push per
 * frame, and frame_done back to the daemon when Flutter releases it.
 *
 * Why the cache is keyed on identity and not id. The daemon's buffer ids
 * never repeat -- every frame carries a new one -- which is what makes a
 * stale id harmless. But it also means an import cached per id is never hit
 * again. The memory does repeat: a three-buffer pool has three DMA-BUFs, and
 * the same three come round for ever. A DMA-BUF is a file, and its inode
 * number identifies it across every descriptor the daemon hands us. So the
 * cache is keyed on (device, inode), holds one EGL image per pool buffer, and
 * a 20 fps stream costs three imports total rather than twenty a second.
 *
 * Header-only so the test and the plugin compile the same code.
 */
#ifndef SYNCN_VIDEO_PROTO_H
#define SYNCN_VIDEO_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------- the line --- */

typedef enum {
    SV_LINE_OTHER = 0,     /* a state or event line we do not act on   */
    SV_LINE_FRAME,         /* a frame; a descriptor travelled with it  */
    SV_LINE_RESET,         /* geometry changed: drop every cached import */
    SV_LINE_STOPPED,       /* the call ended                            */
} sv_line_kind;

typedef struct {
    sv_line_kind kind;
    uint64_t id;
    uint32_t width, height, stride;
    size_t   size;
} sv_line;

/*
 * Pull one unsigned integer field out of a flat JSON object by key. Enough
 * for lines the daemon itself writes with snprintf; not a JSON parser, and
 * makes no attempt to be one. Returns false if the key is absent.
 */
static inline bool sv_json_uint(const char *line, const char *key, uint64_t *out)
{
    char needle[64];
    const int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof needle)
        return false;

    const char *p = strstr(line, needle);
    if (!p)
        return false;
    p += n;
    while (*p == ' ')
        p++;
    if (*p < '0' || *p > '9')
        return false;

    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v > (UINT64_MAX - 9) / 10)
            return false;                           /* absurd; refuse */
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    *out = v;
    return true;
}

static inline bool sv_json_string_is(const char *line, const char *key, const char *want)
{
    char needle[64];
    const int n = snprintf(needle, sizeof needle, "\"%s\":\"%s\"", key, want);
    if (n <= 0 || (size_t)n >= sizeof needle)
        return false;
    return strstr(line, needle) != NULL;
}

/* Classify a line. A frame line must carry every geometry field, or it is
 * treated as not a frame: importing a buffer with a guessed stride shears the
 * picture, and a sheared picture that is almost right is worse than none. */
static inline sv_line sv_parse_line(const char *line)
{
    sv_line r;
    memset(&r, 0, sizeof r);

    if (sv_json_string_is(line, "event", "video_reset")) {
        r.kind = SV_LINE_RESET;
        return r;
    }
    if (sv_json_string_is(line, "event", "video_stopped")) {
        r.kind = SV_LINE_STOPPED;
        return r;
    }
    if (!sv_json_string_is(line, "event", "frame"))
        return r;

    uint64_t id = 0, w = 0, h = 0, st = 0, sz = 0;
    if (!sv_json_uint(line, "id", &id) || id == 0)
        return r;
    if (!sv_json_uint(line, "width", &w) || !sv_json_uint(line, "height", &h) ||
        !sv_json_uint(line, "stride", &st) || !sv_json_uint(line, "size", &sz))
        return r;
    if (w == 0 || h == 0 || st < w * 4 || sz < (uint64_t)st * h)
        return r;                                   /* inconsistent geometry */
    if (w > 8192 || h > 8192)
        return r;

    r.kind   = SV_LINE_FRAME;
    r.id     = id;
    r.width  = (uint32_t)w;
    r.height = (uint32_t)h;
    r.stride = (uint32_t)st;
    r.size   = (size_t)sz;
    return r;
}

/* ------------------------------------------------------ the import cache --- */

#define SV_CACHE_MAX 8     /* the daemon's pool is at most this deep */

typedef struct {
    bool     used;
    dev_t    dev;
    ino_t    ino;
    int      fd;           /* our own descriptor to the buffer, kept open */
    uint32_t width, height, stride;
    void    *import;       /* whatever the glue attached: EGL image + GL name */
    uint64_t hits;
} sv_cache_entry;

typedef struct {
    sv_cache_entry e[SV_CACHE_MAX];
    uint64_t imports, hits, evictions;
} sv_cache;

static inline void sv_cache_init(sv_cache *c) { memset(c, 0, sizeof *c); }

static inline sv_cache_entry *sv_cache_find(sv_cache *c, dev_t dev, ino_t ino)
{
    for (int i = 0; i < SV_CACHE_MAX; i++) {
        if (c->e[i].used && c->e[i].dev == dev && c->e[i].ino == ino) {
            c->e[i].hits++;
            c->hits++;
            return &c->e[i];
        }
    }
    return NULL;
}

/* Claim a slot for a buffer not yet cached. Returns NULL when full, which
 * means the daemon's pool is deeper than SV_CACHE_MAX -- a configuration
 * error worth a log line, not a crash. The caller fills in `import`. */
static inline sv_cache_entry *sv_cache_insert(sv_cache *c, dev_t dev, ino_t ino, int fd,
                                              uint32_t w, uint32_t h, uint32_t stride)
{
    for (int i = 0; i < SV_CACHE_MAX; i++) {
        if (c->e[i].used)
            continue;
        c->e[i].used   = true;
        c->e[i].dev    = dev;
        c->e[i].ino    = ino;
        c->e[i].fd     = fd;
        c->e[i].width  = w;
        c->e[i].height = h;
        c->e[i].stride = stride;
        c->e[i].import = NULL;
        c->e[i].hits   = 0;
        c->imports++;
        return &c->e[i];
    }
    return NULL;
}

/* Drop everything, calling `release` on each import so the glue can destroy
 * its EGL image and GL texture. The descriptors are closed here. Used on
 * video_reset (the memory behind old entries is the wrong size now) and on
 * teardown. */
static inline void sv_cache_clear(sv_cache *c, void (*release)(void *import, void *user), void *user)
{
    for (int i = 0; i < SV_CACHE_MAX; i++) {
        if (!c->e[i].used)
            continue;
        if (c->e[i].import && release)
            release(c->e[i].import, user);
        if (c->e[i].fd >= 0)
            close(c->e[i].fd);
        c->e[i].used   = false;
        c->e[i].import = NULL;
        c->e[i].fd     = -1;
        c->evictions++;
    }
}

static inline unsigned sv_cache_count(const sv_cache *c)
{
    unsigned n = 0;
    for (int i = 0; i < SV_CACHE_MAX; i++)
        if (c->e[i].used) n++;
    return n;
}

#endif /* SYNCN_VIDEO_PROTO_H */
