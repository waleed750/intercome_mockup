/*
 * The video pipeline, end to end.
 *
 * This is the suite that exists because of the original bug report: "video
 * pauses when using the manufacturer's drivers". Everything eliminated so far
 * said the same thing — the stream is fine, the network is fine, CMA is fine,
 * and mpi_dec_test decodes the panel's own captured door stream at 864 fps.
 * What is left is a consumer that stops giving buffers back.
 *
 * So these tests do not check that pixels are correct. They check that the
 * pipeline cannot be made to wait. Every hostile thing a consumer can do —
 * hold everything, never answer, vanish mid-frame, return buffers it was never
 * given, come back at a different resolution — is applied here, and after each
 * one the pipeline must still be delivering frames.
 *
 * The decoder and the colour converter are linked in as fakes; the pipeline is
 * the real source. The fake decoder aborts on a double release, so the rule
 * that matters most on real hardware is enforced rather than asserted.
 */
#include "test.h"
#include "fake_video_hw.h"

#include "syncn/video.h"
#include "syncn/h264.h"

#include <stdlib.h>
#include <string.h>

extern int fake_dmabuf_live;

/* --------------------------------------------------------- a fake consumer --- */

#define HELD_MAX 64

typedef struct {
    bool     take;             /* whether the sink accepts frames at all    */
    bool     recycle;          /* whether it gives them back immediately    */
    unsigned taken;
    unsigned resets;
    uint32_t reset_w, reset_h;
    uint64_t held[HELD_MAX];
    unsigned held_count;
    uint64_t last_id;
    uint32_t last_w, last_h;
} consumer;

static bool on_frame(const syncn_frame_buffer *buf, uint64_t pts_us, void *user)
{
    consumer *c = user;
    (void)pts_us;
    if (!c->take)
        return false;

    c->taken++;
    c->last_id = buf->id;
    c->last_w  = buf->width;
    c->last_h  = buf->height;
    if (c->held_count < HELD_MAX)
        c->held[c->held_count++] = buf->id;
    return true;
}

static void on_reset(unsigned w, unsigned h, void *user)
{
    consumer *c = user;
    c->resets++;
    c->reset_w = w;
    c->reset_h = h;
}

/* Give everything back, as a well-behaved interface does once it has drawn. */
static void drain(syncn_video *v, consumer *c)
{
    for (unsigned i = 0; i < c->held_count; i++)
        syncn_video_recycle(v, c->held[i]);
    c->held_count = 0;
}

/* ------------------------------------------------------- a synthetic stream --- */

/* Minimal but real Annex-B: the parser is the genuine one, so the bytes have to
 * satisfy it. SPS + PPS + IDR for a keyframe, a lone P-slice otherwise. */
static size_t make_au(uint8_t *out, size_t cap, bool keyframe)
{
    static const uint8_t KEY[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e, 0xd9,   /* SPS */
        0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,         /* PPS */
        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33,   /* IDR */
    };
    static const uint8_t DELTA[] = {
        0, 0, 0, 1, 0x41, 0x9a, 0x00, 0x11, 0x22,   /* non-IDR slice */
    };
    const uint8_t *src = keyframe ? KEY : DELTA;
    const size_t   len = keyframe ? sizeof KEY : sizeof DELTA;
    if (len > cap)
        return 0;
    memcpy(out, src, len);
    return len;
}

static void feed_au(syncn_video *v, bool keyframe)
{
    uint8_t buf[64];
    const size_t n = make_au(buf, sizeof buf, keyframe);
    syncn_video_feed(v, buf, n);
}

static syncn_video *fresh(consumer *c, unsigned buffers)
{
    memset(c, 0, sizeof *c);
    c->take = true;

    memset(&fake_hw, 0, sizeof fake_hw);
    fake_hw.width  = 640;
    fake_hw.height = 360;

    const syncn_video_config cfg = { .buffers = buffers, .fast_output = false };
    return syncn_video_create(&cfg, on_frame, on_reset, c);
}

