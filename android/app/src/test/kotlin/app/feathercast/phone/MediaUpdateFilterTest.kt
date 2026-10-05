package app.feathercast.phone

import app.feathercast.protocol.MediaInfo
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MediaUpdateFilterTest {
    private val media = MediaInfo("player", "Player", "Track", "Artist", true, 1000, 60000, 10000, 5, 10)

    @Test fun skipsOrdinaryProgressButSendsSeeks() {
        val filter = MediaUpdateFilter()
        assertTrue(filter.shouldSend(media))
        assertFalse(filter.shouldSend(media.copy(position = 2000, positionAt = 11000)))
        assertTrue(filter.shouldSend(media.copy(position = 30000, positionAt = 12000)))
        assertFalse(filter.shouldSend(media.copy(position = 31000, positionAt = 13000)))
        assertTrue(filter.shouldSend(media.copy(position = 1000, positionAt = 14000)))
    }

    @Test fun preservesPausedSeeksMetadataVolumeAndClockChanges() {
        val filter = MediaUpdateFilter()
        val paused = media.copy(playing = false)
        assertTrue(filter.shouldSend(paused))
        assertFalse(filter.shouldSend(paused.copy(positionAt = 12000)))
        assertTrue(filter.shouldSend(paused.copy(position = 1001, positionAt = 13000)))
        assertTrue(filter.shouldSend(paused.copy(volume = 6, positionAt = 14000)))
        assertTrue(filter.shouldSend(paused.copy(title = "Other", positionAt = 15000)))
        assertTrue(filter.shouldSend(paused.copy(positionAt = 9000)))
        filter.reset()
        assertTrue(filter.shouldSend(paused))
    }

    @Test fun resynchronizesWhenSpeedChangesOrPlaybackReachesTheEnd() {
        val filter = MediaUpdateFilter()
        assertTrue(filter.shouldSend(media))
        assertTrue(filter.shouldSend(media, 2f))
        assertTrue(filter.shouldSend(media.copy(position = 5000, positionAt = 12000), 2f))
        val end = media.copy(position = 60000, positionAt = 40000)
        assertTrue(filter.shouldSend(end))
        assertFalse(filter.shouldSend(end.copy(positionAt = 41000)))
    }
}
