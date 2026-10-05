package app.feathercast.phone

import app.feathercast.protocol.MediaInfo
import kotlin.math.abs

/** Skips timestamp-only updates while preserving seeks and playback changes. */
class MediaUpdateFilter {
    private var previous: MediaInfo? = null
    private var previousSpeed = 1f

    fun reset() {
        previous = null
    }

    fun shouldSend(media: MediaInfo, playbackSpeed: Float = 1f): Boolean {
        val last = previous
        if (last != null && previousSpeed == playbackSpeed &&
            last.copy(position = media.position, positionAt = media.positionAt) == media &&
            media.positionAt >= last.positionAt
        ) {
            // The PC projects an ordinary playing timeline from the last sent
            // position. A seek (or nonstandard playback speed) must update it.
            val elapsed = if (last.playing) media.positionAt - last.positionAt else 0L
            val projected = (last.position + elapsed).let {
                if (last.duration > 0) it.coerceAtMost(last.duration) else it
            }
            val tolerance = if (media.playing) 750L else 0L
            if (abs(media.position - projected) <= tolerance) return false
        }
        previous = media
        previousSpeed = playbackSpeed
        return true
    }
}
