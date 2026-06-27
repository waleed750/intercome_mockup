package com.smarthome.intercom.service

import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.smarthome.intercom.R
import com.smarthome.intercom.appContainer
import com.smarthome.intercom.call.CallController
import com.smarthome.intercom.call.CallPhase
import com.smarthome.intercom.call.CallUiState
import kotlinx.coroutines.launch

/**
 * The always-on owner of the call lifecycle. Runs as a foreground service so
 * discovery keeps answering and calls still ring when the app is backgrounded
 * or the screen is off (INT-E1). Observes the [CallController] state and drives
 * the side effects the controller deliberately doesn't: ringtone/vibration and
 * the notifications (including the full-screen incoming-call intent).
 */

class IntercomService : LifecycleService() {

    private lateinit var controller: CallController
    private lateinit var ringer: Ringer
    private var lastPhase: CallPhase? = null

    override fun onCreate() {
        super.onCreate()
        controller = appContainer.controller
        ringer = Ringer(this)

        CallNotifications.createChannels(this)
        promoteForeground(inCall = false, micActive = false)

        controller.start()
        observeCallState()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_ANSWER -> controller.answer()
            ACTION_DECLINE -> controller.decline()
            ACTION_END -> controller.endCall()
            ACTION_STOP -> {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        ringer.stop()
        controller.shutdown()
        super.onDestroy()
    }

    private fun observeCallState() {
        lifecycleScope.launch {
            controller.uiState.collect { state -> onState(state) }
        }
    }

    private fun onState(state: CallUiState) {
        val phase = state.phase
        if (phase == lastPhase) return
        lastPhase = phase
        when (phase) {
            CallPhase.RINGING -> {
                ringer.start()
                postIncoming(state.callerLabel)
                promoteForeground(inCall = false, micActive = false)
            }
            CallPhase.CONNECTING -> {
                ringer.stop()
                cancelIncoming()
            }
            CallPhase.CONNECTED -> {
                ringer.stop()
                cancelIncoming()
                promoteForeground(inCall = true, micActive = state.micAvailable)
            }
            CallPhase.IDLE -> {
                ringer.stop()
                cancelIncoming()
                promoteForeground(inCall = false, micActive = false)
            }
        }
    }

    private fun promoteForeground(inCall: Boolean, micActive: Boolean) {
        val text = getString(
            if (inCall) R.string.status_connected else R.string.notif_listening_text,
        )
        val notification = CallNotifications.foreground(this, text)
        ServiceCompat.startForeground(
            this,
            CallNotifications.ID_FOREGROUND,
            notification,
            foregroundServiceType(useMicrophone = inCall && micActive),
        )
    }

    private fun postIncoming(callerLabel: String) {
        runCatching {
            NotificationManagerCompat.from(this)
                .notify(CallNotifications.ID_INCOMING, CallNotifications.incoming(this, callerLabel))
        }
    }

    private fun cancelIncoming() {
        NotificationManagerCompat.from(this).cancel(CallNotifications.ID_INCOMING)
    }

    /**
     * Microphone FGS type only exists on API 30+, and we only claim it once the
     * mic is actually in use (and granted) to satisfy Android 14's start rules.
     * Networking is covered by the connected-device type on API 29+.
     */
    private fun foregroundServiceType(useMicrophone: Boolean): Int = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> {
            var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            if (useMicrophone) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            type
        }
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q ->
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        else -> 0
    }

    companion object {
        const val ACTION_ANSWER = "com.smarthome.intercom.ANSWER"
        const val ACTION_DECLINE = "com.smarthome.intercom.DECLINE"
        const val ACTION_END = "com.smarthome.intercom.END"
        const val ACTION_STOP = "com.smarthome.intercom.STOP"

        /** Starts the listening service. Safe to call repeatedly. */
        fun start(context: Context) {
            val intent = Intent(context, IntercomService::class.java)
            ContextCompat.startForegroundService(context, intent)
        }
    }
}
