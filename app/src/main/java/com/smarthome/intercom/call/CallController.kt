package com.smarthome.intercom.call

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioManager
import android.util.Log
import android.view.Surface
import androidx.core.content.ContextCompat
import com.smarthome.intercom.config.DeviceConfig
import com.smarthome.intercom.media.AudioPipeline
import com.smarthome.intercom.media.VideoPipeline
import com.smarthome.intercom.net.CallConnection
import com.smarthome.intercom.net.CallServer
import com.smarthome.intercom.net.DiscoveryResponder
import com.smarthome.intercom.net.NetworkBinder
import com.smarthome.intercom.protocol.Channel
import com.smarthome.intercom.protocol.Commands
import com.smarthome.intercom.protocol.Frame
import com.smarthome.intercom.protocol.InboundCommand
import com.smarthome.intercom.protocol.ScreenInfo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.net.Socket

/**
 * The single source of truth for the call. Owns discovery, the TCP call server,
 * the active connection, and the media pipelines; maps protocol events to the
 * [CallUiState] the UI observes, and turns user intents into protocol commands.
 *
 * State-changing transitions are confined to [stateLock] so socket-thread events
 * (frames arriving) and main-thread intents (Answer/End) can't race. Per-frame
 * media forwarding reads the volatile [phase] without locking to stay off the
 * hot path.
 */
