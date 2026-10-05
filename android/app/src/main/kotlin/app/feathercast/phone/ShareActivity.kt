package app.feathercast.phone

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** "Send to PC" share target for text, pictures and files. */
class ShareActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (PhoneApp.instance.store.load() == null) {
            SendClipboardActivity.toast(this, "Open FeatherCast and pair with your PC first.")
            finish()
            return
        }
        LinkService.start(this)
        val link = LinkManager.instance
        val pc = link.state.value.pcName.ifEmpty { "your PC" }
        val uris = sharedUris(intent)
        val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString().orEmpty()
        lifecycleScope.launch {
            // Give a freshly started service a moment to connect.
            withContext(Dispatchers.IO) {
                repeat(30) {
                    if (link.isConnected) return@withContext
                    Thread.sleep(100)
                }
            }
            val message = if (uris.isNotEmpty()) {
                var sent = 0
                var error: String? = null
                for (uri in uris) {
                    link.sendUri(uri).onSuccess { sent++ }.onFailure { error = it.message }
                }
                when {
                    sent == uris.size -> if (sent == 1) "Sent to $pc" else "$sent files sent to $pc"
                    sent > 0 -> "$sent of ${uris.size} files sent. ${error.orEmpty()}"
                    else -> error ?: "Could not send to $pc"
                }
            } else if (text.isNotEmpty()) {
                val sent = withContext(Dispatchers.IO) { link.sendClipboardText(text) }
                if (sent) "Copied to the clipboard on $pc" else "Not connected to $pc"
            } else {
                "Nothing to send."
            }
            SendClipboardActivity.toast(this@ShareActivity, message)
            finish()
        }
    }

    private fun sharedUris(intent: Intent): List<Uri> {
        @Suppress("DEPRECATION")
        return when (intent.action) {
            Intent.ACTION_SEND -> listOfNotNull(
                if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else intent.getParcelableExtra(Intent.EXTRA_STREAM),
            )
            Intent.ACTION_SEND_MULTIPLE ->
                (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM)) ?: emptyList()
            else -> emptyList()
        }
    }
}
