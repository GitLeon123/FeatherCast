package app.feathercast.protocol

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject

class FileTransferTest {
    @Test fun parsesMetadataAndRejectsInvalidLimits() {
        val start = FileStart("transfer-1", "movie.mp4", 80L * 1024 * 1024, "file")
        assertEquals(start, FileStart.parse(Json.parseToJsonElement(start.message()).jsonObject))
        assertNull(FileStart.parse(Json.parseToJsonElement(start.copy(size = -1).message()).jsonObject))
        assertNull(FileStart.parse(Json.parseToJsonElement(start.copy(id = "../x").message()).jsonObject))
        assertNull(FileStart.parse(Json.parseToJsonElement(start.copy(size = MAX_STREAM_BYTES + 1).message()).jsonObject))
        assertNotNull(FileStart.parse(Json.parseToJsonElement(start.copy(size = 0).message()).jsonObject))
    }
    @Test fun enforcesOrderedBoundedChunksAndExactCompletion() {
        val sequence = FileSequence(FILE_CHUNK_BYTES + 3L)
        assertFalse(sequence.accept(1, FILE_CHUNK_BYTES))
        assertFalse(sequence.accept(0, FILE_CHUNK_BYTES + 1))
        assertTrue(sequence.accept(0, FILE_CHUNK_BYTES))
        assertFalse(sequence.complete)
        assertFalse(sequence.accept(0, 3))
        assertFalse(sequence.accept(FILE_CHUNK_BYTES.toLong(), 4))
        assertTrue(sequence.accept(FILE_CHUNK_BYTES.toLong(), 3))
        assertTrue(sequence.complete)
        assertTrue(FileSequence(0).complete)
    }
}
