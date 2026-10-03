package app.feathercast.phone

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import app.feathercast.protocol.LinkSession
import app.feathercast.protocol.ScreenMessages
import app.feathercast.protocol.ScreenRequest
import app.feathercast.protocol.SCREEN_REQUEST_LIFETIME_MS
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

data class ScreenUiState(val pending: Boolean = false, val active: Boolean = false, val detail: String = "")

/** In-memory consent tied to one authenticated connection. It never survives reconnect. */
object ScreenBridge {
    private val main = Handler(Looper.getMainLooper())
    private val mutableState = MutableStateFlow(ScreenUiState())
    val state = mutableState.asStateFlow()
    private var request: ScreenRequest? = null
    @Volatile private var owner: LinkSession? = null
    private var expiresAt = 0L
    @Volatile var activeId: String = ""
        private set
    @Volatile var generation: Int = 0
        private set
    @Volatile var displayWidth = 0
        private set
    @Volatile var displayHeight = 0
        private set

    @Synchronized
    fun request(context: Context, next: ScreenRequest, connection: LinkSession) {
        stop(context, "The previous screen sharing request was replaced.")
        if (!PhoneApp.instance.store.screenSharing) {
            LinkManager.instance.sendAsync(ScreenMessages.state(next.sessionId, "error", detail = "Enable Screen sharing in the FeatherCast app on your phone."))
            return
        }
        request = next
        owner = connection
        expiresAt = SystemClock.elapsedRealtime() + SCREEN_REQUEST_LIFETIME_MS
        mutableState.value = ScreenUiState(pending = true, detail = "Your PC requests screen sharing. Tap Share screen to approve it.")
        val notifications = context.getSystemService(NotificationManager::class.java)
        notifications.createNotificationChannel(NotificationChannel(CHANNEL, "Screen sharing", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(context, 20,
            Intent(context, MainActivity::class.java).putExtra("screenRequest", next.sessionId),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        try {
            notifications.notify(REQUEST_NOTIFICATION, NotificationCompat.Builder(context, CHANNEL)
                .setSmallIcon(R.drawable.ic_stat_feathercast).setContentTitle("Screen sharing request")
                .setContentText("Open FeatherCast to share your screen with ${connection.pcName}.")
                .setContentIntent(open).setAutoCancel(true).build())
        } catch (_: SecurityException) { /* The in-app approval and PC hint remain available. */ }
        main.postDelayed({
            synchronized(this) {
                if (request?.sessionId == next.sessionId && activeId.isEmpty() && SystemClock.elapsedRealtime() >= expiresAt) {
                    stop(context, "The request expired. Start Phone Screen again on your PC.")
                }
            }
        }, SCREEN_REQUEST_LIFETIME_MS)
    }

    @Synchronized
    fun pendingRequest(): ScreenRequest? = request?.takeIf {
        activeId.isEmpty() && SystemClock.elapsedRealtime() < expiresAt &&
            owner?.let { connection -> LinkManager.instance.ownsSession(connection) } == true && PhoneApp.instance.store.screenSharing
    }

    @Synchronized
    fun begin(id: String): Pair<ScreenRequest, LinkSession>? {
        val pending = pendingRequest()?.takeIf { it.sessionId == id } ?: return null
        val connection = owner ?: return null
        activeId = id
        mutableState.value = ScreenUiState(active = true, detail = "Connecting your screen to the PC…")
        PhoneApp.instance.getSystemService(NotificationManager::class.java).cancel(REQUEST_NOTIFICATION)
        return pending to connection
    }

    @Synchronized
    fun setGeometry(id: String, nextGeneration: Int, width: Int, height: Int) {
        if (activeId != id) return
        generation = nextGeneration
        displayWidth = width
        displayHeight = height
        mutableState.value = ScreenUiState(active = true, detail = "Your screen is shared with the PC. You can stop sharing at any time.")
    }

    fun accepts(id: String, version: Int): Boolean = activeId == id && id.isNotEmpty() && generation == version &&
        PhoneApp.instance.store.screenSharing && owner?.let { LinkManager.instance.ownsSession(it) } == true

    @Synchronized
    fun stop(context: Context, detail: String = "Screen sharing stopped.", id: String? = null) {
        if (id != null && id != request?.sessionId && id != activeId) return
        val previous = request?.sessionId ?: activeId
        request = null
        owner = null
        activeId = ""
        generation = 0
        displayWidth = 0
        displayHeight = 0
        context.getSystemService(NotificationManager::class.java).cancel(REQUEST_NOTIFICATION)
        context.stopService(Intent(context, ScreenCaptureService::class.java))
        main.post { RemoteControlService.instance?.cancelGesture(); PcKeyboardService.instance?.restoreKeyboard() }
        mutableState.value = ScreenUiState(detail = detail)
        if (previous.isNotEmpty()) LinkManager.instance.sendAsync(ScreenMessages.state(previous, "stopped", detail = detail))
    }

    @Synchronized
    fun disconnected(context: Context, connection: LinkSession) {
        if (owner === connection) stop(context, "Connection lost. Start a new screen sharing session on your PC.")
    }

    const val CHANNEL = "screen-sharing"
    const val REQUEST_NOTIFICATION = 20
}
