/*
 * DMA-BUF allocation for the converted video frame.
 *
 * The daemon cannot put pixels on the screen itself. flutter-pi holds DRM
 * master, and a DRM device has exactly one. So the daemon decodes, converts,
 * and hands the result to the UI process as a file descriptor; the UI, being
 * master, creates the framebuffer and commits the plane.
 *
 * That constrains the allocator. DRM dumb buffers need master, and render nodes
 * cannot do KMS-scanoutable memory. DMA heaps can: any process may allocate
 * from the DMA heap devices, the result is a real DMA-BUF, and it can be imported by
 * whoever does hold master.
 *
 * Which heap matters. On this SoC the display controller and the decoder sit
 * behind IOMMUs, so ordinary system memory is scanoutable and the system heap
 * is the right default; the CMA heap exists for hardware that needs physically
 * contiguous pages and is tried as a fallback.
 */
#ifndef SYNCN_DMABUF_H
#define SYNCN_DMABUF_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    fd;     /* -1 when unallocated */
    size_t size;
    void  *map;    /* NULL unless mapped  */
} syncn_dmabuf;

/* True when a usable DMA heap exists. Worth checking once at startup so the
 * daemon can say video will not display, rather than failing per frame. */
bool        syncn_dmabuf_available(void);
const char *syncn_dmabuf_heap_name(void);

bool syncn_dmabuf_alloc(syncn_dmabuf *out, size_t size);
void syncn_dmabuf_free(syncn_dmabuf *b);

/* CPU access, for tests and diagnostics. The video path never maps. */
bool syncn_dmabuf_map(syncn_dmabuf *b);
void syncn_dmabuf_unmap(syncn_dmabuf *b);

/* Bytes needed for a BGRA8888 frame of this geometry. */
size_t syncn_dmabuf_bgra_size(unsigned width, unsigned height, unsigned *stride_out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_DMABUF_H */
