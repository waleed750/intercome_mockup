package com.smarthome.intercom.protocol

/**
 * ITU-T G.711 A-law companding. Pure Kotlin (Android has no `audioop`).
 *
 * The door speaks A-law at 8 kHz mono, 160 bytes (= 160 samples = 20 ms) per
 * audio frame. Decode is table-driven; encode follows the canonical Sun
 * reference search. The invariant `encode(decode(b)) == b` holds for every one
 * of the 256 codes, which is what the round-trip test asserts.
 */
object ALawCodec {

    private const val SIGN_BIT = 0x80
    private const val QUANT_MASK = 0x0F
    private const val SEG_SHIFT = 4
    private const val SEG_MASK = 0x70

    private val SEG_END = intArrayOf(0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF)

    /** Precomputed A-law byte (0..255) -> signed 16-bit PCM. */
    private val DECODE_TABLE = IntArray(256) { decodeSample(it) }

    /** Decodes one A-law code to a signed 16-bit sample. */
    fun decodeByte(aLaw: Int): Short = DECODE_TABLE[aLaw and 0xFF].toShort()

    /** Decodes an A-law buffer to little-endian PCM16 bytes (2 bytes per sample). */
    fun decode(aLaw: ByteArray, offset: Int = 0, length: Int = aLaw.size): ByteArray {
        val out = ByteArray(length * 2)
        var o = 0
        for (i in offset until offset + length) {
            val s = DECODE_TABLE[aLaw[i].toInt() and 0xFF]
            out[o++] = (s and 0xFF).toByte()
            out[o++] = ((s shr 8) and 0xFF).toByte()
        }
        return out
    }

    /** Encodes little-endian PCM16 bytes to A-law (1 byte per sample). */
    fun encode(pcm: ByteArray, offset: Int = 0, length: Int = pcm.size): ByteArray {
        val samples = length / 2
        val out = ByteArray(samples)
        var i = offset
        for (n in 0 until samples) {
            val lo = pcm[i].toInt() and 0xFF
            val hi = pcm[i + 1].toInt()
            i += 2
            out[n] = encodeSample((hi shl 8) or lo)
        }
        return out
    }

    /** Encodes a single signed 16-bit sample. */
    fun encodeSample(pcm16: Int): Byte {
        var pcm = pcm16 shr 3
        val mask: Int
        if (pcm >= 0) {
            mask = 0xD5 // sign bit = 1
        } else {
            mask = 0x55 // sign bit = 0
            pcm = -pcm - 1
        }
        val seg = search(pcm)
        val aval: Int = if (seg >= 8) {
            0x7F // out of range -> max magnitude
        } else {
            val base = seg shl SEG_SHIFT
            val quant = if (seg < 2) (pcm shr 1) and QUANT_MASK else (pcm shr seg) and QUANT_MASK
            base or quant
        }
        return (aval xor mask).toByte()
    }

    private fun decodeSample(aLawByte: Int): Int {
        var a = aLawByte xor 0x55
        var t = (a and QUANT_MASK) shl 4
        val seg = (a and SEG_MASK) shr SEG_SHIFT
        when (seg) {
            0 -> t += 8
            1 -> t += 0x108
            else -> {
                t += 0x108
                t = t shl (seg - 1)
            }
        }
        return if ((a and SIGN_BIT) != 0) t else -t
    }

    private fun search(value: Int): Int {
        for (i in SEG_END.indices) {
            if (value <= SEG_END[i]) return i
        }
        return SEG_END.size
    }
}
