package app.feathercast.phone

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.webkit.MimeTypeMap
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import java.io.File
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Saves files sent from the PC to Download/FeatherCast and announces them. */
object IncomingFiles {
    const val CHANNEL_ID = "files"
    private const val FOLDER = "FeatherCast"
    private var nextNotificationId = 1000

    /** Before Android 10 writing to Download needs the storage permission. */
    fun needsPermission(context: Context): Boolean =
        Build.VERSION.SDK_INT < 29 &&
            context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED

    fun createChannel(context: Context) {
        val channel = NotificationChannel(CHANNEL_ID, "Files from PC", NotificationManager.IMPORTANCE_DEFAULT).apply {
            description = "Shows files that your PC sent to this phone."
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    /** Returns null on success, or a readable error. */
    fun save(context: Context, rawName: String, bytes: ByteArray): String? {
        val name = sanitize(rawName)
        val uri = try {
            if (Build.VERSION.SDK_INT >= 29) saveToMediaStore(context, name, bytes) else saveLegacy(context, name, bytes)
        } catch (error: IOException) {
            return error.message ?: "Could not save $name."
        } catch (_: SecurityException) {
            return "FeatherCast may not save files on this phone."
        }
        announce(context, name, uri)
        return null
    }

    private fun sanitize(name: String): String {
        val clean = name.substringAfterLast('/').substringAfterLast('\\')
            .filter { it >= ' ' && it !in "<>:\"|?*" }
            .trimStart('.')
            .take(120)
        return clean.ifEmpty { "file-from-pc" }
    }

    private fun mimeType(name: String): String =
        MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase())
            ?: "application/octet-stream"

    @androidx.annotation.RequiresApi(29)
    private fun saveToMediaStore(context: Context, name: String, bytes: ByteArray): Uri {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, mimeType(name))
            put(MediaStore.Downloads.RELATIVE_PATH, "${Environment.DIRECTORY_DOWNLOADS}/$FOLDER")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: throw IOException("Could not create $name in Downloads.")
        try {
            resolver.openOutputStream(uri)?.use { it.write(bytes) } ?: throw IOException("Could not write $name.")
            resolver.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        } catch (error: Exception) {
            resolver.delete(uri, null, null)
            throw error as? IOException ?: IOException("Could not write $name.")
        }
        return uri
    }

    @Suppress("DEPRECATION")
    private fun saveLegacy(context: Context, name: String, bytes: ByteArray): Uri? {
        if (needsPermission(context)) throw IOException("Allow storage access in the FeatherCast app to receive files.")
        val dir = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), FOLDER)
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("Could not create Download/$FOLDER.")
        var target = File(dir, name)
        var counter = 2
        while (target.exists() && counter < 1000) {
            target = File(dir, "${name.substringBeforeLast('.', name)} ($counter)" +
                name.substringAfterLast('.', "").let { if (it.isEmpty() || !name.contains('.')) "" else ".$it" })
            counter++
        }
        target.writeBytes(bytes)
        // The scanner hands back a content:// URI that other apps may open.
        val latch = CountDownLatch(1)
        var scanned: Uri? = null
        MediaScannerConnection.scanFile(context, arrayOf(target.absolutePath), arrayOf(mimeType(name))) { _, uri ->
            scanned = uri
            latch.countDown()
        }
        latch.await(3, TimeUnit.SECONDS)
        return scanned
    }

    private fun announce(context: Context, name: String, uri: Uri?) {
        val manager = NotificationManagerCompat.from(context)
        if (!manager.areNotificationsEnabled()) return
        val builder = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_feathercast)
            .setContentTitle("Received $name")
            .setContentText("Saved to Download/$FOLDER")
            .setAutoCancel(true)
        if (uri != null) {
            val open = Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, mimeType(name))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            builder.setContentIntent(
                PendingIntent.getActivity(
                    context, nextNotificationId,
                    Intent.createChooser(open, "Open $name").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                ),
            )
        }
        try {
            manager.notify(nextNotificationId++, builder.build())
        } catch (_: SecurityException) {
        }
    }
}
