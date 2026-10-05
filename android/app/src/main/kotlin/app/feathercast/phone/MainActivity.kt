package app.feathercast.phone

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.view.inputmethod.InputMethodManager
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import app.feathercast.phone.ui.ConnectScreen
import app.feathercast.phone.ui.Feature
import app.feathercast.phone.ui.FeatherTheme
import app.feathercast.phone.ui.HomeActions
import app.feathercast.phone.ui.HomeScreen
import app.feathercast.phone.ui.PairConfirmDialog
import app.feathercast.phone.ui.Permissions
import app.feathercast.phone.ui.Switches
import app.feathercast.protocol.PairingInvite
import app.feathercast.protocol.PhoneMessages
import com.google.zxing.client.android.Intents
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val link get() = LinkManager.instance
    private val store get() = PhoneApp.instance.store

    private var pairing by mutableStateOf(false)
    private var pairError by mutableStateOf<String?>(null)
    private var resumeTick by mutableIntStateOf(0)
    private var toggles by mutableIntStateOf(0)
    private var approvingScreenId = ""
    // A pairing link opened from outside the app waits here until the user confirms it.
    private var pendingPair by mutableStateOf<Pair<String, PairingInvite>?>(null)

    private val screenConsent = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val id = approvingScreenId
        approvingScreenId = ""
        val consent = result.data
        if (result.resultCode == RESULT_OK && consent != null && ScreenBridge.pendingRequest()?.sessionId == id) {
            try { ScreenCaptureService.start(this, id, result.resultCode, consent) }
            catch (_: RuntimeException) { ScreenBridge.stop(this, "Android could not start screen sharing. Start a new request on your PC.", id) }
        } else {
            ScreenBridge.stop(this, "Screen sharing was declined or the request expired.", id)
        }
    }

    private val screenAudioPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        launchScreenConsent()
    }

    private val scanner = registerForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents != null) pairWith(contents)
    }

    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        resumeTick++
    }

    private var pairAfterNetworkPermission: String? = null
    private val networkPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        resumeTick++
        val pending = pairAfterNetworkPermission
        pairAfterNetworkPermission = null
        if (granted) {
            if (pending != null) pairWith(pending) else LinkService.start(this)
        } else pairError = "Allow local network access in Android settings to connect to your PC."
    }

    private val photoPermission = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        resumeTick++
        link.sharePhotosNow()
    }

    private val featurePermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        resumeTick++
        link.refreshStatus()
    }

    private val pickFiles = registerForActivityResult(ActivityResultContracts.GetMultipleContents()) { uris ->
        if (uris.isNotEmpty()) sendFiles(uris)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        approvingScreenId = savedInstanceState?.getString("approvingScreenId").orEmpty()
        savedInstanceState?.getString("pendingPair")?.let { confirmPairing(it) }
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }
        if (store.load() != null) LinkService.start(this)
        handleIntent(intent)
        setContent {
            val state by link.state.collectAsStateWithLifecycle()
            val screenState by ScreenBridge.state.collectAsStateWithLifecycle()
            val tick = resumeTick + toggles
            FeatherTheme {
                if (state.status == LinkStatus.Unpaired) {
                    ConnectScreen(
                        busy = pairing,
                        error = pairError ?: state.detail.ifEmpty { null },
                        onScan = ::startScan,
                    )
                } else {
                    HomeScreen(
                        state = state,
                        permissions = currentPermissions(tick),
                        switches = currentSwitches(tick),
                        actions = homeActions,
                        screen = screenState,
                    )
                }
                pendingPair?.let { (uri, invite) ->
                    PairConfirmDialog(
                        pcName = invite.pcName,
                        hosts = invite.hosts,
                        replaces = state.pcName.ifEmpty { "your PC" }.takeIf { state.status != LinkStatus.Unpaired },
                        onConfirm = {
                            pendingPair = null
                            pairWith(uri)
                        },
                        onDismiss = { pendingPair = null },
                    )
                }
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString("approvingScreenId", approvingScreenId)
        outState.putString("pendingPair", pendingPair?.first)
        super.onSaveInstanceState(outState)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        resumeTick++
        if (store.load() != null) {
            LinkService.start(this)
            link.nudge()
            link.requestPcClipboard()
            // Permissions may have changed in the system settings.
            link.refreshStatus()
            link.sharePhotosNow()
        }
    }

    private fun handleIntent(intent: Intent?) {
        val data = intent?.data ?: return
        if (data.scheme == "feathercast") {
            intent.data = null
            confirmPairing(data.toString())
        }
    }

    /** Links can come from any web page, so they never pair without the user's confirmation. */
    private fun confirmPairing(uri: String) {
        val invite = PairingInvite.parse(uri)
        if (invite == null) {
            pairError = "This is not a valid FeatherCast pairing code."
            return
        }
        pendingPair = uri to invite
    }

    private fun startScan() {
        pairError = null
        scanner.launch(
            ScanOptions()
                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                .setPrompt("Point the camera at the pairing code on your PC")
                .setBeepEnabled(false)
                .setOrientationLocked(false)
                .addExtra(Intents.Scan.SCAN_TYPE, Intents.Scan.MIXED_SCAN),
        )
    }

    private fun pairWith(uri: String) {
        if (pairing) return
        if (!LocalNetworkAccess.allowed(this)) {
            pairAfterNetworkPermission = uri
            networkPermission.launch(LocalNetworkAccess.PERMISSION)
            return
        }
        if (!uri.startsWith("feathercast://pair")) {
            pairError = if (uri.contains("/app.apk") || uri.contains(".apk")) {
                "That is the download code. Scan the pairing code shown next to it on your PC."
            } else {
                "This is not a FeatherCast pairing code."
            }
            return
        }
        pairing = true
        pairError = null
        lifecycleScope.launch {
            link.pair(uri)
                .onSuccess {
                    SendClipboardActivity.toast(this@MainActivity, "Connected to $it")
                    requestNotificationPermission()
                }
                .onFailure { pairError = it.message }
            pairing = false
        }
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33 && !currentPermissions(0).postNotifications) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun currentPermissions(@Suppress("UNUSED_PARAMETER") tick: Int) = Permissions(
        notificationAccess = NotifyListener.hasAccess(this),
        photos = PhotoSource.access(this),
        postNotifications = Build.VERSION.SDK_INT < 33 ||
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == android.content.pm.PackageManager.PERMISSION_GRANTED,
        saveFiles = !IncomingFiles.needsPermission(this),
        sms = SmsBridge.hasPermission(this),
        calls = CallWatcher.hasPermission(this),
        storage = StorageBridge.hasAccess(this),
        remoteControl = RemoteControlService.instance != null,
        pcKeyboard = PcKeyboardService.selected(this),
        deviceAudio = Build.VERSION.SDK_INT >= 29,
        localNetwork = LocalNetworkAccess.allowed(this),
    )

    private fun currentSwitches(@Suppress("UNUSED_PARAMETER") tick: Int) = Switches(
        notifications = store.sendNotifications,
        photos = store.sendPhotos,
        clipboard = store.clipboardSync,
        receiveFiles = store.receiveFiles,
        ring = store.allowRing,
        media = store.mediaControl,
        sms = store.smsAccess,
        calls = store.callAlerts,
        storage = store.storageAccess,
        screen = store.screenSharing,
        remoteControl = store.remoteControl,
    )

    private fun launchScreenConsent() {
        val request = ScreenBridge.pendingRequest()
        if (request == null || request.sessionId != approvingScreenId) {
            approvingScreenId = ""
            return
        }
        val manager = getSystemService(MediaProjectionManager::class.java)
        val capture = if (Build.VERSION.SDK_INT >= 34) manager.createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())
            else manager.createScreenCaptureIntent()
        screenConsent.launch(capture)
    }

    private fun sendFiles(uris: List<Uri>) {
        lifecycleScope.launch {
            var sent = 0
            var error: String? = null
            for (uri in uris) link.sendUri(uri).onSuccess { sent++ }.onFailure { error = it.message }
            val pc = link.state.value.pcName.ifEmpty { "your PC" }
            SendClipboardActivity.toast(
                this@MainActivity,
                when {
                    sent == uris.size -> if (sent == 1) "Sent to $pc (Downloads\\FeatherCast)" else "$sent files sent to $pc"
                    else -> error ?: "Could not send to $pc"
                },
            )
        }
    }

    private val homeActions = object : HomeActions {
        override fun setFeature(feature: Feature, enabled: Boolean) {
            when (feature) {
                Feature.Notifications -> {
                    store.sendNotifications = enabled
                    if (enabled) NotifyListener.sendSnapshot() else NotifyListener.clearOnPc()
                }
                Feature.Photos -> {
                    store.sendPhotos = enabled
                    if (enabled) link.sharePhotosNow() else link.clearPhotosOnPc()
                }
                Feature.Clipboard -> store.clipboardSync = enabled
                Feature.ReceiveFiles -> store.receiveFiles = enabled
                Feature.Ring -> {
                    store.allowRing = enabled
                    if (!enabled) Ringer.stop(this@MainActivity)
                }
                Feature.Media -> {
                    store.mediaControl = enabled
                    if (!enabled) {
                        MediaWatcher.stop()
                        link.sendAsync(PhoneMessages.mediaNone())
                    }
                }
                Feature.Sms -> {
                    store.smsAccess = enabled
                    if (enabled && !SmsBridge.hasPermission(this@MainActivity)) requestSms()
                }
                Feature.Calls -> {
                    store.callAlerts = enabled
                    if (enabled && !CallWatcher.hasPermission(this@MainActivity)) requestCalls()
                }
                Feature.Storage -> {
                    store.storageAccess = enabled
                    if (enabled && !StorageBridge.hasAccess(this@MainActivity)) openStorageAccess()
                }
                Feature.Screen -> {
                    store.screenSharing = enabled
                    if (!enabled) ScreenBridge.stop(this@MainActivity)
                }
                Feature.RemoteControl -> {
                    store.remoteControl = enabled
                    if (!enabled) RemoteControlService.instance?.cancelGesture()
                }
            }
            toggles++
            link.refreshStatus()
        }

        override fun requestSaveFiles() {
            featurePermissions.launch(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE))
        }

        override fun openRemoteControl() {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }

        override fun enablePcKeyboard() {
            PcKeyboardService.rememberPrevious(this@MainActivity)
            startActivity(Intent(Settings.ACTION_INPUT_METHOD_SETTINGS))
        }

        override fun selectPcKeyboard() {
            PcKeyboardService.rememberPrevious(this@MainActivity)
            getSystemService(InputMethodManager::class.java).showInputMethodPicker()
        }

        override fun approveScreen() {
            if (approvingScreenId.isNotEmpty()) return
            val request = ScreenBridge.pendingRequest() ?: return
            approvingScreenId = request.sessionId
            if (request.audio && Build.VERSION.SDK_INT >= 29 && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                screenAudioPermission.launch(Manifest.permission.RECORD_AUDIO)
            } else launchScreenConsent()
        }

        override fun stopScreen() { ScreenBridge.stop(this@MainActivity) }

        override fun requestSms() {
            featurePermissions.launch(SmsBridge.permissions)
        }

        override fun requestCalls() {
            featurePermissions.launch(CallWatcher.permissions)
        }

        override fun openStorageAccess() {
            if (Build.VERSION.SDK_INT >= 30) {
                try {
                    startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName")))
                } catch (_: android.content.ActivityNotFoundException) {
                    startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                }
            } else {
                featurePermissions.launch(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE))
            }
        }

        override fun openNotificationAccess() {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }

        override fun requestPhotos() {
            photoPermission.launch(PhotoSource.permissions())
        }
        override fun cancelTransfers() { link.cancelTransfers() }
        override fun requestLocalNetwork() { networkPermission.launch(LocalNetworkAccess.PERMISSION) }

        override fun requestPostNotifications() {
            if (Build.VERSION.SDK_INT >= 33) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        override fun sendClipboard() {
            SendClipboardActivity.sendPhoneClipboard(this@MainActivity)
        }

        override fun sendFiles() {
            pickFiles.launch("*/*")
        }

        override fun copyPcClip(text: String) {
            link.copyToPhone(text)
            if (Build.VERSION.SDK_INT < 33) SendClipboardActivity.toast(this@MainActivity, "Copied")
        }

        override fun reconnect() {
            LinkService.start(this@MainActivity)
            link.nudge()
        }

        override fun unpair() {
            link.unpair()
        }
    }
}
