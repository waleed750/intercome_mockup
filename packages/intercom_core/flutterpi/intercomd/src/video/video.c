#include "syncn/video.h"
#include "syncn/decoder.h"
#include "syncn/dmabuf.h"
#include "syncn/h264.h"
#include "syncn/rga.h"
#include "syncn/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct syncn_video {
    syncn_video_config   cfg;
    syncn_video_sink     sink;
    syncn_video_reset_cb on_reset;
    void                *user;

    syncn_h264      *parser;
    syncn_decoder   *decoder;
    syncn_rga       *rga;
    syncn_framepool *pool;

    /*
     * Buffer ids below this belonged to a pool that has been torn down — after
     * a resolution change, or a reset. The consumer may still send them back;
     * they are ignored quietly rather than counted as protocol errors, because
     * they are not. Ids never repeat, so there is no chance of matching the
     * wrong buffer.
     */
    uint64_t retired_before;

    syncn_video_stats st;

    /* Rate-limits the conversion-failure message: an RGA that refuses this
     * geometry refuses every frame of it, at 20 a second. */
    uint64_t last_convert_warn_ms;
};

/* --------------------------------------------------------- availability --- */

static const char *g_reason = NULL;

bool syncn_video_available(void)
{
    if (!syncn_decoder_available()) {
        g_reason = "no hardware H.264 decoder (build without librockchip_mpp)";
        return false;
    }
    if (!syncn_rga_available()) {
        g_reason = "no RGA colour conversion (build without librga)";
        return false;
    }
    if (!syncn_dmabuf_available()) {
        g_reason = "no DMA heap under /dev/dma_heap (kernel without CONFIG_DMABUF_HEAPS)";
        return false;
    }
    g_reason = NULL;
    return true;
}

const char *syncn_video_unavailable_reason(void)
{
    if (!g_reason)
        (void)syncn_video_available();
    return g_reason ? g_reason : "none";
}

/* ---------------------------------------------------------------- setup --- */

static void on_access_unit(const uint8_t *data, size_t len, bool keyframe, void *user)
{
    syncn_video *v = user;
    v->st.au_in++;

    if (!syncn_decoder_submit(v->decoder, data, len, 0)) {
        /*
         * A refused access unit is a hole in the reference chain. Telling the
         * parser means it waits for the next keyframe instead of feeding the
         * decoder P-slices that reference a frame it never got — which would
         * produce smeared corruption lasting exactly as long as the wait, and
         * looking far worse.
         */
        syncn_h264_note_loss(v->parser);
        LOG_WARN("video: decoder refused a %s access unit (%zu bytes) — "
                 "waiting for the next keyframe",
                 keyframe ? "key" : "delta", len);
        return;
    }
    v->st.submitted++;
}

syncn_video *syncn_video_create(const syncn_video_config *cfg,
                                syncn_video_sink sink,
                                syncn_video_reset_cb on_reset,
                                void *user)
{
    if (!sink)
        return NULL;

    syncn_video *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;

    if (cfg)
        v->cfg = *cfg;
    v->sink     = sink;
    v->on_reset = on_reset;
    v->user     = user;

    v->parser = syncn_h264_create();
    if (!v->parser) {
        free(v);
        return NULL;
    }

    const syncn_decoder_config dc = {
        /* One deeper than the pool: the decoder may hold a frame that is being
         * converted while the pool's buffers are all out. Any deeper only adds
         * latency, since a frame we cannot convert yet is a frame nobody is
         * going to see in time. */
        .max_inflight = 2,
        .fast_output  = v->cfg.fast_output,
    };
    v->decoder = syncn_decoder_create(&dc);
    if (!v->decoder) {
        LOG_ERR("video: no decoder — %s", syncn_video_unavailable_reason());
        syncn_h264_destroy(v->parser);
        free(v);
        return NULL;
    }

    v->rga = syncn_rga_create();
    if (!v->rga) {
        LOG_ERR("video: no colour conversion — the only usable display plane on "
                "this panel is RGB-only, so NV12 cannot be shown directly");
        syncn_decoder_destroy(v->decoder);
        syncn_h264_destroy(v->parser);
        free(v);
        return NULL;
    }

    /*
     * Every id issued before this pipeline existed belongs to an earlier
     * call. The interface keeps displaying that call's last frame until a new
     * one replaces it, and returns it then -- into this pipeline, whose pool
     * never issued it. That is correct behaviour on both sides, and ids never
     * repeat, so it is not a protocol error and must not be logged as one.
     */
    v->retired_before = syncn_framepool_next_id();

    /* The pool waits for the first decoded frame: only then is the geometry
     * known, and allocating for a guess would mean reallocating immediately. */
    LOG_INFO("video: pipeline ready (decoder %s, converter %s, heap %s)",
             syncn_decoder_backend(), syncn_rga_backend(),
             syncn_dmabuf_heap_name());
    return v;
}

