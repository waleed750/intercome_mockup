package com.smarthome.intercom.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import com.smarthome.intercom.R
import com.smarthome.intercom.ui.MainActivity

/**
 * Builds the two notifications the service relies on:
 *  - a low-key **ongoing** notification that keeps the service in the foreground
 *    so calls arrive while backgrounded;
 *  - a high-importance **incoming-call** notification with a full-screen intent
 *    so the ring UI appears over the lock screen (INT-E2).
 */
object CallNotifications {
    const val CHANNEL_SERVICE = "intercom_service"
    const val CHANNEL_CALLS = "intercom_calls"

    const val ID_FOREGROUND = 1
    const val ID_INCOMING = 2

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java)

        val service = NotificationChannel(
            CHANNEL_SERVICE,
            context.getString(R.string.notif_channel_service),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = context.getString(R.string.notif_channel_service_desc)
            setShowBadge(false)
        }

        val calls = NotificationChannel(
            CHANNEL_CALLS,
            context.getString(R.string.notif_channel_calls),
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = context.getString(R.string.notif_channel_calls_desc)
            setShowBadge(true)
            enableVibration(false) // the service drives vibration so it stops instantly
            setBypassDnd(true)
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        }

        nm.createNotificationChannel(service)
        nm.createNotificationChannel(calls)
    }

    /** Persistent foreground notification: "Listening" or "In call". */
    fun foreground(context: Context, contentText: String): Notification =
        NotificationCompat.Builder(context, CHANNEL_SERVICE)
            .setSmallIcon(R.drawable.ic_call_notification)
            .setContentTitle(context.getString(R.string.notif_listening_title))
            .setContentText(contentText)
            .setContentIntent(openAppIntent(context, incoming = false))
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()

    /** Full-screen incoming-call notification with Answer/Decline actions. */
    fun incoming(context: Context, callerLabel: String): Notification =
        NotificationCompat.Builder(context, CHANNEL_CALLS)
            .setSmallIcon(R.drawable.ic_call_notification)
            .setContentTitle(callerLabel)
            .setContentText(context.getString(R.string.notif_incoming_text))
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setOngoing(true)
            .setAutoCancel(false)
            .setFullScreenIntent(openAppIntent(context, incoming = true), true)
            .setContentIntent(openAppIntent(context, incoming = true))
            .addAction(
                R.drawable.ic_call_notification,
                context.getString(R.string.action_decline),
                servicePendingIntent(context, IntercomService.ACTION_DECLINE, REQ_DECLINE),
            )
            .addAction(
                R.drawable.ic_call_notification,
                context.getString(R.string.action_answer),
                answerIntent(context),
            )
            .build()

    private fun openAppIntent(context: Context, incoming: Boolean): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(MainActivity.EXTRA_FROM_CALL, incoming)
        }
        val req = if (incoming) REQ_OPEN_INCOMING else REQ_OPEN_APP
        return PendingIntent.getActivity(
            context,
            req,
            intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
    }

    /**
     * Answer button: opens the Activity with EXTRA_ANSWER_ON_OPEN=true.
     * The Activity reads the extra and calls controller.answer().
     * This works even when the app is killed because PendingIntent.getActivity
     * is allowed to start an activity from a high-priority notification.
     */
    private fun answerIntent(context: Context): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_SINGLE_TOP or
                    Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra(MainActivity.EXTRA_FROM_CALL, true)
            putExtra(EXTRA_ANSWER_ON_OPEN, true)
        }
        return PendingIntent.getActivity(
            context,
            REQ_ANSWER,
            intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
    }

    const val EXTRA_ANSWER_ON_OPEN = "com.smarthome.intercom.ANSWER_ON_OPEN"

    private fun servicePendingIntent(context: Context, action: String, requestCode: Int): PendingIntent {
        val intent = Intent(context, IntercomService::class.java).setAction(action)
        return PendingIntent.getService(
            context,
            requestCode,
            intent,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
    }

    private const val REQ_OPEN_APP = 10
    private const val REQ_OPEN_INCOMING = 11
    private const val REQ_ANSWER = 12
    private const val REQ_DECLINE = 13
}
