package com.smarthome.intercom.media

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.util.ArrayDeque

/**
 * Decodes the door's H.264 (Annex-B) video to an on-screen [Surface] using
 * hardware [MediaCodec]. Each `BB` frame's payload is fed verbatim; MediaCodec
 * consumes the inline SPS/PPS and locks on at the first IDR, so joining the
 * stream mid-flight (e.g. video already running during ring) just syncs at the
 * next keyframe.
 *
 * Uses async (callback) mode. Input buffers and pending payloads are matched in
 * a small synchronized handshake so frames arriving faster than buffers free up
 * are queued (and the oldest dropped past a cap, rather than growing unbounded).
 *
 * On an unrecoverable decoder error [onError] fires once; the caller keeps audio
 * and shows "Video unavailable" (INT-C3).
 */
class VideoPipeline(
    private val onError: () -> Unit = {},
) {
    private val lock = Any()
    private var codec: MediaCodec? = null
    private val freeInputBuffers = ArrayDeque<Int>()
    private val pendingPayloads = ArrayDeque<ByteArray>()
    private var ptsUs = 0L
    @Volatile private var running = false
    @Volatile private var errored = false

    fun start(surface: Surface, width: Int = DEFAULT_WIDTH, height: Int = DEFAULT_HEIGHT) {
        if (running) return
        try {
            val format = MediaFormat.createVideoFormat(MIME, width, height)
            val mediaCodec = MediaCodec.createDecoderByType(MIME)
            mediaCodec.setCallback(callback)
            mediaCodec.configure(format, surface, null, 0)
            mediaCodec.start()
            synchronized(lock) {
                codec = mediaCodec
                freeInputBuffers.clear()
                pendingPayloads.clear()
                ptsUs = 0L
            }
            running = true
            errored = false
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start video decoder", e)
            reportError()
        }
    }

    /** Feeds one H.264 Annex-B payload (a `BB` frame body, envelope already stripped). */
    fun submit(payload: ByteArray) {
        if (!running || errored || payload.isEmpty()) return
        synchronized(lock) {
            val index = freeInputBuffers.poll()
            if (index != null) {
                feed(index, payload)
            } else {
                if (pendingPayloads.size >= MAX_PENDING) pendingPayloads.poll()
                pendingPayloads.add(payload)
            }
        }
    }

    fun stop() {
        running = false
        val c = synchronized(lock) {
            val current = codec
            codec = null
            freeInputBuffers.clear()
            pendingPayloads.clear()
            current
        }
        c?.let {
            runCatching { it.stop() }
            runCatching { it.release() }
        }
    }

    // --- internals (callbacks run on MediaCodec's own thread) ---

    private val callback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
            synchronized(lock) {
                if (codec !== mc) return
                val payload = pendingPayloads.poll()
                if (payload != null) feed(index, payload) else freeInputBuffers.add(index)
            }
        }

        override fun onOutputBufferAvailable(
            mc: MediaCodec,
            index: Int,
            info: MediaCodec.BufferInfo,
        ) {
            // render=true pushes the frame straight to the Surface.
            runCatching { mc.releaseOutputBuffer(index, true) }
        }

        override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
            Log.d(TAG, "Output format: $format")
        }

        override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
            Log.e(TAG, "Decoder error (transient=${e.isTransient}, recoverable=${e.isRecoverable})", e)
            reportError()
        }
    }

    /** Must be called holding [lock]. */
    private fun feed(index: Int, payload: ByteArray) {
        val c = codec ?: return
        try {
            val input = c.getInputBuffer(index) ?: return
            input.clear()
            input.put(payload)
            c.queueInputBuffer(index, 0, payload.size, ptsUs, 0)
            ptsUs += FRAME_INTERVAL_US
        } catch (e: Exception) {
            Log.e(TAG, "queueInputBuffer failed", e)
            reportError()
        }
    }

    private fun reportError() {
        if (errored) return
        errored = true
        running = false
        onError()
        // Tear the codec down off the callback thread is unsafe; do minimal cleanup.
        val c = synchronized(lock) { val cur = codec; codec = null; cur }
        c?.let { runCatching { it.release() } }
    }

    private companion object {
        const val TAG = "VideoPipeline"
        const val MIME = "video/avc"
        const val DEFAULT_WIDTH = 1920
        const val DEFAULT_HEIGHT = 1080
        const val MAX_PENDING = 60
        const val FRAME_INTERVAL_US = 1_000_000L / 30
    }
}
