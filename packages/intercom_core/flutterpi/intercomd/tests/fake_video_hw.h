#ifndef SYNCN_FAKE_VIDEO_HW_H
#define SYNCN_FAKE_VIDEO_HW_H

#include "syncn/rga.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAKE_MAX_QUEUE 16

typedef struct {
    uint32_t width, height, hstride, vstride;
    uint64_t pts_us;
    size_t   bytes;
} fake_frame;

/* Knobs the tests turn, and what the fakes saw. One global because the
 * pipeline creates its own decoder and converter and the test has no other
 * handle on them. */
typedef struct {
    /* inputs */
    uint32_t width, height;
    bool     submit_fails;
    bool     convert_fails;
    bool     decoder_create_fails;
    bool     rga_create_fails;

    /* observations */
    uint64_t        conversions;
    uint64_t        resets;
    uint64_t        double_release;
    unsigned        peak_inflight;
    syncn_rga_image last_src, last_dst;
} fake_hw_counters;

extern fake_hw_counters fake_hw;

#endif /* SYNCN_FAKE_VIDEO_HW_H */
