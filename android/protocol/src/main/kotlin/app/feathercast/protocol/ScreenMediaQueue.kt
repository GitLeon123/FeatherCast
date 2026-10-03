package app.feathercast.protocol

import java.util.ArrayDeque

enum class ScreenMediaKind { State, VideoConfig, Video, AudioConfig, Audio }
data class ScreenMediaPacket(val kind: ScreenMediaKind, val json: String, val data: ByteArray,
                           val pts: Long = 0, val keyframe: Boolean = false)

/** Bounded latency, not a recording buffer. Never resume a broken delta-frame chain. */
class ScreenMediaQueue {
    private val packets = ArrayDeque<ScreenMediaPacket>()
    private var videoConfig: ScreenMediaPacket? = null
    private var waitingForKeyframe = true

    @Synchronized
    fun offer(packet: ScreenMediaPacket): Boolean {
        if ((packet.data.isEmpty() && packet.kind != ScreenMediaKind.State) || packet.data.size > MAX_SCREEN_PACKET_BYTES) return false
        when (packet.kind) {
            ScreenMediaKind.State -> packets.removeIf { it.kind == ScreenMediaKind.State }
            ScreenMediaKind.VideoConfig -> {
                videoConfig = packet
                packets.removeIf { it.kind == ScreenMediaKind.Video || it.kind == ScreenMediaKind.VideoConfig }
                waitingForKeyframe = true
            }
            ScreenMediaKind.AudioConfig -> packets.removeIf { it.kind == ScreenMediaKind.AudioConfig || it.kind == ScreenMediaKind.Audio }
            ScreenMediaKind.Video -> {
                val video = packets.filter { it.kind == ScreenMediaKind.Video }
                val overflow = video.size >= 6 || (video.firstOrNull()?.let { packet.pts - it.pts > 250_000 } == true) ||
                    packets.sumOf { it.data.size } + packet.data.size > 4 * 1024 * 1024
                if (overflow) {
                    packets.removeIf { it.kind == ScreenMediaKind.Video }
                    waitingForKeyframe = true
                }
                if (waitingForKeyframe) {
                    if (!packet.keyframe) return false
                    videoConfig?.let { config ->
                        packets.removeIf { it.kind == ScreenMediaKind.VideoConfig }
                        packets.addLast(config)
                    } ?: return false
                    waitingForKeyframe = false
                }
            }
            ScreenMediaKind.Audio -> {
                while (packets.count { it.kind == ScreenMediaKind.Audio } >= 6 ||
                    packets.firstOrNull { it.kind == ScreenMediaKind.Audio }?.let { packet.pts - it.pts > 150_000 } == true) {
                    val oldest = packets.firstOrNull { it.kind == ScreenMediaKind.Audio } ?: break
                    packets.remove(oldest)
                }
            }
        }
        packets.addLast(packet)
        return true
    }

    @Synchronized fun poll(): ScreenMediaPacket? = packets.pollFirst()
    @Synchronized fun clear() { packets.clear(); videoConfig = null; waitingForKeyframe = true }
    @Synchronized fun size(): Int = packets.size
}

/** MediaCodec normally emits Annex B; normalize AVC length-prefixed output too. */
fun screenAnnexB(data: ByteArray): ByteArray {
    if (data.size >= 4 && data[0] == 0.toByte() && data[1] == 0.toByte() &&
        (data[2] == 1.toByte() || (data[2] == 0.toByte() && data[3] == 1.toByte()))) return data
    val output = java.io.ByteArrayOutputStream()
    var offset = 0
    while (offset + 4 <= data.size) {
        val length = readU32(data, offset)
        if (length <= 0 || length > data.size - offset - 4) break
        output.write(byteArrayOf(0, 0, 0, 1))
        output.write(data, offset + 4, length.toInt())
        offset += 4 + length.toInt()
    }
    return if (offset == data.size && offset > 0) output.toByteArray() else concat(byteArrayOf(0, 0, 0, 1), data)
}
