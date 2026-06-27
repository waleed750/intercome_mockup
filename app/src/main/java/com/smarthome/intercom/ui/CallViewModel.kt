package com.smarthome.intercom.ui

import android.app.Application
import android.view.Surface
import androidx.lifecycle.AndroidViewModel
import com.smarthome.intercom.appContainer
import com.smarthome.intercom.call.CallController
import com.smarthome.intercom.config.DeviceConfig

/**
 * Thin bridge between Compose and the shared [CallController]. Holds no call
 * state of its own — it republishes the controller's [uiState] and forwards
 * intents — so the UI and the service always agree on what the call is doing.
 */
class CallViewModel(app: Application) : AndroidViewModel(app) {

    private val controller: CallController = app.appContainer.controller
    val deviceConfig: DeviceConfig = app.appContainer.deviceConfig

    val uiState = controller.uiState

    fun onResume() = controller.refreshConnectivity()

    fun answer() = controller.answer()
    fun decline() = controller.decline()
    fun endCall() = controller.endCall()
    fun unlock() = controller.unlock()
    fun toggleMute() = controller.setMuted(!uiState.value.muted)
    fun clearTransientMessage() = controller.clearTransientMessage()
    fun onMicPermissionResult(granted: Boolean) = controller.onMicPermissionResult(granted)

    fun onSurfaceAvailable(surface: Surface) = controller.setVideoSurface(surface)
    fun onSurfaceDestroyed(surface: Surface) = controller.clearVideoSurface(surface)
}
