package app.feathercast.phone

import android.content.Context
import app.feathercast.protocol.FileSequence
import app.feathercast.protocol.FileStart
import app.feathercast.protocol.MAX_STREAM_TRANSFERS
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/** Session-owned temporary files; cancellation and disconnect delete partial data. */
class IncomingTransfers(private val context: Context) : AutoCloseable {
    private data class Incoming(val start: FileStart, val sequence: FileSequence, val file: File, val output: FileOutputStream,
        val cancelled: java.util.concurrent.atomic.AtomicBoolean = java.util.concurrent.atomic.AtomicBoolean(), var finishing: Boolean = false)
    private val files = mutableMapOf<String, Incoming>()
    // Transfers that failed or were cancelled, newest last. The PC was told (or
    // told us), so their frames still on the way need no second answer.
    private val ended = ArrayDeque<String>()

    private fun markEnded(id: String) {
        if (id.isEmpty() || id in ended) return
        ended.addLast(id)
        if (ended.size > 64) ended.removeFirst()
    }

    @Synchronized fun ended(id: String): Boolean = id in ended

    @Synchronized fun begin(start: FileStart) {
        if (files.size >= MAX_STREAM_TRANSFERS || start.id in files) throw IOException("Too many transfers in progress.")
        if (!PhoneApp.instance.store.receiveFiles || IncomingFiles.needsPermission(context)) {
            throw IOException("Allow receiving files in FeatherCast on your phone.")
        }
        val file = File.createTempFile("fc-receive-", ".part", context.cacheDir)
        try { files[start.id] = Incoming(start, FileSequence(start.size), file, FileOutputStream(file)) }
        catch (error: Exception) { file.delete(); throw error }
        ended.remove(start.id)
    }

    @Synchronized fun chunk(id: String, offset: Long, bytes: ByteArray): Pair<FileStart, Long> {
        val incoming = files[id] ?: throw IOException("The transfer is no longer active.")
        if (incoming.finishing) throw IOException("The transfer is already finishing.")
        if (!incoming.sequence.accept(offset, bytes.size)) {
            cancel(id)
            throw IOException("Invalid or out-of-order file chunk.")
        }
        try { incoming.output.write(bytes) }
        catch (error: Exception) { cancel(id); throw error }
        return incoming.start to incoming.sequence.received
    }

    fun finish(id: String): FileStart {
        val incoming = synchronized(this) {
            val value = files[id] ?: throw IOException("The transfer is no longer active.")
            if (value.finishing) throw IOException("The transfer is already finishing.")
            value.finishing = true
            value.output.close()
            value
        }
        try {
            if (!incoming.sequence.complete) throw IOException("The file transfer was incomplete.")
            val error = incoming.file.inputStream().use {
                IncomingFiles.saveStream(context, incoming.start.name, it) { incoming.cancelled.get() }
            }
            if (error != null) throw IOException(error)
            return incoming.start
        } finally {
            synchronized(this) { files.remove(id, incoming) }
            try { incoming.output.close() } catch (_: IOException) { }
            incoming.file.delete()
        }
    }

    @Synchronized fun cancel(id: String = "") {
        val cancelled = if (id.isEmpty()) files.values.toList().also { files.clear() }
                        else listOfNotNull(files.remove(id))
        cancelled.forEach { incoming ->
            incoming.cancelled.set(true)
            try { incoming.output.close() } catch (_: IOException) { }
            incoming.file.delete()
            markEnded(incoming.start.id)
        }
        markEnded(id)
    }
    override fun close() = cancel()
}
