package app.feathercast.protocol

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray

// Session message catalog, mirrored by native/src/phone_messages.hpp.
// Phone → PC messages are built here; PC → phone messages are parsed into
// [PcMessage]. Both sides check the same fixtures in their tests.

/** Largest file either side sends in one message. */
const val MAX_TRANSFER_BYTES = 40 * 1024 * 1024

/** Values of the `features` list in `status`, so the PC only offers what works. */
object Features {
    const val NOTIFICATION_ACTIONS = "notification.actions"
    const val RECEIVE_FILES = "files.receive"
    const val RING = "ring"
    const val MEDIA = "media"
    const val SMS = "sms"
    const val CALLS = "calls"
    const val STORAGE = "storage"
    const val SCREEN = "screen"
    const val SCREEN_CONTROL = "screen.control"
    const val SCREEN_KEYBOARD = "screen.keyboard"
    const val SCREEN_AUDIO = "screen.audio"
}

fun JsonObject.bool(key: String, fallback: Boolean = false): Boolean =
    (this[key] as? JsonPrimitive)?.booleanOrNull ?: fallback

private fun JsonObject.objects(key: String): List<JsonObject> =
    (this[key] as? JsonArray)?.mapNotNull { it as? JsonObject } ?: emptyList()

data class ActionInfo(val index: Int, val title: String, val reply: Boolean)

data class MediaInfo(
    val app: String,
    val appName: String,
    val title: String,
    val artist: String,
    val playing: Boolean,
    val position: Long,
    val duration: Long,
    val positionAt: Long,
    val volume: Int,
    val volumeMax: Int,
)

data class SmsThreadInfo(
    val thread: String,
    val address: String,
    val name: String,
    val snippet: String,
    val time: Long,
    val unread: Boolean,
)

data class SmsMessageInfo(val id: String, val body: String, val time: Long, val outgoing: Boolean)

data class FileEntry(val name: String, val directory: Boolean, val size: Long, val time: Long)

enum class CallState(val wire: String) { Idle("idle"), Ringing("ringing"), Active("active") }

/** Builders for everything the phone sends. */
object PhoneMessages {
    fun status(battery: Int, charging: Boolean, name: String, features: List<String>) = message("status") {
        put("battery", battery)
        put("charging", charging)
        put("name", name)
        putJsonArray("features") { features.forEach { add(JsonPrimitive(it)) } }
    }

    fun notificationPosted(
        key: String,
        app: String,
        appName: String,
        title: String,
        text: String,
        time: Long,
        actions: List<ActionInfo> = emptyList(),
    ) = message("notification.posted") {
        put("key", key)
        put("app", app)
        put("appName", appName)
        put("title", title)
        put("text", text)
        put("time", time)
        if (actions.isNotEmpty()) {
            putJsonArray("actions") {
                for (action in actions) {
                    addJsonObject {
                        put("i", action.index)
                        put("title", action.title)
                        put("reply", action.reply)
                    }
                }
            }
        }
    }

    fun fileReceived(id: String, name: String, ok: Boolean, error: String = "") = message("file.received") {
        put("id", id)
        put("name", name)
        put("ok", ok)
        if (error.isNotEmpty()) put("error", error)
    }

    fun ringState(ringing: Boolean) = message("ring.state") { put("ringing", ringing) }

    fun mediaState(media: MediaInfo) = message("media.state") {
        put("app", media.app)
        put("appName", media.appName)
        put("title", media.title)
        put("artist", media.artist)
        put("playing", media.playing)
        put("pos", media.position)
        put("dur", media.duration)
        put("posAt", media.positionAt)
        put("vol", media.volume)
        put("volMax", media.volumeMax)
    }

    fun mediaNone() = message("media.none")

    fun smsThreads(threads: List<SmsThreadInfo>) = message("sms.threads") {
        putJsonArray("items") {
            for (thread in threads) {
                addJsonObject {
                    put("thread", thread.thread)
                    put("address", thread.address)
                    put("name", thread.name)
                    put("snippet", thread.snippet)
                    put("time", thread.time)
                    put("unread", thread.unread)
                }
            }
        }
    }

    fun smsMessages(thread: String, messages: List<SmsMessageInfo>) = message("sms.messages") {
        put("thread", thread)
        putJsonArray("items") {
            for (item in messages) {
                addJsonObject {
                    put("id", item.id)
                    put("body", item.body)
                    put("time", item.time)
                    put("out", item.outgoing)
                }
            }
        }
    }

    fun smsReceived(thread: String, address: String, name: String, body: String, time: Long) =
        message("sms.received") {
            put("thread", thread)
            put("address", address)
            put("name", name)
            put("body", body)
            put("time", time)
        }

