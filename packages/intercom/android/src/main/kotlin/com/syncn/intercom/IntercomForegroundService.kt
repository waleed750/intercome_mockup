package com.syncn.intercom

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import java.io.BufferedInputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import kotlin.concurrent.thread

class IntercomForegroundService : Service() {
    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null
    private var listenerThread: Thread? = null

    override fun onCreate() {
        super.onCreate()
        running = true
        createChannel()
        promote()
        startFallbackListener()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        promote()
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        running = false
        runCatching { serverSocket?.close() }
        listenerThread?.interrupt()
        listenerThread = null
        super.onDestroy()
    }

    private fun promote() {
        val notification = notification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                ID_FOREGROUND,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } else {
            startForeground(ID_FOREGROUND, notification)
        }
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val channel = NotificationChannel(
            CHANNEL_SERVICE,
            "Intercom service",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Keeps intercom discovery and calls available"
            setShowBadge(false)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        val calls = NotificationChannel(
            CHANNEL_CALLS,
            "Intercom calls",
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = "Incoming intercom calls"
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            setBypassDnd(true)
        }
        getSystemService(NotificationManager::class.java).createNotificationChannel(calls)
    }

    private fun notification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            100,
            packageManager.getLaunchIntentForPackage(packageName),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_SERVICE)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle("Intercom ready")
            .setContentText("Listening for door calls")
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun startFallbackListener() {
        if (listenerThread?.isAlive == true) return
        listenerThread = thread(name = "syncn-intercom-fgs-listener") {
            while (running) {
                val socket = tryBind()
                if (socket == null) {
                    Thread.sleep(2000)
                    continue
                }
                serverSocket = socket
                try {
                    while (running) {
                        val client = socket.accept()
                        handleFallbackCall(client)
                    }
                } catch (_: Exception) {
                    runCatching { socket.close() }
                    serverSocket = null
                }
            }
        }
    }

    private fun tryBind(): ServerSocket? = runCatching {
        ServerSocket().apply {
            reuseAddress = true
            bind(InetSocketAddress("0.0.0.0", 8189), 4)
        }
    }.getOrNull()

    private fun handleFallbackCall(client: Socket) {
        runCatching {
            client.soTimeout = 1500
            val input = BufferedInputStream(client.getInputStream())
            val preview = ByteArray(512)
            input.read(preview)
        }
        postIncomingNotification()
        runCatching { client.close() }
        runCatching { serverSocket?.close() }
        serverSocket = null
    }

    private fun postIncomingNotification() {
        getSharedPreferences(PREFS, MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_PENDING_INCOMING_CALL, true)
            .apply()
        val pendingIntent = PendingIntent.getActivity(
            this,
            101,
            packageManager.getLaunchIntentForPackage(packageName)?.apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_CALLS)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        val notification = builder
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle("Front Door")
            .setContentText("Incoming intercom call")
            .setCategory(Notification.CATEGORY_CALL)
            .setPriority(Notification.PRIORITY_HIGH)
            .setFullScreenIntent(pendingIntent, true)
            .setContentIntent(pendingIntent)
            .setAutoCancel(true)
            .build()
        getSystemService(NotificationManager::class.java).notify(ID_INCOMING, notification)
    }

    companion object {
        const val PREFS = "syncn_intercom_foreground_service"
        const val KEY_PENDING_INCOMING_CALL = "pending_incoming_call"
        const val CHANNEL_SERVICE = "intercom_panel_service"
        const val CHANNEL_CALLS = "intercom_panel_calls"
        const val ID_FOREGROUND = 9001
        const val ID_INCOMING = 9002
    }
}
