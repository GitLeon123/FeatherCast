package app.feathercast.phone

import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import app.feathercast.protocol.MediaInfo
import app.feathercast.protocol.PhoneMessages
import java.io.ByteArrayOutputStream

/**
 * Follows the active media session (Spotify, YouTube, …) and reports it to the
 * PC; the PC can play/pause, skip, seek, and change the volume. Uses the
 * notification listener grant, so no extra permission is needed.
 */
object MediaWatcher {
    private val main = Handler(Looper.getMainLooper())
    private var manager: MediaSessionManager? = null
    private var controller: MediaController? = null
    private var lastTrack = ""
    private var lastJson = ""
    private val sendNow = Runnable { send() }

    private val sessionsListener = MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
        pick(controllers.orEmpty())
    }

    private val callback = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) = schedule()
        override fun onMetadataChanged(metadata: MediaMetadata?) = schedule()
        override fun onAudioInfoChanged(info: MediaController.PlaybackInfo) = schedule()
        override fun onSessionDestroyed() {
            main.post { refresh() }
        }
    }

    private val enabled: Boolean
        get() = PhoneApp.instance.store.mediaControl && NotifyListener.hasAccess(PhoneApp.instance)

    /** Starts watching (idempotent); call when the listener or link connects. */
    fun start(context: Context) {
        main.post {
            if (!enabled) {
                stopWatching()
                return@post
            }
            if (manager == null) {
                val sessions = context.getSystemService(MediaSessionManager::class.java) ?: return@post
                try {
                    sessions.addOnActiveSessionsChangedListener(sessionsListener, NotifyListener.component(context), main)
                    manager = sessions
                } catch (_: SecurityException) {
                    return@post
                }
            }
            refresh()
        }
    }

    fun stop() {
        main.post { stopWatching() }
    }

    /** Resends the current state, e.g. after connecting. */
    fun sendSnapshot() {
        main.post {
            lastTrack = ""
            lastJson = ""
            if (manager != null) refresh() else start(PhoneApp.instance)
        }
    }

    private fun stopWatching() {
        manager?.removeOnActiveSessionsChangedListener(sessionsListener)
        manager = null
        controller?.unregisterCallback(callback)
        controller = null
        main.removeCallbacks(sendNow)
    }

    private fun refresh() {
        val sessions = manager ?: return
        val list = try {
            sessions.getActiveSessions(NotifyListener.component(PhoneApp.instance))
        } catch (_: SecurityException) {
            emptyList()
        }
        pick(list)
    }

    private fun pick(controllers: List<MediaController>) {
        val best = controllers.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: controllers.firstOrNull()
        if (best?.sessionToken != controller?.sessionToken) {
            controller?.unregisterCallback(callback)
            controller = best
            best?.registerCallback(callback, main)
        }
        schedule()
    }

    private fun schedule() {
        main.removeCallbacks(sendNow)
        main.postDelayed(sendNow, 250)
    }

    private fun send() {
        val link = LinkManager.instance
        if (!link.isConnected) return
        val active = controller
        val metadata = active?.metadata
        if (active == null || metadata == null) {
            if (lastJson != "none") {
                lastJson = "none"
                lastTrack = ""
                link.sendAsync(PhoneMessages.mediaNone())
            }
            return
        }
        val state = active.playbackState
        val playing = state?.state == PlaybackState.STATE_PLAYING
        val title = metadata.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: metadata.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE).orEmpty()
        val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
            ?: metadata.getString(MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE).orEmpty()
        val now = System.currentTimeMillis()
        var position = state?.position ?: 0L
        if (playing && state != null && state.lastPositionUpdateTime > 0) {
            position += ((SystemClock.elapsedRealtime() - state.lastPositionUpdateTime) * state.playbackSpeed).toLong()
        }
        val info = active.playbackInfo
        val media = MediaInfo(
            app = active.packageName,
            appName = appLabel(active.packageName),
            title = title,
            artist = artist,
            playing = playing,
            position = position.coerceAtLeast(0),
            duration = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION).coerceAtLeast(0),
            positionAt = now,
            volume = info?.currentVolume ?: -1,
            volumeMax = info?.maxVolume ?: 0,
        )
        val json = PhoneMessages.mediaState(media)
        // Skip repeats that only differ in the timestamp.
        val signature = json.replace(Regex("\"(pos|posAt)\":\\d+"), "")
        val track = "${active.packageName}|$title|$artist"
        if (signature == lastJson && track == lastTrack) return
        lastJson = signature
        val art = if (track != lastTrack) artJpeg(metadata) else ByteArray(0)
        lastTrack = track
        link.sendAsync(json, art)
    }

    private fun appLabel(pkg: String): String {
        val pm = PhoneApp.instance.packageManager
        return try {
            pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
        } catch (_: Exception) {
            pkg
        }
    }

    private fun artJpeg(metadata: MediaMetadata): ByteArray {
        val bitmap = metadata.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            ?: return ByteArray(0)
        return try {
            val scale = 256f / maxOf(bitmap.width, bitmap.height).coerceAtLeast(1)
            val scaled = if (scale < 1f) {
                Bitmap.createScaledBitmap(bitmap, (bitmap.width * scale).toInt().coerceAtLeast(1), (bitmap.height * scale).toInt().coerceAtLeast(1), true)
            } else {
                bitmap
            }
            ByteArrayOutputStream().also { scaled.compress(Bitmap.CompressFormat.JPEG, 85, it) }.toByteArray()
        } catch (_: Exception) {
            ByteArray(0)
        }
    }

    fun command(command: String, position: Long) {
        main.post {
            val active = controller ?: return@post
            val controls = active.transportControls
            when (command) {
                "play" -> controls.play()
                "pause" -> controls.pause()
                "toggle" -> if (active.playbackState?.state == PlaybackState.STATE_PLAYING) controls.pause() else controls.play()
                "next" -> controls.skipToNext()
                "prev" -> controls.skipToPrevious()
                "seek" -> if (position >= 0) controls.seekTo(position)
            }
        }
    }

    fun setVolume(volume: Int) {
        main.post {
            val active = controller ?: return@post
            val max = active.playbackInfo?.maxVolume ?: return@post
            active.setVolumeTo(volume.coerceIn(0, max), 0)
        }
    }
}
