package app.feathercast.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.Closeable
import java.io.DataInputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.net.URLDecoder

class LinkException(val code: String, message: String) : IOException(message)

/** Builds a JSON message with a `type` field. */
fun message(type: String, block: JsonObjectBuilder.() -> Unit = {}): String =
    buildJsonObject {
        put("type", type)
        block()
    }.toString()

fun parseJson(text: String): JsonObject? =
    try {
        Json.parseToJsonElement(text) as? JsonObject
    } catch (_: Exception) {
        null
    }

fun JsonObject.str(key: String): String =
    (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: ""

fun JsonObject.long(key: String, fallback: Long = 0): Long =
    (this[key] as? JsonPrimitive)?.content?.toLongOrNull() ?: fallback

fun JsonElement.strOrEmpty(): String = (this as? JsonPrimitive)?.jsonPrimitive?.content ?: ""

/** Contents of the pairing QR code shown by FeatherCast on the PC. */
data class PairingInvite(
    val pcName: String,
    val pcId: String,
    val hosts: List<String>,
    val port: Int,
    val pcPublicKey: ByteArray,
    val token: ByteArray,
) {
    companion object {
        fun parse(text: String): PairingInvite? {
            val uri = try {
                URI(text.trim())
            } catch (_: Exception) {
                return null
            }
            if (uri.scheme != "feathercast" || uri.host != "pair") return null
            val query = uri.rawQuery ?: return null
            val params = try {
                query.split('&').mapNotNull {
                    val eq = it.indexOf('=')
                    if (eq <= 0) null
                    else it.substring(0, eq) to URLDecoder.decode(it.substring(eq + 1), "UTF-8")
                }.toMap()
            } catch (_: IllegalArgumentException) {
                return null
            }
            if (params["v"] != PROTOCOL_VERSION.toString()) return null
            val key = Base64Url.decode(params["k"] ?: return null) ?: return null
            val token = Base64Url.decode(params["t"] ?: return null) ?: return null
            val hosts = (params["h"] ?: "").split(',').map { it.trim() }.filter { it.isNotEmpty() }
            val port = params["p"]?.toIntOrNull() ?: DEFAULT_PORT
            if (key.size != 65 || key[0] != 4.toByte() || token.size != 16 ||
                hosts.isEmpty() || port !in 1..65535) return null
            return PairingInvite(params["n"] ?: "PC", params["id"] ?: "", hosts, port, key, token)
        }
    }
}

data class PairResult(val pcId: String, val pcName: String, val linkKey: ByteArray, val host: String)

private fun openSocket(host: String, port: Int, timeoutMs: Int): Socket {
    val socket = Socket()
    try {
        socket.tcpNoDelay = true
        socket.connect(InetSocketAddress(host, port), timeoutMs)
        socket.soTimeout = timeoutMs
        return socket
    } catch (error: Exception) {
        socket.close()
        throw error
    }
}

private fun writeFrame(out: OutputStream, body: ByteArray) {
    out.write(encodeFrame(body))
    out.flush()
}

private fun readFrame(input: DataInputStream, maxBytes: Int = MAX_FRAME_BYTES): ByteArray {
    val length = input.readInt().toLong() and 0xffffffffL
    if (length > maxBytes) throw IOException("Frame too large")
    val body = ByteArray(length.toInt())
    input.readFully(body)
    return body
}

private fun readPlain(input: DataInputStream): JsonObject {
    val root = parseJson(String(readFrame(input, MAX_HANDSHAKE_BYTES), Charsets.UTF_8))
        ?: throw IOException("Malformed reply")
    if (root.str("type") == "error") {
        throw LinkException(root.str("code"), root.str("message").ifEmpty { "The PC refused the connection." })
    }
    return root
}

object Pairing {
    /** Pairs with the PC from a scanned invite; tries every advertised address. */
    fun pair(invite: PairingInvite, deviceId: String, deviceName: String, timeoutMs: Int = 4000): PairResult {
        var lastError: IOException = IOException("The PC could not be reached.")
        for (host in invite.hosts) {
            val socket = try {
                openSocket(host, invite.port, timeoutMs)
            } catch (error: IOException) {
                lastError = error
                continue
            }
            socket.use {
                val keys = EcdhKeyPair.generate()
                val proof = Crypto.hmacSha256(
                    invite.token, concat(Labels.PAIR_PROOF.utf8(), keys.publicKey, invite.pcPublicKey),
                )
                writeFrame(socket.getOutputStream(), message("pair") {
                    put("deviceId", deviceId)
                    put("name", deviceName)
                    put("pub", Base64Url.encode(keys.publicKey))
                    put("proof", Base64Url.encode(proof))
                }.utf8())
                val reply = readPlain(DataInputStream(socket.getInputStream()))
                if (reply.str("type") != "paired") throw IOException("Unexpected reply from the PC.")
                val shared = keys.agree(invite.pcPublicKey) ?: throw IOException("Invalid PC key.")
                val linkKey = Crypto.hkdf(shared, invite.token, Labels.LINK_KEY_INFO, 32)
                val expected = Crypto.hmacSha256(linkKey, concat(Labels.PAIRED_PROOF.utf8(), keys.publicKey))
                val confirm = Base64Url.decode(reply.str("proof")) ?: ByteArray(0)
                if (!Crypto.constantTimeEquals(expected, confirm)) {
                    throw LinkException("bad-proof", "The PC could not be verified.")
                }
                return PairResult(
                    reply.str("pcId").ifEmpty { invite.pcId },
                    reply.str("pcName").ifEmpty { invite.pcName },
                    linkKey,
                    host,
                )
            }
        }
        throw lastError
    }
}

/** An authenticated, encrypted connection to FeatherCast on the PC. */
class LinkSession private constructor(
    private val socket: Socket,
    private val sendKey: ByteArray,
    private val recvKey: ByteArray,
    val pcName: String,
    val host: String,
) : Closeable {
    private val input: InputStream = socket.getInputStream()
    private val dataInput = DataInputStream(input)
    private val output: OutputStream = socket.getOutputStream()
    private var sendCounter = 0L
    private var recvCounter = 0L
    private val sendLock = Any()

    fun send(json: String, binary: ByteArray = ByteArray(0)) {
        synchronized(sendLock) {
            val sealed = Crypto.aesGcmEncrypt(sendKey, counterNonce(sendCounter++), packPayload(json, binary))
            writeFrame(output, sealed)
        }
    }

    /** Blocks until the next message arrives; throws on disconnect or tampering. */
    fun receive(maxBytes: Int = MAX_FRAME_BYTES): Payload {
        val frame = readFrame(dataInput, maxBytes)
        val plain = Crypto.aesGcmDecrypt(recvKey, counterNonce(recvCounter++), frame)
            ?: throw IOException("Could not decrypt a message from the PC.")
        return unpackPayload(plain) ?: throw IOException("Malformed message from the PC.")
    }

    fun setReadTimeout(ms: Int) {
        socket.soTimeout = ms
    }

    override fun close() {
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }

    companion object {
        fun connect(host: String, port: Int, deviceId: String, linkKey: ByteArray, timeoutMs: Int = 4000,
                    screenSessionId: String = ""): LinkSession {
            val socket = openSocket(host, port, timeoutMs)
            try {
                val input = DataInputStream(socket.getInputStream())
                val output = socket.getOutputStream()
                val phoneNonce = Crypto.randomBytes(16)
                writeFrame(output, message("hello") {
                    put("deviceId", deviceId)
                    put("nonce", Base64Url.encode(phoneNonce))
                    if (screenSessionId.isNotEmpty()) put("screen", screenSessionId)
                }.utf8())
                val challenge = readPlain(input)
                val pcNonce = Base64Url.decode(challenge.str("nonce"))
                if (challenge.str("type") != "challenge" || pcNonce == null || pcNonce.size != 16) {
                    throw IOException("Unexpected reply from the PC.")
                }
                val mac = Crypto.hmacSha256(linkKey, concat(Labels.PHONE_AUTH.utf8(), phoneNonce, pcNonce))
                writeFrame(output, message("auth") { put("mac", Base64Url.encode(mac)) }.utf8())
                val welcome = readPlain(input)
                val expected = Crypto.hmacSha256(linkKey, concat(Labels.PC_AUTH.utf8(), phoneNonce, pcNonce))
                val pcMac = Base64Url.decode(welcome.str("mac")) ?: ByteArray(0)
                if (welcome.str("type") != "welcome" || !Crypto.constantTimeEquals(expected, pcMac)) {
                    throw LinkException("bad-auth", "The PC could not be verified.")
                }
                val salt = concat(phoneNonce, pcNonce)
                return LinkSession(
                    socket,
                    Crypto.hkdf(linkKey, salt, Labels.PHONE_TO_PC, 32),
                    Crypto.hkdf(linkKey, salt, Labels.PC_TO_PHONE, 32),
                    welcome.str("pcName").ifEmpty { challenge.str("pcName") },
                    host,
                )
            } catch (error: Exception) {
                socket.close()
                throw error
            }
        }
    }
}

/** Parses a UDP beacon `FCAST1 <pcId> <port>`. */
data class Beacon(val pcId: String, val port: Int) {
    companion object {
        fun parse(text: String): Beacon? {
            val parts = text.trim().split(' ')
            if (parts.size < 3 || parts[0] != BEACON_PREFIX) return null
            val port = parts[2].toIntOrNull() ?: return null
            if (port !in 1..65535 || parts[1].isEmpty()) return null
            return Beacon(parts[1], port)
        }
    }
}