    fun smsSent(ref: String, ok: Boolean, error: String = "") = message("sms.sent") {
        put("ref", ref)
        put("ok", ok)
        if (error.isNotEmpty()) put("error", error)
    }

    fun callState(state: CallState, number: String, name: String) = message("call.state") {
        put("state", state.wire)
        put("number", number)
        put("name", name)
    }

    fun filesList(path: String, items: List<FileEntry>, error: String = "") = message("files.list") {
        put("path", path)
        if (error.isNotEmpty()) put("error", error)
        putJsonArray("items") {
            for (item in items) {
                addJsonObject {
                    put("name", item.name)
                    put("dir", item.directory)
                    put("size", item.size)
                    put("time", item.time)
                }
            }
        }
    }

    fun fileData(path: String, name: String, error: String = "") = message("file.data") {
        put("path", path)
        put("name", name)
        if (error.isNotEmpty()) put("error", error)
    }
}

/** Everything the PC sends after the handshake. */
sealed class PcMessage {
    data class Clipboard(val text: String, val time: Long) : PcMessage()
    data class ClipboardHistory(val items: List<Pair<String, Long>>) : PcMessage()
    data class PhotosRequest(val limit: Int) : PcMessage()
    data class PhotoRequest(val id: String) : PcMessage()
    data class NotificationDismiss(val key: String) : PcMessage()
    data class NotificationAction(val key: String, val index: Int, val text: String) : PcMessage()
    data class FileSend(val id: String, val name: String) : PcMessage()
    data class Ring(val start: Boolean) : PcMessage()
    data class MediaCommand(val command: String, val position: Long) : PcMessage()
    data class MediaVolume(val volume: Int) : PcMessage()
    data object SmsThreadsRequest : PcMessage()
    data class SmsMessagesRequest(val thread: String, val limit: Int) : PcMessage()
    data class SmsSend(val ref: String, val address: String, val body: String) : PcMessage()
    data object CallReject : PcMessage()
    data object CallSilence : PcMessage()
    data class FilesListRequest(val path: String) : PcMessage()
    data class FileRequest(val path: String) : PcMessage()
    data object Pong : PcMessage()
    data class ScreenStart(val request: ScreenRequest) : PcMessage()
    data class ScreenStop(val sessionId: String) : PcMessage()

    companion object {
        /** Returns null for malformed JSON and unknown types (forward compatibility). */
        fun parse(json: String, now: Long = System.currentTimeMillis()): PcMessage? {
            val root = parseJson(json) ?: return null
            return when (root.str("type")) {
                "pong" -> Pong
                "screen.start" -> {
                    val id = root.str("session")
                    val key = Base64Url.decode(root.str("key"))
                    if (id.isEmpty() || id.length > 64 || key?.size != 32) null
                    else ScreenStart(ScreenRequest(id, key, root.bool("audio")))
                }
                "screen.stop" -> ScreenStop(root.str("session"))
                "clipboard.set" -> Clipboard(root.str("text"), root.long("time", now))
                "clipboard.history" -> ClipboardHistory(
                    root.objects("items").map { it.str("text") to it.long("time") }.filter { it.first.isNotEmpty() },
                )
                "photos.request" -> PhotosRequest(root.long("limit", 60).toInt())
                "photo.request" -> PhotoRequest(root.str("id"))
                "notification.dismiss" -> NotificationDismiss(root.str("key"))
                "notification.action" -> NotificationAction(root.str("key"), root.long("i", -1).toInt(), root.str("text"))
                "file.send" -> FileSend(root.str("id"), root.str("name"))
                "ring.start" -> Ring(true)
                "ring.stop" -> Ring(false)
                "media.command" -> MediaCommand(root.str("cmd"), root.long("pos", -1))
                "media.volume" -> MediaVolume(root.long("vol", -1).toInt())
                "sms.threads.request" -> SmsThreadsRequest
                "sms.messages.request" -> SmsMessagesRequest(root.str("thread"), root.long("limit", 50).toInt())
                "sms.send" -> SmsSend(root.str("ref"), root.str("address"), root.str("body"))
                "call.reject" -> CallReject
                "call.silence" -> CallSilence
                "files.list.request" -> FilesListRequest(root.str("path").ifEmpty { "/" })
                "file.request" -> FileRequest(root.str("path"))
                else -> null
            }
        }
    }
}

/**
 * Normalizes a PC-supplied storage path ("/" is the shared-storage root).
 * Returns null when it tries to leave the root.
 */
fun normalizeStoragePath(path: String): String? {
    val parts = ArrayList<String>()
    for (part in path.replace('\\', '/').split('/')) {
        when (part) {
            "", "." -> Unit
            ".." -> return null
            else -> parts.add(part)
        }
    }
    return "/" + parts.joinToString("/")
}
