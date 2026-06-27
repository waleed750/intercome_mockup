package com.smarthome.intercom.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * TST-F1: the streaming frame parser must survive the three real socket hazards —
 * frames split across reads, multiple frames merged into one read, and desync
 * (leading garbage / bogus lengths) — and re-lock onto the next valid frame.
 */
class FrameParserTest {

    private fun newParser(
        sink: MutableList<Pair<Channel, ByteArray>>,
        maxPayload: Int = 1 shl 20,
    ) = FrameParser(maxPayloadLength = maxPayload) { ch, payload -> sink += ch to payload }

    private fun payload(vararg bytes: Int) = ByteArray(bytes.size) { bytes[it].toByte() }

    @Test
    fun parsesSingleWholeFrame() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val p = payload(1, 2, 3, 4, 5)

        parser.offer(Frame.encode(Channel.CONTROL, p))

        assertEquals(1, frames.size)
        assertEquals(Channel.CONTROL, frames[0].first)
        assertArrayEquals(p, frames[0].second)
    }

    @Test
    fun parsesEmptyPayloadFrame() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)

        parser.offer(Frame.encode(Channel.AUDIO, ByteArray(0)))

        assertEquals(1, frames.size)
        assertEquals(Channel.AUDIO, frames[0].first)
        assertEquals(0, frames[0].second.size)
    }

    @Test
    fun parsesMergedFramesInOneOffer() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val a = Frame.encode(Channel.CONTROL, payload(10, 11))
        val b = Frame.encode(Channel.VIDEO, payload(20, 21, 22))
        val c = Frame.encode(Channel.AUDIO, payload(30))

        parser.offer(a + b + c)

        assertEquals(3, frames.size)
        assertEquals(Channel.CONTROL, frames[0].first)
        assertEquals(Channel.VIDEO, frames[1].first)
        assertEquals(Channel.AUDIO, frames[2].first)
        assertArrayEquals(payload(20, 21, 22), frames[1].second)
    }

    @Test
    fun reassemblesFrameFedByteByByte() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val p = payload(7, 6, 5, 4, 3, 2)
        val wire = Frame.encode(Channel.VIDEO, p)

        for (b in wire) parser.offer(byteArrayOf(b))

        assertEquals(1, frames.size)
        assertArrayEquals(p, frames[0].second)
    }

    @Test
    fun reassemblesFrameSplitMidHeader() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val p = ByteArray(200) { it.toByte() }
        val wire = Frame.encode(Channel.VIDEO, p)
        val cut = 5 // splits inside the 8-byte header

        parser.offer(wire, 0, cut)
        assertTrue(frames.isEmpty())
        parser.offer(wire, cut, wire.size - cut)

        assertEquals(1, frames.size)
        assertArrayEquals(p, frames[0].second)
    }

    @Test
    fun emitsCompleteFramesAndBuffersPartialTail() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val a = Frame.encode(Channel.CONTROL, payload(1))
        val b = Frame.encode(Channel.VIDEO, payload(2, 3, 4, 5))

        // First offer: all of `a` plus only the first 3 bytes of `b`.
        parser.offer(a + b.copyOfRange(0, 3))
        assertEquals(1, frames.size)
        assertEquals(Channel.CONTROL, frames[0].first)

        // Second offer: the remainder of `b`.
        parser.offer(b, 3, b.size - 3)
        assertEquals(2, frames.size)
        assertEquals(Channel.VIDEO, frames[1].first)
        assertArrayEquals(payload(2, 3, 4, 5), frames[1].second)
    }

    @Test
    fun skipsLeadingGarbageAndResyncs() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val garbage = payload(0x00, 0x01, 0x02, 0x03, 0x99, 0xAA) // lone 0xAA, not a full magic
        val good = Frame.encode(Channel.CONTROL, payload(42))

        parser.offer(garbage + good)

        assertEquals(1, frames.size)
        assertArrayEquals(payload(42), frames[0].second)
    }

    @Test
    fun ignoresFourIdenticalNonMarkerBytes() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val fakeMagic = payload(0x00, 0x00, 0x00, 0x00)
        val good = Frame.encode(Channel.VIDEO, payload(9))

        parser.offer(fakeMagic + good)

        assertEquals(1, frames.size)
        assertEquals(Channel.VIDEO, frames[0].first)
    }

    @Test
    fun resyncsPastImplausibleLength() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames, maxPayload = 1024)
        // A real CONTROL magic, then a length (0x7FFFFFFF) far beyond maxPayload.
        val bogus = payload(0xAA, 0xAA, 0xAA, 0xAA, 0xFF, 0xFF, 0xFF, 0x7F)
        val good = Frame.encode(Channel.AUDIO, payload(1, 2, 3))

        parser.offer(bogus + good)

        assertEquals(1, frames.size)
        assertEquals(Channel.AUDIO, frames[0].first)
        assertArrayEquals(payload(1, 2, 3), frames[0].second)
    }

    @Test
    fun handlesPayloadLargerThanInitialBuffer() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames, maxPayload = 1 shl 21)
        val big = ByteArray(100_000) { (it % 256).toByte() } // exceeds 64 KiB initial capacity

        parser.offer(Frame.encode(Channel.VIDEO, big))

        assertEquals(1, frames.size)
        assertArrayEquals(big, frames[0].second)
    }

    @Test
    fun resetDropsBufferedBytes() {
        val frames = mutableListOf<Pair<Channel, ByteArray>>()
        val parser = newParser(frames)
        val partial = Frame.encode(Channel.CONTROL, payload(1, 2, 3, 4))

        parser.offer(partial, 0, 6) // partial header, nothing emitted
        assertTrue(frames.isEmpty())
        parser.reset()
        parser.offer(Frame.encode(Channel.AUDIO, payload(5)))

        assertEquals(1, frames.size)
        assertEquals(Channel.AUDIO, frames[0].first)
        assertArrayEquals(payload(5), frames[0].second)
    }
}
