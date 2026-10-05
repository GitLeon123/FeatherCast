package app.feathercast.phone

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import app.feathercast.protocol.PhoneMessages

/**
 * "Find my phone": rings at full alarm volume, even in silent mode, until
 * stopped on the phone or the PC, or after a minute.
 */
object Ringer {
    const val CHANNEL_ID = "ring"
    private const val NOTIFICATION_ID = 7
    private const val RING_MS = 60_000L

    private val main = Handler(Looper.getMainLooper())
    private var player: MediaPlayer? = null
    // Ringing also covers vibration when the sound could not play, so it is tracked separately.
    private var active = false
    private var savedVolume = -1
    private val timeout = Runnable { stop(PhoneApp.instance) }

    fun createChannel(context: Context) {
        val channel = NotificationChannel(CHANNEL_ID, "Find my phone", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "Shows while the PC makes this phone ring."
            setSound(null, null)
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    fun start(context: Context) {
        main.post {
            if (active) return@post
            active = true
            val audio = context.getSystemService(AudioManager::class.java)
            try {
                savedVolume = audio.getStreamVolume(AudioManager.STREAM_ALARM)
                audio.setStreamVolume(AudioManager.STREAM_ALARM, audio.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
            } catch (_: SecurityException) {
                savedVolume = -1
            }
            val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_ALARM)
                ?: RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            player = try {
                MediaPlayer().apply {
                    setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_ALARM)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build(),
                    )
                    setDataSource(context, uri)
                    isLooping = true
                    prepare()
                    start()
                }
            } catch (_: Exception) {
                null
            }
            vibrate(context, true)
            showNotification(context)
            main.postDelayed(timeout, RING_MS)
            LinkManager.instance.sendAsync(PhoneMessages.ringState(true))
        }
    }

    fun stop(context: Context) {
        main.post {
            main.removeCallbacks(timeout)
            val wasRinging = active
            active = false
            player?.let {
                try {
                    it.stop()
                } catch (_: IllegalStateException) {
                }
                it.release()
            }
            player = null
            vibrate(context, false)
            if (savedVolume >= 0) {
                try {
                    context.getSystemService(AudioManager::class.java)
                        .setStreamVolume(AudioManager.STREAM_ALARM, savedVolume, 0)
                } catch (_: SecurityException) {
                }
                savedVolume = -1
            }
            NotificationManagerCompat.from(context).cancel(NOTIFICATION_ID)
            if (wasRinging) LinkManager.instance.sendAsync(PhoneMessages.ringState(false))
        }
    }

    private fun vibrator(context: Context): Vibrator? =
        if (Build.VERSION.SDK_INT >= 31) {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        }

    private fun vibrate(context: Context, on: Boolean) {
        val vibrator = vibrator(context) ?: return
        if (on) vibrator.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 800, 600), 0)) else vibrator.cancel()
    }

    private fun showNotification(context: Context) {
        val stop = PendingIntent.getBroadcast(
            context, 0, Intent(context, StopRingReceiver::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val pc = LinkManager.instance.state.value.pcName.ifEmpty { "your PC" }
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_feathercast)
            .setContentTitle("Ringing from $pc")
            .setContentText("Tap to stop.")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .setContentIntent(stop)
            .setDeleteIntent(stop)
            .addAction(0, "Stop", stop)
            .setOngoing(true)
            .build()
        try {
            NotificationManagerCompat.from(context).notify(NOTIFICATION_ID, notification)
        } catch (_: SecurityException) {
        }
    }
}

class StopRingReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        Ringer.stop(context.applicationContext)
    }
}
