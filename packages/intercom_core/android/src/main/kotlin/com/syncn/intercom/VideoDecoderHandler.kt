package com.syncn.intercom

import android.media.MediaCodec
import android.media.MediaFormat
import android.view.Surface
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import java.util.ArrayDeque

object VideoDecoderHandler : MethodChannel.MethodCallHandler {
    private const val CHANNEL = "syncn_intercom/video"
    private const val MIME = "video/avc"
    private const val DEFAULT_WIDTH = 1920
    private const val DEFAULT_HEIGHT = 1080
    private const val MAX_PENDING = 60
    private const val FRAME_INTERVAL_US = 1_000_000L / 30

    private val lock = Any()
    private var textures: TextureRegistry? = null
    private var textureEntry: TextureRegistry.SurfaceTextureEntry? = null
    private var surface: Surface? = null
    private var codec: MediaCodec? = null
    private val freeInputBuffers = ArrayDeque<Int>()
    private val pendingPayloads = ArrayDeque<ByteArray>()
    private var ptsUs = 0L
    @Volatile private var running = false

    fun register(binding: FlutterPlugin.FlutterPluginBinding) {
        textures = binding.textureRegistry
        MethodChannel(binding.binaryMessenger, CHANNEL).setMethodCallHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "start" -> {
                runCatching { start() }
                    .onSuccess { result.success(it) }
                    .onFailure { result.error("video_start_failed", it.message, null) }
            }
            "submit" -> {
                val payload = call.arguments as? ByteArray
                if (payload != null) submit(payload)
                result.success(null)
            }
            "stop" -> {
                stop()
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    private fun start(width: Int = DEFAULT_WIDTH, height: Int = DEFAULT_HEIGHT): Long {
        if (running) return textureEntry?.id() ?: -1L
        val registry = textures ?: return -1L
        val entry = registry.createSurfaceTexture()
        entry.surfaceTexture().setDefaultBufferSize(width, height)
        val renderSurface = Surface(entry.surfaceTexture())
        val mediaCodec = MediaCodec.createDecoderByType(MIME)
        mediaCodec.setCallback(callback)
        mediaCodec.configure(MediaFormat.createVideoFormat(MIME, width, height), renderSurface, null, 0)
        mediaCodec.start()
        synchronized(lock) {
            textureEntry = entry
            surface = renderSurface
            codec = mediaCodec
            freeInputBuffers.clear()
            pendingPayloads.clear()
            ptsUs = 0L
            running = true
        }
        return entry.id()
    }

    private fun submit(payload: ByteArray) {
        if (!running || payload.isEmpty()) return
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

    private fun stop() {
        running = false
        val currentCodec: MediaCodec?
        val currentSurface: Surface?
        val currentTexture: TextureRegistry.SurfaceTextureEntry?
        synchronized(lock) {
            currentCodec = codec
            currentSurface = surface
            currentTexture = textureEntry
            codec = null
            surface = null
            textureEntry = null
            freeInputBuffers.clear()
            pendingPayloads.clear()
        }
        currentCodec?.let {
            runCatching { it.stop() }
            runCatching { it.release() }
        }
        currentSurface?.release()
        currentTexture?.release()
    }

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
            runCatching { mc.releaseOutputBuffer(index, true) }
        }

        override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) = Unit

        override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
            stop()
        }
    }

    private fun feed(index: Int, payload: ByteArray) {
        val currentCodec = codec ?: return
        runCatching {
            val input = currentCodec.getInputBuffer(index) ?: return
            input.clear()
            input.put(payload)
            currentCodec.queueInputBuffer(index, 0, payload.size, ptsUs, 0)
            ptsUs += FRAME_INTERVAL_US
        }.onFailure {
            stop()
        }
    }
}
