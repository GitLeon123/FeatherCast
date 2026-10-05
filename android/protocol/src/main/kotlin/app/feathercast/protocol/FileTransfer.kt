package app.feathercast.protocol

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

const val FILE_CHUNK_BYTES = 256 * 1024
const val MAX_STREAM_BYTES = 8L * 1024 * 1024 * 1024
const val MAX_STREAM_TRANSFERS = 4

fun validTransferId(id: String): Boolean = id.isNotEmpty() && id.length <= 64 &&
    id.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '-' || it == '_' }

data class FileStart(val id: String, val name: String, val size: Long, val purpose: String, val reference: String = "") {
    companion object {
        fun parse(json: JsonObject): FileStart? {
            fun text(key: String) = (json[key] as? kotlinx.serialization.json.JsonPrimitive)?.contentOrNull.orEmpty()
            val size = (json["size"] as? kotlinx.serialization.json.JsonPrimitive)?.longOrNull ?: return null
            val start = FileStart(text("id"), text("name"), size, text("purpose"), text("ref"))
            return start.takeIf { validTransferId(it.id) && it.name.isNotBlank() &&
                it.size in 0..MAX_STREAM_BYTES && it.purpose in setOf("file", "storage", "photo") }
        }
    }

    fun message(): String = message("file.begin") {
        put("id", id); put("name", name); put("size", size)
        put("purpose", purpose); put("ref", reference)
    }
}

/** Validates sequencing before an incoming chunk reaches disk. */
class FileSequence(val total: Long) {
    var received: Long = 0
        private set
    init { require(total in 0..MAX_STREAM_BYTES) }
    fun accept(offset: Long, count: Int): Boolean {
        if (offset != received || count !in 1..FILE_CHUNK_BYTES || count.toLong() > total - received) return false
        received += count
        return true
    }
    val complete: Boolean get() = received == total
}
