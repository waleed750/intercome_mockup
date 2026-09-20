/*
 * The pool of buffers shared with the user interface.
 *
 * The freeze this project was commissioned to fix has the shape of a consumer
 * that stops returning buffers. These tests are the argument that the same bug
 * cannot happen across the process boundary: a consumer which holds everything
 * causes dropped frames and a log line, never a block, and no confusion on the
 * wire can put one buffer in two places at once.
 *
 * They run against memfd rather than a DMA heap, so they run on any machine
 * and not only on the panel. What is under test is the bookkeeping, which is
 * where all of those rules live; memfd gives real descriptors to real distinct
 * memory, which is everything the bookkeeping can observe. The heap itself is
 * four lines of ioctl covered by the panel runbook.
 */
#include "test.h"

#include "syncn/framepool.h"
#include "syncn/dmabuf.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_allocs = 0;
static int g_frees  = 0;

/* Fails every allocation after the first `fail_after`, so the partial-pool
 * path is exercised too. -1 means never fail. */
static int g_fail_after = -1;

static bool memfd_alloc(syncn_dmabuf *out, size_t size, void *user)
{
    (void)user;
    out->fd = -1; out->size = 0; out->map = NULL;

    if (g_fail_after >= 0 && g_allocs >= g_fail_after)
        return false;

    const int fd = memfd_create("syncn-test-frame", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return false;
    }
    out->fd   = fd;
    out->size = size;
    g_allocs++;
    return true;
}

static void memfd_free(syncn_dmabuf *b, void *user)
{
    (void)user;
    if (b && b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
        g_frees++;
    }
}

static const syncn_framepool_allocator TEST_ALLOC = {
    .alloc = memfd_alloc,
    .free  = memfd_free,
    .name  = "memfd (test)",
    .user  = NULL,
};

static syncn_framepool *pool(unsigned count, unsigned w, unsigned h)
{
    return syncn_framepool_create_with(&TEST_ALLOC, count, w, h);
}

/* Small and odd on purpose: a 4-byte-per-pixel row of 100 px is 400 bytes,
 * which is not a multiple of 64, so the stride must be padded. */
#define W 100
#define H 64

