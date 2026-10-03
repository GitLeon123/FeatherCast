package app.feathercast.protocol

import kotlinx.serialization.json.put

const val MAX_SCREEN_PACKET_BYTES = 2 * 1024 * 1024
const val SCREEN_REQUEST_LIFETIME_MS = 60_000L
const val SCREEN_COORDINATE_SCALE = 1_000_000

data class ScreenRequest(val sessionId: String, val key: ByteArray, val audio: Boolean)

data class ScreenInput(
    val sessionId: String,
    val generation: Int,
    val action: String,
    val x: Int = 0,
    val y: Int = 0,
    val value: Int = 0,
    val text: String = "",
) {
    fun valid(): Boolean {
        if (sessionId.isEmpty() || sessionId.length > 64 || generation <= 0 || text.utf8().size > 16 * 1024) return false
        return when (action) {
            "down", "move", "up", "scroll" -> x in 0..SCREEN_COORDINATE_SCALE && y in 0..SCREEN_COORDINATE_SCALE &&
                (action != "scroll" || value in -10..10)
            "key" -> value in listOf(0, 19, 20, 21, 22, 61, 66, 67, 112, 122, 123)
            "text", "back", "home", "recents", "cancel", "keyframe" -> true
            else -> false
        }
    }

    companion object {
        fun parse(json: String): ScreenInput? {
            if (json.length > 32 * 1024) return null
            val root = parseJson(json) ?: return null
            if (root.str("type") != "screen.input") return null
            val numeric = listOf("generation", "x", "y", "value").map { root.long(it) }
            if (numeric.any { it !in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong() }) return null
            return ScreenInput(root.str("session"), numeric[0].toInt(), root.str("action"),
                numeric[1].toInt(), numeric[2].toInt(), numeric[3].toInt(), root.str("text")).takeIf { it.valid() }
        }
    }
}

object ScreenMessages {
    fun state(session: String, state: String, generation: Int = 0, detail: String = "",
              control: Boolean = false, keyboard: Boolean = false, audio: Boolean = false) = message("screen.state") {
        put("session", session); put("state", state); put("generation", generation); put("detail", detail)
        put("control", control); put("keyboard", keyboard); put("audio", audio)
    }
    fun videoConfig(session: String, generation: Int, width: Int, height: Int) = message("screen.video.config") {
        put("session", session); put("generation", generation); put("width", width); put("height", height)
    }
    fun video(session: String, generation: Int, pts: Long, keyframe: Boolean) = message("screen.video") {
        put("session", session); put("generation", generation); put("pts", pts); put("keyframe", keyframe)
    }
    fun audioConfig(session: String, generation: Int) = message("screen.audio.config") {
        put("session", session); put("generation", generation)
    }
    fun audio(session: String, generation: Int, pts: Long) = message("screen.audio") {
        put("session", session); put("generation", generation); put("pts", pts)
    }
}
