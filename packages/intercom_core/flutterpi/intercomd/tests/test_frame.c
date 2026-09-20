#include "test.h"
#include "syncn/frame.h"
#include "syncn/alaw.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ collector --- */

#define MAX_CAPTURED 64

typedef struct {
    syncn_channel ch;
    size_t        len;
    uint8_t       data[512];
} captured;

typedef struct {
    captured frames[MAX_CAPTURED];
    int      count;
    int      stop_after; /* 0 = never stop */
} collector;

static int collect(syncn_channel ch, const uint8_t *payload, size_t len, void *user)
{
    collector *c = user;
    if (c->count < MAX_CAPTURED) {
        c->frames[c->count].ch  = ch;
        c->frames[c->count].len = len;
        if (payload && len <= sizeof c->frames[0].data)
            memcpy(c->frames[c->count].data, payload, len);
        c->count++;
    }
    if (c->stop_after && c->count >= c->stop_after)
        return 99; /* arbitrary non-zero sentinel */
    return 0;
}

/* Build a frame into `out`, returning its total length. */
static size_t build(uint8_t *out, syncn_channel ch, const void *payload, uint32_t len)
{
    syncn_frame_write_header(out, ch, len);
    if (len)
        memcpy(out + SYNCN_FRAME_HEADER_SIZE, payload, len);
    return SYNCN_FRAME_HEADER_SIZE + len;
}

/* ---------------------------------------------------------------- tests --- */

