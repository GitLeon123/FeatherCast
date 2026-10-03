package app.feathercast.phone

import android.content.Context
import android.inputmethodservice.InputMethodService
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import app.feathercast.protocol.ScreenInput

/** A real IME preserves the application's cursor, selection and editing behavior. */
class PcKeyboardService : InputMethodService() {
    private val main = Handler(Looper.getMainLooper())
    override fun onCreate() { super.onCreate(); instance = this; LinkManager.instance.refreshStatus() }
    override fun onDestroy() {
        if (instance === this) instance = null
        LinkManager.instance.refreshStatus()
        super.onDestroy()
    }
    override fun onEvaluateFullscreenMode() = false
    override fun onCreateInputView(): View = Button(this).apply {
        text = "FeatherCast PC Keyboard · Switch keyboard"
        setOnClickListener { getSystemService(InputMethodManager::class.java).showInputMethodPicker() }
    }
    fun input(input: ScreenInput) {
        main.post {
            if (!PhoneApp.instance.store.remoteControl || !ScreenBridge.accepts(input.sessionId, input.generation) || !selected(this)) return@post
            val connection = currentInputConnection ?: return@post
            when (input.action) {
                "text" -> connection.commitText(input.text, 1)
                "key" -> when (input.value) {
                    0 -> connection.performContextMenuAction(android.R.id.selectAll)
                    KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_FORWARD_DEL -> {
                        if (!connection.getSelectedText(0).isNullOrEmpty()) connection.commitText("", 1)
                        else connection.deleteSurroundingTextInCodePoints(
                            if (input.value == KeyEvent.KEYCODE_DEL) 1 else 0,
                            if (input.value == KeyEvent.KEYCODE_FORWARD_DEL) 1 else 0)
                    }
                    KeyEvent.KEYCODE_ENTER -> {
                        val info = currentInputEditorInfo
                        val action = (info?.imeOptions ?: 0) and EditorInfo.IME_MASK_ACTION
                        if (action != EditorInfo.IME_ACTION_NONE && action != EditorInfo.IME_ACTION_UNSPECIFIED &&
                            (info.imeOptions and EditorInfo.IME_FLAG_NO_ENTER_ACTION) == 0) connection.performEditorAction(action)
                        else { connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, input.value)); connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, input.value)) }
                    }
                    else -> { connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, input.value)); connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, input.value)) }
                }
            }
        }
    }
    fun restoreKeyboard() {
        if (!selected(this)) return
        val old = getSharedPreferences("feathercast-link", MODE_PRIVATE).getString("previousKeyboard", "").orEmpty()
        if (old.isNotEmpty() && old != id(this)) {
            try { switchInputMethod(old); return } catch (_: RuntimeException) { }
        }
        getSystemService(InputMethodManager::class.java).showInputMethodPicker()
    }
    companion object {
        @Volatile var instance: PcKeyboardService? = null; private set
        fun id(context: Context) = android.content.ComponentName(context, PcKeyboardService::class.java).flattenToShortString()
        fun selected(context: Context): Boolean = Settings.Secure.getString(context.contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD) == id(context)
        fun rememberPrevious(context: Context) {
            val selected = Settings.Secure.getString(context.contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD).orEmpty()
            if (selected.isNotEmpty() && selected != id(context)) context.getSharedPreferences("feathercast-link", Context.MODE_PRIVATE)
                .edit().putString("previousKeyboard", selected).apply()
        }
    }
}
