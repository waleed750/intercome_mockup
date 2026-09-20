#include "syncn/decoder.h"
#include "syncn/util.h"

#include <stdlib.h>
#include <string.h>

#ifdef SYNCN_HAVE_MPP
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

#define DEFAULT_INFLIGHT 4

struct syncn_decoder {
    MppCtx  ctx;
    MppApi *mpi;

    /*
     * Frames handed to the consumer, still awaiting release.
     *
     * The MppFrame is kept because releasing it is what returns the underlying
     * buffer to MPP's pool. Losing track of one leaks a buffer from a fixed
     * pool, and enough of those is the permanent freeze this module exists to
     * prevent.
     */
    struct {
        MppFrame frame;
        bool     in_use;
    } inflight[SYNCN_DECODER_MAX_INFLIGHT];

    int max_inflight;
    unsigned held;

    uint64_t submitted, submit_failed, frames_out, released;
    uint64_t info_changes, errors, starved;
    uint32_t width, height;

    /* So the "consumer is not returning frames" warning is logged once per
     * episode rather than fifty times a second. */
    bool warned_starved;
};

bool syncn_decoder_available(void) { return true; }
const char *syncn_decoder_backend(void) { return "rockchip-mpp"; }

syncn_decoder *syncn_decoder_create(const syncn_decoder_config *cfg)
{
    syncn_decoder *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;

    d->max_inflight = (cfg && cfg->max_inflight > 0) ? cfg->max_inflight : DEFAULT_INFLIGHT;
    if (d->max_inflight > SYNCN_DECODER_MAX_INFLIGHT)
        d->max_inflight = SYNCN_DECODER_MAX_INFLIGHT;

    if (mpp_create(&d->ctx, &d->mpi) != MPP_OK) {
        LOG_ERR("decoder: mpp_create failed");
        free(d);
        return NULL;
    }

    if (mpp_init(d->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        LOG_ERR("decoder: mpp_init failed — is /dev/mpp_service present?");
        mpp_destroy(d->ctx);
        free(d);
        return NULL;
    }

    /*
     * Non-blocking output.
     *
     * The default blocks in decode_get_frame() until a frame is ready, which is
     * precisely how a starved pool turns into a permanent hang with no
     * diagnostic. Polling lets an empty decoder and a starved one be told
     * apart, and reported.
     */
    RK_S32 timeout = MPP_POLL_NON_BLOCK;
    if (d->mpi->control(d->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK)
        LOG_WARN("decoder: could not set non-blocking output; a starved pool "
                 "will hang rather than report");

    if (cfg && cfg->fast_output) {
        RK_U32 fast = 1;
        d->mpi->control(d->ctx, MPP_DEC_SET_IMMEDIATE_OUT, &fast);
    }

    LOG_INFO("decoder: rockchip mpp, H.264, up to %d frames in flight",
             d->max_inflight);
    return d;
}

void syncn_decoder_destroy(syncn_decoder *d)
{
    if (!d)
        return;

    /* Anything still held would otherwise keep a buffer out of the pool for the
     * lifetime of the process. */
    for (int i = 0; i < SYNCN_DECODER_MAX_INFLIGHT; i++) {
        if (d->inflight[i].in_use) {
            mpp_frame_deinit(&d->inflight[i].frame);
            d->inflight[i].in_use = false;
        }
    }

    if (d->mpi)
        d->mpi->reset(d->ctx);
    if (d->ctx)
        mpp_destroy(d->ctx);
    free(d);
}

bool syncn_decoder_submit(syncn_decoder *d, const uint8_t *au, size_t len,
                          uint64_t pts_us)
{
    MppPacket packet = NULL;
    if (mpp_packet_init(&packet, (void *)au, len) != MPP_OK) {
        d->submit_failed++;
        return false;
    }

    mpp_packet_set_pts(packet, (RK_S64)pts_us);
    mpp_packet_set_pos(packet, (void *)au);
    mpp_packet_set_length(packet, len);

    const MPP_RET ret = d->mpi->decode_put_packet(d->ctx, packet);
    mpp_packet_deinit(&packet);

    if (ret != MPP_OK) {
        /* The decoder's input queue is full, usually because its output is not
         * being drained. Dropping is right: queueing deeper only adds latency
         * to a picture already behind. */
        d->submit_failed++;
        return false;
    }

    d->submitted++;
    return true;
}

bool syncn_decoder_next(syncn_decoder *d, syncn_decoded_frame *out)
{
    if (d->held >= (unsigned)d->max_inflight) {
        d->starved++;
        if (!d->warned_starved) {
            d->warned_starved = true;
            LOG_WARN("decoder: %u frames held by the consumer and none returned. "
                     "MPP serves from a fixed pool, so this is what a permanent "
                     "video freeze looks like before it happens.", d->held);
        }
        return false;
    }
    d->warned_starved = false;

    MppFrame frame = NULL;
    if (d->mpi->decode_get_frame(d->ctx, &frame) != MPP_OK || !frame)
        return false;

    if (mpp_frame_get_info_change(frame)) {
        /*
         * The decoder has worked out the stream geometry and wants buffers.
         * Acknowledging lets it allocate its internal pool; without this it
         * waits forever and no frame ever arrives.
         */
        d->width  = mpp_frame_get_width(frame);
        d->height = mpp_frame_get_height(frame);
        d->info_changes++;

        LOG_INFO("decoder: stream is %ux%u (stride %ux%u)",
                 d->width, d->height,
                 mpp_frame_get_hor_stride(frame), mpp_frame_get_ver_stride(frame));

        d->mpi->control(d->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        mpp_frame_deinit(&frame);
        return false;
    }

    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
        /* Decoded from a broken reference chain. Showing it is worse than
         * showing the previous frame for another 50 ms. */
        d->errors++;
        mpp_frame_deinit(&frame);
        return false;
    }

    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (!buffer) {
        mpp_frame_deinit(&frame);
        return false;
    }

    int slot = -1;
    for (int i = 0; i < d->max_inflight; i++) {
        if (!d->inflight[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Cannot happen given the held check above, but returning the frame is
         * the only safe response to a bookkeeping error here. */
        mpp_frame_deinit(&frame);
        return false;
    }

    d->inflight[slot].frame  = frame;
    d->inflight[slot].in_use = true;
    d->held++;
    d->frames_out++;

    out->dmabuf_fd = mpp_buffer_get_fd(buffer);
    out->width     = mpp_frame_get_width(frame);
    out->height    = mpp_frame_get_height(frame);
    out->hstride   = mpp_frame_get_hor_stride(frame);
    out->vstride   = mpp_frame_get_ver_stride(frame);
    out->fourcc    = 0x3231564E; /* NV12 */
    out->pts_us    = (uint64_t)mpp_frame_get_pts(frame);
    out->slot      = slot;
    return true;
}

void syncn_decoder_release(syncn_decoder *d, const syncn_decoded_frame *frame)
{
    if (!d || !frame || frame->slot < 0 || frame->slot >= SYNCN_DECODER_MAX_INFLIGHT)
        return;
    if (!d->inflight[frame->slot].in_use) {
        LOG_WARN("decoder: frame in slot %d released twice", frame->slot);
        return;
    }

    /* This is what returns the buffer to MPP's pool. */
    mpp_frame_deinit(&d->inflight[frame->slot].frame);
    d->inflight[frame->slot].in_use = false;
    d->held--;
    d->released++;
}

void syncn_decoder_reset(syncn_decoder *d)
{
    if (!d)
        return;

    for (int i = 0; i < SYNCN_DECODER_MAX_INFLIGHT; i++) {
        if (d->inflight[i].in_use) {
            mpp_frame_deinit(&d->inflight[i].frame);
            d->inflight[i].in_use = false;
        }
    }
    d->held = 0;
    d->mpi->reset(d->ctx);
    LOG_INFO("decoder: reset; parameter sets must be re-sent before the next frame");
}

void syncn_decoder_get_stats(const syncn_decoder *d, syncn_decoder_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!d)
        return;
    out->submitted     = d->submitted;
    out->submit_failed = d->submit_failed;
    out->frames_out    = d->frames_out;
    out->released      = d->released;
    out->info_changes  = d->info_changes;
    out->errors        = d->errors;
    out->starved       = d->starved;
    out->inflight      = d->held;
    out->width         = d->width;
    out->height        = d->height;
}

#else /* !SYNCN_HAVE_MPP */

/*
 * No hardware decoder in this build. The daemon still runs: it receives video
 * and can write it to disk, it simply cannot display it. Saying so once at
 * startup beats failing to start on a machine used for protocol work.
 */
bool syncn_decoder_available(void) { return false; }
const char *syncn_decoder_backend(void) { return "none"; }

syncn_decoder *syncn_decoder_create(const syncn_decoder_config *cfg)
{
    (void)cfg;
    LOG_WARN("decoder: built without Rockchip MPP — video cannot be displayed");
    return NULL;
}

void syncn_decoder_destroy(syncn_decoder *d) { (void)d; }
bool syncn_decoder_submit(syncn_decoder *d, const uint8_t *au, size_t len, uint64_t pts)
{ (void)d; (void)au; (void)len; (void)pts; return false; }
bool syncn_decoder_next(syncn_decoder *d, syncn_decoded_frame *out)
{ (void)d; (void)out; return false; }
void syncn_decoder_release(syncn_decoder *d, const syncn_decoded_frame *f)
{ (void)d; (void)f; }
void syncn_decoder_reset(syncn_decoder *d) { (void)d; }
void syncn_decoder_get_stats(const syncn_decoder *d, syncn_decoder_stats *out)
{ (void)d; memset(out, 0, sizeof *out); }

#endif /* SYNCN_HAVE_MPP */