class CallController(
    private val context: Context,
    private val deviceConfig: DeviceConfig,
) {
    private val scope = CoroutineScope(SupervisorJob() + kotlinx.coroutines.Dispatchers.Default)
    private val stateLock = Any()

    private val binder = NetworkBinder(context)
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private val discovery = DiscoveryResponder(scope, binder, ::buildScreenInfo)
    private val server = CallServer(
        scope = scope,
        bindAddressProvider = { binder.lanIpv4Address() },
        onAccepted = ::onSocketAccepted,
    )

    private val video = VideoPipeline(onError = ::onVideoError)
    private var audio: AudioPipeline? = null
    private var connection: CallConnection? = null
    private var surface: Surface? = null
    private var videoRunning = false
    private var transientJob: Job? = null

    @Volatile private var phase: CallPhase = CallPhase.IDLE

    private val _uiState = MutableStateFlow(CallUiState())
    val uiState: StateFlow<CallUiState> = _uiState.asStateFlow()

    // --- lifecycle ---

    fun start() {
        binder.acquireLocks()
        scope.launch {
            deviceConfig.identity.collect { id ->
                _uiState.update { it.copy(unitName = id.alias, pairedDoor = id.doorName, callerLabel = id.doorName) }
            }
        }
        refreshConnectivity()
        discovery.start()
        server.start()
    }

    fun shutdown() {
        teardownCall(showEnded = false)
        discovery.stop()
        server.stop()
        binder.releaseLocks()
    }

    fun refreshConnectivity() {
        val onWifi = binder.isOnLan()
        Log.d(TAG, "Connectivity: onLan=$onWifi lanIp=${binder.lanIpv4Address()?.hostAddress} networks=${binder.describeNetworks()}")
        _uiState.update { it.copy(onWifi = onWifi) }
    }

    // --- inbound: connections & frames ---

    private fun onSocketAccepted(socket: Socket) {
        synchronized(stateLock) {
            if (connection != null) {
                // If the previous connection is stale (socket closed but not cleaned up),
                // tear it down and accept the new one instead of rejecting as busy.
                val prevSocket = try { connection?.remoteAddress } catch (_: Exception) { null }
                Log.d(TAG, "Door connected from ${socket.inetAddress?.hostAddress} — previous connection exists (remote=$prevSocket, phase=$phase)")
                if (phase == CallPhase.RINGING || phase == CallPhase.IDLE) {
                    Log.d(TAG, "  Tearing down stale connection to accept new one")
                    teardownCall(showEnded = false)
                } else {
                    Log.d(TAG, "  Rejected (busy: active call in phase=$phase)")
                    rejectBusy(socket)
                    return
                }
            }
            Log.d(TAG, "Door connected from ${socket.inetAddress?.hostAddress}:${socket.port} -> opening call channel")
            val conn = CallConnection(
                socket = socket,
                scope = scope,
                onFrame = ::onFrame,
                onClosed = ::onConnectionClosed,
            )
            connection = conn
            conn.start()
        }
    }

    private fun rejectBusy(socket: Socket) {
        runCatching {
            socket.getOutputStream().apply {
                write(Commands.deviceBusy())
                flush()
            }
            socket.close()
        }
    }

    private fun onFrame(channel: Channel, payload: ByteArray) {
        when (channel) {
            Channel.CONTROL -> handleControl(payload)
            Channel.VIDEO -> if (phase != CallPhase.IDLE) {
                // Feed only — never start the decoder from this socket-thread path.
                // Attach/detach happens on the locked surface+phase transitions; doing
                // it here would race the main thread into two decoders on one Surface.
                video.submit(payload)
                if (!_uiState.value.hasVideoFrames) {
                    _uiState.update { it.copy(hasVideoFrames = true) }
                }
            }
            Channel.AUDIO -> if (phase == CallPhase.CONNECTED) audio?.playDownlink(payload)
        }
    }

    private fun handleControl(payload: ByteArray) {
        // Log the raw control JSON so a capture shows exactly what the door sent
        // (command name / casing / shape) when classification doesn't match.
        Log.d(TAG, "CONTROL rx: ${payload.decodeToString()}")
        val msg = Commands.parse(payload)
        if (msg == null) {
            Log.d(TAG, "  Unparseable control payload")
            return
        }
        val command = msg.classify()
        Log.d(TAG, "  parsed name='${msg.name}' -> classified=$command")
        when (command) {
            InboundCommand.CALL -> onCall()
            InboundCommand.GET_CALL_INFO -> onGetCallInfo()
            InboundCommand.HANG_UP -> onRemoteHangUp()
            InboundCommand.UNKNOWN -> Log.d(TAG, "  Unknown command: ${msg.name}")
        }
    }

    private fun onCall() = synchronized(stateLock) {
        Log.d(TAG, "onCall() invoked, current phase=$phase, connection=${connection != null}")
        if (phase != CallPhase.IDLE) {
            Log.w(TAG, "Call received but ignored; already in phase=$phase — if stuck, restart the app")
            return // already busy with a call
        }
        Log.d(TAG, "Incoming Call -> RINGING")
        phase = CallPhase.RINGING
        val label = deviceConfig.identity.value.doorName
        _uiState.update {
            it.copy(
                phase = CallPhase.RINGING,
                callerLabel = label,
                videoAvailable = true,
                hasVideoFrames = false,
                muted = false,
                transientMessage = null,
            )
        }
        ensureVideoStarted()
    }

    private fun onGetCallInfo() = synchronized(stateLock) {
        // Re-confirm our answer if the door re-queries mid-call.
        if (phase == CallPhase.CONNECTED) {
            val conn = connection ?: return
            Commands.answerFrames().forEach { conn.enqueue(it) }
        }
    }

    private fun onRemoteHangUp() = synchronized(stateLock) {
        if (phase == CallPhase.IDLE) return
        teardownCall(showEnded = true)
    }

    private fun onConnectionClosed() = synchronized(stateLock) {
        connection = null
        if (phase != CallPhase.IDLE) teardownCall(showEnded = true) else stopVideo()
    }

    // --- user intents ---

    fun answer() = synchronized(stateLock) {
        if (phase != CallPhase.RINGING) return
        val conn = connection ?: run {
            teardownCall(showEnded = true)
            return
        }
        phase = CallPhase.CONNECTING
        _uiState.update { it.copy(phase = CallPhase.CONNECTING) }

        // Answer handshake: the three Answer variants, in order. The reference
        // client sends no StartTalk on accept, so we don't either.
        Commands.answerFrames().forEach { conn.enqueue(it) }

        startAudio()

        phase = CallPhase.CONNECTED
        _uiState.update { it.copy(phase = CallPhase.CONNECTED) }
    }

    fun decline() = synchronized(stateLock) {
        if (phase == CallPhase.IDLE) return
        connection?.enqueue(Commands.hangUp())
        teardownCall(showEnded = false)
    }

    /** End an active call (same protocol as decline; distinct intent for clarity). */
    fun endCall() = decline()

    fun unlock() = synchronized(stateLock) {
        // SEC-2: only from a connected, on-screen call.
        if (phase != CallPhase.CONNECTED) return
        connection?.enqueue(Commands.openDoor())
        showTransient(MESSAGE_UNLOCKED)
    }

    fun setMuted(muted: Boolean) = synchronized(stateLock) {
        audio?.setMuted(muted)
        _uiState.update { it.copy(muted = muted) }
    }

    /**
     * Re-arm capture when the mic is granted *during* a call. The pipeline was
     * started without an uplink (permission was denied at answer time), so we
     * restart it now that recording is allowed.
     */
    fun onMicPermissionResult(granted: Boolean) = synchronized(stateLock) {
        if (!granted || phase != CallPhase.CONNECTED) return
        if (_uiState.value.micAvailable) return // already capturing
        audio?.stop()
        audio = null
        startAudio()
    }

    fun setVideoSurface(newSurface: Surface) = synchronized(stateLock) {
        if (newSurface === surface) return
        surface = newSurface
        if (phase != CallPhase.IDLE) {
            // A new SurfaceView (ring screen -> in-call screen) replaced the old one:
            // detach the decoder bound to the previous surface, then re-attach.
            stopVideo()
            ensureVideoStarted()
        }
    }

    /**
     * The given surface's [SurfaceView] was destroyed. Only tear down if it's still
     * the active surface: when screens swap, the outgoing view's destroy callback can
     * arrive *after* the incoming view's create, and must not kill the new decoder.
     */
    fun clearVideoSurface(oldSurface: Surface) = synchronized(stateLock) {
        if (oldSurface !== surface) return
        surface = null
        stopVideo()
    }

    fun clearTransientMessage() {
        _uiState.update { it.copy(transientMessage = null) }
    }

    // --- media + teardown helpers (call holding stateLock, except submit paths) ---

    private fun startAudio() {
        val micGranted = hasRecordPermission()
        val pipeline = AudioPipeline(
            audioManager = audioManager,
            scope = scope,
            onAudioEncoded = { alaw -> connection?.enqueue(Frame.encode(Channel.AUDIO, alaw)) },
            onMicUnavailable = { _uiState.update { it.copy(micAvailable = false) } },
        )
        pipeline.start(captureEnabled = micGranted)
        pipeline.setMuted(_uiState.value.muted)
        audio = pipeline
        _uiState.update { it.copy(micAvailable = micGranted) }
    }

    private fun ensureVideoStarted() {
        val s = surface ?: return
        if (!videoRunning) {
            video.start(s)
            videoRunning = true
        }
    }

    private fun stopVideo() {
        if (videoRunning) {
            video.stop()
            videoRunning = false
        }
    }

    private fun onVideoError() = synchronized(stateLock) {
        videoRunning = false
        _uiState.update { it.copy(videoAvailable = false) }
    }

    private fun teardownCall(showEnded: Boolean) {
        phase = CallPhase.IDLE
        audio?.stop()
        audio = null
        stopVideo()
        val conn = connection
        connection = null
        conn?.close()
        _uiState.update {
            it.copy(
                phase = CallPhase.IDLE,
                muted = false,
                hasVideoFrames = false,
                videoAvailable = true,
                micAvailable = true,
            )
        }
        if (showEnded) showTransient(MESSAGE_ENDED)
    }

    private fun showTransient(message: String) {
        _uiState.update { it.copy(transientMessage = message) }
        transientJob?.cancel()
        transientJob = scope.launch {
            delay(TRANSIENT_MS)
            _uiState.update { if (it.transientMessage == message) it.copy(transientMessage = null) else it }
        }
    }

    private fun buildScreenInfo(): ScreenInfo {
        val id = deviceConfig.identity.value
        return ScreenInfo(
            alias = id.alias,
            serial = id.serial,
            dstAddr = id.dstAddr,
        )
    }

    private fun hasRecordPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private companion object {
        const val TAG = "CallController"
        const val TRANSIENT_MS = 2500L
        const val MESSAGE_UNLOCKED = "Door unlocked"
        const val MESSAGE_ENDED = "Call ended"
    }
}
