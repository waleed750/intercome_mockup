package com.smarthome.intercom.ui.components

import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

/**
 * Renders the door's video. Wraps a [SurfaceView] and reports its [Surface]
 * lifecycle up so the [com.smarthome.intercom.media.VideoPipeline] can attach
 * the decoder. While no frames have rendered (or video failed) the [overlay]
 * placeholder shows through.
 */
@Composable
fun DoorVideoSurface(
    onSurfaceAvailable: (Surface) -> Unit,
    onSurfaceDestroyed: (Surface) -> Unit,
    modifier: Modifier = Modifier,
    showOverlay: Boolean = false,
    overlayText: String = "",
) {
    Box(modifier = modifier, contentAlignment = Alignment.Center) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    // The door delivers a horizontally mirrored image; flip it back so
                    // on-screen video matches reality (the visitor's left is on the left).
                    scaleX = -1f
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            onSurfaceAvailable(holder.surface)
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int,
                        ) = Unit

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            onSurfaceDestroyed(holder.surface)
                        }
                    })
                }
            },
        )
        if (showOverlay) {
            Text(
                text = overlayText,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodyLarge,
            )
        }
    }
}
