package app.feathercast.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.assertFalse
import kotlin.test.assertContentEquals

class ScreenTest {
    @Test fun inputFixturesAndValidation() {
        val fixture = """{"type":"screen.input","session":"s1","generation":2,"action":"text","x":0,"y":0,"value":0,"text":"Grüße 🌻"}"""
        assertEquals(ScreenInput("s1", 2, "text", text = "Grüße 🌻"), ScreenInput.parse(fixture))
        assertNull(ScreenInput.parse(fixture.replace("\"generation\":2", "\"generation\":0")))
        assertNull(ScreenInput.parse(fixture.replace("\"generation\":2", "\"generation\":4294967298")))
        assertNull(ScreenInput.parse(fixture.replace("\"action\":\"text\"", "\"action\":\"shell\"")))
        assertNull(ScreenInput.parse("""{"type":"screen.input","session":"s1","generation":1,"action":"down","x":-1}"""))
        // The largest allowed text still fits after worst-case JSON escaping; one more byte does not.
        val control = "\u0001".repeat(MAX_SCREEN_TEXT_BYTES)
        val escaped = fixture.replace("Grüße 🌻", control.map { "\\u%04x".format(it.code) }.joinToString(""))
        assertTrue(escaped.length > 6 * MAX_SCREEN_TEXT_BYTES && escaped.length <= MAX_SCREEN_INPUT_BYTES)
        assertEquals(control, ScreenInput.parse(escaped)?.text)
        assertNull(ScreenInput.parse(fixture.replace("Grüße 🌻", "a".repeat(MAX_SCREEN_TEXT_BYTES + 1))))
        val request = PcMessage.parse("""{"type":"screen.start","session":"s1","key":"${Base64Url.encode(ByteArray(32))}","audio":true}""")
        assertNotNull(request as? PcMessage.ScreenStart)
        assertTrue(request.request.audio)
        assertNull(PcMessage.parse("""{"type":"screen.start","session":"s1","key":"AA"}"""))
        assertEquals(PcMessage.ScreenStop("s1"), PcMessage.parse("""{"type":"screen.stop","session":"s1"}"""))
        assertEquals("""{"type":"screen.video.config","session":"s1","generation":2,"width":720,"height":1280}""",
            ScreenMessages.videoConfig("s1", 2, 720, 1280))
    }

    @Test fun boundedVideoRecoversAtKeyframe() {
        val queue = ScreenMediaQueue()
        val config = ScreenMediaPacket(ScreenMediaKind.VideoConfig, "config", byteArrayOf(1))
        fun frame(pts: Long, key: Boolean = false) = ScreenMediaPacket(ScreenMediaKind.Video, "frame", byteArrayOf(2), pts, key)
        assertTrue(queue.offer(config))
        assertFalse(queue.offer(frame(0)))
        assertTrue(queue.offer(frame(0, true)))
        repeat(5) { assertTrue(queue.offer(frame((it + 1) * 30_000L))) }
        assertFalse(queue.offer(frame(250_000)))
        assertFalse(queue.offer(frame(280_000)))
        assertTrue(queue.offer(frame(310_000, true)))
        assertEquals(config, queue.poll())
        assertTrue(queue.poll()!!.keyframe)
        assertNull(queue.poll())
    }

    @Test fun audioDropsOldPacketsAndCodecsNormalize() {
        val queue = ScreenMediaQueue()
        repeat(100) { queue.offer(ScreenMediaPacket(ScreenMediaKind.Audio, "audio", byteArrayOf(1), it * 21_333L)) }
        assertTrue(queue.size() <= 6)
        assertTrue(queue.poll()!!.pts >= 94 * 21_333L)
        val annex = byteArrayOf(0, 0, 0, 1, 0x67, 2)
        assertContentEquals(annex, screenAnnexB(annex))
        assertContentEquals(annex, screenAnnexB(byteArrayOf(0, 0, 0, 2, 0x67, 2)))
        assertContentEquals(annex, screenAnnexB(byteArrayOf(0x67, 2)))
    }

    @Test fun lengthPrefixedCodecPacketsKeepAllNalsAndInput() {
        val input = byteArrayOf(0, 0, 0, 2, 0x67, 2, 0, 0, 0, 3, 0x68, 3, 4)
        val original = input.copyOf()
        assertContentEquals(byteArrayOf(0, 0, 0, 1, 0x67, 2, 0, 0, 0, 1, 0x68, 3, 4), screenAnnexB(input))
        assertContentEquals(original, input)
        for (malformed in listOf(byteArrayOf(), byteArrayOf(0, 0), byteArrayOf(0, 0, 0, 0),
            byteArrayOf(0, 0, 0, 2, 0x67), input + byteArrayOf(0), byteArrayOf(-1, -1, -1, -1))) {
            assertContentEquals(concat(byteArrayOf(0, 0, 0, 1), malformed), screenAnnexB(malformed))
        }
        val annex = byteArrayOf(0, 0, 1, 0x67, 2)
        kotlin.test.assertSame(annex, screenAnnexB(annex))
    }
}
