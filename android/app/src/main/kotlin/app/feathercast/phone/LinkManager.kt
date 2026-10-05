package app.feathercast.phone

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.provider.Settings
import app.feathercast.protocol.BEACON_PORT
import app.feathercast.protocol.Beacon
import app.feathercast.protocol.Features
import app.feathercast.protocol.FileStart
import app.feathercast.protocol.FILE_CHUNK_BYTES
import app.feathercast.protocol.MAX_STREAM_BYTES
import app.feathercast.protocol.LinkException
import app.feathercast.protocol.LinkSession
import app.feathercast.protocol.MAX_FRAME_BYTES
import app.feathercast.protocol.Pairing
import app.feathercast.protocol.PairingInvite
import app.feathercast.protocol.Payload
import app.feathercast.protocol.PcMessage
import app.feathercast.protocol.PhoneMessages
import app.feathercast.protocol.message
import app.feathercast.protocol.readAtMost
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

enum class LinkStatus { Unpaired, Connecting, Connected, Offline }

data class PcClip(val text: String, val time: Long)

data class LinkState(
    val status: LinkStatus = LinkStatus.Unpaired,
    val pcName: String = "",
    val host: String = "",
    val detail: String = "",
    val pcClipboard: List<PcClip> = emptyList(),
    val transferStatus: String = "",
    val transferActive: Boolean = false,
)

/**
 * Owns the connection to the paired PC: reconnects with backoff, finds the PC
 * again through its UDP beacon when its address changes, and dispatches
 * messages in both directions.
 */
