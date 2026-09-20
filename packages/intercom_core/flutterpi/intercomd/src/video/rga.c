#include "syncn/rga.h"
#include "syncn/util.h"

#include <stdlib.h>
#include <string.h>

#ifdef SYNCN_HAVE_RGA
#include <rga/im2d.h>
#include <rga/rga.h>

struct syncn_rga {
    uint64_t converted, failed, last_us;
    bool     warned;   /* one failure message per episode, not per frame */
};

bool syncn_rga_available(void) { return true; }
const char *syncn_rga_backend(void) { return "rockchip-rga"; }

syncn_rga *syncn_rga_create(void)
{
    syncn_rga *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    LOG_INFO("rga: hardware colour conversion available (%s)", querystring(RGA_VERSION));
    return r;
}

void syncn_rga_destroy(syncn_rga *r) { free(r); }

bool syncn_rga_nv12_to_bgra(syncn_rga *r, const syncn_rga_image *src,
                            const syncn_rga_image *dst)
{
    const uint64_t t0 = syncn_now_ms();

    /*
     * Strides, not dimensions. The decoder pads its output — 1280x720 typically
     * arrives with a 1280x736 or wider allocation — and handing RGA the visible
     * size while the buffer is padded shears the image. This is the single
     * easiest thing to get wrong here, and it produces a picture that looks
     * almost right, which is worse than one that looks broken.
     */
    rga_buffer_t s = wrapbuffer_fd_t((int)src->fd, (int)src->width, (int)src->height,
                                     (int)src->hstride, (int)src->vstride,
                                     RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t d = wrapbuffer_fd_t((int)dst->fd, (int)dst->width, (int)dst->height,
                                     (int)dst->hstride, (int)dst->vstride,
                                     RK_FORMAT_BGRA_8888);

    im_rect none;
    memset(&none, 0, sizeof none);

    const IM_STATUS check = imcheck_t(s, d, (rga_buffer_t){0}, none, none, none, 0);
    if (check != IM_STATUS_NOERROR) {
        r->failed++;
        if (!r->warned) {
            r->warned = true;
            LOG_ERR("rga: refuses this conversion: %s "
                    "(src %ux%u stride %ux%u -> dst %ux%u stride %ux%u)",
                    imStrError_t(check),
                    src->width, src->height, src->hstride, src->vstride,
                    dst->width, dst->height, dst->hstride, dst->vstride);
        }
        return false;
    }

    /* sync = 1: return when the destination is complete, so the caller can
     * release the decoder frame straight away. */
    const IM_STATUS ret = imcvtcolor_t(s, d, RK_FORMAT_YCbCr_420_SP,
                                       RK_FORMAT_BGRA_8888,
                                       IM_COLOR_SPACE_DEFAULT, 1);
    if (ret != IM_STATUS_SUCCESS) {
        r->failed++;
        if (!r->warned) {
            r->warned = true;
            LOG_ERR("rga: conversion failed: %s", imStrError_t(ret));
        }
        return false;
    }

    r->warned = false;
    r->converted++;
    r->last_us = (syncn_now_ms() - t0) * 1000;
    return true;
}

void syncn_rga_get_stats(const syncn_rga *r, syncn_rga_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!r)
        return;
    out->converted = r->converted;
    out->failed    = r->failed;
    out->last_us   = r->last_us;
}

#else /* !SYNCN_HAVE_RGA */

bool syncn_rga_available(void) { return false; }
const char *syncn_rga_backend(void) { return "none"; }

syncn_rga *syncn_rga_create(void)
{
    LOG_WARN("rga: built without librga — decoded video cannot be converted "
             "for the display plane");
    return NULL;
}

void syncn_rga_destroy(syncn_rga *r) { (void)r; }

bool syncn_rga_nv12_to_bgra(syncn_rga *r, const syncn_rga_image *src,
                            const syncn_rga_image *dst)
{ (void)r; (void)src; (void)dst; return false; }

void syncn_rga_get_stats(const syncn_rga *r, syncn_rga_stats *out)
{ (void)r; memset(out, 0, sizeof *out); }

#endif /* SYNCN_HAVE_RGA */
