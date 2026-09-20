#include "test.h"
#include "syncn/h264.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- builders --- */

/* Append an Annex-B NAL of `type` with `body` filler bytes. */
static size_t put_nal(uint8_t *buf, size_t off, syncn_nal_type type,
                      size_t body, bool four_byte)
{
    if (four_byte) {
        buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 1;
    } else {
        buf[off++] = 0; buf[off++] = 0; buf[off++] = 1;
    }
    buf[off++] = (uint8_t)(0x60 | (uint8_t)type); /* nal_ref_idc | type */
    for (size_t i = 0; i < body; i++)
        buf[off++] = (uint8_t)(0xA0 + (i & 0x0F));
    return off;
}

/* ------------------------------------------------------------- collector --- */

typedef struct {
    int  count;
    int  keyframes;
    size_t last_len;
    bool last_was_key;
} collector;

static void on_au(const uint8_t *data, size_t len, bool keyframe, void *user)
{
    (void)data;
    collector *c = user;
    c->count++;
    if (keyframe)
        c->keyframes++;
    c->last_len     = len;
    c->last_was_key = keyframe;
}

/* ---------------------------------------------------------------- tests --- */

void suite_h264(void)
{
    uint8_t buf[8192];
    syncn_nal nals[16];

    SUITE("h264: splitting Annex-B");
    {
        size_t n = put_nal(buf, 0, SYNCN_NAL_SPS, 12, true);
        n = put_nal(buf, n, SYNCN_NAL_PPS, 4, true);
        n = put_nal(buf, n, SYNCN_NAL_IDR, 100, true);

        CHECK_EQ_INT(syncn_h264_split(buf, n, nals, 16), 3);
        CHECK_EQ_INT(nals[0].type, SYNCN_NAL_SPS);
        CHECK_EQ_INT(nals[1].type, SYNCN_NAL_PPS);
        CHECK_EQ_INT(nals[2].type, SYNCN_NAL_IDR);

        /* Lengths must include the start code and reach the next one. */
        CHECK_EQ_INT(nals[0].len, 4 + 1 + 12);
        CHECK_EQ_INT(nals[1].len, 4 + 1 + 4);
        CHECK_EQ_INT(nals[2].len, 4 + 1 + 100);
    }

    SUITE("h264: three-byte start codes and mixed lengths");
    {
        size_t n = put_nal(buf, 0, SYNCN_NAL_SPS, 8, false);
        n = put_nal(buf, n, SYNCN_NAL_SLICE, 20, true);
        n = put_nal(buf, n, SYNCN_NAL_SLICE, 6, false);

        CHECK_EQ_INT(syncn_h264_split(buf, n, nals, 16), 3);
        CHECK_EQ_INT(nals[0].type, SYNCN_NAL_SPS);
        CHECK_EQ_INT(nals[1].type, SYNCN_NAL_SLICE);
        CHECK_EQ_INT(nals[2].type, SYNCN_NAL_SLICE);
    }

    SUITE("h264: bytes before the first start code are skipped");
    {
        buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE;
        const size_t n = put_nal(buf, 3, SYNCN_NAL_IDR, 10, true);
        CHECK_EQ_INT(syncn_h264_split(buf, n, nals, 16), 1);
        CHECK_EQ_INT(nals[0].type, SYNCN_NAL_IDR);
    }

    SUITE("h264: degenerate input does not misbehave");
    {
        CHECK_EQ_INT(syncn_h264_split(NULL, 0, nals, 16), 0);
        CHECK_EQ_INT(syncn_h264_split(buf, 0, nals, 16), 0);

        const uint8_t no_sc[] = {0x41, 0x9A, 0x00, 0x11};
        CHECK_EQ_INT(syncn_h264_split(no_sc, sizeof no_sc, nals, 16), 0);

        /* A start code with nothing after it must not read past the end. */
        const uint8_t bare[] = {0, 0, 0, 1};
        CHECK_EQ_INT(syncn_h264_split(bare, sizeof bare, nals, 16), 1);
        CHECK_EQ_INT(nals[0].len, 4);
    }

    SUITE("h264: the door's keyframe carries SPS + PPS + IDR in one payload");
    {
        /*
         * The correction that matters. The protocol document said exactly one
         * NAL per frame; the real door packs three into every keyframe. A
         * reader built on the document mangles every keyframe — and a keyframe
         * is what the decoder needs both to start and to recover.
         */
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};

        size_t n = put_nal(buf, 0, SYNCN_NAL_SPS, 20, true);
        n = put_nal(buf, n, SYNCN_NAL_PPS, 6, true);
        n = put_nal(buf, n, SYNCN_NAL_IDR, 900, true);

        syncn_h264_feed(h, buf, n, on_au, &c);

        CHECK_EQ_INT(c.count, 1);          /* one access unit, not three */
        CHECK(c.last_was_key);
        CHECK_EQ_INT(c.last_len, n);       /* passed through whole */

        syncn_h264_stats s;
        syncn_h264_get_stats(h, &s);
        CHECK_EQ_INT(s.nals_in_last_payload, 3);
        CHECK_EQ_INT(s.keyframes, 1);
        CHECK(syncn_h264_have_parameter_sets(h));
        syncn_h264_destroy(h);
    }

    SUITE("h264: nothing is decoded before the first keyframe");
    {
        /* P-slices arriving before any IDR reference frames that do not exist.
         * The real capture began with two of exactly these. */
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};

        CHECK(syncn_h264_waiting_for_keyframe(h));

        for (int i = 0; i < 5; i++) {
            const size_t n = put_nal(buf, 0, SYNCN_NAL_SLICE, 300, true);
            syncn_h264_feed(h, buf, n, on_au, &c);
        }
        CHECK_EQ_INT(c.count, 0);

        size_t n = put_nal(buf, 0, SYNCN_NAL_SPS, 20, true);
        n = put_nal(buf, n, SYNCN_NAL_PPS, 6, true);
        n = put_nal(buf, n, SYNCN_NAL_IDR, 900, true);
        syncn_h264_feed(h, buf, n, on_au, &c);

        CHECK_EQ_INT(c.count, 1);
        CHECK(!syncn_h264_waiting_for_keyframe(h));

        syncn_h264_stats s;
        syncn_h264_get_stats(h, &s);
        CHECK_EQ_INT(s.skipped_no_ref, 5);
        syncn_h264_destroy(h);
    }

    SUITE("h264: after a loss, nothing is decoded until the next keyframe");
    {
        /*
         * There is no way to ask this door for an IDR — no retransmit, no
         * feedback channel. So after a gap, every P-slice corrupts the picture
         * until the next keyframe arrives anyway. Waiting costs under a second
         * at 20 fps with an IDR every 15 frames, and looks far better.
         */
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};

        size_t key = put_nal(buf, 0, SYNCN_NAL_SPS, 20, true);
        key = put_nal(buf, key, SYNCN_NAL_PPS, 6, true);
        key = put_nal(buf, key, SYNCN_NAL_IDR, 900, true);
        syncn_h264_feed(h, buf, key, on_au, &c);
        CHECK_EQ_INT(c.count, 1);

        uint8_t p[2048];
        const size_t plen = put_nal(p, 0, SYNCN_NAL_SLICE, 400, true);
        syncn_h264_feed(h, p, plen, on_au, &c);
        CHECK_EQ_INT(c.count, 2);

        syncn_h264_note_loss(h);
        CHECK(syncn_h264_waiting_for_keyframe(h));

        for (int i = 0; i < 14; i++)
            syncn_h264_feed(h, p, plen, on_au, &c);
        CHECK_EQ_INT(c.count, 2); /* nothing emitted */

        syncn_h264_feed(h, buf, key, on_au, &c);
        CHECK_EQ_INT(c.count, 3);
        CHECK(c.last_was_key);
        syncn_h264_destroy(h);
    }

    SUITE("h264: parameter sets are kept for decoder restart");
    {
        /*
         * On this stream SPS and PPS only travel with keyframes. Without a
         * cached copy, a decoder restart shows nothing until the door's next
         * IDR — a visible stall, every time.
         */
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};

        CHECK(!syncn_h264_have_parameter_sets(h));

        size_t n = put_nal(buf, 0, SYNCN_NAL_SPS, 20, true);
        n = put_nal(buf, n, SYNCN_NAL_PPS, 6, true);
        n = put_nal(buf, n, SYNCN_NAL_IDR, 100, true);
        syncn_h264_feed(h, buf, n, on_au, &c);

        CHECK(syncn_h264_have_parameter_sets(h));

        uint8_t out[512];
        const size_t got = syncn_h264_parameter_sets(h, out, sizeof out);
        CHECK_EQ_INT(got, (4 + 1 + 20) + (4 + 1 + 6));

        /* What comes back must be valid Annex-B in SPS, PPS order. */
        CHECK_EQ_INT(syncn_h264_split(out, got, nals, 16), 2);
        CHECK_EQ_INT(nals[0].type, SYNCN_NAL_SPS);
        CHECK_EQ_INT(nals[1].type, SYNCN_NAL_PPS);

        /* A buffer too small writes nothing rather than half a parameter set. */
        uint8_t tiny[8] = {0};
        CHECK_EQ_INT(syncn_h264_parameter_sets(h, tiny, sizeof tiny), 0);
        CHECK_EQ_INT(tiny[0], 0);
        syncn_h264_destroy(h);
    }

    SUITE("h264: replay of the measured door stream");
    {
        /*
         * The shape of the real 60-second capture from 192.168.100.193:
         * 1200 payloads carrying 1360 NAL units — 1120 P-slices plus 80 each of
         * SPS, PPS and IDR, one keyframe every 15 frames.
         */
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};
        uint8_t p[2048];
        const size_t plen = put_nal(p, 0, SYNCN_NAL_SLICE, 900, true);

        size_t klen = put_nal(buf, 0, SYNCN_NAL_SPS, 20, true);
        klen = put_nal(buf, klen, SYNCN_NAL_PPS, 6, true);
        klen = put_nal(buf, klen, SYNCN_NAL_IDR, 1200, true);

        for (int frame = 0; frame < 1200; frame++) {
            if (frame % 15 == 0)
                syncn_h264_feed(h, buf, klen, on_au, &c);
            else
                syncn_h264_feed(h, p, plen, on_au, &c);
        }

        syncn_h264_stats s;
        syncn_h264_get_stats(h, &s);
        CHECK_EQ_INT(s.payloads, 1200);
        CHECK_EQ_INT(s.nals, 1360);          /* matches the real capture */
        CHECK_EQ_INT(s.keyframes, 80);
        CHECK_EQ_INT(s.access_units, 1200);  /* every frame decodable */
        CHECK_EQ_INT(s.skipped_no_ref, 0);
        CHECK_EQ_INT(c.count, 1200);
        CHECK_EQ_INT(c.keyframes, 80);
        syncn_h264_destroy(h);
    }

    SUITE("h264: empty and undecodable payloads are counted, not forwarded");
    {
        syncn_h264 *h = syncn_h264_create();
        collector c = {0};

        syncn_h264_feed(h, NULL, 0, on_au, &c);
        syncn_h264_feed(h, buf, 0, on_au, &c);

        const uint8_t junk[] = {0x41, 0x9A, 0x33};
        syncn_h264_feed(h, junk, sizeof junk, on_au, &c);

        CHECK_EQ_INT(c.count, 0);
        syncn_h264_stats s;
        syncn_h264_get_stats(h, &s);
        CHECK_EQ_INT(s.empty_payloads, 3);
        CHECK_EQ_INT(s.access_units, 0);
        syncn_h264_destroy(h);
    }
}
