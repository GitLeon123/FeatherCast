package app.feathercast.phone

import android.app.Activity
import android.content.ClipboardManager
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Invisible activity that sends the phone clipboard to the PC. Android only
 * lets the focused app read the clipboard, so the read happens once this
 * window has focus.
 */
class SendClipboardActivity : ComponentActivity() {
    private var handled = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        LinkService.start(this)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || handled) return
        handled = true
        sendPhoneClipboard(this) { finish() }
    }

    companion object {
        /** Reads the clipboard (caller must have window focus) and sends it to the PC. */
        fun sendPhoneClipboard(activity: ComponentActivity, done: () -> Unit = {}) {
            val clipboard = activity.getSystemService(ClipboardManager::class.java)
            val text = clipboard?.primaryClip?.takeIf { it.itemCount > 0 }
                ?.getItemAt(0)?.coerceToText(activity)?.toString().orEmpty()
            if (text.isEmpty()) {
                toast(activity, "The clipboard is empty.")
                done()
                return
            }
            activity.lifecycleScope.launch {
                val sent = withContext(Dispatchers.IO) { LinkManager.instance.sendClipboardText(text) }
                val pc = LinkManager.instance.state.value.pcName.ifEmpty { "your PC" }
                toast(activity, if (sent) "Clipboard sent to $pc" else "Not connected to $pc")
                done()
            }
        }

        fun toast(activity: Activity, text: String) {
            Toast.makeText(activity.applicationContext, text, Toast.LENGTH_SHORT).show()
        }
    }
}
