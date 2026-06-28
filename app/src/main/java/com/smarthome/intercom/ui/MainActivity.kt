package com.smarthome.intercom.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.smarthome.intercom.call.CallPhase
import com.smarthome.intercom.service.CallNotifications
import com.smarthome.intercom.service.IntercomService
import com.smarthome.intercom.ui.screens.IdleScreen
import com.smarthome.intercom.ui.screens.InCallScreen
import com.smarthome.intercom.ui.screens.IncomingCallScreen
import com.smarthome.intercom.ui.screens.SettingsScreen
import com.smarthome.intercom.ui.theme.IntercomTheme

/**
 * The single activity. It hosts the phase-driven Compose UI and, because it is
 * declared `showWhenLocked` + `turnScreenOn`, is what the service's full-screen
 * incoming-call intent launches over the lock screen (INT-E2). The call lifecycle
 * lives in [IntercomService]/CallController, so the activity only starts the
 * service, requests the runtime permissions, and renders the shared state.
 */
class MainActivity : ComponentActivity() {

    private val viewModel: CallViewModel by viewModels()

    private val startupPermissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { results ->
        if (results[Manifest.permission.RECORD_AUDIO] == true) {
            viewModel.onMicPermissionResult(true)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Show over the keyguard and wake the screen so a ringing call is
        // answerable without unlocking the device first.
        setShowWhenLocked(true)
        setTurnScreenOn(true)

        IntercomService.start(this)
        requestStartupPermissions()
        requestBatteryOptimizationExemption()
        handleAnswerIntent(intent)

        setContent {
            IntercomTheme {
                IntercomApp(viewModel)
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleAnswerIntent(intent)
    }

    private fun handleAnswerIntent(intent: Intent?) {
        if (intent?.getBooleanExtra(CallNotifications.EXTRA_ANSWER_ON_OPEN, false) == true) {
            intent.removeExtra(CallNotifications.EXTRA_ANSWER_ON_OPEN)
            viewModel.answer()
        }
    }

    private fun requestBatteryOptimizationExemption() {
        val pm = getSystemService(PowerManager::class.java)
        if (!pm.isIgnoringBatteryOptimizations(packageName)) {
            val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                data = Uri.parse("package:$packageName")
            }
            startActivity(intent)
        }
    }

    private fun requestStartupPermissions() {
        val wanted = buildList {
            if (!isGranted(Manifest.permission.RECORD_AUDIO)) {
                add(Manifest.permission.RECORD_AUDIO)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                !isGranted(Manifest.permission.POST_NOTIFICATIONS)
            ) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        if (wanted.isNotEmpty()) startupPermissions.launch(wanted.toTypedArray())
    }

    private fun isGranted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        /** Marks intents fired from the call notification / full-screen intent. */
        const val EXTRA_FROM_CALL = "com.smarthome.intercom.FROM_CALL"
    }
}

/**
 * Root of the UI. Routes to a screen purely from the call [CallPhase] so the
 * activity and the service can never disagree about what's on screen: a ringing
 * or active call always preempts Idle/Settings, and a momentary [transientMessage]
 * (e.g. "Door unlocked") floats above whatever is showing.
 */
@Composable
fun IntercomApp(viewModel: CallViewModel) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    var showSettings by rememberSaveable { mutableStateOf(false) }

    // Refresh Wi-Fi/connectivity each time the panel returns to the foreground.
    LifecycleResumeEffect(Unit) {
        viewModel.onResume()
        onPauseOrDispose { }
    }

    // Keep the display awake while ringing or in a call.
    val keepAwake = state.isRinging || state.isInCall
    val view = LocalView.current
    DisposableEffect(keepAwake) {
        view.keepScreenOn = keepAwake
        onDispose { view.keepScreenOn = false }
    }

    // A real call takes over the screen; leave Settings when one starts.
    LaunchedEffect(state.phase) {
        if (state.phase != CallPhase.IDLE) showSettings = false
    }

    val micPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> viewModel.onMicPermissionResult(granted) }

    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        when {
            state.isRinging -> IncomingCallScreen(
                state = state,
                onAnswer = viewModel::answer,
                onDecline = viewModel::decline,
                onSurfaceAvailable = viewModel::onSurfaceAvailable,
                onSurfaceDestroyed = viewModel::onSurfaceDestroyed,
            )

            state.isInCall -> InCallScreen(
                state = state,
                onUnlock = viewModel::unlock,
                onToggleMute = viewModel::toggleMute,
                onEnd = viewModel::endCall,
                onRequestMic = { micPermission.launch(Manifest.permission.RECORD_AUDIO) },
                onSurfaceAvailable = viewModel::onSurfaceAvailable,
                onSurfaceDestroyed = viewModel::onSurfaceDestroyed,
            )

            showSettings -> SettingsScreen(
                viewModel = viewModel,
                onBack = { showSettings = false },
            )

            else -> IdleScreen(
                state = state,
                onOpenSettings = { showSettings = true },
            )
        }

        state.transientMessage?.let { message ->
            TransientMessage(
                text = message,
                modifier = Modifier
                    .align(Alignment.Center)
                    .systemBarsPadding(),
            )
        }
    }
}

@Composable
private fun TransientMessage(text: String, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(50))
            .padding(horizontal = 24.dp, vertical = 14.dp),
    ) {
        Text(
            text = text,
            color = MaterialTheme.colorScheme.onSurface,
            style = MaterialTheme.typography.titleMedium,
        )
    }
}