void syncn_video_destroy(syncn_video *v)
{
    if (!v)
        return;
    /* Decoder first: it may still hold frames, and its teardown releases them.
     * The pool goes last so nothing is writing into a freed buffer. */
    syncn_decoder_destroy(v->decoder);
    syncn_rga_destroy(v->rga);
    syncn_h264_destroy(v->parser);
    syncn_framepool_destroy(v->pool);
    free(v);
}

/* ----------------------------------------------------------------- feed --- */

void syncn_video_feed(syncn_video *v, const uint8_t *payload, size_t len)
{
    if (!v || !payload || len == 0)
        return;
    syncn_h264_feed(v->parser, payload, len, on_access_unit, v);
}

/* ----------------------------------------------------------------- pump --- */

/* Make sure the pool matches this frame's visible geometry, creating or
 * replacing it as needed. Returns false when no pool could be built. */
static bool ensure_pool(syncn_video *v, uint32_t w, uint32_t h)
{
    if (v->pool && syncn_framepool_matches(v->pool, w, h))
        return true;

    if (v->pool) {
        LOG_INFO("video: geometry changed to %ux%u — rebuilding the buffer pool",
                 w, h);
        /*
         * Retire every id this pool ever issued. The consumer may be holding
         * some and will send them back; those releases refer to memory that is
         * the wrong size now, and are ignored rather than treated as errors.
         * Its descriptors keep that memory alive until it drops them, so
         * freeing our side here is safe even mid-scanout.
         */
        v->retired_before = syncn_framepool_next_id();
        syncn_framepool_destroy(v->pool);
        v->pool = NULL;
    }

    v->pool = syncn_framepool_create(v->cfg.buffers, w, h);
    if (!v->pool) {
        LOG_ERR("video: cannot allocate display buffers for %ux%u", w, h);
        return false;
    }

    v->st.width  = w;
    v->st.height = h;
    if (v->on_reset)
        v->on_reset(w, h, v->user);
    return true;
}

static bool convert(syncn_video *v, const syncn_decoded_frame *src,
                    const syncn_frame_buffer *dst)
{
    const syncn_rga_image s = {
        .fd      = src->dmabuf_fd,
        .width   = src->width,
        .height  = src->height,
        .hstride = src->hstride,
        .vstride = src->vstride,
    };
    const syncn_rga_image d = {
        .fd     = dst->fd,
        .width  = dst->width,
        .height = dst->height,
        /* RGA counts horizontal stride in pixels, not bytes; the pool aligns
         * in bytes because that is what DRM's framebuffer pitch wants. Four
         * bytes per pixel is the whole of the conversion, and the pool's
         * alignment keeps it exact. */
        .hstride = dst->stride / 4,
        .vstride = dst->height,
    };
    return syncn_rga_nv12_to_bgra(v->rga, &s, &d);
}

