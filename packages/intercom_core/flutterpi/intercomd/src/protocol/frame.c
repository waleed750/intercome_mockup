#include "syncn/frame.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY (64u * 1024u)

struct syncn_parser {
    uint8_t *buf;
    size_t   cap;    /* allocated bytes                  */
    size_t   start;  /* first unconsumed byte            */
    size_t   end;    /* one past the last byte received  */
    size_t   max_payload;

    uint64_t frames_parsed;
    uint64_t resync_events;
    uint64_t bytes_discarded;
    uint64_t oversize_headers;
};

int syncn_channel_valid(unsigned byte)
{
    return byte == SYNCN_CH_CONTROL || byte == SYNCN_CH_VIDEO || byte == SYNCN_CH_AUDIO;
}

const char *syncn_channel_name(syncn_channel ch)
{
    switch (ch) {
    case SYNCN_CH_CONTROL: return "control";
    case SYNCN_CH_VIDEO:   return "video";
    case SYNCN_CH_AUDIO:   return "audio";
    default:               return "unknown";
    }
}

void syncn_frame_write_header(uint8_t *out, syncn_channel ch, uint32_t payload_len)
{
    const uint8_t marker = (uint8_t)ch;
    out[0] = marker;
    out[1] = marker;
    out[2] = marker;
    out[3] = marker;
    /* Little-endian, written byte by byte so the result does not depend on the
     * host's endianness or on alignment. */
    out[4] = (uint8_t)(payload_len & 0xFFu);
    out[5] = (uint8_t)((payload_len >> 8) & 0xFFu);
    out[6] = (uint8_t)((payload_len >> 16) & 0xFFu);
    out[7] = (uint8_t)((payload_len >> 24) & 0xFFu);
}

syncn_parser *syncn_parser_new(size_t max_payload)
{
    syncn_parser *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;

    p->buf = malloc(INITIAL_CAPACITY);
    if (!p->buf) {
        free(p);
        return NULL;
    }
    p->cap         = INITIAL_CAPACITY;
    p->max_payload = max_payload ? max_payload : SYNCN_MAX_PAYLOAD_DEFAULT;
    return p;
}

void syncn_parser_free(syncn_parser *p)
{
    if (!p)
        return;
    free(p->buf);
    free(p);
}

void syncn_parser_reset(syncn_parser *p)
{
    p->start = p->end = 0;
}

/* Slide unconsumed bytes to the front. Cheap, and it keeps `end` from walking
 * off the end of the buffer on a long-lived connection. */
static void compact(syncn_parser *p)
{
    if (p->start == 0)
        return;
    const size_t held = p->end - p->start;
    if (held)
        memmove(p->buf, p->buf + p->start, held);
    p->start = 0;
    p->end   = held;
}

static int ensure_room(syncn_parser *p, size_t extra)
{
    if (p->cap - p->end >= extra)
        return 0;

    compact(p);
    if (p->cap - p->end >= extra)
        return 0;

    size_t want = p->cap;
    while (want - p->end < extra) {
        /* Doubling, with an overflow guard. The ceiling that actually matters
         * is max_payload, enforced per-header below. */
        if (want > (size_t)-1 / 2)
            return -1;
        want *= 2;
    }

    uint8_t *nb = realloc(p->buf, want);
    if (!nb)
        return -1;
    p->buf = nb;
    p->cap = want;
    return 0;
}

int syncn_parser_feed(syncn_parser *p, const uint8_t *data, size_t len,
                      syncn_frame_cb cb, void *user)
{
    if (len) {
        if (ensure_room(p, len) != 0)
            return -1;
        memcpy(p->buf + p->end, data, len);
        p->end += len;
    }

    for (;;) {
        const size_t avail = p->end - p->start;

        /* Fewer than four bytes: cannot even test the magic yet. */
        if (avail < 4)
            break;

        const uint8_t *q = p->buf + p->start;
        const uint8_t  m = q[0];

        if (!(q[1] == m && q[2] == m && q[3] == m && syncn_channel_valid(m))) {
            /* Not a header here. Advance exactly one byte — the magic may begin
             * at the next offset, and skipping more could step over it. */
            p->start++;
            p->resync_events++;
            p->bytes_discarded++;
            continue;
        }

        /* Magic is good but the length field may not have arrived yet. */
        if (avail < SYNCN_FRAME_HEADER_SIZE)
            break;

        const uint32_t plen = (uint32_t)q[4]
                            | ((uint32_t)q[5] << 8)
                            | ((uint32_t)q[6] << 16)
                            | ((uint32_t)q[7] << 24);

        if ((size_t)plen > p->max_payload) {
            /* Four identical marker bytes can occur inside a payload, so this
             * is more likely a false positive than a real oversized frame.
             * Treat it as a bad header and rescan from the next byte. */
            p->start++;
            p->resync_events++;
            p->bytes_discarded++;
            p->oversize_headers++;
            continue;
        }

        if (avail < SYNCN_FRAME_HEADER_SIZE + (size_t)plen)
            break; /* incomplete frame — wait for the rest */

        const uint8_t *payload = q + SYNCN_FRAME_HEADER_SIZE;
        p->start += SYNCN_FRAME_HEADER_SIZE + (size_t)plen;
        p->frames_parsed++;

        if (cb) {
            const int rc = cb((syncn_channel)m, plen ? payload : NULL, plen, user);
            if (rc != 0)
                return rc; /* caller asked us to stop */
        }
    }

    /* Drained what we could; reclaim the consumed prefix. */
    if (p->start == p->end)
        p->start = p->end = 0;
    else if (p->start > INITIAL_CAPACITY)
        compact(p);

    return 0;
}

void syncn_parser_stats_get(const syncn_parser *p, syncn_parser_stats *out)
{
    out->frames_parsed    = p->frames_parsed;
    out->resync_events    = p->resync_events;
    out->bytes_discarded  = p->bytes_discarded;
    out->oversize_headers = p->oversize_headers;
    out->buffer_capacity  = p->cap;
    out->buffered         = p->end - p->start;
}
