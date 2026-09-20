#include "syncn/h264.h"

#include <stdlib.h>
#include <string.h>

#define PARAM_SET_MAX 4096  /* SPS and PPS together are a few hundred bytes */

struct syncn_h264 {
    uint8_t sps[PARAM_SET_MAX];
    size_t  sps_len;
    uint8_t pps[PARAM_SET_MAX];
    size_t  pps_len;

    bool waiting_for_keyframe;

    uint64_t payloads, nals, access_units, keyframes, skipped, empty;
    unsigned last_payload_nals;
};

const char *syncn_nal_type_name(syncn_nal_type t)
{
    switch (t) {
    case SYNCN_NAL_SLICE: return "slice";
    case SYNCN_NAL_IDR:   return "IDR";
    case SYNCN_NAL_SEI:   return "SEI";
    case SYNCN_NAL_SPS:   return "SPS";
    case SYNCN_NAL_PPS:   return "PPS";
    case SYNCN_NAL_AUD:   return "AUD";
    default:              return "other";
    }
}

/* Start code at `p`? Writes its length (3 or 4) to `sc`. */
static bool at_start_code(const uint8_t *p, size_t remaining, size_t *sc)
{
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        *sc = 4;
        return true;
    }
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        *sc = 3;
        return true;
    }
    return false;
}

size_t syncn_h264_split(const uint8_t *buf, size_t len, syncn_nal *out, size_t max)
{
    if (!buf || len == 0)
        return 0;
    if (!out)
        max = 0;   /* counting pass: caller wants the total before allocating */

    size_t count = 0;
    size_t i = 0, sc = 0;

    /* Skip anything before the first start code. A payload that begins
     * mid-stream is not useful, but it must not desynchronise the rest. */
    while (i < len && !at_start_code(buf + i, len - i, &sc))
        i++;

    while (i < len) {
        const size_t nal_start = i;
        const size_t header_at = i + sc;   /* first byte after the start code */

        i += sc;
        size_t next_sc = 0;
        while (i < len && !at_start_code(buf + i, len - i, &next_sc))
            i++;

        if (count < max) {
            out[count].data = buf + nal_start;
            out[count].len  = i - nal_start;
            /* The NAL header is one byte; the type is its low five bits. */
            out[count].type = (header_at < len)
                            ? (syncn_nal_type)(buf[header_at] & 0x1F)
                            : SYNCN_NAL_UNSPECIFIED;
        }
        count++;
        sc = next_sc;
    }

    return count;
}

syncn_h264 *syncn_h264_create(void)
{
    syncn_h264 *h = calloc(1, sizeof *h);
    if (h) {
        /* Nothing can be decoded until a keyframe arrives, so start in the
         * same state a loss puts us in. */
        h->waiting_for_keyframe = true;
    }
    return h;
}

void syncn_h264_destroy(syncn_h264 *h) { free(h); }

static void remember_parameter_set(syncn_h264 *h, const syncn_nal *n)
{
    if (n->len > PARAM_SET_MAX)
        return;

    if (n->type == SYNCN_NAL_SPS) {
        memcpy(h->sps, n->data, n->len);
        h->sps_len = n->len;
    } else if (n->type == SYNCN_NAL_PPS) {
        memcpy(h->pps, n->data, n->len);
        h->pps_len = n->len;
    }
}

void syncn_h264_feed(syncn_h264 *h, const uint8_t *payload, size_t len,
                     syncn_h264_au_cb cb, void *user)
{
    h->payloads++;

    if (!payload || len == 0) {
        h->empty++;
        h->last_payload_nals = 0;
        return;
    }

    /* Generous: a payload holding more than this is not a frame from this door,
     * and the split is bounded rather than allocating on untrusted input. */
    syncn_nal nals[32];
    const size_t found = syncn_h264_split(payload, len, nals, 32);
    const size_t n = found < 32 ? found : 32;

    h->nals += found;
    h->last_payload_nals = (unsigned)found;

    if (n == 0) {
        /* Bytes with no start code at all. Not decodable, and passing them on
         * would only confuse the decoder. */
        h->empty++;
        return;
    }

    bool keyframe = false;
    for (size_t i = 0; i < n; i++) {
        if (nals[i].type == SYNCN_NAL_SPS || nals[i].type == SYNCN_NAL_PPS)
            remember_parameter_set(h, &nals[i]);
        if (nals[i].type == SYNCN_NAL_IDR)
            keyframe = true;
    }

    if (h->waiting_for_keyframe) {
        if (!keyframe) {
            /* Every P-slice here references frames the decoder does not have.
             * Feeding them produces corruption that lasts until the next IDR
             * regardless, so dropping them costs nothing and looks better. */
            h->skipped++;
            return;
        }
        h->waiting_for_keyframe = false;
    }

    /*
     * The whole payload goes to the decoder as one access unit, exactly as it
     * arrived. The door already packs a complete AU per frame — a lone P-slice,
     * or SPS + PPS + IDR together — so there is nothing to reassemble and
     * splitting it up would only risk separating a keyframe from the parameter
     * sets it needs.
     */
    h->access_units++;
    if (keyframe)
        h->keyframes++;

    if (cb)
        cb(payload, len, keyframe, user);
}

void syncn_h264_note_loss(syncn_h264 *h)
{
    h->waiting_for_keyframe = true;
}

bool syncn_h264_waiting_for_keyframe(const syncn_h264 *h)
{
    return h->waiting_for_keyframe;
}

bool syncn_h264_have_parameter_sets(const syncn_h264 *h)
{
    return h->sps_len > 0 && h->pps_len > 0;
}

size_t syncn_h264_parameter_sets(const syncn_h264 *h, uint8_t *out, size_t cap)
{
    if (!syncn_h264_have_parameter_sets(h))
        return 0;
    if (h->sps_len + h->pps_len > cap)
        return 0;

    memcpy(out, h->sps, h->sps_len);
    memcpy(out + h->sps_len, h->pps, h->pps_len);
    return h->sps_len + h->pps_len;
}

void syncn_h264_get_stats(const syncn_h264 *h, syncn_h264_stats *out)
{
    out->payloads             = h->payloads;
    out->nals                 = h->nals;
    out->access_units         = h->access_units;
    out->keyframes            = h->keyframes;
    out->skipped_no_ref       = h->skipped;
    out->empty_payloads       = h->empty;
    out->nals_in_last_payload = h->last_payload_nals;
}
