package com.smarthome.intercom.media

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
import android.util.Log
import com.smarthome.intercom.protocol.ALawCodec
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Two-way G.711 A-law audio at 8 kHz mono.
 *
 *  - **Downlink:** `CC` payloads are queued, decoded to PCM16 and written to an
 *    [AudioTrack]. Queuing decouples playback from the socket read thread.
 *  - **Uplink:** an [AudioRecord] on the VOICE_COMMUNICATION source is read in
 *    20 ms (160-sample) chunks, A-law encoded and handed to [onAudioEncoded] to
 *    be wrapped in a `CC` frame and sent.
 *  - **Echo:** the session uses `MODE_IN_COMMUNICATION` + the voice-comm source
 *    and enables the platform [AcousticEchoCanceler]/[NoiseSuppressor], the
 *    proper hands-free replacement for the reference client's manual gate.
 *  - **Mute:** keeps the cadence by sending A-law silence, so the door hears
 *    nothing without the stream stalling.
 */
class AudioPipeline(
    private val audioManager: AudioManager,
    private val scope: CoroutineScope,
    private val onAudioEncoded: (ByteArray) -> Unit,
    private val onMicUnavailable: () -> Unit = {},
) {
    private val downlink = Channel<ByteArray>(capacity = DOWNLINK_CAPACITY)
    private var downlinkJob: Job? = null
    private var uplinkJob: Job? = null

    private var audioTrack: AudioTrack? = null
    private var audioRecord: AudioRecord? = null
    private var aec: AcousticEchoCanceler? = null
    private var ns: NoiseSuppressor? = null
    private var agc: AutomaticGainControl? = null
    private var focusRequest: AudioFocusRequest? = null
    private var savedMode = AudioManager.MODE_NORMAL

    @Volatile private var muted = false
    @Volatile private var running = false

    private val silence = ByteArray(ALAW_FRAME) { SILENCE_BYTE }

    fun start(captureEnabled: Boolean) {
        if (running) return
        running = true
        configureAudioSession()
        startDownlink()
        if (captureEnabled) startUplink()
    }

    /** Queues one `CC` payload (A-law bytes) for playback. */
    fun playDownlink(alaw: ByteArray) {
        if (running) downlink.trySend(alaw)
    }

    fun setMuted(value: Boolean) {
        muted = value
    }

    fun stop() {
        if (!running) return
        running = false
        uplinkJob?.cancel(); uplinkJob = null
        downlinkJob?.cancel(); downlinkJob = null

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

    // --- session ---

    private fun configureAudioSession() {
        savedMode = audioManager.mode
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        @Suppress("DEPRECATION")
        audioManager.isSpeakerphoneOn = true
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build()
        focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
            .setAudioAttributes(attrs)
            .build()
            .also { audioManager.requestAudioFocus(it) }
    }

    private fun restoreAudioSession() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
        @Suppress("DEPRECATION")
        audioManager.isSpeakerphoneOn = false
        audioManager.mode = savedMode
    }

    // --- downlink ---

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

        downlinkJob = scope.launch(Dispatchers.IO) {
            try {
                for (alaw in downlink) {
                    val pcm = ALawCodec.decode(alaw)
                    audioTrack?.write(pcm, 0, pcm.size)
                }
            } catch (e: Exception) {
                if (running) Log.w(TAG, "downlink ended", e)
            }
        }
    }

    // --- uplink ---

    private fun startUplink() {
        val minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, IN_CHANNEL, ENCODING)
        val bufferSize = maxOf(minBuf, PCM_FRAME * 4)
        val record = try {
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
        } catch (e: SecurityException) {
            Log.w(TAG, "Mic permission missing", e)
            onMicUnavailable()
            return
        } catch (e: Exception) {
            Log.e(TAG, "AudioRecord init failed", e)
            onMicUnavailable()
            return
        }

        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            onMicUnavailable()
            return
        }

        enableEffects(record.audioSessionId)
        audioRecord = record
        record.startRecording()

        uplinkJob = scope.launch(Dispatchers.IO) {
            val pcm = ByteArray(PCM_FRAME)
            try {
                while (isActive && running) {
                    if (!fill(record, pcm)) break
                    if (muted) {
                        onAudioEncoded(silence.copyOf())
                    } else {
                        onAudioEncoded(ALawCodec.encode(pcm))
                    }
                }
            } catch (e: Exception) {
                if (running) Log.w(TAG, "uplink ended", e)
            }
        }
    }

    /** Blocking-reads exactly [out].size bytes; returns false on error/stop. */
    private fun fill(record: AudioRecord, out: ByteArray): Boolean {
        var read = 0
        while (read < out.size) {
            val n = record.read(out, read, out.size - read)
            if (n <= 0) return false
            read += n
        }
        return true
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

    private companion object {
        const val TAG = "AudioPipeline"
        const val SAMPLE_RATE = 8000
        const val IN_CHANNEL = AudioFormat.CHANNEL_IN_MONO
        const val OUT_CHANNEL = AudioFormat.CHANNEL_OUT_MONO
        const val ENCODING = AudioFormat.ENCODING_PCM_16BIT
        const val ALAW_FRAME = 160          // bytes of A-law per 20 ms
        const val PCM_FRAME = ALAW_FRAME * 2 // 320 bytes of PCM16 per 20 ms
        const val DOWNLINK_CAPACITY = 128
        const val SILENCE_BYTE = 0xD5.toByte() // A-law encoding of PCM 0
    }
}
