/*
 * memfd standing in for a DMA heap, linked into the video-pipeline test binary
 * in place of `src/video/dmabuf.c`.
 *
 * The heap itself is four lines of ioctl and is exercised by the panel runbook.
 * What these descriptors are for here is to let the real framepool and the real
 * pipeline run on a build machine: they are genuine descriptors to genuine
 * distinct memory, which is all either component can observe about them.
 */
#include "syncn/dmabuf.h"

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int fake_dmabuf_live = 0;   /* allocated minus freed; the test asserts it lands at 0 */

bool        syncn_dmabuf_available(void) { return true; }
const char *syncn_dmabuf_heap_name(void) { return "memfd (test)"; }

bool syncn_dmabuf_alloc(syncn_dmabuf *out, size_t size)
{
    out->fd = -1; out->size = 0; out->map = NULL;

    const int fd = memfd_create("syncn-video-test", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return false;
    }
    out->fd   = fd;
    out->size = size;
    fake_dmabuf_live++;
    return true;
}

void syncn_dmabuf_free(syncn_dmabuf *b)
{
    if (!b)
        return;
    syncn_dmabuf_unmap(b);
    if (b->fd >= 0) {
        close(b->fd);
        fake_dmabuf_live--;
    }
    b->fd = -1;
    b->size = 0;
}

bool syncn_dmabuf_map(syncn_dmabuf *b)
{
    if (!b || b->fd < 0)
        return false;
    if (b->map)
        return true;
    void *m = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (m == MAP_FAILED)
        return false;
    b->map = m;
    return true;
}

void syncn_dmabuf_unmap(syncn_dmabuf *b)
{
    if (b && b->map) {
        munmap(b->map, b->size);
        b->map = NULL;
    }
}

size_t syncn_dmabuf_bgra_size(unsigned width, unsigned height, unsigned *stride_out)
{
    unsigned stride = width * 4;
    stride = (stride + 63u) & ~63u;
    if (stride_out)
        *stride_out = stride;
    return (size_t)stride * height;
}
