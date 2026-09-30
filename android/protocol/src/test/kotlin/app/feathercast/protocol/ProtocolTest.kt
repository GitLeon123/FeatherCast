package app.feathercast.protocol

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

// The vectors below are also checked by native/tests/phone_protocol_tests.cpp,
// which keeps the Kotlin and C++ implementations interoperable.
class ProtocolTest {
    @Test
    fun boundedTransfers() {
        assertContentEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 3).inputStream().readAtMost(3))
        assertNull(byteArrayOf(1, 2, 3, 4).inputStream().readAtMost(3))
        assertContentEquals(ByteArray(0), ByteArray(0).inputStream().readAtMost(0))
        assertNull(byteArrayOf(1).inputStream().readAtMost(0))
    }

    @Test
    fun encodings() {
        assertEquals("YW55IGNhcm5hbCBwbGVhcw", Base64Url.encode("any carnal pleas".utf8()))
        assertContentEquals("any carnal pleas".utf8(), Base64Url.decode("YW55IGNhcm5hbCBwbGVhcw=="))
        val binary = byteArrayOf(0xfb.toByte(), 0xff.toByte(), 0x00, 0x10)
        assertEquals("-_8AEA", Base64Url.encode(binary))
        assertEquals("fbff0010", Hex.encode(binary))
    }

    @Test
    fun framing() {
        val decoder = FrameDecoder()
        val stream = concat(encodeFrame("hello".utf8()), encodeFrame("world!".utf8()))
        decoder.push(stream.copyOfRange(0, 3))
        assertNull(decoder.next())
        decoder.push(stream.copyOfRange(3, stream.size))
        assertContentEquals("hello".utf8(), decoder.next())
        assertContentEquals("world!".utf8(), decoder.next())
        assertNull(decoder.next())

        val payload = unpackPayload(packPayload("{\"type\":\"x\"}", byteArrayOf(1, 2, 3)))
        assertNotNull(payload)
        assertEquals("{\"type\":\"x\"}", payload.json)
        assertContentEquals(byteArrayOf(1, 2, 3), payload.binary)
        val nonce = counterNonce(0x0102)
        assertEquals(12, nonce.size)
        assertEquals(1, nonce[10].toInt())
        assertEquals(2, nonce[11].toInt())
    }

    @Test
    fun hmacAndHkdfVectors() {
        assertEquals(
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            Hex.encode(Crypto.hmacSha256("Jefe".utf8(), "what do ya want for nothing?".utf8())),
        )
        val okm = Crypto.hkdf(
            ByteArray(22) { 0x0b },
            Hex.decode("000102030405060708090a0b0c")!!,
            Hex.decode("f0f1f2f3f4f5f6f7f8f9")!!,
            42,
        )
        assertEquals(
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            Hex.encode(okm),
        )
    }

    @Test
    fun aesGcm() {
        assertEquals(
            "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919",
            Hex.encode(Crypto.aesGcmEncrypt(ByteArray(32), ByteArray(12), ByteArray(16))),
        )
        val key = Crypto.randomBytes(32)
        val sealed = Crypto.aesGcmEncrypt(key, counterNonce(7), "secret".utf8())
        assertContentEquals("secret".utf8(), Crypto.aesGcmDecrypt(key, counterNonce(7), sealed))
        assertNull(Crypto.aesGcmDecrypt(key, counterNonce(8), sealed))
    }

    @Test
    fun ecdhAgreement() {
        val a = EcdhKeyPair.generate()
        val b = EcdhKeyPair.generate()
        assertEquals(65, a.publicKey.size)
        val ab = a.agree(b.publicKey)
        assertNotNull(ab)
        assertEquals(32, ab.size)
        assertContentEquals(ab, b.agree(a.publicKey))
        assertNull(a.agree(ByteArray(65)))
    }

    @Test
    fun pairingUri() {
        val key = concat(byteArrayOf(4), ByteArray(64) { 1 })
        val token = ByteArray(16) { 2 }
        val invite = PairingInvite.parse(
            "feathercast://pair?v=1&n=My%20PC&id=abcd&h=192.168.1.5%2C10.0.0.2&p=47800" +
                "&k=${Base64Url.encode(key)}&t=${Base64Url.encode(token)}",
        )
        assertNotNull(invite)
        assertEquals("My PC", invite.pcName)
        assertEquals(listOf("192.168.1.5", "10.0.0.2"), invite.hosts)
        assertEquals(47800, invite.port)
        assertContentEquals(token, invite.token)
        assertNull(PairingInvite.parse("https://example.com"))
        assertEquals(Beacon("abcd", 47800), Beacon.parse("FCAST1 abcd 47800"))
        assertNull(Beacon.parse("FCAST1 abcd 0"))
        assertNull(Beacon.parse("FCAST1 abcd 65536"))
        val uri = "feathercast://pair?v=1&n=My%20PC&id=abcd&h=192.168.1.5&p=47800" +
            "&k=${Base64Url.encode(key)}&t=${Base64Url.encode(token)}"
        assertNull(PairingInvite.parse(uri.replace("p=47800", "p=65536")))
        assertNull(PairingInvite.parse(uri.replace("My%20PC", "My%xxPC")))
    }

    @Test
    fun messages() {
        val json = message("clipboard.set")
        assertTrue(json.contains("\"type\":\"clipboard.set\""))
        val root = parseJson("{\"type\":\"x\",\"n\":5,\"s\":\"a\\\"b\"}")
        assertNotNull(root)
        assertEquals(5, root.long("n"))
        assertEquals("a\"b", root.str("s"))
    }
}