void suite_frame(void)
{
    SUITE("frame: header encoding");
    {
        uint8_t h[8];

        syncn_frame_write_header(h, SYNCN_CH_CONTROL, 0x12345678u);
        const uint8_t want_ctl[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0x78, 0x56, 0x34, 0x12};
        CHECK_EQ_MEM(h, want_ctl, 8);

        /* The exact audio header from section 9 of the protocol reference:
         * CC CC CC CC A0 00 00 00, followed by 160 payload bytes. */
        syncn_frame_write_header(h, SYNCN_CH_AUDIO, SYNCN_AUDIO_FRAME_BYTES);
        const uint8_t want_audio[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xA0, 0x00, 0x00, 0x00};
        CHECK_EQ_MEM(h, want_audio, 8);
        CHECK_EQ_INT(SYNCN_FRAME_HEADER_SIZE + SYNCN_AUDIO_FRAME_BYTES, 168);

        syncn_frame_write_header(h, SYNCN_CH_VIDEO, 0);
        const uint8_t want_video[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0, 0, 0, 0};
        CHECK_EQ_MEM(h, want_video, 8);

        CHECK(syncn_channel_valid(0xAA));
        CHECK(syncn_channel_valid(0xBB));
        CHECK(syncn_channel_valid(0xCC));
        CHECK(!syncn_channel_valid(0xAB));
        CHECK(!syncn_channel_valid(0x00));
        CHECK(strcmp(syncn_channel_name(SYNCN_CH_AUDIO), "audio") == 0);
    }

    SUITE("frame: single frame");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[256];
        const char *json = "{\"command\":\"Answer\"}";
        const size_t n = build(buf, SYNCN_CH_CONTROL, json, (uint32_t)strlen(json));

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_INT(c.frames[0].ch, SYNCN_CH_CONTROL);
        CHECK_EQ_INT(c.frames[0].len, strlen(json));
        CHECK_EQ_MEM(c.frames[0].data, json, strlen(json));
        syncn_parser_free(p);
    }

    SUITE("frame: three channels back to back");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[1024];
        uint8_t audio[SYNCN_AUDIO_FRAME_BYTES];
        const uint8_t nal[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};
        const char *json = "{\"command\":\"StartTalk\"}";

        syncn_alaw_fill_silence(audio, sizeof audio);

        size_t n = 0;
        n += build(buf + n, SYNCN_CH_CONTROL, json, (uint32_t)strlen(json));
        n += build(buf + n, SYNCN_CH_VIDEO, nal, (uint32_t)sizeof nal);
        n += build(buf + n, SYNCN_CH_AUDIO, audio, (uint32_t)sizeof audio);

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 3);
        CHECK_EQ_INT(c.frames[0].ch, SYNCN_CH_CONTROL);
        CHECK_EQ_INT(c.frames[1].ch, SYNCN_CH_VIDEO);
        CHECK_EQ_INT(c.frames[2].ch, SYNCN_CH_AUDIO);
        CHECK_EQ_INT(c.frames[2].len, SYNCN_AUDIO_FRAME_BYTES);
        CHECK_EQ_MEM(c.frames[1].data, nal, sizeof nal);
        syncn_parser_free(p);
    }

    SUITE("frame: arrives one byte at a time");
    {
        /* TCP will happily hand us a header split across four reads. */
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[512];
        uint8_t audio[SYNCN_AUDIO_FRAME_BYTES];
        syncn_alaw_fill_silence(audio, sizeof audio);
        const size_t n = build(buf, SYNCN_CH_AUDIO, audio, (uint32_t)sizeof audio);

        for (size_t i = 0; i < n; i++) {
            CHECK_EQ_INT(syncn_parser_feed(p, buf + i, 1, collect, &c), 0);
            /* Nothing may be emitted until the very last byte lands. */
            if (i + 1 < n)
                CHECK_EQ_INT(c.count, 0);
        }
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_INT(c.frames[0].len, SYNCN_AUDIO_FRAME_BYTES);
        syncn_parser_free(p);
    }

    SUITE("frame: resynchronises after garbage");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[512];
        const char *json = "{\"command\":\"HangUp\"}";

        /* Junk, including bytes that look almost like a marker, then a real
         * frame. The parser must find the frame rather than give up. */
        const uint8_t junk[] = {0x01, 0xAA, 0xAA, 0xAA, 0x02, 0xFF, 0xBB, 0xBB};
        memcpy(buf, junk, sizeof junk);
        const size_t n = sizeof junk +
            build(buf + sizeof junk, SYNCN_CH_CONTROL, json, (uint32_t)strlen(json));

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_MEM(c.frames[0].data, json, strlen(json));

        syncn_parser_stats stats;
        syncn_parser_stats_get(p, &stats);
        CHECK(stats.resync_events > 0);
        CHECK_EQ_INT(stats.frames_parsed, 1);
        syncn_parser_free(p);
    }

    SUITE("frame: absurd length is treated as a bad header, not obeyed");
    {
        /* A length field of 0xFFFFFFFF must not cause an allocation attempt or
         * a permanent stall. It is far more likely to be four marker bytes that
         * happened to appear inside a payload. */
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[512];
        const char *json = "{\"command\":\"OpenDoor\"}";

        uint8_t bad[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(buf, bad, 8);
        const size_t n = 8 + build(buf + 8, SYNCN_CH_CONTROL, json, (uint32_t)strlen(json));

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_MEM(c.frames[0].data, json, strlen(json));

        syncn_parser_stats stats;
        syncn_parser_stats_get(p, &stats);
        CHECK(stats.oversize_headers > 0);
        syncn_parser_free(p);
    }

    SUITE("frame: marker bytes inside a payload do not split the stream");
    {
        /* H.264 payloads are arbitrary binary and will contain AA AA AA AA
         * eventually. Framing is by length, so this must be transparent. */
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t payload[32];
        uint8_t buf[256];

        memset(payload, 0x11, sizeof payload);
        payload[10] = payload[11] = payload[12] = payload[13] = 0xAA;
        payload[20] = payload[21] = payload[22] = payload[23] = 0xCC;

        const size_t n = build(buf, SYNCN_CH_VIDEO, payload, (uint32_t)sizeof payload);
        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_INT(c.frames[0].len, sizeof payload);
        CHECK_EQ_MEM(c.frames[0].data, payload, sizeof payload);

        syncn_parser_stats stats;
        syncn_parser_stats_get(p, &stats);
        CHECK_EQ_INT(stats.resync_events, 0);
        syncn_parser_free(p);
    }

    SUITE("frame: zero-length payload");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[16];
        const size_t n = build(buf, SYNCN_CH_VIDEO, NULL, 0);

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 0);
        CHECK_EQ_INT(c.count, 1);
        CHECK_EQ_INT(c.frames[0].len, 0);
        syncn_parser_free(p);
    }

    SUITE("frame: callback can stop the feed");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        c.stop_after = 2;
        uint8_t buf[1024];
        const char *json = "{\"command\":\"Answer\"}";

        size_t n = 0;
        for (int i = 0; i < 5; i++)
            n += build(buf + n, SYNCN_CH_CONTROL, json, (uint32_t)strlen(json));

        CHECK_EQ_INT(syncn_parser_feed(p, buf, n, collect, &c), 99);
        CHECK_EQ_INT(c.count, 2);

        /* The unconsumed frames are still buffered and come out on the next
         * call, so stopping early loses nothing. */
        c.stop_after = 0;
        CHECK_EQ_INT(syncn_parser_feed(p, NULL, 0, collect, &c), 0);
        CHECK_EQ_INT(c.count, 5);
        syncn_parser_free(p);
    }

    SUITE("frame: sustained stream does not grow the buffer");
    {
        /* A call runs for minutes at 50 audio frames a second. If the buffer
         * crept upward per frame, a long call would eventually exhaust a 2 GB
         * panel. */
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[512];
        uint8_t audio[SYNCN_AUDIO_FRAME_BYTES];
        syncn_alaw_fill_silence(audio, sizeof audio);
        const size_t n = build(buf, SYNCN_CH_AUDIO, audio, (uint32_t)sizeof audio);

        syncn_parser_stats first = {0};
        for (int i = 0; i < 3000; i++) { /* a minute of audio */
            c.count = 0;
            syncn_parser_feed(p, buf, n, collect, &c);
            if (i == 10)
                syncn_parser_stats_get(p, &first);
        }

        syncn_parser_stats last;
        syncn_parser_stats_get(p, &last);
        CHECK_EQ_INT(last.frames_parsed, 3000);
        CHECK_EQ_INT(last.resync_events, 0);
        CHECK_EQ_INT(last.buffered, 0);
        CHECK_EQ_INT(last.buffer_capacity, first.buffer_capacity);
        syncn_parser_free(p);
    }

    SUITE("frame: reset drops partial state");
    {
        syncn_parser *p = syncn_parser_new(0);
        collector c = {0};
        uint8_t buf[512];
        uint8_t audio[SYNCN_AUDIO_FRAME_BYTES];
        syncn_alaw_fill_silence(audio, sizeof audio);
        const size_t n = build(buf, SYNCN_CH_AUDIO, audio, (uint32_t)sizeof audio);

        /* Half a frame, then a reconnect, then a whole frame. The leftover must
         * not corrupt the new connection's stream. */
        syncn_parser_feed(p, buf, n / 2, collect, &c);
        CHECK_EQ_INT(c.count, 0);
        syncn_parser_reset(p);
        syncn_parser_feed(p, buf, n, collect, &c);
        CHECK_EQ_INT(c.count, 1);
        syncn_parser_free(p);
    }

    SUITE("frame: payload larger than the configured ceiling is rejected");
    {
        syncn_parser *p = syncn_parser_new(1024); /* deliberately small */
        collector c = {0};
        uint8_t buf[8];

        syncn_frame_write_header(buf, SYNCN_CH_VIDEO, 2048);
        CHECK_EQ_INT(syncn_parser_feed(p, buf, 8, collect, &c), 0);
        CHECK_EQ_INT(c.count, 0);

        syncn_parser_stats stats;
        syncn_parser_stats_get(p, &stats);
        CHECK(stats.oversize_headers > 0);
        syncn_parser_free(p);
    }
}
