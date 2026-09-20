/*
 * Hardware colour conversion, NV12 to RGB.
 *
 * Needed because of what the panel's display controller actually offers. The
 * diagnostic found the screen driven by CRTC 121 with two planes attached:
 *
 *   Smart1   (plane 93)   RGB only
 *   Cluster1 (plane 107)  AFBC compressed only
 *
 * The planes that accept the decoder's NV12 — Esmart0 and Esmart1 — hang off
 * video ports that drive no display. Reaching them means editing the device
 * tree on a board support package we do not control.
 *
 * So the NV12 has to become RGB somewhere. RGA is a fixed-function 2D block,
 * separate from the GPU, which means the video path still never touches the
 * 2020 Mali blob — the reason for choosing an overlay approach in the first
 * place. It costs one hardware copy per frame, which at 720p20 is nothing on
 * an RK3568, and buys a path that needs neither the GPU nor a device tree
 * change.
 *
 * Both sides are DMA-BUFs, so nothing is copied through the CPU.
 */
#ifndef SYNCN_RGA_H
#define SYNCN_RGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct syncn_rga syncn_rga;

/* Returns NULL when RGA is unavailable; the caller can fall back or report. */
syncn_rga *syncn_rga_create(void);
void       syncn_rga_destroy(syncn_rga *r);

bool        syncn_rga_available(void);
const char *syncn_rga_backend(void);

typedef struct {
    int      fd;                /* DMA-BUF                                  */
    uint32_t width, height;
    uint32_t hstride, vstride;  /* padded; the decoder's strides exceed w/h */
} syncn_rga_image;

/*
 * Convert NV12 to BGRA8888 (little-endian, which is DRM's XR24/AR24 layout).
 *
 * Synchronous: it returns when the destination is complete, so the caller may
 * release the source frame immediately afterwards. That matters more than the
 * few hundred microseconds an async path would save — holding decoder frames
 * longer than necessary is precisely what drains MPP's pool.
 */
bool syncn_rga_nv12_to_bgra(syncn_rga *r, const syncn_rga_image *src,
                            const syncn_rga_image *dst);

typedef struct {
    uint64_t converted;
    uint64_t failed;
    uint64_t last_us;    /* how long the most recent conversion took */
} syncn_rga_stats;

void syncn_rga_get_stats(const syncn_rga *r, syncn_rga_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_RGA_H */
