package com.syncn.intercom

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.content.ContextCompat
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.util.concurrent.LinkedBlockingQueue
import kotlin.concurrent.thread

object AudioPipelineHandler : MethodChannel.MethodCallHandler, EventChannel.StreamHandler {
    private const val METHOD_CHANNEL = "syncn_intercom/audio"
    private const val EVENT_CHANNEL = "syncn_intercom/audio_uplink"
    private const val SAMPLE_RATE = 8000
    private const val IN_CHANNEL = AudioFormat.CHANNEL_IN_MONO
    private const val OUT_CHANNEL = AudioFormat.CHANNEL_OUT_MONO
    private const val ENCODING = AudioFormat.ENCODING_PCM_16BIT
    private const val ALAW_FRAME = 160
    private const val PCM_FRAME = ALAW_FRAME * 2
    private const val DOWNLINK_CAPACITY = 128
    private const val SILENCE_BYTE = 0xD5.toByte()

    private lateinit var context: Context
    private var audioManager: AudioManager? = null
    private var eventSink: EventChannel.EventSink? = null
    private val downlink = LinkedBlockingQueue<ByteArray>(DOWNLINK_CAPACITY)
    private var audioTrack: AudioTrack? = null
    private var audioRecord: AudioRecord? = null
    private var downlinkThread: Thread? = null
    private var uplinkThread: Thread? = null
    private var aec: AcousticEchoCanceler? = null
    private var ns: NoiseSuppressor? = null
    private var agc: AutomaticGainControl? = null
    private var focusRequest: AudioFocusRequest? = null
    private var savedMode = AudioManager.MODE_NORMAL
    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile private var muted = false
    @Volatile private var running = false

    fun register(binding: FlutterPlugin.FlutterPluginBinding) {
        context = binding.applicationContext
        audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        MethodChannel(binding.binaryMessenger, METHOD_CHANNEL).setMethodCallHandler(this)
        EventChannel(binding.binaryMessenger, EVENT_CHANNEL).setStreamHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "start" -> {
                val captureEnabled = call.argument<Boolean>("captureEnabled") == true
                start(captureEnabled)
                result.success(null)
            }
            "playDownlink" -> {
                val payload = call.arguments as? ByteArray
                if (payload != null) playDownlink(payload)
                result.success(null)
            }
            "setMuted" -> {
                muted = call.arguments as? Boolean == true
                result.success(null)
            }
            "stop" -> {
                stop()
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        eventSink = events
    }

    override fun onCancel(arguments: Any?) {
        eventSink = null
    }

    private fun start(captureEnabled: Boolean) {
        if (running) return
        running = true
        configureAudioSession()
        startDownlink()
        if (captureEnabled && hasRecordPermission()) startUplink()
    }

    private fun playDownlink(alaw: ByteArray) {
        if (!running) return
        if (!downlink.offer(alaw)) {
            downlink.poll()
            downlink.offer(alaw)
        }
    }

    private fun stop() {
        if (!running) return
        running = false
        downlinkThread?.interrupt()
        uplinkThread?.interrupt()
        downlinkThread = null
        uplinkThread = null
        downlink.clear()

        runCatching { audioRecord?.stop() }
        runCatching { audioRecord?.release() }
        audioRecord = null
        aec?.release(); aec = null
        ns?.release(); ns = null
        agc?.release(); agc = null

        runCatching { audioTrack?.stop() }
        runCatching { audioTrack?.release() }
        audioTrack = null

        restoreAudioSession()
    }

    private fun configureAudioSession() {
        val manager = audioManager ?: return
        savedMode = manager.mode
        manager.mode = AudioManager.MODE_IN_COMMUNICATION
        @Suppress("DEPRECATION")
        manager.isSpeakerphoneOn = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
            focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(attrs)
                .build()
                .also { manager.requestAudioFocus(it) }
        } else {
            @Suppress("DEPRECATION")
            manager.requestAudioFocus(null, AudioManager.STREAM_VOICE_CALL, AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
        }
    }

    private fun restoreAudioSession() {
        val manager = audioManager ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { manager.abandonAudioFocusRequest(it) }
        } else {
            @Suppress("DEPRECATION")
            manager.abandonAudioFocus(null)
        }
        focusRequest = null
        @Suppress("DEPRECATION")
        manager.isSpeakerphoneOn = false
        manager.mode = savedMode
    }

