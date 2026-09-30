package app.feathercast.protocol

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import java.awt.Color
import java.awt.image.BufferedImage
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.SocketTimeoutException
import javax.imageio.ImageIO
import kotlin.system.exitProcess

// Desktop stand-in for the Android app, used to test FeatherCast end to end:
//   phone-sim pair "<feathercast://pair?...>" [state-file]
//   phone-sim run [state-file] [seconds]

private fun image(format: String, width: Int, height: Int, color: Color): ByteArray {
    val image = BufferedImage(width, height, BufferedImage.TYPE_INT_RGB)
    val graphics = image.createGraphics()
    graphics.color = color
    graphics.fillRect(0, 0, width, height)
    graphics.color = Color.WHITE
    graphics.fillOval(width / 4, height / 4, width / 2, height / 2)
    graphics.dispose()
    return ByteArrayOutputStream().also { ImageIO.write(image, format, it) }.toByteArray()
}

private fun saveState(file: File, values: Map<String, String>) {
    file.writeText(values.entries.joinToString("\n") { "${it.key}=${it.value}" })
}

private fun loadState(file: File): Map<String, String> =
    file.readLines().mapNotNull {
        val eq = it.indexOf('=')
        if (eq <= 0) null else it.substring(0, eq) to it.substring(eq + 1)
    }.toMap()

