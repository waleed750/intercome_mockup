package com.smarthome.intercom.ui.screens

import android.view.Surface
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.smarthome.intercom.R
import com.smarthome.intercom.call.CallUiState
import com.smarthome.intercom.ui.components.DoorVideoSurface
import com.smarthome.intercom.ui.components.RoundActionButton
import com.smarthome.intercom.ui.theme.DeclineRed
import com.smarthome.intercom.ui.theme.DoorBlue
import com.smarthome.intercom.ui.theme.DoorBlueDark
import com.smarthome.intercom.ui.theme.PanelSurfaceVariant

/**
 * In-call screen (frontend spec §3.3). Live video fills; the action bar carries
 * Unlock (primary, prominent), Mute, and End. Unlock is the largest, central,
 * unmistakable target — the most important in-call action.
 */
@Composable
fun InCallScreen(
    state: CallUiState,
    onUnlock: () -> Unit,
    onToggleMute: () -> Unit,
    onEnd: () -> Unit,
    onRequestMic: () -> Unit,
    onSurfaceAvailable: (Surface) -> Unit,
    onSurfaceDestroyed: (Surface) -> Unit,
) {
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

        // Top status row
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(Brush.verticalGradient(listOf(Color.Black.copy(alpha = 0.6f), Color.Transparent)))
                .systemBarsPadding()
                .padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            StatusPill(text = stringResource(R.string.status_connected), color = DoorBlue, textColor = DoorBlueDark)
            if (state.muted) {
                Spacer(Modifier.height(8.dp))
                StatusPill(
                    text = stringResource(R.string.muted_banner),
                    color = PanelSurfaceVariant,
                    textColor = Color.White,
                )
            }
        }

        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.85f))))
                .systemBarsPadding()
                .padding(horizontal = 24.dp, vertical = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (!state.micAvailable) {
                MicBanner(onRequestMic)
                Spacer(Modifier.height(20.dp))
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RoundActionButton(
                    icon = if (state.muted) Icons.Filled.MicOff else Icons.Filled.Mic,
                    label = stringResource(if (state.muted) R.string.action_unmute else R.string.action_mute),
                    containerColor = PanelSurfaceVariant,
                    diameter = 64.dp,
                    onClick = onToggleMute,
                )
                RoundActionButton(
                    icon = Icons.Filled.LockOpen,
                    label = stringResource(R.string.action_unlock),
                    containerColor = DoorBlue,
                    contentColor = DoorBlueDark,
                    diameter = 88.dp,
                    onClick = onUnlock,
                )
                RoundActionButton(
                    icon = Icons.Filled.CallEnd,
                    label = stringResource(R.string.action_end),
                    containerColor = DeclineRed,
                    diameter = 64.dp,
                    onClick = onEnd,
                )
            }
        }
    }
}

@Composable
private fun StatusPill(text: String, color: Color, textColor: Color) {
    Box(
        modifier = Modifier
            .background(color, RoundedCornerShape(50))
            .padding(horizontal = 16.dp, vertical = 6.dp),
    ) {
        Text(text = text, color = textColor, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
private fun MicBanner(onRequestMic: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(PanelSurfaceVariant, RoundedCornerShape(12.dp))
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = stringResource(R.string.rationale_mic),
            color = Color.White,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.weight(1f).padding(end = 12.dp),
        )
        Button(onClick = onRequestMic) { Text(stringResource(R.string.perm_fix)) }
    }
}