    private fun startDownlink() {
        val minBuf = AudioTrack.getMinBufferSize(SAMPLE_RATE, OUT_CHANNEL, ENCODING)
        val bufferSize = maxOf(minBuf, PCM_FRAME * 4)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(ENCODING)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(OUT_CHANNEL)
                    .build(),
            )
            .setBufferSizeInBytes(bufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track.play()
        audioTrack = track

        downlinkThread = thread(name = "syncn-intercom-downlink") {
            while (running) {
                val alaw = runCatching { downlink.take() }.getOrNull() ?: break
                val pcm = ALawCodec.decode(alaw)
                audioTrack?.write(pcm, 0, pcm.size)
            }
        }
    }

    private fun startUplink() {
        val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, IN_CHANNEL, ENCODING)
        val bufferSize = maxOf(minBuf, PCM_FRAME * 4)
        val record = runCatching {
            AudioRecord.Builder()
                .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(ENCODING)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(IN_CHANNEL)
                        .build(),
                )
                .setBufferSizeInBytes(bufferSize)
                .build()
        }.getOrNull() ?: return

        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            return
        }

        enableEffects(record.audioSessionId)
        audioRecord = record
        record.startRecording()

        uplinkThread = thread(name = "syncn-intercom-uplink") {
            val pcm = ByteArray(PCM_FRAME)
            val silence = ByteArray(ALAW_FRAME) { SILENCE_BYTE }
            while (running) {
                if (!fill(record, pcm)) break
                val alaw = if (muted) silence.copyOf() else ALawCodec.encode(pcm)
                mainHandler.post {
                    if (running) eventSink?.success(alaw)
                }
            }
        }
    }

    private fun fill(record: AudioRecord, out: ByteArray): Boolean {
        var read = 0
        while (read < out.size && running) {
            val n = record.read(out, read, out.size - read)
            if (n <= 0) return false
            read += n
        }
        return read == out.size
    }

    private fun enableEffects(sessionId: Int) {
        if (AcousticEchoCanceler.isAvailable()) {
            aec = AcousticEchoCanceler.create(sessionId)?.apply { enabled = true }
        }
        if (NoiseSuppressor.isAvailable()) {
            ns = NoiseSuppressor.create(sessionId)?.apply { enabled = true }
        }
        if (AutomaticGainControl.isAvailable()) {
            agc = AutomaticGainControl.create(sessionId)?.apply { enabled = true }
        }
    }

    private fun hasRecordPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
}

private object ALawCodec {
    private const val SIGN_BIT = 0x80
    private const val QUANT_MASK = 0x0F
    private const val SEG_SHIFT = 4
    private const val SEG_MASK = 0x70
    private val SEG_END = intArrayOf(0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF)
    private val DECODE_TABLE = IntArray(256) { decodeSample(it) }

    fun decode(aLaw: ByteArray): ByteArray {
        val out = ByteArray(aLaw.size * 2)
        var o = 0
        for (value in aLaw) {
            val sample = DECODE_TABLE[value.toInt() and 0xFF]
            out[o++] = (sample and 0xFF).toByte()
            out[o++] = ((sample shr 8) and 0xFF).toByte()
        }
        return out
    }

    fun encode(pcm: ByteArray): ByteArray {
        val out = ByteArray(pcm.size / 2)
        var i = 0
        for (n in out.indices) {
            val lo = pcm[i].toInt() and 0xFF
            val hi = pcm[i + 1].toInt()
            i += 2
            out[n] = encodeSample((hi shl 8) or lo)
        }
        return out
    }

    private fun encodeSample(pcm16: Int): Byte {
        var pcm = pcm16 shr 3
        val mask: Int
        if (pcm >= 0) {
            mask = 0xD5
        } else {
            mask = 0x55
            pcm = -pcm - 1
        }
        val seg = search(pcm)
        val aval = if (seg >= 8) {
            0x7F
        } else {
            val base = seg shl SEG_SHIFT
            val quant = if (seg < 2) (pcm shr 1) and QUANT_MASK else (pcm shr seg) and QUANT_MASK
            base or quant
        }
        return (aval xor mask).toByte()
    }

    private fun decodeSample(aLawByte: Int): Int {
        val a = aLawByte xor 0x55
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
