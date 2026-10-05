package app.feathercast.phone.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.ContentPaste
import androidx.compose.material.icons.outlined.Call
import androidx.compose.material.icons.outlined.Computer
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.LinkOff
import androidx.compose.material.icons.outlined.MusicNote
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.NotificationsActive
import androidx.compose.material.icons.outlined.QrCodeScanner
import androidx.compose.material.icons.outlined.Sms
import androidx.compose.material.icons.outlined.Upload
import androidx.compose.material.icons.automirrored.outlined.ScreenShare
import androidx.compose.material.icons.outlined.TouchApp
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.Typography
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.feathercast.phone.LinkState
import app.feathercast.phone.LinkStatus
import app.feathercast.phone.R
import app.feathercast.phone.ScreenUiState
import android.text.format.DateUtils
import java.text.DateFormat

// Mirrors the default desktop theme in native/src/theme.hpp.
private val Accent = Color(0xFF5C6BFF)
private val Canvas = Color(0xFF101012)
private val Panel = Color(0xFF18181B)
private val Raised = Color(0xFF242428)
private val Border = Color(0xFF303035)
private val PrimaryText = Color(0xFFF2F2F5)
private val MutedText = Color(0xFFB0B0BA)
private val SectionText = Color(0xFFADB2CA)
private val Online = Color(0xFF4DC77A)
private val Waiting = Color(0xFFF5B957)
private val PanelShape = RoundedCornerShape(10.dp)

@Composable
fun FeatherTheme(content: @Composable () -> Unit) {
    val colors = darkColorScheme(
        primary = Accent,
        onPrimary = Color.White,
        primaryContainer = Color(0xFF30356B),
        onPrimaryContainer = PrimaryText,
        background = Canvas,
        onBackground = PrimaryText,
        surface = Canvas,
        onSurface = PrimaryText,
        surfaceVariant = Raised,
        onSurfaceVariant = MutedText,
        surfaceContainer = Panel,
        outline = Border,
        error = Color(0xFFFF5C5C),
    )
    val type = Typography(
        headlineSmall = androidx.compose.ui.text.TextStyle(fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.SemiBold),
        titleMedium = androidx.compose.ui.text.TextStyle(fontSize = 17.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold),
        titleSmall = androidx.compose.ui.text.TextStyle(fontSize = 14.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold),
        bodyMedium = androidx.compose.ui.text.TextStyle(fontSize = 14.sp, lineHeight = 20.sp),
        bodySmall = androidx.compose.ui.text.TextStyle(fontSize = 13.sp, lineHeight = 18.sp),
        labelLarge = androidx.compose.ui.text.TextStyle(fontSize = 13.sp, lineHeight = 18.sp, fontWeight = FontWeight.SemiBold),
    )
    MaterialTheme(colorScheme = colors, typography = type) {
        Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background, content = content)
    }
}

// ---- Connect ------------------------------------------------------------------

@Composable
fun ConnectScreen(busy: Boolean, error: String?, onScan: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.height(20.dp))
        Icon(
            painterResource(R.drawable.ic_stat_feathercast), null,
            tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(48.dp),
        )
        Spacer(Modifier.height(12.dp))
        Text("Connect to your PC", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(6.dp))
        Text(
            "See your phone's notifications, photos and clipboard in FeatherCast on your PC.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(28.dp))
        Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            shape = PanelShape,
            modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape),
        ) {
            Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Step(1, "Open FeatherCast on your PC", "Tray icon → Phone, or search for “Phone”.")
                Step(2, "Scan the pairing code", "Tap the button below and point the camera at the code.")
                Step(3, "Done", "The phone reconnects by itself whenever both are on the same Wi-Fi.")
            }
        }
        Spacer(Modifier.height(32.dp))
        if (error != null) {
            Text(
                error,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(bottom = 12.dp),
            )
        }
        Button(
            onClick = onScan,
            enabled = !busy,
            modifier = Modifier.fillMaxWidth().height(52.dp),
            shape = RoundedCornerShape(8.dp),
        ) {
            if (busy) {
                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.onPrimary)
                Spacer(Modifier.width(12.dp))
                Text("Connecting…")
            } else {
                Icon(Icons.Outlined.QrCodeScanner, null)
                Spacer(Modifier.width(10.dp))
                Text("Scan QR code", style = MaterialTheme.typography.titleMedium)
            }
        }
        Spacer(Modifier.height(16.dp))
    }
}

