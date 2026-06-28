package com.syncn.intercom

import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

object ForegroundServiceHandler : MethodChannel.MethodCallHandler {
    private const val CHANNEL = "syncn_intercom/foreground_service"
    private lateinit var context: Context

    fun register(binding: FlutterPlugin.FlutterPluginBinding) {
        context = binding.applicationContext
        MethodChannel(binding.binaryMessenger, CHANNEL).setMethodCallHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "startPanelService" -> {
                ContextCompat.startForegroundService(
                    context,
                    Intent(context, IntercomForegroundService::class.java),
                )
                result.success(null)
            }
            "stopPanelService" -> {
                context.stopService(Intent(context, IntercomForegroundService::class.java))
                result.success(null)
            }
            "takePendingIncomingCall" -> {
                val prefs = context.getSharedPreferences(IntercomForegroundService.PREFS, Context.MODE_PRIVATE)
                val pending = prefs.getBoolean(IntercomForegroundService.KEY_PENDING_INCOMING_CALL, false)
                if (pending) {
                    prefs.edit().putBoolean(IntercomForegroundService.KEY_PENDING_INCOMING_CALL, false).apply()
                }
                result.success(pending)
            }
            else -> result.notImplemented()
        }
    }
}
