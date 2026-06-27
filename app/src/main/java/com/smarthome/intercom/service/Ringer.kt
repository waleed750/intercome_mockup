package com.smarthome.intercom.service

import android.content.Context
import android.media.AudioAttributes
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log

/**
 * Loops the system ringtone and vibration while a call is ringing, and stops
 * both the instant the user answers or declines (INT-E5). Uses the device's
 * default ringtone rather than a bundled asset.
 */
class Ringer(private val context: Context) {

    private var ringtone: Ringtone? = null
    private val vibrator: Vibrator? = run {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val mgr = context.getSystemService(VibratorManager::class.java)
            mgr?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    @Volatile private var ringing = false

    fun start() {
        if (ringing) return
        ringing = true
        startRingtone()
        startVibration()
    }

    fun stop() {
        if (!ringing) return
        ringing = false
        runCatching { ringtone?.stop() }
        ringtone = null
        runCatching { vibrator?.cancel() }
    }

    private fun startRingtone() {
        try {
            val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            val tone = RingtoneManager.getRingtone(context, uri) ?: return
            tone.audioAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                tone.isLooping = true
            }
            tone.play()
            ringtone = tone
        } catch (e: Exception) {
            Log.w(TAG, "ringtone failed", e)
        }
    }

    private fun startVibration() {
        val v = vibrator ?: return
        if (!v.hasVibrator()) return
        val pattern = longArrayOf(0, 800, 600) // wait, buzz, gap
        val effect = VibrationEffect.createWaveform(pattern, 0) // repeat from index 0
        runCatching { v.vibrate(effect) }
    }

    private companion object {
        const val TAG = "Ringer"
    }
}
