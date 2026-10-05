package app.feathercast.phone

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Environment
import app.feathercast.protocol.FileEntry
import app.feathercast.protocol.MAX_TRANSFER_BYTES
import app.feathercast.protocol.PhoneMessages
import app.feathercast.protocol.normalizeStoragePath
import app.feathercast.protocol.readAtMost
import java.io.File
import java.io.IOException

/** Lets the PC browse shared storage (/sdcard) and download files from it. */
object StorageBridge {
    private const val MAX_ENTRIES = 2000

    fun hasAccess(context: Context): Boolean =
        if (Build.VERSION.SDK_INT >= 30) {
            Environment.isExternalStorageManager()
        } else {
            context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
        }

    val active: Boolean
        get() = PhoneApp.instance.store.storageAccess && hasAccess(PhoneApp.instance)

    private val root: File get() = Environment.getExternalStorageDirectory()

    /** Maps a PC path to a file under the storage root, or null when it escapes it. */
    private fun resolve(path: String): Pair<String, File>? {
        val normalized = normalizeStoragePath(path) ?: return null
        val base = root.canonicalFile
        val file = File(base, normalized.removePrefix("/")).canonicalFile
        if (file != base && !file.path.startsWith(base.path + File.separator)) return null
        return normalized to file
    }

    fun list(path: String): String {
        if (!active) return PhoneMessages.filesList(path, emptyList(), "Allow “Phone storage” in the FeatherCast app on the phone.")
        val (normalized, dir) = resolve(path) ?: return PhoneMessages.filesList(path, emptyList(), "This folder is not available.")
        val children = dir.listFiles()
            ?: return PhoneMessages.filesList(normalized, emptyList(), "This folder cannot be opened.")
        val entries = children.asSequence()
            .filter { !it.name.startsWith(".") }
            .take(MAX_ENTRIES)
            .map { FileEntry(it.name, it.isDirectory, if (it.isDirectory) 0 else it.length(), it.lastModified()) }
            .toList()
        return PhoneMessages.filesList(normalized, entries)
    }

    fun streamFile(path: String): File? {
        if (!active) return null
        return resolve(path)?.second?.takeIf { it.isFile }
    }

    /** Returns the legacy file.data message and its bytes (empty on error). */
    fun read(path: String): Pair<String, ByteArray> {
        val fallbackName = path.substringAfterLast('/')
        if (!active) {
            return PhoneMessages.fileData(path, fallbackName, "Allow “Phone storage” in the FeatherCast app on the phone.") to ByteArray(0)
        }
        val (normalized, file) = resolve(path)
            ?: return PhoneMessages.fileData(path, fallbackName, "This file is not available.") to ByteArray(0)
        if (!file.isFile) return PhoneMessages.fileData(normalized, file.name, "This file is not available.") to ByteArray(0)
        if (file.length() > MAX_TRANSFER_BYTES) {
            return PhoneMessages.fileData(normalized, file.name, "${file.name} is larger than 40 MB.") to ByteArray(0)
        }
        return try {
            val bytes = file.inputStream().use { it.readAtMost(MAX_TRANSFER_BYTES.toInt(), sizeHint = file.length()) }
                ?: return PhoneMessages.fileData(normalized, file.name, "${file.name} is larger than 40 MB.") to ByteArray(0)
            PhoneMessages.fileData(normalized, file.name) to bytes
        } catch (_: IOException) {
            PhoneMessages.fileData(normalized, file.name, "Could not read ${file.name}.") to ByteArray(0)
        }
    }
}