class LinkManager(private val context: Context, private val store: LinkStore) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val main = Handler(Looper.getMainLooper())
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private val _state = MutableStateFlow(initialState())
    val state: StateFlow<LinkState> = _state.asStateFlow()

    @Volatile private var session: LinkSession? = null
    @Volatile private var incomingTransfers: IncomingTransfers? = null
    private val transferEpoch = java.util.concurrent.atomic.AtomicLong()
    private val transferAcks = java.util.concurrent.ConcurrentHashMap<String, CompletableDeferred<String?>>()
    private val cancelledTransfers = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()
    private var loop: Job? = null

    // Bulk replies (photo lists, file reads and writes, SMS scans) run one at a time off
    // the receive loop, so control messages such as ring, media and screen stay responsive.
    private val bulk = Dispatchers.IO.limitedParallelism(1)

    val deviceName: String
        get() = Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
            ?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}".trim()

    private fun initialState(): LinkState {
        val pc = store.load() ?: return LinkState()
        return LinkState(LinkStatus.Offline, pc.pcName, pc.host)
    }

    val isConnected: Boolean get() = session != null
    fun ownsSession(connection: LinkSession): Boolean = session === connection

    /** Starts the reconnect loop; called by [LinkService]. */
    @Synchronized
    fun start() {
        if (!LocalNetworkAccess.allowed(context)) {
            _state.update { it.copy(status = LinkStatus.Offline, detail = "Allow local network access to connect to your PC.") }
            return
        }
        if (loop?.isActive == true) {
            wake.trySend(Unit)
            return
        }
        val previous = loop
        loop = scope.launch {
            previous?.join()
            runLoop()
        }
    }

    @Synchronized
    fun stop() {
        val job = loop ?: return
        job.cancel()
        session?.close()
        val pc = store.load()
        // Keep the reason when the PC ended the pairing, so the connect screen can show it.
        _state.update { if (pc == null) it.copy(status = LinkStatus.Unpaired) else it.copy(status = LinkStatus.Offline, detail = "") }
    }

    /** Wakes the loop immediately, for example after the network changed. */
    fun nudge() {
        wake.trySend(Unit)
    }

    suspend fun pair(uri: String): Result<String> = withContext(Dispatchers.IO) {
        if (!LocalNetworkAccess.allowed(context)) return@withContext Result.failure(IOException("Allow local network access to pair with your PC."))
        val invite = PairingInvite.parse(uri.trim())
            ?: return@withContext Result.failure(IOException("This is not a FeatherCast pairing code."))
        try {
            val result = Pairing.pair(invite, store.deviceId, deviceName)
            stop()
            store.save(PairedPc(result.pcId, result.pcName, result.host, invite.port, result.linkKey))
            _state.value = LinkState(LinkStatus.Connecting, result.pcName, result.host)
            LinkService.start(context)
            Result.success(result.pcName)
        } catch (error: LinkException) {
            Result.failure(IOException(pairingMessage(error.code, error.message)))
        } catch (error: IOException) {
            Result.failure(IOException("Could not reach ${invite.pcName}. Make sure the phone is on the same Wi-Fi and FeatherCast is allowed through the Windows firewall."))
        }
    }

    private fun pairingMessage(code: String, fallback: String?): String = when (code) {
        "expired" -> "This pairing code has expired. Show a new one on the PC and scan again."
        "bad-proof" -> "The pairing code did not match. Scan the code again."
        else -> fallback ?: "Pairing failed."
    }

    fun unpair() {
        stop()
        store.clear()
        _state.value = LinkState()
        LinkService.stop(context)
    }

    // ---- Outgoing -------------------------------------------------------------

    /** Sends a message if connected; returns false when offline. */
    fun send(json: String, binary: ByteArray = ByteArray(0)): Boolean {
        val active = session ?: return false
        return sendToSession(active, json, binary)
    }

    private fun sendToSession(active: LinkSession, json: String, binary: ByteArray = ByteArray(0)): Boolean {
        if (session !== active) return false
        return try {
            active.send(json, binary)
            true
        } catch (_: IOException) {
            active.close()
            false
        }
    }

    fun sendAsync(json: String, binary: ByteArray = ByteArray(0)) {
        val active = session ?: return
        scope.launch { sendToSession(active, json, binary) }
    }

    fun sendClipboardText(text: String): Boolean {
        if (text.isEmpty()) return false
        return send(message("clipboard.set") { put("text", text) })
    }

    /** Sends a shared file or picture; the PC saves it to Downloads\FeatherCast. */
    suspend fun sendUri(uri: Uri): Result<String> = withContext(Dispatchers.IO) {
        val active = session ?: return@withContext Result.failure(IOException("Not connected to your PC."))
        val resolver = context.contentResolver
        var name = "shared-file"
        var size = -1L
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use {
            if (it.moveToFirst()) {
                it.getString(0)?.let { value -> name = value }
                if (!it.isNull(1)) size = it.getLong(1)
            }
        }
        if (active.fileStreamSupported) {
            val preparationEpoch = transferEpoch.get()
            var staged: java.io.File? = null
            try {
                if (size < 0) {
                    val preparation = java.io.File.createTempFile("fc-send-", ".part", context.cacheDir)
                    staged = preparation
                    _state.update { it.copy(transferStatus = "Preparing $name…", transferActive = true) }
                    var count = 0L
                    resolver.openInputStream(uri)?.use { input ->
                        preparation.outputStream().use { output ->
                            val buffer = ByteArray(FILE_CHUNK_BYTES)
                            while (true) {
                                currentCoroutineContext().ensureActive()
                                if (transferEpoch.get() != preparationEpoch || session !== active) throw IOException("File transfer cancelled.")
                                val read = input.read(buffer)
                                if (read < 0) break
                                count += read
                                if (count > MAX_STREAM_BYTES) throw IOException("Files must be 8 GB or smaller.")
                                output.write(buffer, 0, read)
                            }
                        }
                    } ?: throw IOException("Could not read $name.")
                    size = count
                }
                if (transferEpoch.get() != preparationEpoch || session !== active) throw IOException("File transfer cancelled.")
                val input = staged?.inputStream() ?: resolver.openInputStream(uri) ?: throw IOException("Could not read $name.")
                input.use { sendStream(active, name, size, "file", "", it) }
                return@withContext Result.success(name)
            } catch (error: Exception) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                _state.update { it.copy(transferStatus = error.message ?: "File transfer failed.", transferActive = false) }
                return@withContext Result.failure(error)
            } finally { staged?.delete() }
        }
        if (size > MAX_SEND_BYTES) {
            return@withContext Result.failure(IOException("$name is too large (max 40 MB)."))
        }
        val bytes = try {
            resolver.openInputStream(uri)?.use { it.readAtMost(MAX_SEND_BYTES.toInt(), sizeHint = size) }
        } catch (_: IOException) {
            null
        } catch (_: SecurityException) {
            null
        } ?: return@withContext Result.failure(IOException("Could not read $name."))
        if (bytes.size > MAX_SEND_BYTES) {
            return@withContext Result.failure(IOException("$name is too large (max 40 MB)."))
        }
        currentCoroutineContext().ensureActive()
        if (sendToSession(active, message("file.send") { put("name", name) }, bytes)) {
            Result.success(name)
        } else {
            Result.failure(IOException("Not connected to your PC."))
        }
    }

    /** Sends the newest photo list now, e.g. right after the permission was granted. */
    fun sharePhotosNow() {
        val active = session ?: return
        scope.launch(bulk) { if (store.sendPhotos && session === active) sendPhotoList(active, 60) }
    }

    fun clearPhotosOnPc() {
        sendAsync(message("photos.list") { putJsonArray("items") {} })
    }

    fun requestPcClipboard() {
        sendAsync(message("clipboard.history.request"))
    }

    // ---- Connection loop ------------------------------------------------------

    private suspend fun runLoop() {
        var backoff = 2_000L
        // An address from a beacon is only a candidate until a session authenticates there.
        var candidate: Pair<String, Int>? = null
        while (currentCoroutineContext().isActive) {
            if (!LocalNetworkAccess.allowed(context)) {
                _state.update { it.copy(status = LinkStatus.Offline, detail = "Allow local network access to connect to your PC.") }
                return
            }
            val pc = store.load() ?: run {
                _state.value = LinkState()
                return
            }
            val target = candidate ?: (pc.host to pc.port)
            val fromBeacon = candidate != null
            candidate = null
            _state.update { it.copy(status = LinkStatus.Connecting, pcName = pc.pcName, host = target.first, detail = "") }
            val connected = try {
                connect(target.first, target.second, pc.linkKey)
            } catch (error: LinkException) {
                currentCoroutineContext().ensureActive()
                // The plaintext refusal is unauthenticated, so never wipe the pairing because of it.
                // Only the last authenticated address may report it, and the user decides what to do.
                if (error.code == "unpaired" && !fromBeacon && store.load()?.linkKey?.contentEquals(pc.linkKey) == true) {
                    _state.update {
                        it.copy(status = LinkStatus.Offline, detail = "The PC no longer knows this phone. If you removed it on the PC, disconnect below and pair again.")
                    }
                    // Retry when the app opens again or the network changes.
                    wake.receive()
                    continue
                }
                null
            } catch (_: IOException) {
                null
            }
            if (connected != null) {
                backoff = 2_000L
                try {
                    currentCoroutineContext().ensureActive()
                    // The PC authenticated at the beacon address, so it becomes the stored address.
                    if (fromBeacon && store.load()?.linkKey?.contentEquals(pc.linkKey) == true) {
                        store.updateAddress(target.first, target.second)
                    }
                    runSession(connected)
                } finally {
                    ScreenBridge.disconnected(context, connected)
                    connected.close()
                    if (session === connected) session = null
                }
                currentCoroutineContext().ensureActive()
                _state.update { it.copy(status = LinkStatus.Offline, detail = "Connection lost. Reconnecting…") }
                delay(1_000)
                continue
            }
            _state.update { it.copy(status = LinkStatus.Offline, detail = "Looking for ${pc.pcName}…") }
            // Wait for a beacon from the PC, a nudge, or the backoff to pass.
            val found = withTimeoutOrNull(backoff) { awaitBeaconOrWake(pc.pcId) }
            if (found != null && found.first.isNotEmpty()) {
                candidate = found
                continue
            }
            if (found == null) backoff = (backoff * 2).coerceAtMost(30_000L) else backoff = 2_000L
        }
    }

    private fun connect(host: String, port: Int, linkKey: ByteArray): LinkSession? {
        if (host.isEmpty() || port == 0) return null
        return LinkSession.connect(host, port, store.deviceId, linkKey)
    }

    /** Returns (host, port) of the PC's beacon, or ("", 0) when woken by [nudge]. */
    private suspend fun awaitBeaconOrWake(pcId: String): Pair<String, Int> = coroutineScope {
        val result = Channel<Pair<String, Int>>(1)
        val listener = launch {
            listenForBeacon(pcId)?.let { result.trySend(it) }
        }
        val waker = launch {
            wake.receive()
            result.trySend("" to 0)
        }
        try {
            result.receive()
        } finally {
            listener.cancel()
            waker.cancel()
        }
    }

    private suspend fun listenForBeacon(pcId: String): Pair<String, Int>? {
        val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
        val lock = wifi?.createMulticastLock("feathercast-beacon")?.apply {
            setReferenceCounted(false)
            acquire()
        }
        val socket = try {
            DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                soTimeout = 1_000
                bind(InetSocketAddress(BEACON_PORT))
            }
        } catch (_: IOException) {
            lock?.release()
            return null
        }
        try {
            val buffer = ByteArray(256)
            while (currentCoroutineContext().isActive) {
                val packet = DatagramPacket(buffer, buffer.size)
                try {
                    socket.receive(packet)
                } catch (_: SocketTimeoutException) {
                    kotlinx.coroutines.yield()
                    continue
                }
                val beacon = Beacon.parse(String(packet.data, 0, packet.length, Charsets.UTF_8)) ?: continue
                if (beacon.pcId == pcId) {
                    return packet.address.hostAddress.orEmpty() to beacon.port
                }
            }
            return null
        } catch (_: IOException) {
            return null
        } finally {
            socket.close()
            lock?.release()
        }
    }

    private suspend fun runSession(active: LinkSession) = coroutineScope {
        val bulkJobs = SupervisorJob()
        val incoming = IncomingTransfers(context)
        incomingTransfers = incoming
        session = active
        store.updatePcName(active.pcName)
        _state.update {
            it.copy(status = LinkStatus.Connected, pcName = active.pcName.ifEmpty { it.pcName }, host = active.host, detail = "")
        }
        sendStatus()
        NotifyListener.sendSnapshot()
        MediaWatcher.sendSnapshot()
        CallWatcher.sendSnapshot()
        send(message("clipboard.history.request"))
        val pinger = launch {
            while (isActive) {
                delay(PING_INTERVAL_MS)
                if (!LocalNetworkAccess.allowed(context)) { active.close(); break }
                if (!send(message("ping"))) break
                sendStatus()
            }
        }
        try {
            active.setReadTimeout(READ_TIMEOUT_MS)
            while (currentCoroutineContext().isActive) {
                val payload = try {
                    active.receive()
                } catch (_: SocketTimeoutException) {
                    break
                } catch (_: IOException) {
                    break
                }
                handle(payload, active, bulkJobs, incoming)
            }
        } finally {
            pinger.cancel()
            bulkJobs.cancel()
            active.close()
            incoming.close()
            if (incomingTransfers === incoming) incomingTransfers = null
            transferEpoch.incrementAndGet()
            transferAcks.values.forEach { it.complete("The phone connection was lost.") }
            _state.update { it.copy(transferStatus = "", transferActive = false) }
            Ringer.stop(context)
            ScreenBridge.disconnected(context, active)
        }
    }

    private fun sendStatus() {
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = (battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        send(PhoneMessages.status(if (level < 0 || scale <= 0) -1 else level * 100 / scale, plugged, deviceName, features()))
    }

    // ---- Incoming -------------------------------------------------------------

    private suspend fun handle(payload: Payload, active: LinkSession, bulkJobs: Job, incoming: IncomingTransfers) {
        // Runs [work] on the serialized bulk worker while [active] is still the current session.
        fun inBackground(work: () -> Unit) {
            scope.launch(bulk + bulkJobs) { if (session === active) work() }
        }
        val transferJson = runCatching {
            kotlinx.serialization.json.Json.parseToJsonElement(payload.json) as? kotlinx.serialization.json.JsonObject
        }.getOrNull() ?: return
        fun text(key: String) = (transferJson[key] as? JsonPrimitive)?.content.orEmpty()
        val type = text("type")
        val transferId = text("id")
        if (type == "file.received") {
            transferAcks[transferId]?.complete(if (text("ok") == "true") null else text("error").ifEmpty { "The PC could not save the file." })
            return
        }
        if (type == "file.cancel") {
            if (transferId.isEmpty()) transferEpoch.incrementAndGet()
            else if (transferAcks.containsKey(transferId)) cancelledTransfers.add(transferId)
            incoming.cancel(transferId)
            if (transferId.isEmpty()) transferAcks.values.forEach { it.complete("File transfer cancelled.") }
            else transferAcks[transferId]?.complete("File transfer cancelled.")
            _state.update { it.copy(transferStatus = "File transfer cancelled.", transferActive = false) }
            return
        }
        if (type == "file.begin" || type == "file.chunk" || type == "file.end") {
            try {
                // Backpressure: at most one received chunk is awaiting a disk write.
                withContext(bulk + bulkJobs) {
                    when (type) {
                        "file.begin" -> {
                            val start = FileStart.parse(transferJson) ?: throw IOException("Invalid file transfer.")
                            incoming.begin(start)
                            _state.update { it.copy(transferStatus = "Receiving ${start.name}…", transferActive = true) }
                        }
                        "file.chunk" -> {
                            if (!store.receiveFiles) throw IOException("Receiving files is turned off.")
                            val offset = (transferJson["offset"] as? JsonPrimitive)?.longOrNull ?: -1
                            val (start, received) = incoming.chunk(transferId, offset, payload.binary)
                            val percent = if (start.size == 0L) 100 else received * 100 / start.size
                            _state.update { it.copy(transferStatus = "Receiving ${start.name}: $percent%", transferActive = true) }
                        }
                        else -> {
                            val start = incoming.finish(transferId)
                            sendToSession(active, PhoneMessages.fileReceived(transferId, start.name, true, ""))
                            _state.update { it.copy(transferStatus = "Saved ${start.name} to Downloads/FeatherCast.", transferActive = false) }
                        }
                    }
                }
            } catch (error: Exception) {
                incoming.cancel(transferId)
                if (error is kotlinx.coroutines.CancellationException) throw error
                sendToSession(active, PhoneMessages.fileReceived(transferId, "", false, error.message.orEmpty()))
                sendToSession(active, message("file.cancel") { put("id", transferId) })
                _state.update { it.copy(transferStatus = error.message ?: "File transfer failed.", transferActive = false) }
            }
            return
        }
        when (val msg = PcMessage.parse(payload.json) ?: return) {
            is PcMessage.Clipboard -> onPcClipboard(msg.text, msg.time)
            is PcMessage.ClipboardHistory -> onPcHistory(msg.items)
            is PcMessage.PhotosRequest -> if (store.sendPhotos) inBackground { sendPhotoList(active, msg.limit) }
            is PcMessage.PhotoRequest -> if (store.sendPhotos) inBackground { sendFullPhoto(active, msg.id) }
            is PcMessage.NotificationDismiss -> NotifyListener.dismiss(msg.key)
            is PcMessage.NotificationAction -> NotifyListener.performAction(msg.key, msg.index, msg.text)
            is PcMessage.FileSend -> inBackground { onFileFromPc(active, msg.id, msg.name, payload.binary) }
            is PcMessage.Ring -> when {
                !msg.start -> Ringer.stop(context)
                store.allowRing -> Ringer.start(context)
            }
            is PcMessage.MediaCommand -> if (store.mediaControl) MediaWatcher.command(msg.command, msg.position)
            is PcMessage.MediaVolume -> if (store.mediaControl) MediaWatcher.setVolume(msg.volume)
            PcMessage.SmsThreadsRequest -> inBackground {
                sendToSession(active, PhoneMessages.smsThreads(if (SmsBridge.active) SmsBridge.threads(context) else emptyList()))
            }
            is PcMessage.SmsMessagesRequest -> if (SmsBridge.active) inBackground {
                sendToSession(active, PhoneMessages.smsMessages(msg.thread, SmsBridge.messages(context, msg.thread, msg.limit)))
            }
            is PcMessage.SmsSend -> SmsBridge.send(context, msg.ref, msg.address, msg.body)
            PcMessage.CallReject -> if (CallWatcher.active) CallWatcher.reject(context)
            PcMessage.CallSilence -> if (CallWatcher.active) CallWatcher.silence(context)
            is PcMessage.FilesListRequest -> inBackground { sendToSession(active, StorageBridge.list(msg.path)) }
            is PcMessage.FileRequest -> inBackground {
                if (active.fileStreamSupported) {
                    scope.launch(bulk + bulkJobs) {
                        try {
                            val file = StorageBridge.streamFile(msg.path) ?: throw IOException("Allow phone storage access or choose an available file.")
                            file.inputStream().use { sendStream(active, file.name, file.length(), "storage", msg.path, it) }
                        } catch (error: Exception) {
                            if (error is kotlinx.coroutines.CancellationException) throw error
                            sendToSession(active, PhoneMessages.fileData(msg.path, "", error.message ?: "Could not read the file."))
                        }
                    }
                } else StorageBridge.read(msg.path).let { (json, bytes) -> sendToSession(active, json, bytes) }
            }
            PcMessage.Pong -> Unit
            is PcMessage.ScreenStart -> ScreenBridge.request(context, msg.request, active)
            is PcMessage.ScreenStop -> ScreenBridge.stop(context, id = msg.sessionId)
        }
    }

    private suspend fun sendStream(active: LinkSession, name: String, size: Long,
                                   purpose: String, reference: String, input: java.io.InputStream) {
        if (size !in 0..MAX_STREAM_BYTES) throw IOException("Files must be 8 GB or smaller.")
        val id = java.util.UUID.randomUUID().toString()
        val epoch = transferEpoch.get()
        val acknowledgment = CompletableDeferred<String?>()
        synchronized(transferAcks) {
            if (transferAcks.size >= app.feathercast.protocol.MAX_STREAM_TRANSFERS) throw IOException("Up to four files can be sent at once.")
            transferAcks[id] = acknowledgment
        }
        fun sendChecked(json: String, bytes: ByteArray = ByteArray(0)) {
            if (session !== active || transferEpoch.get() != epoch || id in cancelledTransfers || !sendToSession(active, json, bytes)) {
                throw IOException("File transfer cancelled or the connection was lost.")
            }
        }
        try {
            sendChecked(FileStart(id, name, size, purpose, reference).message())
            val buffer = ByteArray(FILE_CHUNK_BYTES)
            var offset = 0L
            var lastPercent = -1L
            while (offset < size) {
                currentCoroutineContext().ensureActive()
                val read = input.read(buffer, 0, minOf(buffer.size.toLong(), size - offset).toInt())
                if (read < 0) throw IOException("The file changed or could not be read completely.")
                if (read == 0) continue
                sendChecked(message("file.chunk") { put("id", id); put("offset", offset) }, buffer.copyOf(read))
                offset += read
                val percent = if (size == 0L) 100 else offset * 100 / size
                if (percent != lastPercent) {
                    lastPercent = percent
                    _state.update { it.copy(transferStatus = "Sending $name: $percent%", transferActive = true) }
                }
            }
            sendChecked(message("file.end") { put("id", id) })
            val result = withTimeoutOrNull(30_000) { acknowledgment.await() to true }
                ?: throw IOException("The PC did not confirm the saved file.")
            result.first?.let { throw IOException(it) }
            _state.update { it.copy(transferStatus = "Saved $name on your PC.", transferActive = false) }
        } catch (error: Exception) {
            sendToSession(active, message("file.cancel") { put("id", id) })
            _state.update { it.copy(transferStatus = error.message ?: "File transfer failed.", transferActive = false) }
            throw error
        } finally { transferAcks.remove(id); cancelledTransfers.remove(id) }
    }

    fun cancelTransfers() {
        transferEpoch.incrementAndGet()
        // Disk cleanup stays off the UI thread; publication checks the cancellation flag.
        val receiver = incomingTransfers
        scope.launch { receiver?.cancel() }
        sendAsync(message("file.cancel") { put("id", "") })
        transferAcks.values.forEach { it.complete("File transfer cancelled.") }
        _state.update { it.copy(transferStatus = "File transfers cancelled.", transferActive = false) }
    }

    private fun onFileFromPc(active: LinkSession, id: String, name: String, bytes: ByteArray) {
        val error = when {
            !store.receiveFiles -> "Receiving files is turned off on the phone."
            IncomingFiles.needsPermission(context) -> "Allow FeatherCast to save files on the phone."
            else -> IncomingFiles.save(context, name, bytes)
        }
        sendToSession(active, PhoneMessages.fileReceived(id, name, error == null, error.orEmpty()))
    }

    /** The features this phone can serve right now, so the PC hides the rest. */
    private fun features(): List<String> = buildList {
        add("file.stream.v1")
        if (NotifyListener.hasAccess(context)) add(Features.NOTIFICATION_ACTIONS)
        if (store.receiveFiles && !IncomingFiles.needsPermission(context)) add(Features.RECEIVE_FILES)
        if (store.allowRing) add(Features.RING)
        if (store.mediaControl && NotifyListener.hasAccess(context)) add(Features.MEDIA)
        if (SmsBridge.active) add(Features.SMS)
        if (CallWatcher.active) add(Features.CALLS)
        if (StorageBridge.active) add(Features.STORAGE)
        if (store.screenSharing) add(Features.SCREEN)
        if (store.remoteControl && RemoteControlService.instance != null) add(Features.SCREEN_CONTROL)
        if (store.remoteControl && PcKeyboardService.selected(context)) add(Features.SCREEN_KEYBOARD)
        if (Build.VERSION.SDK_INT >= 29) add(Features.SCREEN_AUDIO)
    }

    /** Resends status (e.g. after a switch changed) so the PC updates its actions. */
    fun refreshStatus() {
        scope.launch { if (session != null) sendStatus() }
        MediaWatcher.start(context)
    }

    private fun onPcClipboard(text: String, time: Long) {
        if (text.isEmpty()) return
        _state.update { current ->
            val rest = current.pcClipboard.filterNot { it.text == text }
            current.copy(pcClipboard = (listOf(PcClip(text, time)) + rest).take(MAX_CLIPS))
        }
        if (!store.clipboardSync) return
        main.post { copyToPhone(text, announce = false) }
    }

    private fun onPcHistory(history: List<Pair<String, Long>>) {
        val items = history.map { (text, time) -> PcClip(text, time) }
        _state.update { it.copy(pcClipboard = items.take(MAX_CLIPS)) }
    }

    /** Copies text to the phone clipboard (used for PC clipboard entries). */
    fun copyToPhone(text: String, announce: Boolean = true) {
        val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
        clipboard.setPrimaryClip(ClipData.newPlainText(if (announce) "FeatherCast" else "From PC", text))
    }

    private fun sendPhotoList(active: LinkSession, limit: Int) {
        val access = PhotoSource.access(context)
        val photos = if (access != PhotoAccess.Denied) PhotoSource.latest(context, limit.coerceIn(1, 120)) else emptyList()
        sendToSession(active, message("photos.list") {
            put("access", access.name.lowercase())
            if (access == PhotoAccess.Denied) put("error", "Allow photo access in FeatherCast on your phone.")
            putJsonArray("items") {
                for (photo in photos) {
                    addJsonObject {
                        put("id", photo.id.toString())
                        put("name", photo.name)
                        put("time", photo.time)
                        put("w", photo.width)
                        put("h", photo.height)
                    }
                }
            }
        })
        for (photo in photos) {
            if (session !== active) return
            val thumb = PhotoSource.thumbnail(context, photo) ?: continue
            sendToSession(active, message("photo.thumb") { put("id", photo.id.toString()) }, thumb)
        }
    }

    private fun sendFullPhoto(active: LinkSession, id: String) {
        val photo = id.toLongOrNull()?.let { PhotoSource.find(context, it) } ?: return
        val bytes = PhotoSource.fullBytes(context, photo, MAX_SEND_BYTES) ?: return
        sendToSession(active, message("photo.full") {
            put("id", id)
            put("name", photo.name)
        }, bytes)
    }

    companion object {
        const val PING_INTERVAL_MS = 20_000L
        const val READ_TIMEOUT_MS = 60_000
        const val MAX_CLIPS = 30
        val MAX_SEND_BYTES = (MAX_FRAME_BYTES - 1024 * 1024).toLong().coerceAtMost(40L * 1024 * 1024)

        val instance: LinkManager get() = PhoneApp.instance.link
    }
}
