package com.smarthome.intercom

import android.app.Application
import android.content.Context
import com.smarthome.intercom.call.CallController
import com.smarthome.intercom.config.DeviceConfig
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/**
 * Hand-rolled DI container. The [CallController] is a process-wide singleton so
 * the foreground service (which drives the call) and the UI (which observes it)
 * share one instance without service binding.
 */
class AppContainer(context: Context) {
    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    val deviceConfig = DeviceConfig(context.applicationContext, appScope)
    val controller = CallController(context.applicationContext, deviceConfig)
}

class IntercomApplication : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
    }
}

/** Convenience accessor from any context. */
val Context.appContainer: AppContainer
    get() = (applicationContext as IntercomApplication).container
