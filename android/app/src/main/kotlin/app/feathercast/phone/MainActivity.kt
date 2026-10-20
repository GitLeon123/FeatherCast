package app.feathercast.phone

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.view.inputmethod.InputMethodManager
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
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
import app.feathercast.phone.ui.PermissionTopic
import app.feathercast.phone.ui.Permissions
import app.feathercast.phone.ui.SharingChoices
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
            catch (_: RuntimeException) { ScreenBridge.stop(this, getString(R.string.screen_start_failed), id) }
        } else {
            ScreenBridge.stop(this, getString(R.string.screen_declined), id)
        }
    }

    private val screenAudioPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        launchScreenConsent()
    }

    private val scanner = registerForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents != null) pairWith(contents)
    }

    private val cameraPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        noteResult(mapOf(Manifest.permission.CAMERA to granted))
        resumeTick++
        if (granted) launchScanner()
        // A permanent denial is explained, with a button to the settings, on the connect screen.
        else pairError = if (isBlocked(Manifest.permission.CAMERA)) null else getString(R.string.camera_denied)
    }

    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (Build.VERSION.SDK_INT >= 33) noteResult(mapOf(Manifest.permission.POST_NOTIFICATIONS to granted))
        resumeTick++
    }

    private var pairAfterNetworkPermission: String? = null
    private val networkPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        noteResult(mapOf(LocalNetworkAccess.PERMISSION to granted))
        resumeTick++
        val pending = pairAfterNetworkPermission
        pairAfterNetworkPermission = null
        if (granted) {
            if (pending != null) pairWith(pending) else LinkService.start(this)
        } else pairError = getString(R.string.network_denied)
    }

    private val photoPermission = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        noteResult(it)
        resumeTick++
        link.sharePhotosNow()
    }

    private val featurePermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        noteResult(it)
        resumeTick++
        link.refreshStatus()
    }

    private val pickFiles = registerForActivityResult(ActivityResultContracts.GetMultipleContents()) { uris ->
        if (uris.isNotEmpty()) sendFiles(uris)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // The system bar icons follow the light or dark theme.
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        approvingScreenId = savedInstanceState?.getString("approvingScreenId").orEmpty()
        savedInstanceState?.getString("pendingPair")?.let { confirmPairing(it) }
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
                        cameraBlocked = isBlocked(Manifest.permission.CAMERA, tick),
                        onScan = ::startScan,
                        onPasteLink = ::confirmPairing,
                        onOpenSettings = ::openAppSettings,
                    )
                } else {
                    HomeScreen(
                        state = state,
                        permissions = currentPermissions(tick),
                        switches = currentSwitches(tick),
                        actions = homeActions,
                        screen = screenState,
                        chooseSharing = tick >= 0 && !store.sharingChosen,
                    )
                }
                pendingPair?.let { (uri, invite) ->
                    PairConfirmDialog(
                        pcName = invite.pcName,
                        hosts = invite.hosts,
                        replaces = state.pcName.ifEmpty { getString(R.string.your_pc) }.takeIf { state.status != LinkStatus.Unpaired },
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

    /**
     * Links can come from any web page, so they never pair without the user's confirmation.
     * A pasted link takes the same path as one opened from the camera app or a browser.
     */
    private fun confirmPairing(uri: String) {
        pairError = null
        val invite = PairingInvite.parse(uri)
        if (invite == null) {
            pairError = getString(R.string.pair_invalid)
            return
        }
        pendingPair = uri to invite
    }

    // ---- Permission bookkeeping -------------------------------------------------

    private fun isGranted(permission: String) = checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    /**
     * True when Android will no longer show a dialog for a permission that is still missing,
     * so only the app's settings can grant it. [tick] only makes Compose re-read this.
     */
    private fun isBlocked(permission: String, @Suppress("UNUSED_PARAMETER") tick: Int = 0) =
        store.isPermissionBlocked(permission) && !isGranted(permission)

    private fun anyBlocked(permissions: Array<String>) = permissions.any { isBlocked(it) }

    /** Remembers which denials were final: Android stops offering a rationale after them. */
    private fun noteResult(result: Map<String, Boolean>) {
        for ((permission, granted) in result) {
            store.setPermissionBlocked(permission, !granted && !shouldShowRequestPermissionRationale(permission))
        }
    }

    /** Asks for [permissions], or opens the app's settings when a required one can no longer be asked for. */
    private fun requestOrSettings(permissions: Array<String>, required: Array<String> = permissions, request: (Array<String>) -> Unit) {
        if (anyBlocked(required.filter { !isGranted(it) }.toTypedArray())) openAppSettings() else request(permissions)
    }

    private fun openAppSettings() {
        startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
    }

    // ---- Pairing ----------------------------------------------------------------

    private fun startScan() {
        pairError = null
        when {
            isGranted(Manifest.permission.CAMERA) -> launchScanner()
            isBlocked(Manifest.permission.CAMERA) -> openAppSettings()
            else -> cameraPermission.launch(Manifest.permission.CAMERA)
        }
    }

    private fun launchScanner() {
        scanner.launch(
            ScanOptions()
                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                .setPrompt(getString(R.string.scan_prompt))
                .setBeepEnabled(false)
                .setOrientationLocked(false)
                .addExtra(Intents.Scan.SCAN_TYPE, Intents.Scan.MIXED_SCAN),
        )
    }

    private fun pairWith(uri: String) {
        if (pairing) return
        if (!LocalNetworkAccess.allowed(this)) {
            pairAfterNetworkPermission = uri
            if (isBlocked(LocalNetworkAccess.PERMISSION)) {
                pairError = getString(R.string.network_denied)
                openAppSettings()
            } else networkPermission.launch(LocalNetworkAccess.PERMISSION)
            return
        }
        if (!uri.startsWith("feathercast://pair")) {
            pairError = if (uri.contains("/app.apk") || uri.contains(".apk")) {
                getString(R.string.pair_download_code)
            } else {
                getString(R.string.pair_not_code)
            }
            return
        }
        pairing = true
        pairError = null
        lifecycleScope.launch {
            link.pair(uri)
                .onSuccess {
                    SendClipboardActivity.toast(this@MainActivity, getString(R.string.toast_connected, it))
                    // The "Choose what to share" step appears first; permissions follow from it.
                    toggles++
                }
                .onFailure { pairError = it.message }
            pairing = false
        }
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33 && !currentPermissions(0).postNotifications && !isBlocked(Manifest.permission.POST_NOTIFICATIONS)) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun currentPermissions(@Suppress("UNUSED_PARAMETER") tick: Int): Permissions {
        val photos = PhotoSource.access(this)
        val postNotifications = Build.VERSION.SDK_INT < 33 || isGranted(Manifest.permission.POST_NOTIFICATIONS)
        val saveFiles = !IncomingFiles.needsPermission(this)
        val sms = SmsBridge.hasPermission(this)
        val calls = CallWatcher.hasPermission(this)
        val storage = StorageBridge.hasAccess(this)
        val localNetwork = LocalNetworkAccess.allowed(this)
        val blocked = buildSet {
            if (photos == PhotoAccess.Denied && anyBlocked(PhotoSource.permissions())) add(PermissionTopic.Photos)
            if (!postNotifications && isBlocked(Manifest.permission.POST_NOTIFICATIONS)) add(PermissionTopic.PostNotifications)
            if (!saveFiles && isBlocked(Manifest.permission.WRITE_EXTERNAL_STORAGE)) add(PermissionTopic.SaveFiles)
            if (!sms && anyBlocked(SMS_REQUIRED)) add(PermissionTopic.Sms)
            if (!calls && isBlocked(Manifest.permission.READ_PHONE_STATE)) add(PermissionTopic.Calls)
            if (!storage && Build.VERSION.SDK_INT < 30 && isBlocked(Manifest.permission.READ_EXTERNAL_STORAGE)) add(PermissionTopic.Storage)
            if (!localNetwork && isBlocked(LocalNetworkAccess.PERMISSION)) add(PermissionTopic.LocalNetwork)
        }
        return Permissions(
            notificationAccess = NotifyListener.hasAccess(this),
            photos = photos,
            postNotifications = postNotifications,
            saveFiles = saveFiles,
            sms = sms,
            calls = calls,
            storage = storage,
            remoteControl = RemoteControlService.instance != null,
            pcKeyboard = PcKeyboardService.selected(this),
            deviceAudio = Build.VERSION.SDK_INT >= 29,
            localNetwork = localNetwork,
            blocked = blocked,
            ignoringBattery = getSystemService(PowerManager::class.java)?.isIgnoringBatteryOptimizations(packageName) ?: true,
        )
    }

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
        // Not tied to this activity, so leaving the app does not cancel the upload.
        link.sendUrisInBackground(uris) { sent, error ->
            val pc = link.state.value.pcName.ifEmpty { getString(R.string.your_pc) }
            when {
                sent == uris.size -> resources.getQuantityString(R.plurals.picked_sent, sent, sent, pc)
                else -> error ?: getString(R.string.send_failed, pc)
            }
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
                // The permission prompts for these start from the consent dialog on the home screen.
                Feature.Sms -> store.smsAccess = enabled
                Feature.Calls -> store.callAlerts = enabled
                Feature.Storage -> store.storageAccess = enabled
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

        override fun finishSharingChoice(choices: SharingChoices?) {
            if (choices != null) {
                setFeature(Feature.Notifications, choices.notifications)
                setFeature(Feature.Photos, choices.photos)
                setFeature(Feature.Clipboard, choices.clipboard)
                setFeature(Feature.ReceiveFiles, choices.receiveFiles)
                setFeature(Feature.Ring, choices.ring)
                setFeature(Feature.Media, choices.media)
            }
            store.sharingChosen = true
            toggles++
            requestNotificationPermission()
        }

        override fun requestSaveFiles() {
            requestOrSettings(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE)) { featurePermissions.launch(it) }
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
            if (request.audio && Build.VERSION.SDK_INT >= 29 && !isGranted(Manifest.permission.RECORD_AUDIO)) {
                screenAudioPermission.launch(Manifest.permission.RECORD_AUDIO)
            } else launchScreenConsent()
        }

        override fun stopScreen() { ScreenBridge.stop(this@MainActivity) }

        override fun requestSms() {
            requestOrSettings(SmsBridge.permissions, SMS_REQUIRED) { featurePermissions.launch(it) }
        }

        override fun requestCalls() {
            requestOrSettings(CallWatcher.permissions, arrayOf(Manifest.permission.READ_PHONE_STATE)) { featurePermissions.launch(it) }
        }

        override fun openStorageAccess() {
            if (Build.VERSION.SDK_INT >= 30) {
                try {
                    startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:$packageName")))
                } catch (_: ActivityNotFoundException) {
                    startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                }
            } else {
                requestOrSettings(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)) { featurePermissions.launch(it) }
            }
        }

        override fun openNotificationAccess() {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }

        override fun requestPhotos() {
            val permissions = PhotoSource.permissions()
            // "Choose photos" (partial access) asks again; only a missing permission can be blocked.
            if (PhotoSource.access(this@MainActivity) == PhotoAccess.Denied && anyBlocked(permissions)) openAppSettings()
            else photoPermission.launch(permissions)
        }
        override fun cancelTransfers() { link.cancelTransfers() }
        override fun requestLocalNetwork() {
            if (isBlocked(LocalNetworkAccess.PERMISSION)) openAppSettings() else networkPermission.launch(LocalNetworkAccess.PERMISSION)
        }

        override fun requestPostNotifications() {
            if (Build.VERSION.SDK_INT < 33) return
            if (isBlocked(Manifest.permission.POST_NOTIFICATIONS)) openAppSettings()
            else notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        override fun openAppSettings() { this@MainActivity.openAppSettings() }

        override fun openBatterySettings() {
            try {
                startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
            } catch (_: ActivityNotFoundException) {
                this@MainActivity.openAppSettings()
            }
        }

        override fun sendClipboard() {
            SendClipboardActivity.sendPhoneClipboard(this@MainActivity)
        }

        override fun sendFiles() {
            pickFiles.launch("*/*")
        }

        override fun copyPcClip(text: String) {
            link.copyToPhone(text)
            if (Build.VERSION.SDK_INT < 33) SendClipboardActivity.toast(this@MainActivity, getString(R.string.copied))
        }

        override fun reconnect() {
            LinkService.start(this@MainActivity)
            link.nudge()
        }

        override fun unpair() {
            link.unpair()
        }
    }

    private companion object {
        /** The SMS permissions the feature cannot work without; READ_CONTACTS only adds names. */
        val SMS_REQUIRED = arrayOf(
            Manifest.permission.READ_SMS,
            Manifest.permission.SEND_SMS,
            Manifest.permission.RECEIVE_SMS,
        )
    }
}
