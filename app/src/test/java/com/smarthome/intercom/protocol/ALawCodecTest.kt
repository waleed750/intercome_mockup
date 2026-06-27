package com.smarthome.intercom.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * TST-F2: G.711 A-law companding. The defining property is that re-encoding a
 * decoded code reproduces it exactly — `encode(decode(b)) == b` for every one of
 * the 256 codes — so the audio path never drifts on a pass-through sample.
 */
class ALawCodecTest {

    @Test
    fun encodeOfDecodeIsIdentityForAll256Codes() {
        for (code in 0..255) {
            val pcm = ALawCodec.decodeByte(code).toInt()
            val reencoded = ALawCodec.encodeSample(pcm)
            assertEquals("round-trip failed for code $code", code.toByte(), reencoded)
        }
    }

    @Test
    fun encodeBufferRoundTripsThroughDecodeBuffer() {
        val codes = ByteArray(256) { it.toByte() }
        val pcm = ALawCodec.decode(codes)
        val reencoded = ALawCodec.encode(pcm)
        assertArrayEquals(codes, reencoded)
    }

    @Test
    fun decodeBufferEmitsLittleEndianPcm16PerSample() {
        val codes = payload(0x00, 0x55, 0xD5, 0x2A, 0x7F, 0xFF)
        val pcm = ALawCodec.decode(codes)

        assertEquals(codes.size * 2, pcm.size)
        for (i in codes.indices) {
            val expected = ALawCodec.decodeByte(codes[i].toInt())
            val lo = pcm[i * 2].toInt() and 0xFF
            val hi = pcm[i * 2 + 1].toInt() and 0xFF
            val sample = ((hi shl 8) or lo).toShort()
            assertEquals("sample $i", expected, sample)
        }
    }

    @Test
    fun signedZeroCodesDecodeToOppositeSmallMagnitudes() {
        // The A-law ±0 region: 0xD5 is +8, 0x55 is -8. Both must round-trip.
        assertEquals(8.toShort(), ALawCodec.decodeByte(0xD5))
        assertEquals((-8).toShort(), ALawCodec.decodeByte(0x55))
        assertEquals(0xD5.toByte(), ALawCodec.encodeSample(8))
        assertEquals(0x55.toByte(), ALawCodec.encodeSample(-8))
    }

    @Test
    fun encodesFullScaleSamplesToExpectedCodes() {
        // Full-scale PCM lands in the top segment: aval 0x7F before the sign mask.
        // 0x7F xor 0xD5 = 0xAA (positive extreme); 0x7F xor 0x55 = 0x2A (negative).
        // Also guards the Short.MIN_VALUE negation: we shift right *before* negating,
        // so -32768 becomes 4095 with no overflow.
        assertEquals(0xAA.toByte(), ALawCodec.encodeSample(Short.MAX_VALUE.toInt()))
        assertEquals(0x2A.toByte(), ALawCodec.encodeSample(Short.MIN_VALUE.toInt()))
    }

    private fun payload(vararg bytes: Int) = ByteArray(bytes.size) { bytes[it].toByte() }
}
