#include "syncn/dmabuf.h"
#include "syncn/util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Tried in order. System first: the display controller and decoder are both
 * behind IOMMUs on this SoC, so scattered pages scan out fine and the system
 * heap does not consume the contiguous reserve. CMA is the fallback for
 * hardware that genuinely needs contiguous memory.
 */
static const char *const HEAPS[] = {
    "/dev/dma_heap/system",
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/cma",
    "/dev/dma_heap/reserved",
};

static const char *g_heap_name = NULL;

static int open_heap(void)
{
    for (size_t i = 0; i < sizeof HEAPS / sizeof HEAPS[0]; i++) {
        const int fd = open(HEAPS[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            g_heap_name = HEAPS[i];
            return fd;
        }
    }
    return -1;
}

bool syncn_dmabuf_available(void)
{
    const int fd = open_heap();
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

const char *syncn_dmabuf_heap_name(void)
{
    if (!g_heap_name)
        (void)syncn_dmabuf_available();
    return g_heap_name ? g_heap_name : "none";
}

bool syncn_dmabuf_alloc(syncn_dmabuf *out, size_t size)
{
    out->fd   = -1;
    out->size = 0;
    out->map  = NULL;

    const int heap = open_heap();
    if (heap < 0) {
        LOG_ERR("dmabuf: no DMA heap under /dev/dma_heap. Video cannot be "
                "displayed; the kernel needs CONFIG_DMABUF_HEAPS.");
        return false;
    }

    struct dma_heap_allocation_data req;
    memset(&req, 0, sizeof req);
    req.len      = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;

    const int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req);
    close(heap);

    if (rc < 0) {
        LOG_ERR("dmabuf: allocating %zu bytes from %s: %s",
                size, g_heap_name, strerror(errno));
        return false;
    }

    out->fd   = (int)req.fd;
    out->size = size;
    return true;
}

void syncn_dmabuf_free(syncn_dmabuf *b)
{
    if (!b)
        return;
    syncn_dmabuf_unmap(b);
    if (b->fd >= 0)
        close(b->fd);
    b->fd   = -1;
    b->size = 0;
}

bool syncn_dmabuf_map(syncn_dmabuf *b)
{
    if (!b || b->fd < 0)
        return false;
    if (b->map)
        return true;

    void *m = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (m == MAP_FAILED) {
        LOG_WARN("dmabuf: mmap failed: %s", strerror(errno));
        return false;
    }
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
    /*
     * 64-byte stride alignment. Display controllers and 2D engines both prefer
     * it, and an unaligned stride is the kind of thing that works on the bench
     * and tears on a different panel.
     */
    unsigned stride = width * 4;
    stride = (stride + 63u) & ~63u;

    if (stride_out)
        *stride_out = stride;
    return (size_t)stride * height;
}
