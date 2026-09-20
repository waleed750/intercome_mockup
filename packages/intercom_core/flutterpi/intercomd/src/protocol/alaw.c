#include "syncn/alaw.h"

/*
 * Standard ITU-T G.711 A-law, matching the reference implementation the door
 * station's vendor app uses. The constants below are from the standard and
 * should not be "tidied" — the segment table in particular is exact.
 */

/* Upper bound of each of the eight A-law segments, in 13-bit linear space. */
static const int16_t seg_end[8] = {
    0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF
};

static int segment_for(int value)
{
    for (int i = 0; i < 8; i++) {
        if (value <= seg_end[i])
            return i;
    }
    return 8; /* saturated */
}

uint8_t syncn_alaw_encode_sample(int16_t pcm)
{
    /* A-law works in 13 bits; drop the low three. */
    int value = pcm >> 3;
    int mask;

    if (value >= 0) {
        mask = 0xD5; /* alternate-bit inversion, positive half */
    } else {
        mask  = 0x55; /* ... negative half */
        value = -value - 1;
    }

    const int seg = segment_for(value);
    if (seg >= 8)
        return (uint8_t)(0x7F ^ mask); /* clipped */

    uint8_t out = (uint8_t)(seg << 4);
    out |= (seg < 2) ? (uint8_t)((value >> 1) & 0x0F)
                     : (uint8_t)((value >> seg) & 0x0F);
    return (uint8_t)(out ^ mask);
}

int16_t syncn_alaw_decode_sample(uint8_t alaw)
{
    const int a = alaw ^ 0x55;
    int t       = (a & 0x0F) << 4;
    const int seg = (a & 0x70) >> 4;

    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= seg - 1;
        break;
    }
    return (int16_t)((a & 0x80) ? t : -t);
}

void syncn_alaw_encode(const int16_t *pcm, size_t count, uint8_t *out)
{
    for (size_t i = 0; i < count; i++)
        out[i] = syncn_alaw_encode_sample(pcm[i]);
}

void syncn_alaw_decode(const uint8_t *alaw, size_t count, int16_t *out)
{
    for (size_t i = 0; i < count; i++)
        out[i] = syncn_alaw_decode_sample(alaw[i]);
}

void syncn_alaw_fill_silence(uint8_t *out, size_t count)
{
    for (size_t i = 0; i < count; i++)
        out[i] = (uint8_t)SYNCN_ALAW_SILENCE;
}
