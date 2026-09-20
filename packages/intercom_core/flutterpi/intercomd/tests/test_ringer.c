/* The chime, tested without a speaker. */
#include "test.h"

#include "syncn/ringer.h"
#include "syncn/dsp.h"

#include <math.h>
#include <stdlib.h>

void suite_ringer(void)
{
    SUITE("ringer: one cycle is ding, dong, silence, at a sane level");
    {
        const size_t cap = SYNCN_RING_RATE * 3;
        int16_t *buf = calloc(cap, sizeof *buf);
        const size_t n = syncn_ringer_synth(buf, cap, 0.0f);

        /* 350 + 500 + 1900 ms */
        CHECK_EQ_INT(n, (size_t)SYNCN_RING_RATE * 2750 / 1000);

        int peak = 0;
        for (size_t i = 0; i < n; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
        const double peak_db = 20 * log10(peak / 32768.0);
        printf("        peak %.1f dBFS, %zu samples\n", peak_db, n);
        CHECK(peak_db < -3.0);                     /* never near the rail   */
        CHECK(peak_db > -12.0);                    /* but not timid either  */

        /* The gap really is silence. */
        const size_t gap_start = (size_t)SYNCN_RING_RATE * 850 / 1000 + 100;
        int nonzero = 0;
        for (size_t i = gap_start; i < n; i++) if (buf[i] != 0) nonzero++;
        CHECK_EQ_INT(nonzero, 0);

        /* Each note decays: the end of the ding is much quieter than its
         * start, so it sounds struck rather than held. */
        const size_t ding_n = (size_t)SYNCN_RING_RATE * 350 / 1000;
        const double head = syncn_rms_dbfs(buf + 200, 400);
        const double tail = syncn_rms_dbfs(buf + ding_n - 600, 400);
        printf("        ding: %.1f dBFS at the start, %.1f at the end\n", head, tail);
        CHECK(head - tail > 10.0);

        free(buf);
    }

    SUITE("ringer: the gain trim works and a small buffer is not overrun");
    {
        int16_t big[SYNCN_RING_RATE];
        const size_t n0 = syncn_ringer_synth(big, sizeof big / sizeof big[0], 0.0f);
        int p0 = 0; for (size_t i = 0; i < n0; i++) if (abs(big[i]) > p0) p0 = abs(big[i]);
        const size_t n1 = syncn_ringer_synth(big, sizeof big / sizeof big[0], -6.0f);
        int p1 = 0; for (size_t i = 0; i < n1; i++) if (abs(big[i]) > p1) p1 = abs(big[i]);
        const double diff = 20 * log10((double)p0 / p1);
        CHECK(fabs(diff - 6.0) < 0.3);

        /* Room for the ding only: the dong and the gap are left out, and
         * nothing is written past the end. */
        int16_t small[SYNCN_RING_RATE * 400 / 1000 + 1];
        small[sizeof small / sizeof small[0] - 1] = 12345;
        const size_t n2 = syncn_ringer_synth(small, sizeof small / sizeof small[0] - 1, 0.0f);
        CHECK_EQ_INT(n2, (size_t)SYNCN_RING_RATE * 350 / 1000);
        CHECK_EQ_INT(small[sizeof small / sizeof small[0] - 1], 12345);
    }
}
