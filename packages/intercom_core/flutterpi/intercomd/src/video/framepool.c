#include "syncn/framepool.h"
#include "syncn/dmabuf.h"
#include "syncn/util.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    syncn_dmabuf       mem;
    syncn_frame_buffer pub;
    bool               out;    /* held by the consumer */
} slot;

struct syncn_framepool {
    syncn_framepool_allocator alloc;
    slot     slots[SYNCN_FRAMEPOOL_MAX];
    unsigned count;
    unsigned width, height;

    syncn_framepool_stats st;

    /* Rate-limits the starvation warning. Once the consumer wedges, every
     * frame would log, and a log that scrolls is a log nobody reads. */
    uint64_t last_warn_ms;
};

/*
 * One counter for every pool in the process, never reset.
 *
 * Per-pool counters would restart at 1 each time the pool is rebuilt for a new
 * resolution, and the interface caches an imported framebuffer per id — so id 1
 * of the new pool would be drawn from its cached import of id 1 of the old one,
 * which is memory of the wrong size. That is a garbled picture whose cause is
 * three components away from where it shows up.
 *
 * Making the counter global means an id issued by any pool is never issued
 * again by any pool, so a cached import is either current or unknown, and never
 * silently wrong. Ids start at 1, so a zeroed struct or a missing JSON field is
 * never mistaken for a valid buffer.
 *
 * Single-threaded by construction: the video path runs entirely on the call
 * loop. Nothing here needs an atomic, and a comment is cheaper than one.
 */
static uint64_t g_next_id = 1;

uint64_t syncn_framepool_next_id(void)
{
    return g_next_id;
}

static bool heap_alloc(syncn_dmabuf *out, size_t size, void *user)
{
    (void)user;
    return syncn_dmabuf_alloc(out, size);
}

static void heap_free(syncn_dmabuf *b, void *user)
{
    (void)user;
    syncn_dmabuf_free(b);
}

static const syncn_framepool_allocator DMA_HEAP = {
    .alloc = heap_alloc,
    .free  = heap_free,
    .name  = NULL,        /* filled in from the heap actually opened */
    .user  = NULL,
};

syncn_framepool *syncn_framepool_create(unsigned count,
                                        unsigned width, unsigned height)
{
    return syncn_framepool_create_with(&DMA_HEAP, count, width, height);
}

syncn_framepool *syncn_framepool_create_with(const syncn_framepool_allocator *a,
                                             unsigned count,
                                             unsigned width, unsigned height)
{
    if (!a || !a->alloc || !a->free)
        return NULL;
    if (width == 0 || height == 0) {
        LOG_ERR("framepool: refusing a %ux%u pool", width, height);
        return NULL;
    }
    if (count == 0)
        count = SYNCN_FRAMEPOOL_DEFAULT;
    if (count > SYNCN_FRAMEPOOL_MAX)
        count = SYNCN_FRAMEPOOL_MAX;

    syncn_framepool *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;

    p->alloc   = *a;
    p->width  = width;
    p->height = height;

    unsigned     stride = 0;
    const size_t size   = syncn_dmabuf_bgra_size(width, height, &stride);

    for (unsigned i = 0; i < count; i++) {
        slot *s = &p->slots[i];
        if (!p->alloc.alloc(&s->mem, size, p->alloc.user)) {
            /*
             * Partial success is still success, down to one buffer. A panel
             * that shows video with a slot to spare beats a panel that shows
             * none because the heap was tight — and the count is reported, so
             * a degraded pool is visible rather than mysterious.
             */
            LOG_WARN("framepool: only %u of %u buffers allocated (%zu bytes each)",
                     i, count, size);
            break;
        }
        s->pub.id     = 0;         /* assigned at acquire */
        s->pub.fd     = s->mem.fd;
        s->pub.width  = width;
        s->pub.height = height;
        s->pub.stride = stride;
        s->pub.size   = size;
        p->count++;
    }

    const char *from = p->alloc.name ? p->alloc.name : syncn_dmabuf_heap_name();

    if (p->count == 0) {
        LOG_ERR("framepool: no buffers could be allocated — video cannot be "
                "displayed. Source is %s.", from);
        free(p);
        return NULL;
    }

    p->st.count = p->count;
    LOG_INFO("framepool: %u buffer%s of %ux%u, stride %u, %zu bytes each, from %s",
             p->count, p->count == 1 ? "" : "s", width, height, stride, size, from);
    return p;
}

void syncn_framepool_destroy(syncn_framepool *p)
{
    if (!p)
        return;

    if (p->st.outstanding) {
        /* Not a leak: the consumer holds its own descriptors, so the memory
         * outlives us by design. Worth saying, because it is also what a
         * wedged consumer looks like at shutdown. */
        LOG_INFO("framepool: %u buffer(s) still with the interface at teardown",
                 p->st.outstanding);
    }

    for (unsigned i = 0; i < p->count; i++)
        p->alloc.free(&p->slots[i].mem, p->alloc.user);
    free(p);
}

const syncn_frame_buffer *syncn_framepool_acquire(syncn_framepool *p)
{
    if (!p)
        return NULL;

    for (unsigned i = 0; i < p->count; i++) {
        slot *s = &p->slots[i];
        if (s->out)
            continue;

        s->out    = true;
        s->pub.id = g_next_id++;
        p->st.acquired++;
        p->st.outstanding++;
        return &s->pub;
    }

    p->st.dropped_no_buffer++;

    const uint64_t now = syncn_now_ms();
    if (now - p->last_warn_ms >= 1000) {
        p->last_warn_ms = now;
        LOG_WARN("framepool: all %u buffers are with the interface — dropping "
                 "frames (%llu so far). The UI is not returning them.",
                 p->count, (unsigned long long)p->st.dropped_no_buffer);
    }
    return NULL;
}

bool syncn_framepool_release(syncn_framepool *p, uint64_t id)
{
    if (!p || id == 0) {
        if (p)
            p->st.bad_release++;
        return false;
    }

    for (unsigned i = 0; i < p->count; i++) {
        slot *s = &p->slots[i];
        if (!s->out || s->pub.id != id)
            continue;

        s->out = false;
        p->st.released++;
        p->st.outstanding--;
        return true;
    }

    /*
     * An id we never issued, or one released twice. Refuse it. Marking a slot
     * free on a stale id would hand the same memory to the converter while the
     * display is still scanning it, which tears — and tearing traced back to a
     * duplicate message is a genuinely horrible afternoon.
     */
    p->st.bad_release++;
    LOG_WARN("framepool: ignoring release of unknown or already-free buffer %llu",
             (unsigned long long)id);
    return false;
}

void syncn_framepool_release_all(syncn_framepool *p)
{
    if (!p)
        return;

    unsigned freed = 0;
    for (unsigned i = 0; i < p->count; i++) {
        if (p->slots[i].out) {
            p->slots[i].out = false;
            freed++;
        }
    }
    if (freed) {
        p->st.released      += freed;
        p->st.outstanding    = 0;
        LOG_INFO("framepool: reclaimed %u buffer(s)", freed);
    }
}

unsigned syncn_framepool_count(const syncn_framepool *p)
{
    return p ? p->count : 0;
}

unsigned syncn_framepool_outstanding(const syncn_framepool *p)
{
    return p ? p->st.outstanding : 0;
}

bool syncn_framepool_matches(const syncn_framepool *p,
                             unsigned width, unsigned height)
{
    return p && p->width == width && p->height == height;
}

void syncn_framepool_get_stats(const syncn_framepool *p,
                              syncn_framepool_stats *out)
{
    if (!p || !out)
        return;
    *out = p->st;
}
