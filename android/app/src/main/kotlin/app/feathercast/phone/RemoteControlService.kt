package app.feathercast.phone

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.os.Handler
import android.os.Looper
import android.view.accessibility.AccessibilityEvent
import app.feathercast.protocol.ScreenInput
import app.feathercast.protocol.SCREEN_COORDINATE_SCALE

/** Touch gestures only: no screen-content inspection or permission automation. */
class RemoteControlService : AccessibilityService() {
    private val main = Handler(Looper.getMainLooper())
    private var stroke: GestureDescription.StrokeDescription? = null
    private var busy = false
    private var held = false
    private var x = 0f
    private var y = 0f
    private var latest: ScreenInput? = null
    private var revision = 0

    override fun onServiceConnected() {
        instance = this
        LinkManager.instance.refreshStatus()
    }
    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit
    override fun onInterrupt() = cancelGesture()
    override fun onDestroy() {
        cancelGesture()
        if (instance === this) instance = null
        LinkManager.instance.refreshStatus()
        super.onDestroy()
    }

    fun input(input: ScreenInput) {
        main.post {
            if (!PhoneApp.instance.store.remoteControl || !ScreenBridge.accepts(input.sessionId, input.generation)) return@post
            when (input.action) {
                "back" -> { cancelGesture(); performGlobalAction(GLOBAL_ACTION_BACK) }
                "home" -> { cancelGesture(); performGlobalAction(GLOBAL_ACTION_HOME) }
                "recents" -> { cancelGesture(); performGlobalAction(GLOBAL_ACTION_RECENTS) }
                "cancel" -> cancelGesture()
                "down", "move", "up" -> {
                    if (input.action == "down") { cancelGesture(); held = true }
                    latest = input
                    pump()
                }
                "scroll" -> {
                    if (busy || held) return@post
                    val width = ScreenBridge.displayWidth.toFloat()
                    val height = ScreenBridge.displayHeight.toFloat()
                    val sx = (input.x.toFloat() / SCREEN_COORDINATE_SCALE * width).coerceIn(1f, width - 1f)
                    val sy = (input.y.toFloat() / SCREEN_COORDINATE_SCALE * height).coerceIn(height * .25f, height * .75f)
                    val end = (sy + input.value.coerceIn(-3, 3) * height * .12f).coerceIn(1f, height - 1f)
                    val path = Path().apply { moveTo(sx, sy); lineTo(sx, end) }
                    dispatchGesture(GestureDescription.Builder().addStroke(GestureDescription.StrokeDescription(path, 0, 180)).build(), null, main)
                }
            }
        }
    }

    private fun pump() {
        if (busy || !held) return
        val input = latest
        if (input != null && !ScreenBridge.accepts(input.sessionId, input.generation)) { cancelGesture(); return }
        latest = null
        val nx = input?.let { (it.x.toFloat() / SCREEN_COORDINATE_SCALE * ScreenBridge.displayWidth).coerceIn(0f, (ScreenBridge.displayWidth - 1).toFloat()) } ?: x
        val ny = input?.let { (it.y.toFloat() / SCREEN_COORDINATE_SCALE * ScreenBridge.displayHeight).coerceIn(0f, (ScreenBridge.displayHeight - 1).toFloat()) } ?: y
        val release = input?.action == "up"
        val path = Path().apply {
            moveTo(if (stroke == null) nx else x, if (stroke == null) ny else y)
            if (nx != x || ny != y) lineTo(nx, ny)
        }
        val next = stroke?.continueStroke(path, 0, 32, !release)
            ?: GestureDescription.StrokeDescription(path, 0, 32, !release)
        stroke = next; x = nx; y = ny; busy = true
        val current = revision
        val ok = dispatchGesture(GestureDescription.Builder().addStroke(next).build(), object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                if (current != revision) return
                busy = false
                if (release) { stroke = null; held = false } else main.post { pump() }
            }
            override fun onCancelled(gestureDescription: GestureDescription?) {
                if (current != revision) return
                stroke = null; busy = false; held = false; latest = null
            }
        }, main)
        if (!ok) { stroke = null; busy = false; held = false; latest = null }
    }

    fun cancelGesture() {
        revision++
        val previous = stroke
        stroke = null; latest = null; busy = false; held = false
        if (previous != null) {
            // End the continuation at its last position so no pointer remains down.
            val path = Path().apply { moveTo(x, y) }
            try {
                dispatchGesture(GestureDescription.Builder().addStroke(previous.continueStroke(path, 0, 1, false)).build(), null, main)
            } catch (_: IllegalArgumentException) { /* The system already cancelled the gesture. */ }
        }
    }

    companion object { @Volatile var instance: RemoteControlService? = null; private set }
}
