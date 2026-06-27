package com.smarthome.intercom.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

// The intercom is a dedicated indoor panel, so it uses one cohesive dark scheme
// regardless of system theme — bright video and clear actions on a dark ground.
private val IntercomColors = darkColorScheme(
    primary = DoorBlue,
    onPrimary = DoorBlueDark,
    secondary = DoorBlue,
    onSecondary = DoorBlueDark,
    background = PanelBackground,
    onBackground = OnPanel,
    surface = PanelSurface,
    onSurface = OnPanel,
    surfaceVariant = PanelSurfaceVariant,
    onSurfaceVariant = OnPanelMuted,
    error = DeclineRed,
    onError = OnAction,
)

@Composable
fun IntercomTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = IntercomColors,
        typography = IntercomTypography,
        content = content,
    )
}
