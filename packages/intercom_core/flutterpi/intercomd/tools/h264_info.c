/*
 * Report the NAL structure of a captured door stream.
 *
 * Run it against a capture from the panel to check the assumptions the decoder
 * is built on hold for the real stream, rather than for a synthetic one that
 * matches our own arithmetic:
 *
 *     h264-info /var/lib/syncn/door-20260916-190650.h264
 *
 * What to look for: SPS, PPS and IDR counts that match each other, a keyframe
 * interval that matches the door's configuration, and no long runs of slices
 * without a keyframe. A stream that never repeats its parameter sets would be
 * one a decoder restart could not recover from.
 */
#include "syncn/h264.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <capture.h264>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fprintf(stderr, "%s is empty\n", argv[1]);
        fclose(f);
        return 1;
    }

    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "could not read %s\n", argv[1]);
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Count first so the array can be sized exactly. */
    const size_t total = syncn_h264_split(data, (size_t)size, NULL, 0);
    syncn_nal *nals = calloc(total ? total : 1, sizeof *nals);
    if (!nals) {
        free(data);
        return 1;
    }
    syncn_h264_split(data, (size_t)size, nals, total);

    unsigned counts[32] = {0};
    size_t bytes_by_type[32] = {0};

    long last_key = -1;
    long min_gap = -1, max_gap = -1;
    double gap_sum = 0;
    long gaps = 0;

    for (size_t i = 0; i < total; i++) {
        const unsigned t = (unsigned)nals[i].type & 0x1F;
        counts[t]++;
        bytes_by_type[t] += nals[i].len;

        if (nals[i].type == SYNCN_NAL_IDR) {
            if (last_key >= 0) {
                /* Distance in slices, which is frames for this door. */
                long gap = 0;
                for (size_t k = (size_t)last_key + 1; k < i; k++)
                    if (nals[k].type == SYNCN_NAL_SLICE || nals[k].type == SYNCN_NAL_IDR)
                        gap++;
                gap++;
                if (min_gap < 0 || gap < min_gap) min_gap = gap;
                if (gap > max_gap) max_gap = gap;
                gap_sum += (double)gap;
                gaps++;
            }
            last_key = (long)i;
        }
    }

    printf("file            : %s\n", argv[1]);
    printf("size            : %ld bytes\n", size);
    printf("NAL units       : %zu\n\n", total);

    printf("  type            count      bytes\n");
    printf("  ---------------------------------\n");
    for (unsigned t = 0; t < 32; t++) {
        if (!counts[t])
            continue;
        printf("  %-12s %8u %10zu\n",
               syncn_nal_type_name((syncn_nal_type)t), counts[t], bytes_by_type[t]);
    }

    const unsigned sps = counts[SYNCN_NAL_SPS];
    const unsigned pps = counts[SYNCN_NAL_PPS];
    const unsigned idr = counts[SYNCN_NAL_IDR];
    const unsigned slices = counts[SYNCN_NAL_SLICE];

    printf("\nframes (slices + IDR): %u\n", slices + idr);
    if (gaps)
        printf("keyframe interval    : min %ld, max %ld, mean %.1f frames\n",
               min_gap, max_gap, gap_sum / (double)gaps);

    printf("\nchecks:\n");
    printf("  parameter sets travel with keyframes : %s\n",
           (sps == idr && pps == idr) ? "yes" :
           (sps == 0 || pps == 0) ? "NO — none present at all" : "not one-to-one");
    printf("  stream starts with a keyframe        : %s\n",
           (total > 0 && (nals[0].type == SYNCN_NAL_SPS ||
                          nals[0].type == SYNCN_NAL_IDR)) ? "yes"
                        : "no — leading slices are undecodable, which is expected "
                          "for a capture started mid-stream");
    if (max_gap > 0 && max_gap > min_gap * 3)
        printf("  WARNING: keyframe spacing is uneven (%ld vs %ld) — a loss during "
               "a long gap would freeze the picture for that whole interval\n",
               min_gap, max_gap);

    free(nals);
    free(data);
    return 0;
}
