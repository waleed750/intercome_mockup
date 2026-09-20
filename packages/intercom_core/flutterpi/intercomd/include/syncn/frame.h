/*
 * Frame codec for the door station call stream (TCP 8189).
 *
 * Every byte on the call socket sits inside a frame. One framing format
 * carries all three channels, and the 4-byte magic IS the channel selector:
 *
 *   0        1        2        3        4        5        6        7        8
 *  +--------+--------+--------+--------+--------+--------+--------+--------+------//
 *  | MARKER | MARKER | MARKER | MARKER |  LEN0  |  LEN1  |  LEN2  |  LEN3  | PAYLOAD
 *  +--------+--------+--------+--------+--------+--------+--------+--------+------//
 *          magic (4x same byte)          length, uint32 little-endian       LEN bytes
 *
 * Length counts the payload only. There is no checksum, no sequence number and
 * no timestamp — ordering and integrity are TCP's job.
 *
 * See docs/10-protocol-reference.md section 3.
 */
#ifndef SYNCN_FRAME_H
#define SYNCN_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_FRAME_HEADER_SIZE 8u

/* Default payload ceiling. A header claiming more than this is treated as
 * corrupt rather than trusted — otherwise one bad length field would have us
 * allocate arbitrarily and stall waiting for bytes that never arrive. */
#define SYNCN_MAX_PAYLOAD_DEFAULT (4u * 1024u * 1024u)

typedef enum {
    SYNCN_CH_CONTROL = 0xAA, /* UTF-8 JSON command          */
    SYNCN_CH_VIDEO   = 0xBB, /* one H.264 Annex-B NAL unit  */
    SYNCN_CH_AUDIO   = 0xCC  /* 160 bytes G.711 A-law       */
} syncn_channel;

/* True for the three markers above and nothing else. */
int syncn_channel_valid(unsigned byte);

/* Human-readable channel name, for logs. Never NULL. */
const char *syncn_channel_name(syncn_channel ch);

/* Write an 8-byte header. `out` must have room for SYNCN_FRAME_HEADER_SIZE. */
void syncn_frame_write_header(uint8_t *out, syncn_channel ch, uint32_t payload_len);

/* ------------------------------------------------------------------ parser */

/*
 * A resynchronising stream parser.
 *
 * The door's stream can start mid-frame, and a corrupt byte must not kill the
 * connection. So on any header that does not check out — wrong magic, or a
 * length beyond the ceiling — the parser advances exactly ONE byte and rescans
 * rather than discarding the buffer or closing the socket. That is what lets a
 * damaged stream recover on its own.
 */
typedef struct syncn_parser syncn_parser;

/* Called once per complete frame. `payload` is only valid for the duration of
 * the callback; copy it if you need to keep it. Return non-zero to stop feeding
 * early — syncn_parser_feed will return the same value. */
typedef int (*syncn_frame_cb)(syncn_channel ch, const uint8_t *payload,
                              size_t len, void *user);

typedef struct {
    uint64_t frames_parsed;    /* complete frames delivered to the callback   */
    uint64_t resync_events;    /* times a bad header forced a one-byte skip   */
    uint64_t bytes_discarded;  /* bytes thrown away while resynchronising     */
    uint64_t oversize_headers; /* headers whose length exceeded the ceiling   */
    size_t   buffer_capacity;  /* current buffer size, for leak watching      */
    size_t   buffered;         /* bytes held pending more input               */
} syncn_parser_stats;

/* max_payload of 0 means SYNCN_MAX_PAYLOAD_DEFAULT. Returns NULL on OOM. */
syncn_parser *syncn_parser_new(size_t max_payload);
void          syncn_parser_free(syncn_parser *p);

/* Feed received bytes. Returns 0 on success, -1 on allocation failure, or
 * whatever non-zero value the callback returned if it asked to stop. */
int syncn_parser_feed(syncn_parser *p, const uint8_t *data, size_t len,
                      syncn_frame_cb cb, void *user);

void syncn_parser_stats_get(const syncn_parser *p, syncn_parser_stats *out);

/* Drop everything buffered. Use when a connection is torn down and reused. */
void syncn_parser_reset(syncn_parser *p);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_FRAME_H */
