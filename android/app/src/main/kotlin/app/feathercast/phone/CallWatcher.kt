package app.feathercast.phone

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioManager
import android.os.Build
import android.telecom.TelecomManager
import android.telephony.TelephonyManager
import app.feathercast.protocol.CallState
import app.feathercast.protocol.PhoneMessages

/** Tells the PC about incoming calls; the PC can reject or silence them. */
object CallWatcher {
    val permissions = buildList {
        add(Manifest.permission.READ_PHONE_STATE)
        add(Manifest.permission.READ_CALL_LOG)
        add(Manifest.permission.READ_CONTACTS)
        if (Build.VERSION.SDK_INT >= 28) add(Manifest.permission.ANSWER_PHONE_CALLS)
    }.toTypedArray()

    private var state = CallState.Idle
    private var number = ""
    private var silencedFrom: Int? = null

    fun hasPermission(context: Context): Boolean =
        context.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) == PackageManager.PERMISSION_GRANTED

    val active: Boolean
        get() = PhoneApp.instance.store.callAlerts && hasPermission(PhoneApp.instance)

    @Synchronized
    fun onPhoneState(context: Context, extraState: String?, extraNumber: String?) {
        val next = when (extraState) {
            TelephonyManager.EXTRA_STATE_RINGING -> CallState.Ringing
            TelephonyManager.EXTRA_STATE_OFFHOOK -> CallState.Active
            else -> CallState.Idle
        }
        // Android sends the broadcast twice when the app may read the number.
        val nextNumber = extraNumber?.takeIf { it.isNotEmpty() } ?: if (next == state) number else ""
        if (next == state && nextNumber == number) return
        state = next
        number = nextNumber
        if (next == CallState.Idle) restoreRinger(context)
        if (!active) return
        val name = SmsBridge.contactName(context, nextNumber)
        LinkManager.instance.sendAsync(PhoneMessages.callState(next, nextNumber, name))
    }

    /** Resends the current call, e.g. right after connecting. */
    fun sendSnapshot() {
        if (!active || state == CallState.Idle) return
        LinkManager.instance.sendAsync(
            PhoneMessages.callState(state, number, SmsBridge.contactName(PhoneApp.instance, number)),
        )
    }

    fun reject(context: Context) {
        if (Build.VERSION.SDK_INT < 28 ||
            context.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) != PackageManager.PERMISSION_GRANTED
        ) {
            silence(context)
            return
        }
        try {
            @Suppress("DEPRECATION")
            context.getSystemService(TelecomManager::class.java)?.endCall()
        } catch (_: SecurityException) {
        }
    }

    @Synchronized
    fun silence(context: Context) {
        val audio = context.getSystemService(AudioManager::class.java) ?: return
        if (silencedFrom != null) return
        val mode = audio.ringerMode
        try {
            audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_MUTE, 0)
            silencedFrom = mode
        } catch (_: SecurityException) {
            // Muting may count as a Do Not Disturb change; vibrate-only is always allowed.
            try {
                audio.ringerMode = AudioManager.RINGER_MODE_VIBRATE
                silencedFrom = mode
            } catch (_: SecurityException) {
            }
        }
    }

    private fun restoreRinger(context: Context) {
        val mode = silencedFrom ?: return
        silencedFrom = null
        val audio = context.getSystemService(AudioManager::class.java) ?: return
        try {
            audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_UNMUTE, 0)
            if (audio.ringerMode != mode) audio.ringerMode = mode
        } catch (_: SecurityException) {
        }
    }
}

class CallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return
        @Suppress("DEPRECATION")
        CallWatcher.onPhoneState(
            context.applicationContext,
            intent.getStringExtra(TelephonyManager.EXTRA_STATE),
            intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER),
        )
    }
}
