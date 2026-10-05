package app.feathercast.protocol

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.Base64

// Wire format shared with native/src/phone_protocol.hpp. Keep both in sync.

const val PROTOCOL_VERSION = 1
const val DEFAULT_PORT = 47800
const val BEACON_PORT = 47801
const val MAX_FRAME_BYTES = 48 * 1024 * 1024
const val MAX_HANDSHAKE_BYTES = 64 * 1024
const val BEACON_PREFIX = "FCAST1"

object Labels {
    const val LINK_KEY_INFO = "feathercast-link-v1"
    const val PAIR_PROOF = "pair"
    const val PAIRED_PROOF = "paired"
    const val PHONE_AUTH = "auth-phone"
    const val PC_AUTH = "auth-pc"
    const val PHONE_TO_PC = "p2c"
    const val PC_TO_PHONE = "c2p"
}

fun String.utf8(): ByteArray = toByteArray(Charsets.UTF_8)

fun concat(vararg parts: ByteArray): ByteArray {
    val out = ByteArray(parts.sumOf { it.size })
    var offset = 0
    for (part in parts) {
        part.copyInto(out, offset)
        offset += part.size
    }
    return out
}

object Base64Url {
    private val encoder = Base64.getUrlEncoder().withoutPadding()
    private val decoder = Base64.getUrlDecoder()

    fun encode(data: ByteArray): String = encoder.encodeToString(data)

    fun decode(text: String): ByteArray? =
        try {
            decoder.decode(text.trimEnd('='))
        } catch (_: IllegalArgumentException) {
            null
        }
}

object Hex {
    fun encode(data: ByteArray): String = data.joinToString("") { "%02x".format(it) }

    fun decode(text: String): ByteArray? {
        if (text.length % 2 != 0) return null
        return try {
            ByteArray(text.length / 2) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
        } catch (_: NumberFormatException) {
            null
        }
    }
}

fun u32(value: Int): ByteArray = byteArrayOf(
    (value ushr 24).toByte(), (value ushr 16).toByte(), (value ushr 8).toByte(), value.toByte(),
)

fun readU32(data: ByteArray, offset: Int = 0): Long =
    ((data[offset].toLong() and 0xff) shl 24) or
        ((data[offset + 1].toLong() and 0xff) shl 16) or
        ((data[offset + 2].toLong() and 0xff) shl 8) or
        (data[offset + 3].toLong() and 0xff)

fun encodeFrame(body: ByteArray): ByteArray {
    require(body.size <= MAX_FRAME_BYTES) { "Frame too large" }
    return concat(u32(body.size), body)
}

/**
 * Reads at most [maxBytes], rejecting streams whose reported size was wrong. With the
 * reported size as [sizeHint], a correct report fills one array without further copies.
 */
fun InputStream.readAtMost(maxBytes: Int, sizeHint: Long = -1): ByteArray? {
    require(maxBytes >= 0)
    val out = ByteArrayOutputStream(minOf(maxBytes, 8192))
    if (sizeHint in 0..maxBytes.toLong()) {
        val exact = ByteArray(sizeHint.toInt())
        var filled = 0
        while (filled < exact.size) {
            val count = read(exact, filled, exact.size - filled)
            if (count < 0) return exact.copyOf(filled)
            filled += count
        }
        val next = read()
        if (next < 0) return exact
        if (exact.size >= maxBytes) return null
        out.write(exact)
        out.write(next)
    }
    val buffer = ByteArray(8192)
    while (true) {
        val count = read(buffer, 0, minOf(buffer.size.toLong(), maxBytes.toLong() - out.size() + 1).toInt())
        if (count < 0) return out.toByteArray()
        if (count > maxBytes - out.size()) return null
        out.write(buffer, 0, count)
    }
}

/** Incremental decoder for `[u32 BE length][body]` frames. */
class FrameDecoder {
    private val buffer = ByteArrayOutputStream()
    private var pending = ByteArray(0)
    var failed = false
        private set

    fun push(data: ByteArray, size: Int = data.size) {
        buffer.write(data, 0, size)
    }

    fun next(): ByteArray? {
        if (failed) return null
        if (buffer.size() > 0) {
            pending = concat(pending, buffer.toByteArray())
            buffer.reset()
        }
        if (pending.size < 4) return null
        val length = readU32(pending)
        if (length > MAX_FRAME_BYTES) {
            failed = true
            return null
        }
        if (pending.size - 4 < length) return null
        val end = 4 + length.toInt()
        val frame = pending.copyOfRange(4, end)
        pending = pending.copyOfRange(end, pending.size)
        return frame
    }
}

/** 12-byte AES-GCM nonce: four zero bytes followed by a big-endian counter. */
fun counterNonce(counter: Long): ByteArray {
    val nonce = ByteArray(12)
    for (i in 0 until 8) nonce[11 - i] = (counter ushr (8 * i)).toByte()
    return nonce
}

class Payload(val json: String, val binary: ByteArray = ByteArray(0))

fun packPayload(json: String, binary: ByteArray = ByteArray(0)): ByteArray {
    val jsonBytes = json.utf8()
    return concat(u32(jsonBytes.size), jsonBytes, binary)
}

fun unpackPayload(data: ByteArray): Payload? {
    if (data.size < 4) return null
    val length = readU32(data)
    if (length > data.size - 4) return null
    val end = 4 + length.toInt()
    return Payload(String(data, 4, length.toInt(), Charsets.UTF_8), data.copyOfRange(end, data.size))
}
