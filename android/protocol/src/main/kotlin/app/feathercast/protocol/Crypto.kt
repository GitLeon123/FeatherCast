package app.feathercast.protocol

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object Crypto {
    private val random = SecureRandom()

    fun randomBytes(size: Int): ByteArray = ByteArray(size).also(random::nextBytes)

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        // HMAC keys may be empty in HKDF; SecretKeySpec rejects that.
        mac.init(SecretKeySpec(if (key.isEmpty()) ByteArray(32) else key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    /** RFC 5869 HKDF-SHA256. */
    fun hkdf(ikm: ByteArray, salt: ByteArray, info: String, length: Int): ByteArray =
        hkdf(ikm, salt, info.utf8(), length)

    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int): ByteArray {
        val prk = hmacSha256(if (salt.isEmpty()) ByteArray(32) else salt, ikm)
        val out = ByteArray(length)
        var previous = ByteArray(0)
        var offset = 0
        var counter = 1
        while (offset < length) {
            previous = hmacSha256(prk, concat(previous, info, byteArrayOf(counter.toByte())))
            val take = minOf(previous.size, length - offset)
            previous.copyInto(out, offset, 0, take)
            offset += take
            counter++
        }
        return out
    }

    fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    /** AES-256-GCM; the 16-byte tag is appended to the ciphertext. */
    fun aesGcmEncrypt(key: ByteArray, nonce: ByteArray, plaintext: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
        return cipher.doFinal(plaintext)
    }

    fun aesGcmDecrypt(key: ByteArray, nonce: ByteArray, sealed: ByteArray): ByteArray? =
        try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
            cipher.doFinal(sealed)
        } catch (_: Exception) {
            null
        }
}

/** Ephemeral ECDH P-256 key pair used during pairing. */
class EcdhKeyPair private constructor(private val pair: KeyPair) {
    /** 65-byte uncompressed public point (0x04 || X || Y). */
    val publicKey: ByteArray by lazy {
        val point = (pair.public as ECPublicKey).w
        concat(byteArrayOf(4), fixed32(point.affineX), fixed32(point.affineY))
    }

    /** Big-endian X coordinate of the shared point, or null for an invalid peer key. */
    fun agree(peerPublicKey: ByteArray): ByteArray? =
        try {
            require(peerPublicKey.size == 65 && peerPublicKey[0] == 4.toByte())
            val x = BigInteger(1, peerPublicKey.copyOfRange(1, 33))
            val y = BigInteger(1, peerPublicKey.copyOfRange(33, 65))
            val spec = ECPublicKeySpec(ECPoint(x, y), curve())
            val peer = KeyFactory.getInstance("EC").generatePublic(spec)
            val agreement = KeyAgreement.getInstance("ECDH")
            agreement.init(pair.private)
            agreement.doPhase(peer, true)
            agreement.generateSecret()
        } catch (_: Exception) {
            null
        }

    companion object {
        fun generate(): EcdhKeyPair {
            val generator = KeyPairGenerator.getInstance("EC")
            generator.initialize(ECGenParameterSpec("secp256r1"))
            return EcdhKeyPair(generator.generateKeyPair())
        }

        private fun curve(): ECParameterSpec {
            val parameters = AlgorithmParameters.getInstance("EC")
            parameters.init(ECGenParameterSpec("secp256r1"))
            return parameters.getParameterSpec(ECParameterSpec::class.java)
        }

        private fun fixed32(value: BigInteger): ByteArray {
            val raw = value.toByteArray()
            return when {
                raw.size == 32 -> raw
                raw.size > 32 -> raw.copyOfRange(raw.size - 32, raw.size)
                else -> ByteArray(32 - raw.size) + raw
            }
        }
    }
}