unsigned syncn_video_pump(syncn_video *v)
{
    if (!v)
        return 0;

    unsigned published = 0;

    for (;;) {
        syncn_decoded_frame f;
        if (!syncn_decoder_next(v->decoder, &f))
            break;                       /* nothing ready, or we hold too many */

        v->st.decoded++;

        if (!ensure_pool(v, f.width, f.height)) {
            syncn_decoder_release(v->decoder, &f);
            break;                       /* no point pulling more */
        }

        const syncn_frame_buffer *buf = syncn_framepool_acquire(v->pool);
        if (!buf) {
            /* Every buffer is with the interface. Drop this frame and give the
             * decoder its own back at once — holding it here would drain MPP's
             * pool as well, turning a dropped frame into a dead pipeline. */
            syncn_decoder_release(v->decoder, &f);
            v->st.dropped_no_buffer++;
            break;
        }

        const bool ok = convert(v, &f, buf);

        /* Unconditionally, and before anything else: RGA is synchronous, so
         * the pixels are copied out by now whether it succeeded or not. */
        syncn_decoder_release(v->decoder, &f);

        if (!ok) {
            syncn_framepool_release(v->pool, buf->id);
            v->st.dropped_convert++;
            const uint64_t now = syncn_now_ms();
            if (now - v->last_convert_warn_ms >= 1000) {
                v->last_convert_warn_ms = now;
                LOG_WARN("video: colour conversion failing (%llu frames) — "
                         "the picture will not update",
                         (unsigned long long)v->st.dropped_convert);
            }
            continue;
        }
        v->st.converted++;

        if (!v->sink(buf, f.pts_us, v->user)) {
            /* Nobody took it. Back to the pool immediately: a buffer handed to
             * a consumer that never heard about it is a buffer lost forever. */
            syncn_framepool_release(v->pool, buf->id);
            v->st.dropped_sink++;
            continue;
        }

        v->st.published++;
        published++;
    }

    v->st.waiting_for_keyframe = syncn_h264_waiting_for_keyframe(v->parser);
    return published;
}

/* ------------------------------------------------------------ recycling --- */

bool syncn_video_recycle(syncn_video *v, uint64_t id)
{
    if (!v || !v->pool)
        return false;

    /* A buffer from a pool that no longer exists. Not an error — the consumer
     * is answering a question we stopped asking. */
    if (id < v->retired_before)
        return true;

    /* An id this process has not issued yet cannot be ours: it is the
     * interface returning a buffer from the daemon that ran before this one,
     * which it kept displaying across our restart. Same answer. */
    if (id >= syncn_framepool_next_id())
        return true;

    return syncn_framepool_release(v->pool, id);
}

void syncn_video_consumer_gone(syncn_video *v)
{
    if (!v || !v->pool)
        return;
    syncn_framepool_release_all(v->pool);
}

void syncn_video_reset(syncn_video *v)
{
    if (!v)
        return;

    syncn_decoder_reset(v->decoder);
    syncn_h264_note_loss(v->parser);

    /*
     * Re-inject the cached parameter sets. Without them the decoder produces
     * nothing until the door's next SPS, and on this stream the parameter sets
     * only travel with keyframes — so every reset would cost a visible stall
     * of up to an IDR interval.
     */
    uint8_t  sets[512];
    const size_t n = syncn_h264_parameter_sets(v->parser, sets, sizeof sets);
    if (n > 0 && syncn_decoder_submit(v->decoder, sets, n, 0))
        LOG_DBG("video: re-sent %zu bytes of parameter sets after reset", n);

    if (v->pool)
        syncn_framepool_release_all(v->pool);
    v->st.waiting_for_keyframe = true;
}

/* ---------------------------------------------------------------- stats --- */

void syncn_video_get_stats(const syncn_video *v, syncn_video_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!v)
        return;
    *out = v->st;

    /* Read live rather than from a cached copy. Buffers come back between
     * pumps — that is the normal case, not the exception — and a stats line
     * that only refreshed on pump would report a consumer as holding buffers
     * it had already returned. Which is exactly the symptom being hunted. */
    out->outstanding = syncn_framepool_outstanding(v->pool);
}

void syncn_video_format_stats(const syncn_video *v, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!v)
        return;

    syncn_decoder_stats d;
    memset(&d, 0, sizeof d);
    syncn_decoder_get_stats(v->decoder, &d);

    snprintf(out, cap,
             "video: %ux%u au=%llu decoded=%llu shown=%llu | "
             "dropped: buffer=%llu convert=%llu sink=%llu | "
             "decoder: starved=%llu errors=%llu inflight=%u | held=%u%s",
             v->st.width, v->st.height,
             (unsigned long long)v->st.au_in,
             (unsigned long long)v->st.decoded,
             (unsigned long long)v->st.published,
             (unsigned long long)v->st.dropped_no_buffer,
             (unsigned long long)v->st.dropped_convert,
             (unsigned long long)v->st.dropped_sink,
             (unsigned long long)d.starved,
             (unsigned long long)d.errors,
             d.inflight,
             syncn_framepool_outstanding(v->pool),
             v->st.waiting_for_keyframe ? " [waiting for keyframe]" : "");
}
