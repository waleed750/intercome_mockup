package com.smarthome.intercom.protocol

/**
 * The three logical channels multiplexed over the single TCP call connection.
 * On the wire each frame is `[4-byte magic][4-byte little-endian length][payload]`
 * where the magic is the channel's marker byte repeated four times.
 */
enum class Channel(val marker: Byte) {
    /** `AA AA AA AA` — UTF-8 JSON control commands. */
    CONTROL(0xAA.toByte()),

    /** `BB BB BB BB` — H.264 Annex-B video. */
    VIDEO(0xBB.toByte()),

    /** `CC CC CC CC` — G.711 A-law audio, 160 bytes per 20 ms. */
    AUDIO(0xCC.toByte());

    companion object {
        /** Maps a marker byte to its channel, or null if it is not a known marker. */
        fun forMarker(b: Byte): Channel? = when (b) {
            CONTROL.marker -> CONTROL
            VIDEO.marker -> VIDEO
            AUDIO.marker -> AUDIO
            else -> null
        }
    }
}

object Frame {
    const val MAGIC_SIZE = 4
    const val LENGTH_SIZE = 4
    const val HEADER_SIZE = MAGIC_SIZE + LENGTH_SIZE

    /**
     * Serialises one frame: marker ×4, then the payload length as a 4-byte
     * little-endian integer, then the payload bytes.
     */
    fun encode(channel: Channel, payload: ByteArray): ByteArray {
        val out = ByteArray(HEADER_SIZE + payload.size)
        val m = channel.marker
        out[0] = m; out[1] = m; out[2] = m; out[3] = m
        val len = payload.size
        out[4] = (len and 0xFF).toByte()
        out[5] = ((len ushr 8) and 0xFF).toByte()
        out[6] = ((len ushr 16) and 0xFF).toByte()
        out[7] = ((len ushr 24) and 0xFF).toByte()
        System.arraycopy(payload, 0, out, HEADER_SIZE, payload.size)
        return out
    }
}
