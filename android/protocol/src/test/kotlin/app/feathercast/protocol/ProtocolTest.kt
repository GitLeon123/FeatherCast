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
        // A reported size only presizes the buffer; wrong reports still read the real stream.
        assertContentEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 3).inputStream().readAtMost(3, sizeHint = 3))
        assertContentEquals(byteArrayOf(1, 2), byteArrayOf(1, 2).inputStream().readAtMost(3, sizeHint = 3))
        assertContentEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 3).inputStream().readAtMost(3, sizeHint = 1))
        assertContentEquals(byteArrayOf(1, 2), byteArrayOf(1, 2).inputStream().readAtMost(3, sizeHint = 0))
        assertNull(byteArrayOf(1, 2, 3, 4).inputStream().readAtMost(3, sizeHint = 3))
        assertNull(byteArrayOf(1, 2, 3, 4).inputStream().readAtMost(3, sizeHint = 2))
        val large = ByteArray(20_000) { it.toByte() }
        assertContentEquals(large, large.inputStream().readAtMost(30_000, sizeHint = 10_000))
        assertNull(large.inputStream().readAtMost(19_999, sizeHint = 19_999))
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
    fun reusablePayloadBuffer() {
        val json = "{\"type\":\"file.chunk\",\"name\":\"Grüße 🌻\"}"
        val buffer = ByteArray(32) { it.toByte() }
        for (size in listOf(0, 1, 7, buffer.size)) {
            val packed = packPayload(json, buffer, size)
            assertContentEquals(packPayload(json, buffer.copyOf(size)), packed)
            val payload = unpackPayload(packed)!!
            assertEquals(json, payload.json)
            assertContentEquals(buffer.copyOf(size), payload.binary)
        }
        val owned = packPayload(json, buffer, 7)
        buffer.fill(99)
        assertContentEquals(ByteArray(7) { it.toByte() }, unpackPayload(owned)!!.binary)
        kotlin.test.assertFailsWith<IllegalArgumentException> { packPayload(json, buffer, -1) }
        kotlin.test.assertFailsWith<IllegalArgumentException> { packPayload(json, buffer, buffer.size + 1) }
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

        // Pairing links may only point at local network addresses, never host names or the internet.
        assertNull(PairingInvite.parse(uri.replace("h=192.168.1.5", "h=8.8.8.8")))
        assertNull(PairingInvite.parse(uri.replace("h=192.168.1.5", "h=example.com")))
        assertNull(PairingInvite.parse(uri.replace("h=192.168.1.5", "h=192.168.1.5.example.com")))
        assertEquals(
            listOf("172.16.0.9", "169.254.3.4"),
            PairingInvite.parse(uri.replace("h=192.168.1.5", "h=1.2.3.4%2C172.16.0.9%2Cevil.test%2C169.254.3.4"))?.hosts,
        )
    }

    @Test
    fun localNetworkHosts() {
        for (host in listOf("10.0.0.1", "127.0.0.1", "172.16.0.1", "172.31.255.255", "192.168.0.1", "169.254.1.1",
            "::1", "fe80::1", "[fe80::1]", "fe80::1%wlan0", "fd12:3456::1", "fc00::")) {
            assertTrue(isLocalNetworkHost(host), host)
        }
        for (host in listOf("", "8.8.8.8", "172.32.0.1", "172.15.0.1", "192.169.0.1", "11.0.0.1", "0.0.0.0",
            "010.0.0.1", "10.0.0", "10.0.0.256", "10.0.0.1.2", "localhost", "router.local", "2001:db8::1",
            "::", "::ffff:192.168.0.1", "fe80::1::2", "fe80:0:0:0:0:0:0:0:1", "fe80::12345")) {
            assertTrue(!isLocalNetworkHost(host), host)
        }
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
