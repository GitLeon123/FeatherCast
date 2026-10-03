package app.feathercast.phone

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import app.feathercast.protocol.Base64Url
import app.feathercast.protocol.Crypto
import app.feathercast.protocol.Hex
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

data class PairedPc(
    val pcId: String,
    val pcName: String,
    val host: String,
    val port: Int,
    val linkKey: ByteArray,
)

/**
 * Pairing data and feature switches. The link key is encrypted with a
 * non-exportable Android Keystore key before it is written to disk.
 */
class LinkStore(context: Context) {
    private val prefs = context.getSharedPreferences("feathercast-link", Context.MODE_PRIVATE)

    val deviceId: String
        get() = prefs.getString("deviceId", null) ?: Hex.encode(Crypto.randomBytes(8)).also {
            prefs.edit().putString("deviceId", it).apply()
        }

    fun load(): PairedPc? {
        val sealed = prefs.getString("linkKey", null) ?: return null
        val linkKey = unwrap(sealed) ?: return null
        return PairedPc(
            prefs.getString("pcId", "") ?: "",
            prefs.getString("pcName", "PC") ?: "PC",
            prefs.getString("host", "") ?: "",
            prefs.getInt("port", 0),
            linkKey,
        )
    }

    fun save(pc: PairedPc) {
        prefs.edit()
            .putString("pcId", pc.pcId)
            .putString("pcName", pc.pcName)
            .putString("host", pc.host)
            .putInt("port", pc.port)
            .putString("linkKey", wrap(pc.linkKey))
            .apply()
    }

    fun updateAddress(host: String, port: Int) {
        prefs.edit().putString("host", host).putInt("port", port).apply()
    }

    fun updatePcName(name: String) {
        if (name.isNotEmpty()) prefs.edit().putString("pcName", name).apply()
    }

    fun clear() {
        prefs.edit().remove("pcId").remove("pcName").remove("host").remove("port").remove("linkKey").apply()
    }

    var sendNotifications: Boolean
        get() = prefs.getBoolean("sendNotifications", true)
        set(value) = prefs.edit().putBoolean("sendNotifications", value).apply()

    var sendPhotos: Boolean
        get() = prefs.getBoolean("sendPhotos", true)
        set(value) = prefs.edit().putBoolean("sendPhotos", value).apply()

    var clipboardSync: Boolean
        get() = prefs.getBoolean("clipboardSync", true)
        set(value) = prefs.edit().putBoolean("clipboardSync", value).apply()

    var receiveFiles: Boolean
        get() = prefs.getBoolean("receiveFiles", true)
        set(value) = prefs.edit().putBoolean("receiveFiles", value).apply()

    var allowRing: Boolean
        get() = prefs.getBoolean("allowRing", true)
        set(value) = prefs.edit().putBoolean("allowRing", value).apply()

    var mediaControl: Boolean
        get() = prefs.getBoolean("mediaControl", true)
        set(value) = prefs.edit().putBoolean("mediaControl", value).apply()

    // Features that need extra permissions start switched off.
    var smsAccess: Boolean
        get() = prefs.getBoolean("smsAccess", false)
        set(value) = prefs.edit().putBoolean("smsAccess", value).apply()

    var callAlerts: Boolean
        get() = prefs.getBoolean("callAlerts", false)
        set(value) = prefs.edit().putBoolean("callAlerts", value).apply()

    var storageAccess: Boolean
        get() = prefs.getBoolean("storageAccess", false)
        set(value) = prefs.edit().putBoolean("storageAccess", value).apply()

    var screenSharing: Boolean
        get() = prefs.getBoolean("screenSharing", false)
        set(value) = prefs.edit().putBoolean("screenSharing", value).apply()

    var remoteControl: Boolean
        get() = prefs.getBoolean("remoteControl", false)
        set(value) = prefs.edit().putBoolean("remoteControl", value).apply()

    private fun keystoreKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    private fun wrap(secret: ByteArray): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, keystoreKey())
        return Base64Url.encode(cipher.iv) + "." + Base64Url.encode(cipher.doFinal(secret))
    }

    private fun unwrap(sealed: String): ByteArray? =
        try {
            val (iv, data) = sealed.split('.').let { Base64Url.decode(it[0])!! to Base64Url.decode(it[1])!! }
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, keystoreKey(), GCMParameterSpec(128, iv))
            cipher.doFinal(data)
        } catch (_: Exception) {
            null
        }

    private companion object {
        const val KEY_ALIAS = "feathercast-link-key"
    }
}
