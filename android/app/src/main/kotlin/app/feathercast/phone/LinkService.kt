package app.feathercast.phone

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/** Keeps the PC connection alive in the background with a quiet notification. */
class LinkService : Service() {
    private val scope = CoroutineScope(Dispatchers.Main)
    private var watcher: Job? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val link = LinkManager.instance
        // startForegroundService() requires startForeground() before the service may stop itself.
        val type = if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0
        try {
            ServiceCompat.startForeground(this, NOTIFICATION_ID, buildNotification(link.state.value), type)
        } catch (_: Exception) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (PhoneApp.instance.store.load() == null || !LocalNetworkAccess.allowed(this)) {
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        if (watcher == null) {
            watcher = scope.launch {
                link.state.map { Triple(it.status, it.pcName, it.detail) }.distinctUntilChanged().collect {
                    if (NotificationManagerCompat.from(this@LinkService).areNotificationsEnabled()) {
                        try {
                            NotificationManagerCompat.from(this@LinkService)
                                .notify(NOTIFICATION_ID, buildNotification(link.state.value))
                        } catch (_: SecurityException) {
                        }
                    }
                }
            }
            registerNetworkCallback()
        }
        link.start()
        return START_STICKY
    }

    private fun registerNetworkCallback() {
        val connectivity = getSystemService(ConnectivityManager::class.java) ?: return
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = LinkManager.instance.nudge()
        }
        val request = NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build()
        try {
            connectivity.registerNetworkCallback(request, callback)
            networkCallback = callback
        } catch (_: Exception) {
        }
    }

    override fun onDestroy() {
        watcher?.cancel()
        networkCallback?.let {
            try {
                getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(it)
            } catch (_: Exception) {
            }
        }
        LinkManager.instance.stop()
        super.onDestroy()
    }

    private fun buildNotification(state: LinkState): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val sendClip = PendingIntent.getActivity(
            this, 1,
            Intent(this, SendClipboardActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val name = state.pcName.ifEmpty { "your PC" }
        val title = when (state.status) {
            LinkStatus.Connected -> "Connected to $name"
            LinkStatus.Connecting -> "Connecting to $name…"
            else -> "Waiting for $name"
        }
        val text = if (state.status == LinkStatus.Connected) {
            "Notifications, photos and clipboard are shared."
        } else {
            state.detail.ifEmpty { "Reconnects automatically when the PC is on the same Wi-Fi." }
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_feathercast)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .addAction(0, "Send clipboard", sendClip)
            .build()
    }

    companion object {
        const val CHANNEL_ID = "link"
        const val NOTIFICATION_ID = 1

        fun start(context: Context) {
            try {
                context.startForegroundService(Intent(context, LinkService::class.java))
            } catch (_: Exception) {
                // Background start restrictions: the app starts it again when opened.
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LinkService::class.java))
        }
    }
}