/** Confirms a pairing link that was opened from outside the app, such as a browser. */
@Composable
fun PairConfirmDialog(pcName: String, hosts: List<String>, replaces: String?, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Pair with $pcName?", maxLines = 2, overflow = TextOverflow.Ellipsis) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("This phone will connect to “$pcName” at ${hosts.joinToString(", ")}.", maxLines = 6, overflow = TextOverflow.Ellipsis)
                if (replaces != null) Text("This replaces the pairing with $replaces.")
                Text("Only continue if you opened the pairing code that FeatherCast shows on your own PC.")
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Pair") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun Step(number: Int, title: String, text: String) {
    Row(verticalAlignment = Alignment.Top) {
        Box(
            Modifier.size(28.dp).background(Color(0xFF30356B), RoundedCornerShape(6.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("$number", color = PrimaryText, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.width(14.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

// ---- Home ---------------------------------------------------------------------

data class Permissions(
    val notificationAccess: Boolean,
    val photos: app.feathercast.phone.PhotoAccess,
    val postNotifications: Boolean,
    val saveFiles: Boolean = true,
    val sms: Boolean = false,
    val calls: Boolean = false,
    val storage: Boolean = false,
    val remoteControl: Boolean = false,
    val pcKeyboard: Boolean = false,
    val deviceAudio: Boolean = false,
    val localNetwork: Boolean = true,
)

enum class Feature { Notifications, Photos, Clipboard, ReceiveFiles, Ring, Media, Sms, Calls, Storage, Screen, RemoteControl }

data class Switches(
    val notifications: Boolean,
    val photos: Boolean,
    val clipboard: Boolean,
    val receiveFiles: Boolean,
    val ring: Boolean,
    val media: Boolean,
    val sms: Boolean,
    val calls: Boolean,
    val storage: Boolean,
    val screen: Boolean = false,
    val remoteControl: Boolean = false,
)

interface HomeActions {
    fun setFeature(feature: Feature, enabled: Boolean)
    fun openNotificationAccess()
    fun requestPhotos()
    fun requestSaveFiles()
    fun requestSms()
    fun requestCalls()
    fun openStorageAccess()
    fun requestPostNotifications()
    fun sendClipboard()
    fun sendFiles()
    fun copyPcClip(text: String)
    fun reconnect()
    fun unpair()
    fun openRemoteControl()
    fun enablePcKeyboard()
    fun selectPcKeyboard()
    fun approveScreen()
    fun stopScreen()
    fun cancelTransfers()
    fun requestLocalNetwork()
}

@Composable
fun HomeScreen(
    state: LinkState,
    permissions: Permissions,
    switches: Switches,
    actions: HomeActions,
    screen: ScreenUiState = ScreenUiState(),
) {
    var confirmUnpair by remember { mutableStateOf(false) }
    LazyColumn(
        modifier = Modifier.fillMaxSize().safeDrawingPadding(),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        if (state.transferStatus.isNotEmpty()) {
            item {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(state.transferStatus, modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodyMedium)
                    if (state.transferActive) TextButton(onClick = actions::cancelTransfers) { Text("Cancel") }
                }
            }
        }
        item {
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 2.dp, bottom = 4.dp)) {
                Icon(painterResource(R.drawable.ic_stat_feathercast), null, tint = Accent, modifier = Modifier.size(24.dp))
                Spacer(Modifier.width(10.dp))
                Text("FeatherCast Phone", style = MaterialTheme.typography.titleMedium)
            }
        }
        item { StatusCard(state, actions) }
        if (!permissions.localNetwork) item {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
                Column(Modifier.padding(16.dp)) {
                    Text("Local network access is needed to connect to your PC.")
                    TextButton(onClick = actions::requestLocalNetwork) { Text("Allow local network access") }
                }
            }
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
                QuickAction(Icons.Outlined.ContentPaste, "Send clipboard", Modifier.weight(1f), actions::sendClipboard)
                QuickAction(Icons.Outlined.Upload, "Send photo or file", Modifier.weight(1f), actions::sendFiles)
            }
        }
        item { SectionTitle("Shared with your PC") }
        item {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer), shape = PanelShape,
                modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape)) {
                Column {
                    FeatureRow(
                        Icons.Outlined.Notifications, "Notifications",
                        "Messages and alerts appear on the PC, and you can reply from there.",
                        checked = switches.notifications, onChecked = { actions.setFeature(Feature.Notifications, it) },
                        missing = if (permissions.notificationAccess) null else "Allow notification access",
                        onFix = actions::openNotificationAccess,
                    )
                    FeatureRow(
                        Icons.Outlined.Image, "Photos",
                        if (permissions.photos == app.feathercast.phone.PhotoAccess.Selected)
                            "Only your selected photos are shared. You can change the selection."
                        else "Your newest photos can be opened on the PC.",
                        checked = switches.photos, onChecked = { actions.setFeature(Feature.Photos, it) },
                        missing = when (permissions.photos) {
                            app.feathercast.phone.PhotoAccess.Full -> null
                            app.feathercast.phone.PhotoAccess.Selected -> "Choose photos"
                            app.feathercast.phone.PhotoAccess.Denied -> "Allow photo access"
                        },
                        onFix = actions::requestPhotos,
                    )
                    FeatureRow(
                        Icons.Outlined.ContentCopy, "Clipboard sync",
                        "Text copied on the PC is copied on the phone. Use “Send clipboard” for the other way.",
                        checked = switches.clipboard, onChecked = { actions.setFeature(Feature.Clipboard, it) },
                        missing = null, onFix = {},
                    )
                    FeatureRow(
                        Icons.Outlined.Download, "Files from the PC",
                        "Files sent from the PC are saved to Download/FeatherCast.",
                        checked = switches.receiveFiles, onChecked = { actions.setFeature(Feature.ReceiveFiles, it) },
                        missing = if (permissions.saveFiles) null else "Allow saving files",
                        onFix = actions::requestSaveFiles,
                    )
                    FeatureRow(
                        Icons.Outlined.NotificationsActive, "Find my phone",
                        "The PC can make this phone ring loudly, even when it is on silent.",
                        checked = switches.ring, onChecked = { actions.setFeature(Feature.Ring, it) },
                        missing = null, onFix = {},
                    )
                    FeatureRow(
                        Icons.Outlined.MusicNote, "Media controls",
                        "See what is playing and control it from the PC.",
                        checked = switches.media, onChecked = { actions.setFeature(Feature.Media, it) },
                        missing = if (permissions.notificationAccess) null else "Allow notification access",
                        onFix = actions::openNotificationAccess,
                    )
                }
            }
        }
        item { SectionTitle("Phone screen") }
        item {
            Column(Modifier.fillMaxWidth()) {
                FeatureRow(
                    Icons.AutoMirrored.Outlined.ScreenShare, "Screen sharing",
                    "Open Phone Screen on your PC. You approve each session here before your screen is shared.",
                    checked = switches.screen, onChecked = { actions.setFeature(Feature.Screen, it) },
                    missing = null, onFix = {},
                )
                FeatureRow(
                    Icons.Outlined.TouchApp, "Remote control",
                    "Allow your paired PC to tap, swipe and navigate during an approved screen sharing session.",
                    checked = switches.remoteControl, onChecked = { actions.setFeature(Feature.RemoteControl, it) },
                    missing = if (permissions.remoteControl) null else "Enable FeatherCast Remote Control",
                    onFix = actions::openRemoteControl,
                )
                Column(Modifier.padding(horizontal = 16.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("PC keyboard", style = MaterialTheme.typography.titleSmall)
                    Text(if (permissions.pcKeyboard) "FeatherCast PC Keyboard is selected. Type directly in phone apps from your PC."
                        else "Enable FeatherCast PC Keyboard in Android settings, then select it for direct PC typing.",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(onClick = actions::enablePcKeyboard) { Text("Enable keyboard") }
                        TextButton(onClick = actions::selectPcKeyboard) { Text("Select keyboard") }
                    }
                    Text(if (permissions.deviceAudio) "Device audio is requested when you share. Some apps do not allow audio capture."
                        else "This Android version supports screen sharing without device audio.",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (switches.remoteControl && !permissions.remoteControl) {
                        Text("If Android blocks the accessibility setting after installing the APK, open FeatherCast's App info and allow restricted settings first.",
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    if (screen.detail.isNotEmpty()) Text(screen.detail, style = MaterialTheme.typography.bodyMedium)
                    if (screen.pending) {
                        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Button(onClick = actions::approveScreen) { Text("Share screen") }
                            TextButton(onClick = actions::stopScreen) { Text("Decline") }
                        }
                    }
                    if (screen.active) OutlinedButton(onClick = actions::stopScreen) { Text("Stop sharing") }
                }
            }
        }
        item { SectionTitle("Optional") }
        item {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer), shape = PanelShape,
                modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape)) {
                Column {
                    FeatureRow(
                        Icons.Outlined.Sms, "Text messages",
                        "Read and send SMS from the PC.",
                        checked = switches.sms, onChecked = { actions.setFeature(Feature.Sms, it) },
                        missing = if (permissions.sms) null else "Allow SMS access",
                        onFix = actions::requestSms,
                    )
                    FeatureRow(
                        Icons.Outlined.Call, "Calls",
                        "Incoming calls show on the PC, where you can reject or silence them.",
                        checked = switches.calls, onChecked = { actions.setFeature(Feature.Calls, it) },
                        missing = if (permissions.calls) null else "Allow phone access",
                        onFix = actions::requestCalls,
                    )
                    FeatureRow(
                        Icons.Outlined.Folder, "Phone storage",
                        "Browse the phone's files from the PC and download them.",
                        checked = switches.storage, onChecked = { actions.setFeature(Feature.Storage, it) },
                        missing = if (permissions.storage) null else "Allow all files access",
                        onFix = actions::openStorageAccess,
                    )
                }
            }
        }
        if (!permissions.postNotifications) {
            item {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer), shape = PanelShape,
                    modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape)) {
                    Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "Allow notifications to see connection status in the notification drawer. Connecting does not require this permission.",
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = actions::requestPostNotifications) { Text("Allow") }
                    }
                }
            }
        }
        item { SectionTitle("PC clipboard") }
        if (state.pcClipboard.isEmpty()) {
            item {
                Text(
                    if (state.status == LinkStatus.Connected) {
                        "Nothing yet. Text you copy on the PC shows up here."
                    } else {
                        "Shows the PC clipboard once connected."
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp),
                )
            }
        } else {
            items(state.pcClipboard, key = { it.time.toString() + it.text.hashCode() }) { clip ->
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
                    shape = PanelShape,
                    modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape).clickable { actions.copyPcClip(clip.text) },
                ) {
                    Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(clip.text, maxLines = 3, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
                            if (clip.time > 0) {
                                Text(
                                    DateUtils.formatSameDayTime(clip.time, System.currentTimeMillis(), DateFormat.MEDIUM, DateFormat.SHORT).toString(),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                        Icon(Icons.Outlined.ContentCopy, "Copy", tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
                    }
                }
            }
        }
        item {
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = { confirmUnpair = true }, modifier = Modifier.fillMaxWidth()) {
                Icon(Icons.Outlined.LinkOff, null, tint = MaterialTheme.colorScheme.error)
                Spacer(Modifier.width(8.dp))
                Text("Disconnect from ${state.pcName.ifEmpty { "PC" }}", color = MaterialTheme.colorScheme.error)
            }
        }
    }
    if (confirmUnpair) {
        AlertDialog(
            onDismissRequest = { confirmUnpair = false },
            title = { Text("Disconnect this phone?") },
            text = { Text("You can pair again at any time by scanning the code on your PC.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmUnpair = false
                    actions.unpair()
                }) { Text("Disconnect") }
            },
            dismissButton = { TextButton(onClick = { confirmUnpair = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun StatusCard(state: LinkState, actions: HomeActions) {
    val connected = state.status == LinkStatus.Connected
    val (dot, label) = when (state.status) {
        LinkStatus.Connected -> Online to "Connected"
        LinkStatus.Connecting -> Waiting to "Connecting…"
        else -> Waiting to "Not connected"
    }
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer), shape = PanelShape,
        modifier = Modifier.fillMaxWidth().border(1.dp, Border, PanelShape)) {
        Row(Modifier.padding(16.dp).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(44.dp).background(Color(0xFF30356B), RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Outlined.Computer, null, tint = MaterialTheme.colorScheme.primary)
            }
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    state.pcName.ifEmpty { "Your PC" },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(8.dp).background(dot, CircleShape))
                    Spacer(Modifier.width(6.dp))
                    Text(label, style = MaterialTheme.typography.bodySmall)
                }
                val hint = if (connected) "" else state.detail.ifEmpty { "Make sure FeatherCast runs and both are on the same Wi-Fi." }
                if (hint.isNotEmpty()) {
                    Text(hint, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            if (!connected) {
                OutlinedButton(onClick = actions::reconnect) { Text("Retry") }
            }
        }
    }
}

@Composable
private fun QuickAction(icon: ImageVector, label: String, modifier: Modifier, onClick: () -> Unit) {
    FilledTonalButton(
        onClick = onClick,
        modifier = modifier.height(60.dp).border(1.dp, Border, PanelShape),
        shape = PanelShape,
    ) {
        Icon(icon, null, modifier = Modifier.size(20.dp))
        Spacer(Modifier.width(8.dp))
        Text(label, maxLines = 2, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        color = SectionText,
        modifier = Modifier.padding(start = 4.dp, top = 8.dp),
    )
}

@Composable
private fun FeatureRow(
    icon: ImageVector,
    title: String,
    text: String,
    checked: Boolean,
    onChecked: (Boolean) -> Unit,
    missing: String?,
    onFix: () -> Unit,
) {
    Column {
        Row(Modifier.padding(horizontal = 14.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(32.dp).background(Raised, RoundedCornerShape(7.dp)), contentAlignment = Alignment.Center) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(18.dp))
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(4.dp))
            Switch(checked = checked, onCheckedChange = onChecked,
                modifier = Modifier.semantics { contentDescription = "$title. $text" })
        }
        if (checked && missing != null) {
            OutlinedButton(
                onClick = onFix,
                modifier = Modifier.padding(start = 58.dp, bottom = 12.dp),
                shape = RoundedCornerShape(8.dp),
            ) { Text(missing) }
        }
        HorizontalDivider(color = Border, thickness = 1.dp)
    }
}
