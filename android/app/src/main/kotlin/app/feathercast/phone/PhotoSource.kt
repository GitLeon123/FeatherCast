package app.feathercast.phone

import android.Manifest
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.util.Size
import androidx.core.content.ContextCompat
import app.feathercast.protocol.readAtMost
import java.io.ByteArrayOutputStream

data class PhoneImage(
    val id: Long,
    val name: String,
    val time: Long,
    val width: Int,
    val height: Int,
    val size: Long,
) {
    val uri: Uri get() = ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, id)
}

/** Reads the newest pictures from the MediaStore. */
object PhotoSource {
    val permission: String
        get() = if (Build.VERSION.SDK_INT >= 33) Manifest.permission.READ_MEDIA_IMAGES else Manifest.permission.READ_EXTERNAL_STORAGE

    fun hasPermission(context: Context): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    private val projection = arrayOf(
        MediaStore.Images.Media._ID,
        MediaStore.Images.Media.DISPLAY_NAME,
        MediaStore.Images.Media.DATE_TAKEN,
        MediaStore.Images.Media.DATE_ADDED,
        MediaStore.Images.Media.WIDTH,
        MediaStore.Images.Media.HEIGHT,
        MediaStore.Images.Media.SIZE,
    )

    fun latest(context: Context, limit: Int): List<PhoneImage> = query(context, null, null, limit)

    fun find(context: Context, id: Long): PhoneImage? =
        query(context, "${MediaStore.Images.Media._ID} = ?", arrayOf(id.toString()), 1).firstOrNull()

    private fun query(context: Context, selection: String?, args: Array<String>?, limit: Int): List<PhoneImage> {
        val items = mutableListOf<PhoneImage>()
        try {
            context.contentResolver.query(
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI, projection, selection, args,
                "${MediaStore.Images.Media.DATE_ADDED} DESC",
            )?.use { cursor ->
                while (cursor.moveToNext() && items.size < limit) {
                    val taken = cursor.getLong(2)
                    items += PhoneImage(
                        id = cursor.getLong(0),
                        name = cursor.getString(1) ?: "photo.jpg",
                        time = if (taken > 0) taken else cursor.getLong(3) * 1000,
                        width = cursor.getInt(4),
                        height = cursor.getInt(5),
                        size = cursor.getLong(6),
                    )
                }
            }
        } catch (_: Exception) {
        }
        return items
    }

    fun thumbnail(context: Context, image: PhoneImage): ByteArray? = try {
        val bitmap: Bitmap = if (Build.VERSION.SDK_INT >= 29) {
            context.contentResolver.loadThumbnail(image.uri, Size(320, 320), null)
        } else {
            @Suppress("DEPRECATION")
            MediaStore.Images.Thumbnails.getThumbnail(
                context.contentResolver, image.id, MediaStore.Images.Thumbnails.MINI_KIND, null,
            ) ?: return null
        }
        ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.JPEG, 80, it) }.toByteArray()
    } catch (_: Exception) {
        null
    }

    fun fullBytes(context: Context, image: PhoneImage, maxBytes: Long): ByteArray? = try {
        if (image.size > maxBytes) null else context.contentResolver.openInputStream(image.uri)?.use {
            it.readAtMost(maxBytes.coerceAtMost(Int.MAX_VALUE.toLong()).toInt())
        }
    } catch (_: Exception) {
        null
    }
}
