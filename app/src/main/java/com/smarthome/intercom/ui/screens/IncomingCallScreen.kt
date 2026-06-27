package com.smarthome.intercom.ui.screens

import android.view.Surface
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.smarthome.intercom.R
import com.smarthome.intercom.call.CallUiState
import com.smarthome.intercom.ui.components.DoorVideoSurface
import com.smarthome.intercom.ui.components.RoundActionButton
import com.smarthome.intercom.ui.theme.AnswerGreen
import com.smarthome.intercom.ui.theme.DeclineRed

/**
 * Full-screen incoming call (frontend spec §3.2). Live video fills the screen
 * during ring; the visitor is visible before the user answers. Large Answer
 * (green) / Decline (red) targets sit at the bottom over a legibility scrim.
 */
@Composable
fun IncomingCallScreen(
    state: CallUiState,
    onAnswer: () -> Unit,
    onDecline: () -> Unit,
    onSurfaceAvailable: (Surface) -> Unit,
    onSurfaceDestroyed: (Surface) -> Unit,
) {
    val pulse = rememberInfiniteTransition(label = "ring")
    val statusAlpha by pulse.animateFloat(
        initialValue = 0.45f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(900), RepeatMode.Reverse),
        label = "statusAlpha",
    )

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        DoorVideoSurface(
            onSurfaceAvailable = onSurfaceAvailable,
            onSurfaceDestroyed = onSurfaceDestroyed,
            modifier = Modifier.fillMaxSize(),
            showOverlay = !state.hasVideoFrames || !state.videoAvailable,
            overlayText = if (!state.videoAvailable) {
                stringResource(R.string.video_unavailable)
            } else {
                stringResource(R.string.connecting_to_door)
            },
        )

        // Top scrim + caller label
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    Brush.verticalGradient(listOf(Color.Black.copy(alpha = 0.7f), Color.Transparent)),
                )
                .systemBarsPadding()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = stringResource(R.string.incoming_calling, state.callerLabel),
                style = MaterialTheme.typography.headlineMedium,
                color = Color.White,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = stringResource(R.string.incoming_status),
                style = MaterialTheme.typography.titleLarge,
                color = Color.White,
                modifier = Modifier.alpha(statusAlpha),
            )
        }

        // Bottom scrim + actions
        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .background(
                    Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.8f))),
                )
                .systemBarsPadding()
                .padding(horizontal = 48.dp, vertical = 32.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            RoundActionButton(
                icon = Icons.Filled.CallEnd,
                label = stringResource(R.string.action_decline),
                containerColor = DeclineRed,
                diameter = 80.dp,
                onClick = onDecline,
            )
            RoundActionButton(
                icon = Icons.Filled.Call,
                label = stringResource(R.string.action_answer),
                containerColor = AnswerGreen,
                diameter = 80.dp,
                onClick = onAnswer,
            )
        }
    }
}
