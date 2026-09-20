/*
 * The flutter-pi plugin's parsing and cache, tested without flutter-pi.
 *
 * Two things matter here. A frame line with any geometry missing or
 * inconsistent must not become a frame -- importing with a guessed stride
 * shears the picture, and almost-right is worse than nothing. And the cache
 * must be keyed on the buffer's identity, so that a three-buffer pool costs
 * three imports for ever rather than one per frame.
 */
#include "test.h"

#include "../plugins/flutter-pi/syncn_video_proto.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

void suite_video_proto(void)
{
    SUITE("video plugin: a well-formed frame line parses completely");
    {
        const sv_line l = sv_parse_line(
            "{\"event\":\"frame\",\"id\":41,\"width\":1280,\"height\":720,"
            "\"stride\":5120,\"size\":3686400,\"format\":\"BGRA8888\",\"pts_us\":0}");
        CHECK_EQ_INT(l.kind, SV_LINE_FRAME);
        CHECK_EQ_INT(l.id, 41);
        CHECK_EQ_INT(l.width, 1280);
        CHECK_EQ_INT(l.height, 720);
        CHECK_EQ_INT(l.stride, 5120);
        CHECK_EQ_INT(l.size, 3686400);
    }

    SUITE("video plugin: field order does not matter");
    {
        const sv_line l = sv_parse_line(
            "{\"size\":3686400,\"stride\":5120,\"height\":720,\"width\":1280,"
            "\"id\":7,\"event\":\"frame\"}");
        CHECK_EQ_INT(l.kind, SV_LINE_FRAME);
        CHECK_EQ_INT(l.id, 7);
        CHECK_EQ_INT(l.stride, 5120);
    }

    SUITE("video plugin: a frame with missing or inconsistent geometry is refused");
    {
        /* no stride */
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"frame\",\"id\":1,\"width\":640,\"height\":360,\"size\":921600}").kind, SV_LINE_OTHER);
        /* stride shorter than a row */
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"frame\",\"id\":1,\"width\":640,\"height\":360,\"stride\":1000,\"size\":921600}").kind, SV_LINE_OTHER);
        /* size smaller than stride*height */
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"frame\",\"id\":1,\"width\":640,\"height\":360,\"stride\":2560,\"size\":100}").kind, SV_LINE_OTHER);
        /* id 0 is the "no id" sentinel */
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"frame\",\"id\":0,\"width\":640,\"height\":360,\"stride\":2560,\"size\":921600}").kind, SV_LINE_OTHER);
        /* zero dimensions */
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"frame\",\"id\":1,\"width\":0,\"height\":360,\"stride\":2560,\"size\":921600}").kind, SV_LINE_OTHER);
    }

    SUITE("video plugin: reset and stopped are recognised, other events ignored");
    {
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"video_reset\",\"width\":640,\"height\":360}").kind, SV_LINE_RESET);
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"video_stopped\"}").kind, SV_LINE_STOPPED);
        CHECK_EQ_INT(sv_parse_line("{\"event\":\"incoming_call\",\"door\":\"1.2.3.4\"}").kind, SV_LINE_OTHER);
        CHECK_EQ_INT(sv_parse_line("{\"state\":\"idle\",\"video_frames\":12}").kind, SV_LINE_OTHER);
        CHECK_EQ_INT(sv_parse_line("").kind, SV_LINE_OTHER);
        CHECK_EQ_INT(sv_parse_line("not json at all").kind, SV_LINE_OTHER);
    }

    SUITE("video plugin: the id survives past 2^32");
    {
        const sv_line l = sv_parse_line(
            "{\"event\":\"frame\",\"id\":5000000000,\"width\":16,\"height\":16,\"stride\":64,\"size\":1024}");
        CHECK_EQ_INT(l.kind, SV_LINE_FRAME);
        CHECK(l.id == 5000000000ull);
    }

    SUITE("video plugin: the cache is keyed on the buffer, not the frame id");
    {
        /* Three real files stand in for the pool's three DMA-BUFs. Each has
         * its own inode; a fresh dup of one still has the same inode, which
         * is exactly the situation every frame presents. */
        sv_cache c;
        sv_cache_init(&c);

        int base[3];
        for (int i = 0; i < 3; i++) {
            base[i] = memfd_create("pool", MFD_CLOEXEC);
            CHECK(base[i] >= 0);
        }

        /* Twenty frames cycling over three buffers. */
        int imports = 0;
        for (int f = 0; f < 20; f++) {
            const int slot = f % 3;
            const int dupfd = dup(base[slot]);           /* what SCM_RIGHTS gives us */
            struct stat st;
            CHECK_EQ_INT(fstat(dupfd, &st), 0);

            sv_cache_entry *e = sv_cache_find(&c, st.st_dev, st.st_ino);
            if (!e) {
                e = sv_cache_insert(&c, st.st_dev, st.st_ino, dupfd, 16, 16, 64);
                CHECK(e != NULL);
                imports++;
            } else {
                close(dupfd);                             /* a duplicate: not needed */
            }
        }

        printf("        20 frames over a 3-buffer pool: %d imports, %llu cache hits\n",
               imports, (unsigned long long)c.hits);
        CHECK_EQ_INT(imports, 3);
        CHECK_EQ_INT(c.hits, 17);
        CHECK_EQ_INT(sv_cache_count(&c), 3);

        /* A reset drops all of them and closes our descriptors. */
        sv_cache_clear(&c, NULL, NULL);
        CHECK_EQ_INT(sv_cache_count(&c), 0);
        CHECK_EQ_INT(c.evictions, 3);

        /* The pool's own descriptors are untouched: the daemon owns those. */
        for (int i = 0; i < 3; i++) {
            struct stat st;
            CHECK_EQ_INT(fstat(base[i], &st), 0);
            close(base[i]);
        }
    }

    SUITE("video plugin: a pool deeper than the cache is refused, not overflowed");
    {
        sv_cache c;
        sv_cache_init(&c);
        for (int i = 0; i < SV_CACHE_MAX; i++)
            CHECK(sv_cache_insert(&c, 1, (ino_t)(100 + i), -1, 16, 16, 64) != NULL);
        CHECK(sv_cache_insert(&c, 1, 999, -1, 16, 16, 64) == NULL);
        CHECK_EQ_INT(sv_cache_count(&c), SV_CACHE_MAX);
        sv_cache_clear(&c, NULL, NULL);
    }
}