/* ------------------------------------------------------------------ tests --- */

void suite_video(void)
{
    SUITE("video: a keyframe becomes a frame the interface can import");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        /* Nothing before the first keyframe: a P-slice references frames the
         * decoder does not have. */
        feed_au(v, false);
        CHECK_EQ_INT(syncn_video_pump(v), 0);
        CHECK_EQ_INT(c.taken, 0);

        feed_au(v, true);
        CHECK_EQ_INT(syncn_video_pump(v), 1);
        CHECK_EQ_INT(c.taken, 1);
        CHECK_EQ_INT(c.last_w, 640);
        CHECK_EQ_INT(c.last_h, 360);
        CHECK(c.last_id != 0);

        /* The interface is told the geometry once, when the pool appears. */
        CHECK_EQ_INT(c.resets, 1);
        CHECK_EQ_INT(c.reset_w, 640);
        CHECK_EQ_INT(c.reset_h, 360);

        syncn_video_destroy(v);
    }

    SUITE("video: strides are passed through, not dimensions");
    {
        /* The single easiest thing to get wrong in this path. The decoder pads
         * its output, and converting by the visible size shears the picture
         * into something that looks almost right. */
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);

        CHECK_EQ_INT(fake_hw.last_src.width,  640);
        CHECK_EQ_INT(fake_hw.last_src.vstride, 376);   /* padded, not 360 */
        CHECK(fake_hw.last_src.vstride > fake_hw.last_src.height);

        /* RGA counts horizontal stride in pixels; the pool aligns in bytes. */
        CHECK_EQ_INT(fake_hw.last_dst.width, 640);
        CHECK(fake_hw.last_dst.hstride >= 640);
        CHECK_EQ_INT(fake_hw.last_dst.hstride % 16, 0);  /* 64-byte / 4bpp */

        syncn_video_destroy(v);
    }

    SUITE("video: a consumer that never returns a buffer drops frames, not the daemon");
    {
        /* The reported fault, reproduced. An interface takes every buffer and
         * answers nothing. The picture stops updating — and the pipeline keeps
         * running, so audio and control carry on and the log names the cause. */
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        for (int i = 0; i < 40; i++) {
            feed_au(v, false);
            syncn_video_pump(v);      /* must return, every time */
        }

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);

        /* Exactly the pool's worth got out, and everything after was dropped. */
        CHECK_EQ_INT(st.published, 3);
        CHECK_EQ_INT(c.taken, 3);
        CHECK(st.dropped_no_buffer > 0);

        /* And the decoder was never left holding anything — which is the
         * difference between a dropped frame and a dead pipeline. */
        CHECK_EQ_INT(fake_hw.double_release, 0);
        CHECK(fake_hw.peak_inflight <= 2);

        /* Now the interface catches up. The picture must resume without a
         * restart: this is what a slow UI recovering looks like. */
        drain(v, &c);
        feed_au(v, true);
        CHECK_EQ_INT(syncn_video_pump(v), 1);

        syncn_video_destroy(v);
    }

    SUITE("video: a well-behaved consumer runs indefinitely on three buffers");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);
        drain(v, &c);

        /* 200 frames is ten seconds of the door's 20 fps. Nothing may
         * accumulate over it. */
        for (int i = 0; i < 200; i++) {
            feed_au(v, false);
            CHECK_EQ_INT(syncn_video_pump(v), 1);
            drain(v, &c);
        }

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK_EQ_INT(st.published, 201);
        CHECK_EQ_INT(st.dropped_no_buffer, 0);
        CHECK_EQ_INT(st.dropped_sink, 0);
        CHECK_EQ_INT(st.outstanding, 0);

        syncn_video_destroy(v);
    }

    SUITE("video: a sink that refuses gets its buffer straight back");
    {
        /* No interface connected. Every frame is dropped, and every buffer
         * must return to the pool — otherwise the first UI to connect finds an
         * empty pool and never gets a picture. */
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;
        c.take = false;

        feed_au(v, true);
        for (int i = 0; i < 30; i++) {
            feed_au(v, false);
            syncn_video_pump(v);
        }

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK_EQ_INT(st.published, 0);
        CHECK(st.dropped_sink > 0);
        CHECK_EQ_INT(st.dropped_no_buffer, 0);   /* nothing was ever stranded */
        CHECK_EQ_INT(st.outstanding, 0);

        /* The UI arrives. It gets a picture on the next frame. */
        c.take = true;
        feed_au(v, false);
        CHECK_EQ_INT(syncn_video_pump(v), 1);

        syncn_video_destroy(v);
    }

    SUITE("video: a conversion failure loses the frame, not the buffer");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        fake_hw.convert_fails = true;
        feed_au(v, true);
        for (int i = 0; i < 20; i++) {
            feed_au(v, false);
            syncn_video_pump(v);
        }

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK(st.dropped_convert > 0);
        CHECK_EQ_INT(st.published, 0);
        CHECK_EQ_INT(st.outstanding, 0);
        /* The decoder frame is released whether conversion worked or not —
         * RGA is synchronous, so by then the pixels are read either way. */
        CHECK_EQ_INT(fake_hw.double_release, 0);

        fake_hw.convert_fails = false;
        feed_au(v, false);
        CHECK_EQ_INT(syncn_video_pump(v), 1);

        syncn_video_destroy(v);
    }

    SUITE("video: the interface vanishing reclaims everything it held");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);
        feed_au(v, false);
        syncn_video_pump(v);
        CHECK_EQ_INT(c.taken, 2);

        /* The UI is killed while holding two buffers. It keeps its own
         * descriptors, so the memory is safe; the slots are ours again. */
        syncn_video_consumer_gone(v);

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK_EQ_INT(st.outstanding, 0);

        /* A new UI connects and immediately gets a full pool. */
        c.held_count = 0;
        for (int i = 0; i < 3; i++) {
            feed_au(v, false);
            CHECK_EQ_INT(syncn_video_pump(v), 1);
        }

        syncn_video_destroy(v);
    }

    SUITE("video: a stale recycle from the old pool is ignored, not obeyed");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);
        const uint64_t old_id = c.last_id;
        CHECK(old_id != 0);

        /* The door switches resolution mid-call. */
        fake_hw.width  = 1280;
        fake_hw.height = 720;
        feed_au(v, true);
        syncn_video_pump(v);

        CHECK_EQ_INT(c.resets, 2);
        CHECK_EQ_INT(c.reset_w, 1280);
        CHECK_EQ_INT(c.last_w, 1280);
        CHECK(c.last_id > old_id);

        /* The old UI's late release arrives. It must not free a slot of the
         * new pool — that slot is being scanned out. */
        const unsigned before = syncn_video_pump(v), unused = before;
        (void)unused;
        syncn_video_stats a, b;
        syncn_video_get_stats(v, &a);
        syncn_video_recycle(v, old_id);
        syncn_video_get_stats(v, &b);
        CHECK_EQ_INT(a.outstanding, b.outstanding);

        syncn_video_destroy(v);
    }

    SUITE("video: a refused access unit makes the stream wait for a keyframe");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);
        drain(v, &c);

        /* The decoder rejects one. Feeding it P-slices from here would build a
         * picture on a reference it never received. */
        fake_hw.submit_fails = true;
        feed_au(v, false);
        syncn_video_pump(v);
        fake_hw.submit_fails = false;

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK(st.waiting_for_keyframe);

        /* Deltas alone do not restart it. */
        for (int i = 0; i < 10; i++) {
            feed_au(v, false);
            CHECK_EQ_INT(syncn_video_pump(v), 0);
            drain(v, &c);
        }

        feed_au(v, true);
        CHECK_EQ_INT(syncn_video_pump(v), 1);
        syncn_video_get_stats(v, &st);
        CHECK(!st.waiting_for_keyframe);

        syncn_video_destroy(v);
    }

    SUITE("video: reset returns to a clean stream without leaking buffers");
    {
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        syncn_video_pump(v);
        feed_au(v, false);
        syncn_video_pump(v);

        syncn_video_reset(v);
        CHECK(fake_hw.resets > 0);

        syncn_video_stats st;
        syncn_video_get_stats(v, &st);
        CHECK_EQ_INT(st.outstanding, 0);
        CHECK(st.waiting_for_keyframe);

        c.held_count = 0;
        feed_au(v, true);
        CHECK_EQ_INT(syncn_video_pump(v), 1);

        syncn_video_destroy(v);
    }

    SUITE("video: the stats line names the cause of a stall");
    {
        consumer c;
        syncn_video *v = fresh(&c, 2);
        CHECK(v != NULL);
        if (!v)
            return;

        feed_au(v, true);
        for (int i = 0; i < 10; i++) {
            feed_au(v, false);
            syncn_video_pump(v);
        }

        char line[512];
        syncn_video_format_stats(v, line, sizeof line);
        printf("        %s\n", line);

        /* A technician reading journalctl has to be able to tell a stalled
         * picture from a stalled stream without attaching a debugger. */
        CHECK(strstr(line, "640x360") != NULL);
        CHECK(strstr(line, "buffer=") != NULL);
        CHECK(strstr(line, "shown=2") != NULL);

        /* A buffer too small truncates rather than overflowing. */
        char tiny[16];
        memset(tiny, 'x', sizeof tiny);
        syncn_video_format_stats(v, tiny, sizeof tiny);
        CHECK(tiny[sizeof tiny - 1] == '\0');

        syncn_video_destroy(v);
    }

    SUITE("video: no descriptors are leaked across the pipeline's life");
    {
        CHECK_EQ_INT(fake_dmabuf_live, 0);

        for (int round = 0; round < 3; round++) {
            consumer c;
            syncn_video *v = fresh(&c, 3);
            CHECK(v != NULL);
            if (!v)
                break;
            feed_au(v, true);
            syncn_video_pump(v);
            drain(v, &c);

            /* Include a resolution change, which builds a second pool. */
            fake_hw.width = 1280; fake_hw.height = 720;
            feed_au(v, true);
            syncn_video_pump(v);
            drain(v, &c);

            syncn_video_destroy(v);
            CHECK_EQ_INT(fake_dmabuf_live, 0);
        }
    }

    SUITE("video: missing hardware is refused at startup, not per frame");
    {
        consumer c;
        memset(&c, 0, sizeof c);
        memset(&fake_hw, 0, sizeof fake_hw);
        fake_hw.width = 640; fake_hw.height = 360;

        fake_hw.decoder_create_fails = true;
        CHECK(syncn_video_create(NULL, on_frame, on_reset, &c) == NULL);
        fake_hw.decoder_create_fails = false;

        fake_hw.rga_create_fails = true;
        CHECK(syncn_video_create(NULL, on_frame, on_reset, &c) == NULL);
        fake_hw.rga_create_fails = false;

        /* And a pipeline with nowhere to send frames is refused outright. */
        CHECK(syncn_video_create(NULL, NULL, NULL, &c) == NULL);

        CHECK_EQ_INT(fake_dmabuf_live, 0);
    }

    SUITE("video: a buffer id from a previous daemon process is ignored quietly");
    {
        /* After a deploy the interface still holds the old process's last
         * frame and returns its id -- one the new process's counter has not
         * reached. It must not be reported as a bad release. */
        consumer c;
        syncn_video *v = fresh(&c, 3);
        CHECK(v != NULL);
        if (!v)
            return;
        feed_au(v, true);
        syncn_video_pump(v);

        syncn_video_stats a, b;
        syncn_video_get_stats(v, &a);
        CHECK(syncn_video_recycle(v, 6074000));      /* far beyond anything issued */
        syncn_video_get_stats(v, &b);
        CHECK_EQ_INT(a.outstanding, b.outstanding); /* nothing freed */

        syncn_video_destroy(v);
    }
}