fun main(args: Array<String>) {
    if (args.isEmpty()) {
        println("usage: phone-sim pair <uri> [state-file] | run [state-file] [seconds]")
        exitProcess(2)
    }
    when (args[0]) {
        "pair" -> {
            val invite = PairingInvite.parse(args.getOrNull(1) ?: "") ?: run {
                println("Invalid pairing URI")
                exitProcess(1)
            }
            val state = File(args.getOrNull(2) ?: "phone-sim.state")
            val deviceId = Hex.encode(Crypto.randomBytes(8))
            val result = Pairing.pair(invite, deviceId, "Phone Simulator")
            saveState(state, mapOf(
                "deviceId" to deviceId,
                "linkKey" to Base64Url.encode(result.linkKey),
                "host" to result.host,
                "port" to invite.port.toString(),
                "pcName" to result.pcName,
            ))
            println("PAIRED with ${result.pcName} at ${result.host}:${invite.port}")
        }
        "run" -> {
            val state = loadState(File(args.getOrNull(1) ?: "phone-sim.state"))
            val seconds = args.getOrNull(2)?.toIntOrNull() ?: 20
            val session = LinkSession.connect(
                state.getValue("host"), state.getValue("port").toInt(),
                state.getValue("deviceId"), Base64Url.decode(state.getValue("linkKey"))!!,
            )
            println("CONNECTED to ${session.pcName}")
            val allFeatures = listOf(
                Features.NOTIFICATION_ACTIONS, Features.RECEIVE_FILES, Features.RING, Features.MEDIA,
                Features.SMS, Features.CALLS, Features.STORAGE,
            )
            session.send(PhoneMessages.status(76, true, "Phone Simulator", allFeatures))
            val now = System.currentTimeMillis()
            session.send(
                PhoneMessages.notificationPosted(
                    "sim|1", "com.whatsapp", "WhatsApp", "Anna", "Are we still meeting at 7?", now,
                    listOf(ActionInfo(0, "Reply", true), ActionInfo(1, "Mark as read", false)),
                ),
                image("png", 48, 48, Color(37, 211, 102)),
            )
            session.send(message("notification.posted") {
                put("key", "sim|2")
                put("app", "com.google.android.gm")
                put("appName", "Gmail")
                put("title", "Your order has shipped")
                put("text", "Package arrives Tuesday.")
                put("time", now - 60_000)
            }, image("png", 48, 48, Color(234, 67, 53)))
            session.send(message("clipboard.set") { put("text", "Copied on the phone simulator") })
            session.send(message("clipboard.history.request"))
            val photos = listOf("sim-photo-1" to Color(90, 120, 220), "sim-photo-2" to Color(220, 140, 60))
            fun sendPhotoList() {
                session.send(message("photos.list") {
                    putJsonArray("items") {
                        photos.forEachIndexed { index, (id, _) ->
                            addJsonObject {
                                put("id", id)
                                put("name", "$id.jpg")
                                put("time", now - index * 3_600_000L)
                                put("w", 1200)
                                put("h", 900)
                            }
                        }
                    }
                })
                for ((id, color) in photos) {
                    session.send(message("photo.thumb") { put("id", id) }, image("jpg", 240, 180, color))
                }
            }
            sendPhotoList()

            var playing = true
            var volume = 9
            var position = 42_000L
            fun sendMedia(art: Boolean) {
                val media = MediaInfo(
                    app = "com.spotify.music", appName = "Spotify", title = "Simulated Song", artist = "The Simulators",
                    playing = playing, position = position, duration = 215_000, positionAt = System.currentTimeMillis(),
                    volume = volume, volumeMax = 15,
                )
                session.send(PhoneMessages.mediaState(media), if (art) image("jpg", 256, 256, Color(30, 185, 84)) else ByteArray(0))
            }
            sendMedia(true)

            val threads = mutableListOf(
                SmsThreadInfo("11", "+15550100", "Anna", "See you at 7!", now - 120_000, true),
                SmsThreadInfo("12", "+15550199", "", "Your code is 481516", now - 7_200_000, false),
            )
            val messages = mutableMapOf(
                "11" to mutableListOf(
                    SmsMessageInfo("1", "Dinner tonight?", now - 600_000, true),
                    SmsMessageInfo("2", "See you at 7!", now - 120_000, false),
                ),
                "12" to mutableListOf(SmsMessageInfo("3", "Your code is 481516", now - 7_200_000, false)),
            )
            val folders = mapOf(
                "/" to listOf(
                    FileEntry("DCIM", true, 0, now), FileEntry("Download", true, 0, now), FileEntry("notes.txt", false, 11, now),
                ),
                "/Download" to listOf(FileEntry("report.pdf", false, 23, now - 86_400_000)),
                "/DCIM" to emptyList(),
            )
            val callAt = System.currentTimeMillis() + 30_000
            var callState = CallState.Idle

            session.setReadTimeout(1000)
            val deadline = System.currentTimeMillis() + seconds * 1000L
            while (System.currentTimeMillis() < deadline) {
                if (callState == CallState.Idle && System.currentTimeMillis() >= callAt) {
                    callState = CallState.Ringing
                    session.send(PhoneMessages.callState(callState, "+15550100", "Anna"))
                }
                val payload = try {
                    session.receive()
                } catch (_: SocketTimeoutException) {
                    continue
                }
                val root: JsonObject = parseJson(payload.json) ?: continue
                println("RECV ${root.str("type")} ${payload.json.take(200)}")
                when (val msg = PcMessage.parse(payload.json)) {
                    is PcMessage.FileSend ->
                        session.send(PhoneMessages.fileReceived(msg.id, msg.name, true))
                    is PcMessage.Ring -> session.send(PhoneMessages.ringState(msg.start))
                    is PcMessage.NotificationAction ->
                        if (msg.text.isNotEmpty()) println("REPLY ${msg.key}: ${msg.text}")
                    is PcMessage.MediaCommand -> {
                        when (msg.command) {
                            "toggle" -> playing = !playing
                            "play" -> playing = true
                            "pause" -> playing = false
                            "next", "prev" -> position = 0
                            "seek" -> position = msg.position.coerceAtLeast(0)
                        }
                        sendMedia(false)
                    }
                    is PcMessage.MediaVolume -> {
                        volume = msg.volume.coerceIn(0, 15)
                        sendMedia(false)
                    }
                    PcMessage.SmsThreadsRequest -> session.send(PhoneMessages.smsThreads(threads))
                    is PcMessage.SmsMessagesRequest ->
                        session.send(PhoneMessages.smsMessages(msg.thread, messages[msg.thread].orEmpty()))
                    is PcMessage.SmsSend -> {
                        val thread = threads.firstOrNull { it.address == msg.address }?.thread ?: "11"
                        messages.getOrPut(thread) { mutableListOf() }
                            .add(SmsMessageInfo("s${System.nanoTime()}", msg.body, System.currentTimeMillis(), true))
                        session.send(PhoneMessages.smsSent(msg.ref, true))
                    }
                    PcMessage.CallReject, PcMessage.CallSilence -> {
                        callState = CallState.Idle
                        session.send(PhoneMessages.callState(callState, "+15550100", "Anna"))
                    }
                    is PcMessage.FilesListRequest -> {
                        val path = normalizeStoragePath(msg.path) ?: "/"
                        val items = folders[path]
                        session.send(PhoneMessages.filesList(path, items.orEmpty(), if (items == null) "This folder is not available." else ""))
                    }
                    is PcMessage.FileRequest -> {
                        val name = msg.path.substringAfterLast('/')
                        session.send(PhoneMessages.fileData(msg.path, name), "Simulated file: $name\n".toByteArray())
                    }
                    else -> Unit
                }
                when (root.str("type")) {
                    "photos.request" -> sendPhotoList()
                    "photo.request" -> {
                        val id = root.str("id")
                        val color = photos.firstOrNull { it.first == id }?.second ?: Color.GRAY
                        session.send(message("photo.full") {
                            put("id", id)
                            put("name", "$id.jpg")
                        }, image("jpg", 1200, 900, color))
                    }
                }
            }
            session.send(message("ping"))
            session.close()
            println("DONE")
        }
        else -> exitProcess(2)
    }
}
