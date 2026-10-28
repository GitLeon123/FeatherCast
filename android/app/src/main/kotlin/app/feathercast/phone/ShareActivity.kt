package app.feathercast.phone

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Parcelable
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
            SendClipboardActivity.toast(this, getString(R.string.share_pair_first))
            finish()
            return
        }
        LinkService.start(this)
        val link = LinkManager.instance
        val pc = link.state.value.pcName.ifEmpty { getString(R.string.your_pc) }
        val uris = sharedUris(intent)
        val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString().orEmpty()
        if (uris.isEmpty() && text.isEmpty()) {
            SendClipboardActivity.toast(this, getString(R.string.share_nothing))
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
                                    sent == uris.size -> resources.getQuantityString(R.plurals.share_sent, sent, sent, pc)
                                    sent > 0 -> getString(R.string.share_partial, sent, uris.size, error.orEmpty())
                                    else -> error ?: getString(R.string.send_failed, pc)
                                }
                            }
                        } else {
                            link.sendTextInBackground(text) { sent ->
                                if (sent) getString(R.string.share_text_done, pc) else getString(R.string.not_connected_to, pc)
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
            // Another app chooses the extras, so read them untyped and keep only Uris.
            Intent.ACTION_SEND -> listOfNotNull(
                if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else intent.getParcelableExtra<Parcelable>(Intent.EXTRA_STREAM) as? Uri,
            )
            Intent.ACTION_SEND_MULTIPLE ->
                (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else intent.getParcelableArrayListExtra<Parcelable>(Intent.EXTRA_STREAM)?.filterIsInstance<Uri>()) ?: emptyList()
            else -> emptyList()
        }
    }
}
