/*
 * Hardware H.264 decode.
 *
 * One rule shapes this whole module: every frame the decoder hands out must be
 * given back, and the code must notice immediately when it is not.
 *
 * MPP serves frames from a fixed internal buffer pool. A consumer that holds
 * frames without releasing them drains that pool, and the next
 * decode_get_frame() blocks forever. Audio runs on another thread and carries
 * on perfectly, so what the user sees is a frozen picture with working sound —
 * which is exactly the fault reported on this panel, and exactly what a
 * "manufacturer driver problem" looks like from the outside.
 *
 * It is not a driver problem. mpi_dec_test decodes this panel's own captured
 * door stream at 864 fps without stalling, because it returns every frame
 * immediately. So this module:
 *
 *   - hands out at most `max_inflight` frames at once,
 *   - refuses to hand out more rather than blocking,
 *   - counts and logs when the consumer is holding too long,
 *   - and releases everything it holds on teardown.
 *
 * A stall becomes a log line naming the cause instead of a silent freeze.
 */
#ifndef SYNCN_DECODER_H
#define SYNCN_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* More than a handful in flight means the consumer is not keeping up, and
 * queueing deeper only delays the picture. */
#define SYNCN_DECODER_MAX_INFLIGHT 8

typedef struct {
    /* DMA-BUF file descriptor, borrowed. Valid until syncn_decoder_release().
     * Do not close it; the decoder owns it. */
    int      dmabuf_fd;

    uint32_t width, height;
    uint32_t hstride, vstride;  /* padded, and usually larger than width/height */
    uint32_t fourcc;            /* NV12 for this decoder                        */
    uint64_t pts_us;

    /* Opaque; pass the frame back unchanged to release it. */
    int      slot;
} syncn_decoded_frame;

typedef struct {
    int  max_inflight;      /* 0 -> 4                                     */
    bool fast_output;       /* lower latency, at a small cost in reorder  */
} syncn_decoder_config;

typedef struct syncn_decoder syncn_decoder;

/* Returns NULL when no hardware decoder is available. */
syncn_decoder *syncn_decoder_create(const syncn_decoder_config *cfg);
void           syncn_decoder_destroy(syncn_decoder *d);

/* Whether this build has a hardware decoder at all. */
bool        syncn_decoder_available(void);
const char *syncn_decoder_backend(void);

/*
 * Submit one complete access unit. Returns false when the decoder cannot take
 * it, which is a drop rather than an error — the caller decides whether that
 * matters and must call syncn_h264_note_loss() if it does, so the stream waits
 * for the next keyframe instead of feeding the decoder a broken reference
 * chain.
 */
bool syncn_decoder_submit(syncn_decoder *d, const uint8_t *au, size_t len,
                          uint64_t pts_us);

/*
 * Take the next decoded frame, if one is ready. Never blocks.
 *
 * Returns false when nothing is ready OR when max_inflight frames are already
 * out. The second case is the important one: it means the consumer is not
 * returning frames, and it is reported rather than waited on.
 */
bool syncn_decoder_next(syncn_decoder *d, syncn_decoded_frame *out);

/* Give a frame back. Every frame from syncn_decoder_next() must reach here. */
void syncn_decoder_release(syncn_decoder *d, const syncn_decoded_frame *frame);

/* Discard decoder state, e.g. after a loss. Parameter sets must be re-sent
 * afterwards — see syncn_h264_parameter_sets(). */
void syncn_decoder_reset(syncn_decoder *d);

typedef struct {
    uint64_t submitted;
    uint64_t submit_failed;
    uint64_t frames_out;
    uint64_t released;
    uint64_t info_changes;
    uint64_t errors;          /* frames the decoder flagged as corrupt      */
    uint64_t starved;         /* next() refused because the consumer holds  */
    unsigned inflight;
    uint32_t width, height;
} syncn_decoder_stats;

void syncn_decoder_get_stats(const syncn_decoder *d, syncn_decoder_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_DECODER_H */
