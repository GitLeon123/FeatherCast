package app.feathercast.phone

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import app.feathercast.phone.ui.FeatherTheme
import app.feathercast.phone.ui.ShareConfirmDialog

/**
 * "Send to PC" share target for text, pictures and files. Any app can open it,
 * so the user confirms first; the upload then runs in the app's scope, so
 * closing this window does not cancel it.
 */
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
        if (uris.isEmpty() && text.isEmpty()) {
            SendClipboardActivity.toast(this, "Nothing to send.")
            finish()
            return
        }
        setContent {
            FeatherTheme(fillScreen = false) {
                ShareConfirmDialog(
                    pcName = pc,
                    fileCount = uris.size,
                    text = text,
                    onConfirm = {
                        if (uris.isNotEmpty()) {
                            // Takes the app's own read grant while this activity still holds one.
                            link.sendUrisInBackground(uris) { sent, error ->
                                when {
                                    sent == uris.size -> if (sent == 1) "Sent to $pc" else "$sent files sent to $pc"
                                    sent > 0 -> "$sent of ${uris.size} files sent. ${error.orEmpty()}"
                                    else -> error ?: "Could not send to $pc"
                                }
                            }
                        } else {
                            link.sendTextInBackground(text) { sent ->
                                if (sent) "Copied to the clipboard on $pc" else "Not connected to $pc"
                            }
                        }
                        finish()
                    },
                    onDismiss = { finish() },
                )
            }
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
