/*
 * H.264 bitstream handling for the door's video channel.
 *
 * Sits between the frame parser and the decoder. Its job is to hand the decoder
 * complete, decodable access units and never anything else — because the one
 * thing this protocol cannot do is ask for a new keyframe. There is no
 * retransmit, no IDR request, no feedback channel of any kind. If the reference
 * chain is broken, the picture stays broken until the door happens to send its
 * next IDR, and nothing we do makes that arrive sooner.
 *
 * That constraint drives every decision here.
 *
 * What the door actually sends, measured on the bench at 192.168.100.193:
 *
 *   1280x720, 20 fps, ~1.5 Mbit/s, IDR every 15 frames.
 *   One 0xBB frame per access unit. Usually a single P-slice NAL — but at every
 *   keyframe the frame carries SPS + PPS + IDR together, three NALs in one
 *   payload. A 60-second capture held 1200 frames and 1360 NAL units.
 *
 * The protocol document said exactly one NAL per frame. A reader built on that
 * mangles every keyframe, which is precisely what the decoder needs both to
 * start and to recover.
 */
#ifndef SYNCN_H264_H
#define SYNCN_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNCN_NAL_UNSPECIFIED = 0,
    SYNCN_NAL_SLICE       = 1,  /* non-IDR coded slice        */
    SYNCN_NAL_IDR         = 5,  /* keyframe slice             */
    SYNCN_NAL_SEI         = 6,
    SYNCN_NAL_SPS         = 7,  /* sequence parameter set     */
    SYNCN_NAL_PPS         = 8,  /* picture parameter set      */
    SYNCN_NAL_AUD         = 9   /* access unit delimiter      */
} syncn_nal_type;

const char *syncn_nal_type_name(syncn_nal_type t);

typedef struct {
    const uint8_t *data;   /* including its start code   */
    size_t         len;
    syncn_nal_type type;
} syncn_nal;

/*
 * Split an Annex-B buffer into NAL units. Handles both 3-byte and 4-byte start
 * codes, and leading bytes before the first start code are skipped. Returns how
 * many were found, writing at most `max`.
 *
 * Pass out = NULL to count without writing, so a caller can size an array
 * exactly rather than guessing.
 */
size_t syncn_h264_split(const uint8_t *buf, size_t len, syncn_nal *out, size_t max);

/* ------------------------------------------------------------- assembler --- */

typedef struct syncn_h264 syncn_h264;

/*
 * Called with a complete access unit ready for the decoder. `data` is valid
 * only for the duration of the call.
 */
typedef void (*syncn_h264_au_cb)(const uint8_t *data, size_t len,
                                 bool keyframe, void *user);

syncn_h264 *syncn_h264_create(void);
void        syncn_h264_destroy(syncn_h264 *h);

/* Feed one 0xBB payload. May emit one access unit. */
void syncn_h264_feed(syncn_h264 *h, const uint8_t *payload, size_t len,
                     syncn_h264_au_cb cb, void *user);

/*
 * Tell the assembler a frame was lost — dropped for backpressure, or missing
 * after a reconnect.
 *
 * From here it emits nothing until the next IDR. That is not caution, it is
 * correctness: every P-slice after a gap references frames the decoder does not
 * have, so feeding them produces corruption that persists until the next
 * keyframe anyway. At 20 fps with an IDR every 15 frames, the wait is under a
 * second; the corruption would have lasted just as long and looked far worse.
 */
void syncn_h264_note_loss(syncn_h264 *h);
bool syncn_h264_waiting_for_keyframe(const syncn_h264 *h);

/*
 * Copy the cached SPS and PPS, as Annex-B, for re-injection when the decoder is
 * restarted. Returns bytes written, or 0 if none have been seen yet.
 *
 * Without this a decoder restart mid-stream produces a black picture until the
 * door's next IDR — and on a stream whose parameter sets only travel with
 * keyframes, that is a visible stall every single time.
 */
size_t syncn_h264_parameter_sets(const syncn_h264 *h, uint8_t *out, size_t cap);
bool   syncn_h264_have_parameter_sets(const syncn_h264 *h);

typedef struct {
    uint64_t payloads;        /* 0xBB frames fed in                       */
    uint64_t nals;            /* NAL units found within them              */
    uint64_t access_units;    /* emitted to the decoder                   */
    uint64_t keyframes;
    uint64_t skipped_no_ref;  /* dropped while waiting for a keyframe     */
    uint64_t empty_payloads;
    unsigned nals_in_last_payload;
} syncn_h264_stats;

void syncn_h264_get_stats(const syncn_h264 *h, syncn_h264_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_H264_H */
