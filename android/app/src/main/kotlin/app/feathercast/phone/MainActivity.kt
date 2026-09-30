package app.feathercast.phone

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
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
import app.feathercast.phone.ui.Permissions
import app.feathercast.phone.ui.Switches
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

    private val scanner = registerForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents != null) pairWith(contents)
    }

    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        resumeTick++
    }

    private val photoPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        resumeTick++
        if (granted) link.sharePhotosNow()
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
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = false
            isAppearanceLightNavigationBars = false
        }
        if (store.load() != null) LinkService.start(this)
        handleIntent(intent)
        setContent {
            val state by link.state.collectAsStateWithLifecycle()
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
                    )
                }
            }
        }
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
        }
    }

    private fun handleIntent(intent: Intent?) {
        val data = intent?.data ?: return
        if (data.scheme == "feathercast") {
            intent.data = null
            pairWith(data.toString())
        }
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
        photos = PhotoSource.hasPermission(this),
        postNotifications = Build.VERSION.SDK_INT < 33 ||
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == android.content.pm.PackageManager.PERMISSION_GRANTED,
        saveFiles = !IncomingFiles.needsPermission(this),
        sms = SmsBridge.hasPermission(this),
        calls = CallWatcher.hasPermission(this),
        storage = StorageBridge.hasAccess(this),
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
    )

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
            }
            toggles++
            link.refreshStatus()
        }

        override fun requestSaveFiles() {
            featurePermissions.launch(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE))
        }

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
            photoPermission.launch(PhotoSource.permission)
        }

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