void suite_framepool(void)
{
    SUITE("framepool");

    g_allocs = g_frees = 0;
    g_fail_after = -1;

    SUITE("framepool: a pool hands out distinct buffers and takes them back");
    {
        syncn_framepool *p = pool(3, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        CHECK_EQ_INT(syncn_framepool_count(p), 3);
        CHECK_EQ_INT(syncn_framepool_outstanding(p), 0);

        const syncn_frame_buffer *a = syncn_framepool_acquire(p);
        const syncn_frame_buffer *b = syncn_framepool_acquire(p);
        CHECK(a && b);
        if (a && b) {
            /* Distinct memory, distinct identity. Two frames converted at once
             * must never land in the same buffer. */
            CHECK(a->fd != b->fd);
            CHECK(a->id != b->id);
            CHECK(a->id != 0 && b->id != 0);

            /* Stride padded past the row length, not equal to it. */
            CHECK(a->stride >= W * 4);
            CHECK_EQ_INT(a->stride % 64, 0);
            CHECK(a->size >= (size_t)a->stride * H);
            CHECK_EQ_INT(a->width, W);
            CHECK_EQ_INT(a->height, H);
        }
        CHECK_EQ_INT(syncn_framepool_outstanding(p), 2);

        CHECK(syncn_framepool_release(p, a->id));
        CHECK_EQ_INT(syncn_framepool_outstanding(p), 1);

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: a consumer holding everything drops frames, never blocks");
    {
        syncn_framepool *p = pool(2, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        CHECK(syncn_framepool_acquire(p) != NULL);
        CHECK(syncn_framepool_acquire(p) != NULL);

        /* The whole point. This call returns — it does not wait for a buffer
         * that a wedged interface is never going to return. */
        CHECK(syncn_framepool_acquire(p) == NULL);
        CHECK(syncn_framepool_acquire(p) == NULL);

        syncn_framepool_stats st;
        syncn_framepool_get_stats(p, &st);
        CHECK_EQ_INT(st.dropped_no_buffer, 2);
        CHECK_EQ_INT(st.acquired, 2);
        CHECK_EQ_INT(st.outstanding, 2);

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: ids are never reused, so a cached import cannot go stale");
    {
        syncn_framepool *p = pool(1, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        uint64_t seen[8];
        for (int i = 0; i < 8; i++) {
            const syncn_frame_buffer *f = syncn_framepool_acquire(p);
            CHECK(f != NULL);
            if (!f)
                break;
            seen[i] = f->id;
            CHECK(syncn_framepool_release(p, f->id));
        }

        /* Same slot, same fd, eight times — and eight different ids. The
         * interface may cache a framebuffer per id forever and still be
         * correct. */
        for (int i = 1; i < 8; i++)
            CHECK(seen[i] > seen[i - 1]);

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: a bad release is refused rather than freeing a live slot");
    {
        syncn_framepool *p = pool(2, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        const syncn_frame_buffer *a = syncn_framepool_acquire(p);
        CHECK(a != NULL);
        const uint64_t id = a->id;

        CHECK(syncn_framepool_release(p, id));
        /* Released twice: the second must not free a slot that has since been
         * handed to someone else. */
        CHECK(!syncn_framepool_release(p, id));
        /* Never issued. */
        CHECK(!syncn_framepool_release(p, 999999));
        /* Zero is the "no id" sentinel and is never valid. */
        CHECK(!syncn_framepool_release(p, 0));

        syncn_framepool_stats st;
        syncn_framepool_get_stats(p, &st);
        CHECK_EQ_INT(st.bad_release, 3);
        CHECK_EQ_INT(st.released, 1);
        CHECK_EQ_INT(st.outstanding, 0);

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: a disconnected interface gives everything back at once");
    {
        syncn_framepool *p = pool(3, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        uint64_t before = 0;
        for (int i = 0; i < 3; i++) {
            const syncn_frame_buffer *f = syncn_framepool_acquire(p);
            CHECK(f != NULL);
            if (f)
                before = f->id;
        }
        CHECK(syncn_framepool_acquire(p) == NULL);

        syncn_framepool_release_all(p);
        CHECK_EQ_INT(syncn_framepool_outstanding(p), 0);

        /* And the pool is immediately usable again, which is what lets a
         * restarted UI get a picture without restarting the daemon. */
        const syncn_frame_buffer *f = syncn_framepool_acquire(p);
        CHECK(f != NULL);
        /* Still a fresh id, even though the slot was reclaimed rather than
         * returned: the old UI may have cached the previous one. */
        if (f)
            CHECK(f->id > before);

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: geometry is checked, so a resize cannot be ignored");
    {
        syncn_framepool *p = pool(1, W, H);
        CHECK(p != NULL);
        if (!p)
            return;

        CHECK(syncn_framepool_matches(p, W, H));
        CHECK(!syncn_framepool_matches(p, W, H + 1));
        CHECK(!syncn_framepool_matches(p, 1280, 720));

        syncn_framepool_destroy(p);
    }

    SUITE("framepool: a degenerate geometry is refused, not allocated");
    {
        CHECK(pool(3, 0, H) == NULL);
        CHECK(pool(3, W, 0) == NULL);
    }

    SUITE("framepool: destroying a pool the interface still holds is safe");
    {
        syncn_framepool *p = pool(2, W, H);
        CHECK(p != NULL);
        if (!p)
            return;
        CHECK(syncn_framepool_acquire(p) != NULL);
        /* Under ASan. The consumer's descriptors keep the memory alive; ours
         * going away must not touch anything it is reading. */
        syncn_framepool_destroy(p);
        CHECK(true);
    }

    SUITE("framepool: a partly-satisfied heap gives a smaller pool, not none");
    {
        /* Two allocations succeed, the third fails. A panel with a tight heap
         * should show video with fewer buffers rather than show none — and the
         * reduced count has to be visible, or the dropped frames later look
         * inexplicable. */
        g_allocs = g_frees = 0;
        g_fail_after = 2;

        syncn_framepool *p = pool(3, W, H);
        CHECK(p != NULL);
        if (p) {
            CHECK_EQ_INT(syncn_framepool_count(p), 2);
            CHECK(syncn_framepool_acquire(p) != NULL);
            CHECK(syncn_framepool_acquire(p) != NULL);
            CHECK(syncn_framepool_acquire(p) == NULL);
            syncn_framepool_destroy(p);
        }

        /* Every descriptor the pool took is given back. */
        CHECK_EQ_INT(g_frees, g_allocs);

        /* And a heap that can satisfy nothing at all yields no pool, rather
         * than an empty one that would divide by zero later. */
        g_allocs = g_frees = 0;
        g_fail_after = 0;
        CHECK(pool(3, W, H) == NULL);

        g_fail_after = -1;
    }

    SUITE("framepool: a count beyond the maximum is clamped, not trusted");
    {
        g_allocs = g_frees = 0;
        syncn_framepool *p = pool(SYNCN_FRAMEPOOL_MAX + 5, W, H);
        CHECK(p != NULL);
        if (p) {
            CHECK_EQ_INT(syncn_framepool_count(p), SYNCN_FRAMEPOOL_MAX);
            syncn_framepool_destroy(p);
        }

        /* Zero means "use the default", not "allocate nothing". */
        p = pool(0, W, H);
        CHECK(p != NULL);
        if (p) {
            CHECK_EQ_INT(syncn_framepool_count(p), SYNCN_FRAMEPOOL_DEFAULT);
            syncn_framepool_destroy(p);
        }
        CHECK_EQ_INT(g_frees, g_allocs);
    }

    SUITE("framepool: no descriptors are leaked over a full cycle");
    {
        g_allocs = g_frees = 0;
        for (int round = 0; round < 5; round++) {
            syncn_framepool *p = pool(3, W, H);
            CHECK(p != NULL);
            if (!p)
                break;
            for (int i = 0; i < 10; i++) {
                const syncn_frame_buffer *f = syncn_framepool_acquire(p);
                if (f)
                    syncn_framepool_release(p, f->id);
            }
            syncn_framepool_destroy(p);
        }
        CHECK_EQ_INT(g_allocs, 15);
        CHECK_EQ_INT(g_frees, 15);
    }

    SUITE("framepool: ids do not repeat across pools, so a rebuild is safe");
    {
        /* The case this guards is a resolution change mid-call: the old pool is
         * destroyed while the interface still holds imports keyed by id. If the
         * new pool restarted at 1, the interface would draw the new id 1 from
         * its cached import of the old id 1 — memory of the wrong size, and a
         * garbled picture three components away from its cause. */
        syncn_framepool *a = pool(2, W, H);
        CHECK(a != NULL);
        if (!a)
            return;

        uint64_t from_a[2];
        for (int i = 0; i < 2; i++) {
            const syncn_frame_buffer *f = syncn_framepool_acquire(a);
            CHECK(f != NULL);
            from_a[i] = f ? f->id : 0;
        }

        /* Every id the old pool could have issued is below this. */
        const uint64_t watermark = syncn_framepool_next_id();
        CHECK(from_a[0] < watermark && from_a[1] < watermark);

        syncn_framepool_destroy(a);

        syncn_framepool *b = pool(2, W * 2, H * 2);
        CHECK(b != NULL);
        if (!b)
            return;
        CHECK(!syncn_framepool_matches(b, W, H));

        for (int i = 0; i < 2; i++) {
            const syncn_frame_buffer *f = syncn_framepool_acquire(b);
            CHECK(f != NULL);
            if (!f)
                continue;
            CHECK(f->id >= watermark);
            CHECK(f->id != from_a[0] && f->id != from_a[1]);
        }

        /* And a late release carrying an old id finds no match here, rather
         * than freeing a live slot of the new pool. */
        CHECK(!syncn_framepool_release(b, from_a[0]));
        CHECK_EQ_INT(syncn_framepool_outstanding(b), 2);

        syncn_framepool_destroy(b);
    }
}
