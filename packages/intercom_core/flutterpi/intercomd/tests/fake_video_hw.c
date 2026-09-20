/*
 * Stand-ins for the decoder and the colour converter, linked in place of the
 * real ones so `src/video/video.c` can be tested on a machine with no Rockchip
 * hardware — which is every machine except the panel.
 *
 * Nothing in the production build links this. There is no seam in the shipping
 * code and no flag to get wrong: the test target simply lists these files
 * instead of `decoder_mpp.c` and `rga.c`. The pipeline under test is the same
 * source, byte for byte, that runs on the panel.
 *
 * These fakes are deliberately strict about the one contract that matters —
 * every frame handed out must come back, and it must come back before the
 * pool runs dry. They abort the test rather than tolerate a violation, because
 * tolerating it is exactly what produces a frozen picture with working sound.
 */
#include "syncn/decoder.h"
#include "syncn/rga.h"

#include <stdlib.h>
#include <string.h>

#include "fake_video_hw.h"

/* ------------------------------------------------------------- decoder --- */

struct syncn_decoder {
    syncn_decoder_config cfg;
    syncn_decoder_stats  st;

    /* Frames waiting to be taken, in order. */
    fake_frame queue[FAKE_MAX_QUEUE];
    unsigned   head, tail;

    bool held[FAKE_MAX_QUEUE];   /* indexed by slot */
};

fake_hw_counters fake_hw;

bool        syncn_decoder_available(void) { return true; }
const char *syncn_decoder_backend(void)   { return "fake"; }

syncn_decoder *syncn_decoder_create(const syncn_decoder_config *cfg)
{
    if (fake_hw.decoder_create_fails)
        return NULL;
    syncn_decoder *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    if (cfg)
        d->cfg = *cfg;
    if (d->cfg.max_inflight <= 0)
        d->cfg.max_inflight = 4;
    return d;
}

void syncn_decoder_destroy(syncn_decoder *d)
{
    if (!d)
        return;
    /* Teardown releases what it still holds, exactly as MPP's does. */
    d->st.inflight = 0;
    free(d);
}

bool syncn_decoder_submit(syncn_decoder *d, const uint8_t *au, size_t len,
                          uint64_t pts_us)
{
    d->st.submitted++;

    if (fake_hw.submit_fails) {
        d->st.submit_failed++;
        return false;
    }

    /*
     * A picture comes out only when the access unit contains a slice. Real MPP
     * absorbs a parameter-set-only submission and emits nothing, and the
     * pipeline re-sends SPS and PPS after every reset — so a fake that
     * produced a frame for those would hide a genuine double-count and invent
     * a picture the panel would never show.
     */
    bool has_slice = false;
    for (size_t i = 0; i + 4 < len; i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            const uint8_t type = au[i + 4] & 0x1f;
            if (type == 1 || type == 5) {   /* non-IDR slice, IDR slice */
                has_slice = true;
                break;
            }
        }
    }
    if (!has_slice)
        return true;      /* accepted, but nothing to show for it */

    const unsigned next = (d->tail + 1) % FAKE_MAX_QUEUE;
    if (next == d->head) {
        d->st.submit_failed++;
        return false;
    }

    fake_frame *f = &d->queue[d->tail];
    f->width   = fake_hw.width;
    f->height  = fake_hw.height;
    f->hstride = fake_hw.width;          /* padded on real hardware; the pipeline
                                          * must pass whatever it is told */
    f->vstride = fake_hw.height + 16;
    f->pts_us  = pts_us;
    f->bytes   = len;
    d->tail = next;
    return true;
}

bool syncn_decoder_next(syncn_decoder *d, syncn_decoded_frame *out)
{
    if (d->head == d->tail)
        return false;

    if ((int)d->st.inflight >= d->cfg.max_inflight) {
        /* The real decoder refuses here rather than blocking. So does this
         * one, and the test asserts the pipeline copes. */
        d->st.starved++;
        return false;
    }

    /* Find a free slot to describe this frame; slots are what the caller
     * gives back. */
    int slot = -1;
    for (unsigned i = 0; i < FAKE_MAX_QUEUE; i++) {
        if (!d->held[i]) { slot = (int)i; break; }
    }
    if (slot < 0) {
        d->st.starved++;
        return false;
    }

    const fake_frame *f = &d->queue[d->head];
    d->head = (d->head + 1) % FAKE_MAX_QUEUE;

    memset(out, 0, sizeof *out);
    out->dmabuf_fd = 4242 + slot;   /* never dereferenced; the fake RGA ignores it */
    out->width     = f->width;
    out->height    = f->height;
    out->hstride   = f->hstride;
    out->vstride   = f->vstride;
    out->fourcc    = 0x3231564e;    /* NV12 */
    out->pts_us    = f->pts_us;
    out->slot      = slot;

    d->held[slot] = true;
    d->st.inflight++;
    d->st.frames_out++;
    d->st.width  = f->width;
    d->st.height = f->height;

    if (d->st.inflight > fake_hw.peak_inflight)
        fake_hw.peak_inflight = d->st.inflight;
    return true;
}

void syncn_decoder_release(syncn_decoder *d, const syncn_decoded_frame *frame)
{
    if (!d || !frame)
        return;
    if (frame->slot < 0 || frame->slot >= (int)FAKE_MAX_QUEUE)
        abort();
    if (!d->held[frame->slot]) {
        /* Released twice. On real hardware this corrupts MPP's refcounts and
         * the failure surfaces somewhere else entirely, so fail loudly here. */
        fake_hw.double_release++;
        abort();
    }
    d->held[frame->slot] = false;
    d->st.inflight--;
    d->st.released++;
}

void syncn_decoder_reset(syncn_decoder *d)
{
    if (!d)
        return;
    d->head = d->tail = 0;
    fake_hw.resets++;
}

void syncn_decoder_get_stats(const syncn_decoder *d, syncn_decoder_stats *out)
{
    memset(out, 0, sizeof *out);
    if (d)
        *out = d->st;
}

/* ----------------------------------------------------------------- rga --- */

struct syncn_rga { int unused; };

bool        syncn_rga_available(void) { return true; }
const char *syncn_rga_backend(void)   { return "fake"; }

syncn_rga *syncn_rga_create(void)
{
    if (fake_hw.rga_create_fails)
        return NULL;
    static syncn_rga r;
    return &r;
}

void syncn_rga_destroy(syncn_rga *r) { (void)r; }

bool syncn_rga_nv12_to_bgra(syncn_rga *r, const syncn_rga_image *src,
                            const syncn_rga_image *dst)
{
    (void)r;
    fake_hw.conversions++;
    fake_hw.last_src = *src;
    fake_hw.last_dst = *dst;
    return !fake_hw.convert_fails;
}

void syncn_rga_get_stats(const syncn_rga *r, syncn_rga_stats *out)
{
    (void)r;
    memset(out, 0, sizeof *out);
    out->converted = fake_hw.conversions;
}
