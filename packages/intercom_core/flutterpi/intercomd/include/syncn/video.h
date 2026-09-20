/*
 * The video path, end to end.
 *
 * Everything this module needs already existed in pieces and was connected to
 * nothing: the daemon received the door's H.264 and wrote it to a file. This
 * is the wiring, and it is where the freeze either happens or does not.
 *
 *     door 0xBB payload
 *       -> h264.c       split into access units, hold until a keyframe
 *       -> decoder.c    MPP, bounded in flight, never blocking
 *       -> rga.c        NV12 to BGRA, synchronous
 *       -> framepool.c  into a buffer the interface can import
 *       -> the sink     a descriptor over the control socket
 *
 * Two rules run through all of it, and they are the same rule:
 *
 *   Nothing here ever waits for the consumer. Not the decoder, which refuses
 *   rather than blocks when frames are outstanding; not the pool, which drops
 *   rather than blocks when buffers are outstanding. A frame that cannot be
 *   delivered is dropped and counted. The reported fault on this panel is a
 *   frozen picture with working audio, which is precisely the signature of a
 *   pipeline that waited for something that was never coming.
 *
 *   A decoder frame is released the instant its pixels have been copied out.
 *   RGA is synchronous for exactly this reason. Holding MPP's frames is how
 *   MPP's pool drains, and a drained pool is a permanent freeze rather than a
 *   dropped frame.
 *
 * Single-threaded: feed and pump are both called from the call loop, so there
 * is no locking here and no lock to get wrong. Audio has its own thread
 * because it has a hard deadline; video does not.
 */
#ifndef SYNCN_VIDEO_H
#define SYNCN_VIDEO_H

#include "syncn/framepool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A converted frame is ready.
 *
 * The pipeline owns `buf`; the sink must not close its descriptor, and must
 * not use it after returning. Passing it on means duplicating it, which is
 * what SCM_RIGHTS does.
 *
 * Return true if the consumer has taken it and will return it by id later.
 * Return false if it could not be delivered — no interface connected, socket
 * full — and the buffer goes straight back to the pool rather than being lost
 * to a consumer that was never told about it.
 */
typedef bool (*syncn_video_sink)(const syncn_frame_buffer *buf,
                                 uint64_t pts_us, void *user);

/*
 * The geometry changed, or the pipeline restarted.
 *
 * Any framebuffer the consumer imported for an earlier buffer id is now for
 * memory of the wrong size. Buffer ids are never reused, so an unheeded reset
 * cannot cause the wrong memory to be displayed — it only leaks the
 * consumer's cache. Still worth sending.
 */
typedef void (*syncn_video_reset_cb)(unsigned width, unsigned height, void *user);

typedef struct {
    unsigned buffers;      /* 0 -> SYNCN_FRAMEPOOL_DEFAULT */
    bool     fast_output;  /* decoder: lower latency, slight reorder cost */
} syncn_video_config;

typedef struct syncn_video syncn_video;

/*
 * Returns NULL when this build or this machine cannot decode or convert. That
 * is a startup-time answer, so the daemon reports "video cannot be displayed"
 * once rather than once per frame for the length of a call.
 */
syncn_video *syncn_video_create(const syncn_video_config *cfg,
                                syncn_video_sink sink,
                                syncn_video_reset_cb on_reset,
                                void *user);
void         syncn_video_destroy(syncn_video *v);

/* Whether a complete path exists: decoder, converter and a heap to allocate
 * from. Checked before creating, so the reason can be named. */
bool        syncn_video_available(void);
const char *syncn_video_unavailable_reason(void);

/* One 0xBB payload from the door, exactly as it came off the wire. */
void syncn_video_feed(syncn_video *v, const uint8_t *payload, size_t len);

/*
 * Convert and publish whatever the decoder has ready. Never blocks.
 *
 * Call it once per pass of the call loop. Returns how many frames were
 * published, which is 0 most passes and is not a problem.
 */
unsigned syncn_video_pump(syncn_video *v);

/* The consumer has finished with a buffer and it may be written again. */
bool syncn_video_recycle(syncn_video *v, uint64_t id);

/* The consumer is gone. Reclaims every outstanding buffer; safe because the
 * consumer holds its own descriptors to the memory. */
void syncn_video_consumer_gone(syncn_video *v);

/* Drop decoder and parser state, e.g. between calls. The next keyframe starts
 * a clean stream. */
void syncn_video_reset(syncn_video *v);

typedef struct {
    uint64_t au_in;              /* access units from the parser        */
    uint64_t submitted;
    uint64_t decoded;
    uint64_t converted;
    uint64_t published;
    uint64_t dropped_no_buffer;  /* consumer holds every buffer         */
    uint64_t dropped_convert;    /* RGA refused or failed               */
    uint64_t dropped_sink;       /* nobody to deliver to                */
    uint32_t width, height;
    unsigned outstanding;
    bool     waiting_for_keyframe;
} syncn_video_stats;

void syncn_video_get_stats(const syncn_video *v, syncn_video_stats *out);

/* One line for the periodic log, so a stalled path is legible in journalctl
 * without a debugger. Always NUL-terminated. */
void syncn_video_format_stats(const syncn_video *v, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_VIDEO_H */
