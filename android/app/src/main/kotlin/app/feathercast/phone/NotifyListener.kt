package app.feathercast.phone

import android.app.Notification
import android.app.PendingIntent
import android.app.RemoteInput
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.os.Bundle
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationManagerCompat
import app.feathercast.protocol.ActionInfo
import app.feathercast.protocol.PhoneMessages
import app.feathercast.protocol.message
import kotlinx.serialization.json.put
import java.io.ByteArrayOutputStream
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

/** Forwards phone notifications (messages, mail, …) to the PC. */
class NotifyListener : NotificationListenerService() {
    private val iconCache = ConcurrentHashMap<String, ByteArray>()
    private val labelCache = ConcurrentHashMap<String, String>()
    private val actions = ConcurrentHashMap<String, Array<Notification.Action>>()

    override fun onListenerConnected() {
        active = this
        sendSnapshot()
        MediaWatcher.start(applicationContext)
    }

    override fun onListenerDisconnected() {
        if (active === this) active = null
    }

    override fun onDestroy() {
        if (active === this) active = null
        super.onDestroy()
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (!forwardingEnabled() || !shouldForward(sbn)) return
        worker.execute { post(sbn) }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (!forwardingEnabled() || !shouldForward(sbn)) return
        // On the same worker as post(), so a queued post cannot re-add the key afterwards.
        worker.execute {
            actions.remove(sbn.key)
            LinkManager.instance.send(message("notification.removed") { put("key", sbn.key) })
        }
    }

    private fun forwardingEnabled(): Boolean =
        PhoneApp.instance.store.sendNotifications && LinkManager.instance.isConnected

    private fun shouldForward(sbn: StatusBarNotification): Boolean {
        val notification = sbn.notification
        if (sbn.packageName == packageName) return false
        if (sbn.isOngoing) return false
        if (notification.flags and Notification.FLAG_GROUP_SUMMARY != 0) return false
        val extras = notification.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)
        val text = extras.getCharSequence(Notification.EXTRA_TEXT)
        return !title.isNullOrBlank() || !text.isNullOrBlank()
    }

    private fun post(sbn: StatusBarNotification) {
        if (!forwardingEnabled()) return
        val extras = sbn.notification.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val text = (extras.getCharSequence(Notification.EXTRA_BIG_TEXT)
            ?: extras.getCharSequence(Notification.EXTRA_TEXT))?.toString().orEmpty()
        val list: Array<Notification.Action> = sbn.notification.actions?.filterNotNull()?.toTypedArray() ?: emptyArray()
        if (list.isEmpty()) actions.remove(sbn.key) else actions[sbn.key] = list
        val infos = list.mapIndexedNotNull { index, action ->
            val label = action.title?.toString().orEmpty()
            if (label.isBlank() || action.actionIntent == null) null
            else ActionInfo(index, label.take(40), action.remoteInputs?.any { it.allowFreeFormInput } == true)
        }
        LinkManager.instance.send(
            PhoneMessages.notificationPosted(
                sbn.key, sbn.packageName, appLabel(sbn.packageName), title.take(200), text.take(1000), sbn.postTime, infos,
            ),
            appIcon(sbn.packageName),
        )
    }

    /** Runs a notification button; [text] fills its reply field, if any. */
    fun runAction(key: String, index: Int, text: String) {
        val action = actions[key]?.getOrNull(index) ?: return
        val intent = action.actionIntent ?: return
        val fillIn = Intent()
        val inputs = action.remoteInputs.orEmpty()
        if (inputs.isNotEmpty()) {
            if (text.isEmpty()) return
            val results = Bundle()
            inputs.forEach { results.putCharSequence(it.resultKey, text) }
            RemoteInput.addResultsToIntent(inputs, fillIn, results)
        }
        try {
            intent.send(this, 0, fillIn)
        } catch (_: PendingIntent.CanceledException) {
        }
    }

    private fun appLabel(pkg: String): String = labelCache.getOrPut(pkg) {
        try {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(pkg, 0)).toString()
        } catch (_: Exception) {
            pkg
        }
    }

    private fun appIcon(pkg: String): ByteArray = iconCache.getOrPut(pkg) {
        try {
            val drawable = packageManager.getApplicationIcon(pkg)
            val size = 64
            val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            drawable.setBounds(0, 0, size, size)
            drawable.draw(canvas)
            ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
        } catch (_: Exception) {
            ByteArray(0)
        }
    }

    private fun snapshot() {
        if (!forwardingEnabled()) return
        actions.clear()
        LinkManager.instance.send(message("notifications.reset"))
        val current = try {
            activeNotifications ?: emptyArray()
        } catch (_: Exception) {
            emptyArray()
        }
        current.filter(::shouldForward).sortedBy { it.postTime }.takeLast(50).forEach(::post)
    }

    fun dismissKey(key: String) {
        try {
            cancelNotification(key)
        } catch (_: Exception) {
        }
    }

    companion object {
        @Volatile private var active: NotifyListener? = null
        private val worker = Executors.newSingleThreadExecutor()

        fun hasAccess(context: Context): Boolean =
            NotificationManagerCompat.getEnabledListenerPackages(context).contains(context.packageName)

        fun component(context: Context) = ComponentName(context, NotifyListener::class.java)

        /** Resends all current notifications, e.g. right after connecting. */
        fun sendSnapshot() {
            val listener = active ?: return
            if (!listener.forwardingEnabled()) return
            worker.execute { listener.snapshot() }
        }

        fun clearOnPc() {
            worker.execute { LinkManager.instance.send(message("notifications.reset")) }
        }

        fun dismiss(key: String) {
            if (key.isNotEmpty() && PhoneApp.instance.store.sendNotifications) active?.dismissKey(key)
        }

        fun performAction(key: String, index: Int, text: String) {
            val listener = active ?: return
            if (key.isEmpty() || index < 0 || !PhoneApp.instance.store.sendNotifications) return
            worker.execute { listener.runAction(key, index, text) }
        }
    }
}
