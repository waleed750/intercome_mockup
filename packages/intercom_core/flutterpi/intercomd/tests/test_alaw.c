#include "test.h"
#include "syncn/alaw.h"

#include <stdlib.h>

void suite_alaw(void)
{
    SUITE("alaw: reference vectors");
    {
        /* Cross-checked byte for byte against an independent G.711
         * implementation (Python's audioop.lin2alaw). If a refactor ever breaks
         * the codec, this table is what catches it. */
        static const struct { int16_t pcm; uint8_t alaw; } ref[] = {
            {      0, 0xD5 }, {      1, 0xD5 }, {      8, 0xD5 },
            {    100, 0xD3 }, {   -100, 0x53 },
            {   1000, 0xFA }, {  -1000, 0x7A },
            {   5000, 0x86 }, {  -5000, 0x06 },
            {  20000, 0xA6 }, { -20000, 0x26 },
            {  32767, 0xAA }, { -32768, 0x2A },
        };
        for (size_t i = 0; i < sizeof ref / sizeof ref[0]; i++)
            CHECK_EQ_INT(syncn_alaw_encode_sample(ref[i].pcm), ref[i].alaw);
    }

    SUITE("alaw: silence");
    {
        /* The encoding of PCM zero, and the byte every silence frame is filled
         * with. The uplink cadence mechanism depends on this exact value. */
        CHECK_EQ_INT(syncn_alaw_encode_sample(0), SYNCN_ALAW_SILENCE);

        /* A-law has no exact zero; 0xD5 decodes to +8, the nearest code. */
        CHECK_EQ_INT(syncn_alaw_decode_sample(SYNCN_ALAW_SILENCE), 8);

        uint8_t silence[SYNCN_AUDIO_FRAME_BYTES];
        syncn_alaw_fill_silence(silence, sizeof silence);
        for (size_t i = 0; i < sizeof silence; i++)
            CHECK_EQ_INT(silence[i], SYNCN_ALAW_SILENCE);
    }

    SUITE("alaw: every code survives a decode/encode round trip");
    {
        /* If encode and decode ever disagree, audio degrades on every hop.
         * All 256 codes must map back to themselves. */
        for (int c = 0; c < 256; c++) {
            const int16_t pcm = syncn_alaw_decode_sample((uint8_t)c);
            CHECK_EQ_INT(syncn_alaw_encode_sample(pcm), c);
        }
    }

    SUITE("alaw: quantisation error stays within the segment step");
    {
        /* A-law is logarithmic and drops the low three bits, so the error grows
         * with magnitude: roughly one part in sixteen at the top of the range,
         * a flat +/-16 near zero. Anything worse than that means a real bug. */
        for (int v = -32768; v < 32767; v += 97) {
            const int16_t back = syncn_alaw_decode_sample(syncn_alaw_encode_sample((int16_t)v));
            const int err      = abs((int)back - v);
            const int allowed  = (abs(v) >> 4) + 16;
            CHECK(err <= allowed);
        }
    }

    SUITE("alaw: positive and negative reconstruct symmetrically");
    {
        /* The codec folds negatives as -value-1, so the two codes are not
         * always exact bit mirrors. What must hold is that the reconstructed
         * levels are equal and opposite to within one step — otherwise a
         * symmetric waveform would pick up a DC offset. */
        for (int v = 1; v < 32000; v += 137) {
            const int p = syncn_alaw_decode_sample(syncn_alaw_encode_sample((int16_t)v));
            const int n = syncn_alaw_decode_sample(syncn_alaw_encode_sample((int16_t)-v));
            CHECK(abs(p + n) <= (v >> 4) + 16);
        }
    }

    SUITE("alaw: clipping does not wrap");
    {
        /* Full scale must land on the top code. A wrap to a small value would
         * be an audible click on every loud transient. */
        CHECK(syncn_alaw_decode_sample(syncn_alaw_encode_sample(32767)) > 30000);
        CHECK(syncn_alaw_decode_sample(syncn_alaw_encode_sample(-32768)) < -30000);
    }

    SUITE("alaw: bulk helpers match the per-sample ones");
    {
        int16_t pcm_in[64], pcm_out[64];
        uint8_t coded[64];
        for (int i = 0; i < 64; i++)
            pcm_in[i] = (int16_t)((i - 32) * 500);

        syncn_alaw_encode(pcm_in, 64, coded);
        syncn_alaw_decode(coded, 64, pcm_out);
        for (int i = 0; i < 64; i++) {
            CHECK_EQ_INT(coded[i], syncn_alaw_encode_sample(pcm_in[i]));
            CHECK_EQ_INT(pcm_out[i], syncn_alaw_decode_sample(coded[i]));
        }
    }

    SUITE("alaw: frame geometry matches the protocol");
    {
        CHECK_EQ_INT(SYNCN_AUDIO_FRAME_SAMPLES, 160);
        CHECK_EQ_INT(SYNCN_AUDIO_FRAME_BYTES, 160);
        CHECK_EQ_INT(SYNCN_AUDIO_RATE * SYNCN_AUDIO_FRAME_MS / 1000, SYNCN_AUDIO_FRAME_SAMPLES);
    }
}
