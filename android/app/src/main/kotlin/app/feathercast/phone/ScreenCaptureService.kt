package app.feathercast.phone

import android.Manifest
import android.app.Activity
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.view.Surface
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import app.feathercast.protocol.LinkSession
import app.feathercast.protocol.MAX_SCREEN_PACKET_BYTES
import app.feathercast.protocol.ScreenInput
import app.feathercast.protocol.ScreenMediaKind
import app.feathercast.protocol.ScreenMediaPacket
import app.feathercast.protocol.ScreenMediaQueue
import app.feathercast.protocol.ScreenMessages
import app.feathercast.protocol.screenAnnexB
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/** User-approved foreground capture. The OS grant and stream key stay in memory. */
class ScreenCaptureService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val codecThread = HandlerThread("phone-screen-codec").apply { start() }
    private val handler = Handler(codecThread.looper)
    private val stopped = AtomicBoolean(false)
    private val packets = ScreenMediaQueue()
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var video: MediaCodec? = null
    private var videoSurface: Surface? = null
    @Volatile private var media: LinkSession? = null
    @Volatile private var audioRecord: AudioRecord? = null
    @Volatile private var audioActive = false
    private var sessionId = ""
    @Volatile private var version = 0
    private var sourceWidth = 0
    private var sourceHeight = 0
    private var fullDisplay = true
    private var lastKeyRequest = 0L
    private var lastCapabilities = ""
    private val screenOff = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action == Intent.ACTION_SCREEN_OFF) finish("The phone screen was locked or turned off. Start a new session to share again.")
        }
    }

    override fun onCreate() {
        super.onCreate()
        ContextCompat.registerReceiver(this, screenOff, IntentFilter(Intent.ACTION_SCREEN_OFF), ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == STOP) { finish("Screen sharing stopped on your phone."); return START_NOT_STICKY }
        if (sessionId.isNotEmpty()) return START_NOT_STICKY
        val id = intent?.getStringExtra("session").orEmpty()
        val accepted = ScreenBridge.begin(id) ?: run { stopSelf(); return START_NOT_STICKY }
        sessionId = id
        val stop = PendingIntent.getService(this, 21, Intent(this, ScreenCaptureService::class.java).setAction(STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val open = PendingIntent.getActivity(this, 22, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val notification = NotificationCompat.Builder(this, ScreenBridge.CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_feathercast).setContentTitle("Screen shared with ${accepted.second.pcName}")
            .setContentText("Your PC can see this screen. Tap Stop to end sharing.").setOngoing(true)
            .setContentIntent(open).addAction(0, "Stop", stop).build()
        try {
            ServiceCompat.startForeground(this, 21, notification,
                if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION else 0)
            @Suppress("DEPRECATION")
            val consent = if (Build.VERSION.SDK_INT >= 33) intent?.getParcelableExtra("consent", Intent::class.java)
                else intent?.getParcelableExtra<Intent>("consent")
            if (consent == null || intent?.getIntExtra("result", 0) != Activity.RESULT_OK) {
                finish("Screen sharing was not approved."); return START_NOT_STICKY
            }
            projection = getSystemService(MediaProjectionManager::class.java).getMediaProjection(Activity.RESULT_OK, consent)
            projection?.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() { finish("Android stopped screen sharing. Start a new session on your PC.") }
                override fun onCapturedContentResize(width: Int, height: Int) {
                    if (!stopped.get() && (width != sourceWidth || height != sourceHeight)) configureVideo(width, height)
                }
            }, handler)
        } catch (_: RuntimeException) {
            finish("Android could not start screen sharing. Open FeatherCast and approve a new request.")
            return START_NOT_STICKY
        }
        scope.launch {
            try {
                val pc = PhoneApp.instance.store.load() ?: error("The phone is no longer paired.")
                if (!LinkManager.instance.ownsSession(accepted.second) || stopped.get()) return@launch
                val connection = LinkSession.connect(accepted.second.host, pc.port, PhoneApp.instance.store.deviceId,
                    accepted.first.key, screenSessionId = id)
                if (stopped.get() || !LinkManager.instance.ownsSession(accepted.second)) { connection.close(); return@launch }
                media = connection
                connection.setReadTimeout(0)
                handler.post {
                    val size = displaySize()
                    configureVideo(size.widthPixels, size.heightPixels)
                    getSystemService(DisplayManager::class.java).registerDisplayListener(displayListener, handler)
                }
                launch { sendLoop(connection) }
                if (accepted.first.audio && Build.VERSION.SDK_INT >= 29 && checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                    launch { captureAudio() }
                }
                while (isActive && !stopped.get()) {
                    val payload = connection.receive(32 * 1024)
                    val input = ScreenInput.parse(payload.json) ?: continue
                    if (!ScreenBridge.accepts(input.sessionId, input.generation)) continue
                    when (input.action) {
                        "keyframe" -> handler.post { requestKeyframe() }
                        "text", "key" -> if (fullDisplay) PcKeyboardService.instance?.input(input)
                        else -> if (fullDisplay) RemoteControlService.instance?.input(input)
                    }
                }
            } catch (_: Exception) {
                if (!stopped.get()) finish("The screen connection was lost. Start Phone Screen again on your PC.")
            }
        }
        return START_NOT_STICKY
    }

    @Suppress("DEPRECATION")
    private fun displaySize() = DisplayMetrics().also { getSystemService(WindowManager::class.java).defaultDisplay.getRealMetrics(it) }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (Build.VERSION.SDK_INT >= 34) return // The capture-resize callback is authoritative.
            val size = displaySize()
            if (size.widthPixels != sourceWidth || size.heightPixels != sourceHeight) configureVideo(size.widthPixels, size.heightPixels)
        }
    }

    private fun configureVideo(width: Int, height: Int) {
        if (stopped.get() || width <= 0 || height <= 0 || media == null) return
        try {
            display?.surface = null
            video?.let { try { it.stop() } finally { it.release() } }
            videoSurface?.release()
            sourceWidth = width; sourceHeight = height; version++
            val current = version
            val realSize = displaySize()
            fullDisplay = width == realSize.widthPixels && height == realSize.heightPixels
            RemoteControlService.instance?.let { android.os.Handler(mainLooper).post { it.cancelGesture() } }
            ScreenBridge.setGeometry(sessionId, current, realSize.widthPixels, realSize.heightPixels)
            val scale = minOf(1f, 1280f / maxOf(width, height))
            val encodedWidth = ((width * scale).toInt() / 2 * 2).coerceAtLeast(2)
            val encodedHeight = ((height * scale).toInt() / 2 * 2).coerceAtLeast(2)
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, encodedWidth, encodedHeight).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, 3_000_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, 30)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000)
                if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
            val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            video = codec
            codec.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit
                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    if (video !== codec || current != version || stopped.get()) return
                    val config = (0..1).mapNotNull { format.getByteBuffer("csd-$it")?.bytes()?.let(::screenAnnexB) }
                        .fold(ByteArray(0)) { combined, bytes -> combined + bytes }
                    offer(ScreenMediaPacket(ScreenMediaKind.VideoConfig,
                        ScreenMessages.videoConfig(sessionId, current, encodedWidth, encodedHeight), config))
                }
                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    try {
                        if (video !== codec || current != version || stopped.get() || info.size <= 0 ||
                            (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) return
                        val buffer = codec.getOutputBuffer(index) ?: return
                        buffer.position(info.offset); buffer.limit(info.offset + info.size)
                        val keyframe = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                        val packet = ScreenMediaPacket(ScreenMediaKind.Video,
                            ScreenMessages.video(sessionId, current, info.presentationTimeUs, keyframe),
                            screenAnnexB(buffer.bytes()), info.presentationTimeUs, keyframe)
                        if (!offer(packet)) requestKeyframe()
                        sendCapabilities()
                    } finally {
                        try { codec.releaseOutputBuffer(index, false) } catch (_: IllegalStateException) { }
                    }
                }
                override fun onError(codec: MediaCodec, error: MediaCodec.CodecException) {
                    if (video === codec && !stopped.get()) finish("This phone could not encode its screen. Stop sharing and try again.")
                }
            }, handler)
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = codec.createInputSurface()
            videoSurface = surface
            codec.start()
            if (display == null) {
                display = projection?.createVirtualDisplay("FeatherCast Phone Screen", encodedWidth, encodedHeight,
                    realSize.densityDpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, handler)
            } else {
                display?.resize(encodedWidth, encodedHeight, realSize.densityDpi)
                display?.surface = surface
            }
            sendCapabilities(force = true)
        } catch (_: RuntimeException) {
            finish("Screen capture could not be configured on this phone. Start a new session and try again.")
        }
    }

    private fun requestKeyframe() {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastKeyRequest < 250) return
        lastKeyRequest = now
        try { video?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
        catch (_: RuntimeException) { }
    }

    private fun offer(packet: ScreenMediaPacket): Boolean {
        if (stopped.get()) return false
        val accepted = packets.offer(packet)
        if (accepted) wake.trySend(Unit)
        return accepted
    }

    private suspend fun sendLoop(connection: LinkSession) {
        try {
            while (!stopped.get()) {
                val packet = packets.poll()
                if (packet == null) { wake.receive(); continue }
                connection.send(packet.json, packet.data)
            }
        } catch (_: Exception) {
            if (!stopped.get()) finish("The screen connection was lost. Start a new session on your PC.")
        }
    }

    private fun sendCapabilities(force: Boolean = false) {
        val store = PhoneApp.instance.store
        val control = fullDisplay && store.remoteControl && RemoteControlService.instance != null
        val keyboard = fullDisplay && store.remoteControl && PcKeyboardService.selected(this)
        val summary = "$version/$control/$keyboard/$audioActive"
        if (!force && summary == lastCapabilities) return
        lastCapabilities = summary
        val detail = when {
            !fullDisplay -> "Only an app is being shared. Start again and share the entire screen to enable control."
            !control -> "To control this screen, enable Remote control and the FeatherCast accessibility service on your phone."
            !keyboard -> "Mouse control is ready. Enable and select FeatherCast PC Keyboard on your phone for typing."
            else -> "Mouse and PC keyboard are ready."
        }
        offer(ScreenMediaPacket(ScreenMediaKind.State,
            ScreenMessages.state(sessionId, "streaming", version, detail, control, keyboard, audioActive), ByteArray(0)))
    }

    @android.annotation.TargetApi(29)
    private fun captureAudio() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return
        var recorder: AudioRecord? = null
        var codec: MediaCodec? = null
        try {
            val capture = AudioPlaybackCaptureConfiguration.Builder(projection ?: return)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA).addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN).build()
            val audioFormat = AudioFormat.Builder().setSampleRate(48_000).setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build()
            val minimum = AudioRecord.getMinBufferSize(48_000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            recorder = AudioRecord.Builder().setAudioFormat(audioFormat).setAudioPlaybackCaptureConfig(capture)
                .setBufferSizeInBytes(maxOf(minimum * 2, 8192)).build()
            if (stopped.get()) return
            audioRecord = recorder
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
            val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, 48_000, 2).apply {
                setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4096)
            }
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start(); recorder.startRecording(); audioActive = true
            handler.post { sendCapabilities(force = true) }
            val data = ByteArray(4096)
            val info = MediaCodec.BufferInfo()
            val base = System.nanoTime() / 1000
            var frames = 0L
            var audioVersion = -1
            var config: ByteArray? = null
            while (!stopped.get()) {
                val input = codec.dequeueInputBuffer(10_000)
                if (input >= 0) {
                    val read = recorder.read(data, 0, data.size, AudioRecord.READ_BLOCKING)
                    if (read <= 0) break
                    codec.getInputBuffer(input)?.put(data, 0, read)
                    codec.queueInputBuffer(input, 0, read, base + frames * 1_000_000 / 48_000, 0)
                    frames += read / 4
                }
                var output = codec.dequeueOutputBuffer(info, 0)
                if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) config = codec.outputFormat.getByteBuffer("csd-0")?.bytes()
                while (output >= 0) {
                    val current = version
                    if (current > 0 && info.size > 0 && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        if (audioVersion != current) {
                            config?.let { offer(ScreenMediaPacket(ScreenMediaKind.AudioConfig, ScreenMessages.audioConfig(sessionId, current), it)) }
                            audioVersion = current
                        }
                        val buffer = codec.getOutputBuffer(output)
                        if (buffer != null) {
                            buffer.position(info.offset); buffer.limit(info.offset + info.size)
                            offer(ScreenMediaPacket(ScreenMediaKind.Audio, ScreenMessages.audio(sessionId, current, info.presentationTimeUs),
                                buffer.bytes(), info.presentationTimeUs))
                        }
                    }
                    codec.releaseOutputBuffer(output, false)
                    output = codec.dequeueOutputBuffer(info, 0)
                }
            }
        } catch (_: RuntimeException) {
            // Video and control remain usable when audio is unavailable.
        } finally {
            audioActive = false
            audioRecord = null
            try { recorder?.stop() } catch (_: RuntimeException) { }
            recorder?.release()
            try { codec?.stop() } catch (_: RuntimeException) { }
            codec?.release()
            if (!stopped.get()) handler.post { sendCapabilities(force = true) }
        }
    }

    private fun ByteBuffer.bytes(): ByteArray = ByteArray(remaining()).also { duplicate().get(it) }

    private fun finish(detail: String) {
        if (!stopped.compareAndSet(false, true)) return
        media?.close()
        try { audioRecord?.stop() } catch (_: RuntimeException) { }
        android.os.Handler(mainLooper).post { ScreenBridge.stop(this, detail, sessionId); stopSelf() }
    }

    override fun onDestroy() {
        stopped.set(true)
        unregisterReceiver(screenOff)
        media?.close()
        try { audioRecord?.stop() } catch (_: RuntimeException) { }
        scope.cancel()
        getSystemService(DisplayManager::class.java).unregisterDisplayListener(displayListener)
        handler.post {
            display?.release(); display = null
            try { video?.stop() } catch (_: RuntimeException) { }
            video?.release(); video = null
            videoSurface?.release(); videoSurface = null
            projection?.stop(); projection = null
            packets.clear()
            codecThread.quitSafely()
        }
        ScreenBridge.stop(this, id = sessionId)
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    companion object {
        private const val STOP = "app.feathercast.phone.STOP_SCREEN"
        fun start(context: Context, sessionId: String, result: Int, consent: Intent) {
            context.startForegroundService(Intent(context, ScreenCaptureService::class.java)
                .putExtra("session", sessionId).putExtra("result", result).putExtra("consent", consent))
        }
    }
}
