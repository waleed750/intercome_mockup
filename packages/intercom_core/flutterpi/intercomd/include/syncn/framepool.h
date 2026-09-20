/*
 * The pool of BGRA buffers handed to the user interface.
 *
 * This module exists because of a rule learned the hard way one layer down:
 * a fixed pool of buffers shared with a consumer that does not give them back
 * is exactly how a video path freezes while audio keeps running. MPP has such
 * a pool internally, and `decoder.h` guards it. This is the same hazard on the
 * other side of the process boundary, where it is worse — the consumer is a
 * separate program that can be slow, wedged, or killed, and the daemon cannot
 * reach into it to take a buffer back.
 *
 * So the pool is built on three positions:
 *
 *   - Acquire never blocks. When every buffer is out, the frame is DROPPED and
 *     counted. The picture holds on the last frame for 50 ms; the daemon does
 *     not stop. A dropped frame is a scratch, a blocked daemon is a dead panel.
 *   - Buffer ids never repeat, across the life of the process. The interface
 *     caches an imported framebuffer per id, and a reused id would have it
 *     scanning out the wrong memory after a resolution change. Monotonic ids
 *     make that impossible rather than merely unlikely.
 *   - A consumer that goes away releases everything at once. Descriptor
 *     passing means the interface holds its own reference to the memory; the
 *     pages survive until it closes them. So reclaiming the slot the moment
 *     the socket drops is safe, and it is what lets the next UI start clean.
 *
 * The buffers themselves are DMA heap allocations, because the daemon cannot
 * be DRM master and DRM master is what the interface is — see `dmabuf.h`.
 */
#ifndef SYNCN_FRAMEPOOL_H
#define SYNCN_FRAMEPOOL_H

#include "syncn/dmabuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Four. Three is the minimum that keeps the pipeline moving -- one on screen,
 * one in the interface's hands, one free for the converter -- and on the
 * panel three ran dry for six frames in six thousand, each time Flutter was
 * holding two across a vsync. The fourth costs one 3.6 MB buffer at 720p and
 * buys the margin. */
#define SYNCN_FRAMEPOOL_DEFAULT 4
#define SYNCN_FRAMEPOOL_MAX     8

typedef struct {
    /* Unique for the life of the process. The interface keys its imported
     * framebuffers on this and may cache them indefinitely. */
    uint64_t id;

    /* Borrowed. The pool keeps ownership; the sender passes a copy over
     * SCM_RIGHTS and does not close this one. */
    int      fd;

    uint32_t width, height;
    uint32_t stride;      /* bytes per row, >= width * 4 */
    size_t   size;
} syncn_frame_buffer;

typedef struct syncn_framepool syncn_framepool;

/*
 * Allocate `count` buffers of this geometry. Returns NULL if no DMA heap is
 * available or allocation fails — in which case video cannot be displayed and
 * the caller should say so once, not once per frame.
 *
 * count 0 uses SYNCN_FRAMEPOOL_DEFAULT and is clamped to SYNCN_FRAMEPOOL_MAX.
 */
syncn_framepool *syncn_framepool_create(unsigned count,
                                        unsigned width, unsigned height);
void             syncn_framepool_destroy(syncn_framepool *p);

/*
 * The same pool over a caller-supplied allocator.
 *
 * This exists so the bookkeeping above can be tested on a machine with no DMA
 * heap — which is every build machine that is not the panel. The rules this
 * module enforces (drop rather than block, never reuse an id, refuse a stale
 * release) are pure logic with nothing hardware-specific in them, and they are
 * the rules that decide whether the picture freezes. Leaving them provable
 * only on the panel would mean never proving them.
 *
 * The daemon always uses syncn_framepool_create(). Nothing in the shipping
 * path calls this.
 */
typedef struct {
    bool (*alloc)(syncn_dmabuf *out, size_t size, void *user);
    void (*free)(syncn_dmabuf *b, void *user);
    const char *name;
    void *user;
} syncn_framepool_allocator;

syncn_framepool *syncn_framepool_create_with(const syncn_framepool_allocator *a,
                                             unsigned count,
                                             unsigned width, unsigned height);

/*
 * Take a free buffer, or NULL when all of them are out with the consumer.
 *
 * NULL is a normal outcome under load, not an error. The caller drops the
 * frame. It is also the early warning for a wedged interface, so the pool
 * counts these and reports them through the stats below.
 */
const syncn_frame_buffer *syncn_framepool_acquire(syncn_framepool *p);

/*
 * Give a buffer back by id.
 *
 * Returns false for an id this pool never issued, or one that is already free.
 * Both mean the consumer is confused or malicious, and neither is fatal — the
 * pool refuses and counts it rather than double-freeing a slot, which would
 * hand the same memory to two writers.
 */
bool syncn_framepool_release(syncn_framepool *p, uint64_t id);

/* Reclaim every outstanding buffer. For when the interface disconnects: it
 * holds its own descriptors, so the memory stays alive as long as it needs. */
void syncn_framepool_release_all(syncn_framepool *p);

/*
 * The id the next acquire will return, from any pool in this process.
 *
 * Used when a pool is replaced for a new resolution: every id below this value
 * belonged to the old one, so releases carrying them can be ignored quietly
 * rather than reported as protocol errors. They are not errors — they are the
 * consumer answering a question that has since been withdrawn.
 */
uint64_t syncn_framepool_next_id(void);

unsigned syncn_framepool_count(const syncn_framepool *p);
unsigned syncn_framepool_outstanding(const syncn_framepool *p);
bool     syncn_framepool_matches(const syncn_framepool *p,
                                 unsigned width, unsigned height);

typedef struct {
    uint64_t acquired;
    uint64_t released;
    uint64_t dropped_no_buffer;   /* frames lost because the consumer holds all */
    uint64_t bad_release;         /* unknown or already-free id                 */
    unsigned outstanding;
    unsigned count;
} syncn_framepool_stats;

void syncn_framepool_get_stats(const syncn_framepool *p,
                              syncn_framepool_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_FRAMEPOOL_H */
