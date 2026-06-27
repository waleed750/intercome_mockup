package com.smarthome.intercom.protocol

/**
 * Streaming parser for the length-prefixed call protocol. Bytes are fed in via
 * [offer] in whatever chunks the socket delivers them; complete frames are
 * emitted through [onFrame]. Handles the three real-world hazards:
 *
 *  - **Split frames** — a frame spanning multiple reads is buffered until whole.
 *  - **Merged frames** — multiple frames in one read are all emitted.
 *  - **Desync** — if the read head is not on a valid magic (e.g. after a dropped
 *    byte or a bogus length), it advances one byte at a time until it re-locks
 *    onto the next known marker.
 *
 * This is a direct port of the reference client's `_parse`. The class is not
 * thread-safe; drive it from a single reader coroutine.
 */
class FrameParser(
    private val maxPayloadLength: Int = DEFAULT_MAX_PAYLOAD,
    private val onFrame: (Channel, ByteArray) -> Unit,
) {
    private var buf = ByteArray(INITIAL_CAPACITY)
    private var size = 0

    /** Appends [length] bytes from [data] starting at [offset] and drains whole frames. */
    fun offer(data: ByteArray, offset: Int = 0, length: Int = data.size) {
        require(offset >= 0 && length >= 0 && offset + length <= data.size) { "bad range" }
        ensureCapacity(size + length)
        System.arraycopy(data, offset, buf, size, length)
        size += length
        drain()
    }

    /** Drops all buffered bytes. Call on socket reset to start clean. */
    fun reset() {
        size = 0
    }

    private fun drain() {
        var pos = 0
        while (true) {
            // Not enough bytes to even confirm a magic — keep the tail and wait.
            if (size - pos < Frame.MAGIC_SIZE) break

            val channel = magicAt(pos)
            if (channel == null) {
                // Desync: skip one byte and keep scanning for the next marker.
                pos++
                continue
            }
            // Have a valid magic but maybe not the full length header yet.
            if (size - pos < Frame.HEADER_SIZE) break

            val len = readLengthLE(pos + Frame.MAGIC_SIZE)
            if (len < 0 || len > maxPayloadLength) {
                // Implausible length: treat the magic as coincidental, resync.
                pos++
                continue
            }
            val frameEnd = pos + Frame.HEADER_SIZE + len.toInt()
            if (frameEnd > size) break // wait for the rest of the payload

            val payload = buf.copyOfRange(pos + Frame.HEADER_SIZE, frameEnd)
            onFrame(channel, payload)
            pos = frameEnd
        }
        compact(pos)
    }

    /** Returns the channel if [i]..[i]+3 are four identical known marker bytes. */
    private fun magicAt(i: Int): Channel? {
        val b = buf[i]
        val channel = Channel.forMarker(b) ?: return null
        return if (buf[i + 1] == b && buf[i + 2] == b && buf[i + 3] == b) channel else null
    }

    private fun readLengthLE(o: Int): Long =
        (buf[o].toLong() and 0xFF) or
            ((buf[o + 1].toLong() and 0xFF) shl 8) or
            ((buf[o + 2].toLong() and 0xFF) shl 16) or
            ((buf[o + 3].toLong() and 0xFF) shl 24)

    /** Discards the first [consumed] bytes, sliding the remainder to the front. */
    private fun compact(consumed: Int) {
        if (consumed == 0) return
        val remaining = size - consumed
        if (remaining > 0) {
            System.arraycopy(buf, consumed, buf, 0, remaining)
        }
        size = remaining
    }

    private fun ensureCapacity(needed: Int) {
        if (needed <= buf.size) return
        var newCap = buf.size
        while (newCap < needed) newCap = newCap shl 1
        buf = buf.copyOf(newCap)
    }

    companion object {
        const val INITIAL_CAPACITY = 64 * 1024
        const val DEFAULT_MAX_PAYLOAD = 4 * 1024 * 1024
    }
}
