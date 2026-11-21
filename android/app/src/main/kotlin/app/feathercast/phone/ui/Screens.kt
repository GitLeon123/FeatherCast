package app.feathercast.phone.ui

import android.os.Build
import android.os.SystemClock
import android.text.format.DateUtils
import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ScreenShare
import androidx.compose.material.icons.outlined.Call
import androidx.compose.material.icons.outlined.Computer
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.ContentPaste
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material.icons.outlined.LinkOff
import androidx.compose.material.icons.outlined.MusicNote
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.NotificationsActive
import androidx.compose.material.icons.outlined.QrCodeScanner
import androidx.compose.material.icons.outlined.Sms
import androidx.compose.material.icons.outlined.TouchApp
import androidx.compose.material.icons.outlined.Upload
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.feathercast.phone.LinkState
import app.feathercast.phone.LinkStatus
import app.feathercast.phone.PhotoAccess
import app.feathercast.phone.R
import app.feathercast.phone.ScreenUiState
import kotlinx.coroutines.delay
import java.text.DateFormat

// ---- Theme --------------------------------------------------------------------

/** Colors that Material's color scheme has no slot for. */
@Immutable
class FeatherTokens(
    val border: Color,
    val raised: Color,
    val accentContainer: Color,
    val sectionText: Color,
    val online: Color,
    val waiting: Color,
)

// Brand palette, mirroring the default desktop theme in native/src/theme.hpp. The light
// primary is dark enough for white text (5.0:1); the dark primary is light, with dark text.
private val BrandPrimary = Color(0xFF4F5EF0)
private val OnlineDark = Color(0xFF4DC77A)
private val WaitingDark = Color(0xFFF5B957)
private val OnlineLight = Color(0xFF1B7F3F)
private val WaitingLight = Color(0xFF8A5A00)

private val DarkColors = darkColorScheme(
    primary = Color(0xFF99A3FF),
    onPrimary = Color(0xFF0E1033),
    primaryContainer = Color(0xFF30356B),
    onPrimaryContainer = Color(0xFFF2F2F5),
    background = Color(0xFF101012),
    onBackground = Color(0xFFF2F2F5),
    surface = Color(0xFF101012),
    onSurface = Color(0xFFF2F2F5),
    surfaceVariant = Color(0xFF242428),
    onSurfaceVariant = Color(0xFFB0B0BA),
    surfaceContainer = Color(0xFF18181B),
    outline = Color(0xFF6F6F7C),
    outlineVariant = Color(0xFF303035),
    error = Color(0xFFFF5C5C),
)

private val LightColors = lightColorScheme(
    primary = BrandPrimary,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFDDE0FF),
    onPrimaryContainer = Color(0xFF1A1F6B),
    background = Color(0xFFF6F6F9),
    onBackground = Color(0xFF17171A),
    surface = Color(0xFFF6F6F9),
    onSurface = Color(0xFF17171A),
    surfaceVariant = Color(0xFFECECF3),
    onSurfaceVariant = Color(0xFF53535C),
    surfaceContainer = Color.White,
    outline = Color(0xFF7A7A86),
    outlineVariant = Color(0xFFD4D4DC),
    error = Color(0xFFB3261E),
)

private val DarkTokens = FeatherTokens(
    border = Color(0xFF303035),
    raised = Color(0xFF242428),
    accentContainer = Color(0xFF30356B),
    sectionText = Color(0xFFADB2CA),
    online = OnlineDark,
    waiting = WaitingDark,
)

private val LightTokens = FeatherTokens(
    border = Color(0xFFD4D4DC),
    raised = Color(0xFFECECF3),
    accentContainer = Color(0xFFDDE0FF),
    sectionText = Color(0xFF4A4E6B),
    online = OnlineLight,
    waiting = WaitingLight,
)

private fun tokensFor(scheme: ColorScheme, dark: Boolean) = FeatherTokens(
    border = scheme.outlineVariant,
    raised = scheme.surfaceContainerHighest,
    accentContainer = scheme.primaryContainer,
    sectionText = scheme.onSurfaceVariant,
    online = if (dark) OnlineDark else OnlineLight,
    waiting = if (dark) WaitingDark else WaitingLight,
)

private val LocalTokens = staticCompositionLocalOf { DarkTokens }
private val tokens: FeatherTokens @Composable @ReadOnlyComposable get() = LocalTokens.current

private val PanelShape = RoundedCornerShape(10.dp)
private val ContentMaxWidth = 600.dp

private val FeatherType = Typography(
    headlineSmall = TextStyle(fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.SemiBold),
    titleMedium = TextStyle(fontSize = 17.sp, lineHeight = 23.sp, fontWeight = FontWeight.SemiBold),
    titleSmall = TextStyle(fontSize = 14.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold),
    bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontSize = 13.sp, lineHeight = 18.sp),
    labelLarge = TextStyle(fontSize = 13.sp, lineHeight = 18.sp, fontWeight = FontWeight.SemiBold),
)

/**
 * Follows the system light/dark setting, and the wallpaper colors on Android 12+.
 * [fillScreen] false leaves the window transparent, e.g. under a dialog.
 */
@Composable
fun FeatherTheme(fillScreen: Boolean = true, content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val dynamic = Build.VERSION.SDK_INT >= 31
    val colors = when {
        dynamic && dark -> dynamicDarkColorScheme(context)
        dynamic -> dynamicLightColorScheme(context)
        dark -> DarkColors
        else -> LightColors
    }
    val extra = when {
        dynamic -> tokensFor(colors, dark)
        dark -> DarkTokens
        else -> LightTokens
    }
    CompositionLocalProvider(LocalTokens provides extra) {
        MaterialTheme(colorScheme = colors, typography = FeatherType) {
            if (fillScreen) {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background, content = content)
            } else {
                content()
            }
        }
    }
}

/** A bordered card in the panel color; the section container used across the app. */
@Composable
private fun PanelCard(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
        shape = PanelShape,
        modifier = modifier.fillMaxWidth().border(1.dp, tokens.border, PanelShape),
    ) { content() }
}

// ---- Connect ------------------------------------------------------------------

@Composable
fun ConnectScreen(
    busy: Boolean,
    error: String?,
    cameraBlocked: Boolean,
    onScan: () -> Unit,
    onPasteLink: (String) -> Unit,
    onOpenSettings: () -> Unit,
) {
    var showPaste by rememberSaveable { mutableStateOf(false) }
    var link by rememberSaveable { mutableStateOf("") }
    Box(Modifier.fillMaxSize().safeDrawingPadding(), contentAlignment = Alignment.TopCenter) {
        Column(
            modifier = Modifier
                .widthIn(max = ContentMaxWidth)
                .fillMaxWidth()
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
            Text(
                stringResource(R.string.connect_title),
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.semantics { heading() },
            )
            Spacer(Modifier.height(6.dp))
            Text(
                stringResource(R.string.connect_intro),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(28.dp))
            PanelCard {
                Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    Step(1, stringResource(R.string.step_open_title), stringResource(R.string.step_open_text))
                    Step(2, stringResource(R.string.step_scan_title), stringResource(R.string.step_scan_text))
                    Step(3, stringResource(R.string.step_done_title), stringResource(R.string.step_done_text))
                }
            }
            Spacer(Modifier.height(32.dp))
            if (error != null) {
                Text(
                    error,
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(bottom = 12.dp).semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
            if (cameraBlocked) {
                Text(
                    stringResource(R.string.camera_blocked),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = 4.dp),
                )
                TextButton(onClick = onOpenSettings) { Text(stringResource(R.string.open_app_settings)) }
                Spacer(Modifier.height(8.dp))
            }
            Button(
                onClick = onScan,
                enabled = !busy,
                modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                shape = RoundedCornerShape(8.dp),
            ) {
                if (busy) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.onPrimary)
                    Spacer(Modifier.width(12.dp))
                    Text(stringResource(R.string.connecting))
                } else {
                    Icon(Icons.Outlined.QrCodeScanner, null)
                    Spacer(Modifier.width(10.dp))
                    Text(stringResource(R.string.scan_qr), style = MaterialTheme.typography.titleMedium)
                }
            }
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = { showPaste = !showPaste }) { Text(stringResource(R.string.paste_link)) }
            if (showPaste) {
                val submit = { if (link.isNotBlank() && !busy) onPasteLink(link.trim()) }
                OutlinedTextField(
                    value = link,
                    onValueChange = { link = it },
                    label = { Text(stringResource(R.string.pairing_link_label)) },
                    supportingText = { Text(stringResource(R.string.pairing_link_hint)) },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go),
                    keyboardActions = KeyboardActions(onGo = { submit() }),
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(8.dp))
                FilledTonalButton(
                    onClick = { submit() },
                    enabled = link.isNotBlank() && !busy,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    shape = RoundedCornerShape(8.dp),
                ) { Text(stringResource(R.string.use_link)) }
            }
            Spacer(Modifier.height(16.dp))
        }
    }
}

/** Confirms a pairing link that was opened from outside the app, such as a browser. */
@Composable
fun PairConfirmDialog(pcName: String, hosts: List<String>, replaces: String?, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.pair_title, pcName), maxLines = 2, overflow = TextOverflow.Ellipsis) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.pair_connect_to, pcName))
                Column {
                    hosts.forEach { Text(it, maxLines = 2, overflow = TextOverflow.Ellipsis, fontWeight = FontWeight.SemiBold) }
                }
                if (replaces != null) Text(stringResource(R.string.pair_replaces, replaces))
                Text(stringResource(R.string.pair_warning))
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text(stringResource(R.string.pair)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

/**
 * Confirms what another app shared before it goes to the PC: any app can open the
 * share target, so nothing is sent without the user's consent.
 */
@Composable
fun ShareConfirmDialog(pcName: String, fileCount: Int, text: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.share_title, pcName), maxLines = 2, overflow = TextOverflow.Ellipsis) },
        text = {
            if (fileCount > 0) {
                Text(pluralStringResource(R.plurals.share_files_saved, fileCount, fileCount))
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.share_text_copied))
                    Text(
                        text.take(500),
                        maxLines = 6,
                        overflow = TextOverflow.Ellipsis,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text(stringResource(R.string.send)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun Step(number: Int, title: String, text: String) {
    Row(verticalAlignment = Alignment.Top) {
        Box(
            Modifier.defaultMinSize(minWidth = 28.dp, minHeight = 28.dp).background(tokens.accentContainer, RoundedCornerShape(6.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("$number", color = MaterialTheme.colorScheme.onPrimaryContainer, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.width(14.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

// ---- Home ---------------------------------------------------------------------

/** Features that ask for a runtime permission or special access. */
enum class PermissionTopic { Notifications, Photos, SaveFiles, Sms, Calls, Storage, PostNotifications, LocalNetwork }

data class Permissions(
    val notificationAccess: Boolean,
    val photos: PhotoAccess,
    val postNotifications: Boolean,
    val saveFiles: Boolean = true,
    val sms: Boolean = false,
    val calls: Boolean = false,
    val storage: Boolean = false,
    val remoteControl: Boolean = false,
    val pcKeyboard: Boolean = false,
    val deviceAudio: Boolean = false,
    val localNetwork: Boolean = true,
    /** True when Android no longer shows a dialog for the permission, so only Settings can grant it. */
    val blocked: Set<PermissionTopic> = emptySet(),
    val ignoringBattery: Boolean = true,
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

/** What the user picks in the "Choose what to share" step after pairing. */
data class SharingChoices(
    val notifications: Boolean,
    val photos: Boolean,
    val clipboard: Boolean,
    val receiveFiles: Boolean,
    val ring: Boolean,
    val media: Boolean,
)

interface HomeActions {
    fun setFeature(feature: Feature, enabled: Boolean)
    /** Called with the user's picks, or null when the step was dismissed (defaults stay). */
    fun finishSharingChoice(choices: SharingChoices?)
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
    fun openAppSettings()
    fun openBatterySettings()
}

private const val OFFLINE_HINT_DELAY_MS = 30_000L

@Composable
fun HomeScreen(
    state: LinkState,
    permissions: Permissions,
    switches: Switches,
    actions: HomeActions,
    screen: ScreenUiState = ScreenUiState(),
    chooseSharing: Boolean = false,
) {
    var confirmUnpair by rememberSaveable { mutableStateOf(false) }
    var consent by rememberSaveable { mutableStateOf<PermissionTopic?>(null) }
    val connected = state.status == LinkStatus.Connected

    // Offline for a while usually means Android put the app to sleep.
    var offlineLong by remember { mutableStateOf(false) }
    LaunchedEffect(connected) {
        offlineLong = false
        if (!connected) {
            delay(OFFLINE_HINT_DELAY_MS)
            offlineLong = true
        }
    }

    fun launchPermission(topic: PermissionTopic) = when (topic) {
        PermissionTopic.Notifications -> actions.openNotificationAccess()
        PermissionTopic.Photos -> actions.requestPhotos()
        PermissionTopic.SaveFiles -> actions.requestSaveFiles()
        PermissionTopic.Sms -> actions.requestSms()
        PermissionTopic.Calls -> actions.requestCalls()
        PermissionTopic.Storage -> actions.openStorageAccess()
        PermissionTopic.PostNotifications -> actions.requestPostNotifications()
        PermissionTopic.LocalNetwork -> actions.requestLocalNetwork()
    }

    // Explains what a feature reads before Android's own permission prompt appears.
    // A permission Android no longer prompts for goes straight to the app settings.
    fun ask(topic: PermissionTopic) {
        val needsConsent = topic == PermissionTopic.Notifications || topic == PermissionTopic.Photos ||
            topic == PermissionTopic.Sms || topic == PermissionTopic.Calls || topic == PermissionTopic.Storage
        if (needsConsent && topic !in permissions.blocked) consent = topic else launchPermission(topic)
    }

    fun toggle(feature: Feature, enabled: Boolean) {
        actions.setFeature(feature, enabled)
        if (!enabled) return
        when (feature) {
            Feature.Sms -> if (!permissions.sms) ask(PermissionTopic.Sms)
            Feature.Calls -> if (!permissions.calls) ask(PermissionTopic.Calls)
            Feature.Storage -> if (!permissions.storage) ask(PermissionTopic.Storage)
            else -> Unit
        }
    }

    @Composable fun note(topic: PermissionTopic): String? =
        if (topic in permissions.blocked) stringResource(R.string.turn_on_in_permissions, stringResource(permissionLabel(topic))) else null

    Box(Modifier.fillMaxSize().safeDrawingPadding(), contentAlignment = Alignment.TopCenter) {
        LazyColumn(
            modifier = Modifier.fillMaxHeight().widthIn(max = ContentMaxWidth).fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (screen.pending) {
                item { ScreenRequestCard(screen, state.pcName, actions) }
            }
            if (state.transferStatus.isNotEmpty()) {
                item {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(state.transferStatus, modifier = Modifier.weight(1f),
                            style = MaterialTheme.typography.bodyMedium)
                        if (state.transferActive) TextButton(onClick = actions::cancelTransfers) { Text(stringResource(R.string.cancel)) }
                    }
                }
            }
            item {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 2.dp, bottom = 4.dp)) {
                    Icon(painterResource(R.drawable.ic_stat_feathercast), null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(24.dp))
                    Spacer(Modifier.width(10.dp))
                    Text(stringResource(R.string.home_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
                }
            }
            item { StatusCard(state, actions) }
            if (!connected && offlineLong && !permissions.ignoringBattery) {
                item {
                    PanelCard {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(stringResource(R.string.battery_title), style = MaterialTheme.typography.titleSmall)
                            Text(
                                stringResource(R.string.battery_text),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            TextButton(onClick = actions::openBatterySettings) { Text(stringResource(R.string.battery_button)) }
                        }
                    }
                }
            }
            if (!permissions.localNetwork) item {
                PanelCard {
                    Column(Modifier.padding(16.dp)) {
                        Text(stringResource(R.string.local_network_needed))
                        note(PermissionTopic.LocalNetwork)?.let {
                            Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        TextButton(onClick = { ask(PermissionTopic.LocalNetwork) }) { Text(stringResource(R.string.allow_local_network)) }
                    }
                }
            }
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
                    QuickAction(Icons.Outlined.ContentPaste, stringResource(R.string.send_clipboard), Modifier.weight(1f), actions::sendClipboard)
                    QuickAction(Icons.Outlined.Upload, stringResource(R.string.send_file), Modifier.weight(1f), actions::sendFiles)
                }
            }
            item { SectionTitle(stringResource(R.string.section_shared)) }
            item {
                PanelCard {
                    Column {
                        FeatureRow(
                            Icons.Outlined.Notifications, stringResource(R.string.feature_notifications),
                            stringResource(R.string.feature_notifications_text),
                            checked = switches.notifications, onChecked = { toggle(Feature.Notifications, it) },
                            missing = if (permissions.notificationAccess) null else stringResource(R.string.allow_notification_access),
                            onFix = { ask(PermissionTopic.Notifications) },
                        )
                        FeatureRow(
                            Icons.Outlined.Image, stringResource(R.string.feature_photos),
                            if (permissions.photos == PhotoAccess.Selected) stringResource(R.string.feature_photos_selected)
                            else stringResource(R.string.feature_photos_text),
                            checked = switches.photos, onChecked = { toggle(Feature.Photos, it) },
                            missing = when (permissions.photos) {
                                PhotoAccess.Full -> null
                                PhotoAccess.Selected -> stringResource(R.string.choose_photos)
                                PhotoAccess.Denied -> stringResource(R.string.allow_photo_access)
                            },
                            onFix = { ask(PermissionTopic.Photos) },
                            note = note(PermissionTopic.Photos),
                        )
                        FeatureRow(
                            Icons.Outlined.ContentCopy, stringResource(R.string.feature_clipboard),
                            stringResource(R.string.feature_clipboard_text),
                            checked = switches.clipboard, onChecked = { toggle(Feature.Clipboard, it) },
                            missing = null, onFix = {},
                        )
                        FeatureRow(
                            Icons.Outlined.Download, stringResource(R.string.feature_files),
                            stringResource(R.string.feature_files_text),
                            checked = switches.receiveFiles, onChecked = { toggle(Feature.ReceiveFiles, it) },
                            missing = if (permissions.saveFiles) null else stringResource(R.string.allow_saving_files),
                            onFix = { ask(PermissionTopic.SaveFiles) },
                            note = note(PermissionTopic.SaveFiles),
                        )
                        FeatureRow(
                            Icons.Outlined.NotificationsActive, stringResource(R.string.feature_ring),
                            stringResource(R.string.feature_ring_text),
                            checked = switches.ring, onChecked = { toggle(Feature.Ring, it) },
                            missing = null, onFix = {},
                        )
                        FeatureRow(
                            Icons.Outlined.MusicNote, stringResource(R.string.feature_media),
                            stringResource(R.string.feature_media_text),
                            checked = switches.media, onChecked = { toggle(Feature.Media, it) },
                            missing = if (permissions.notificationAccess) null else stringResource(R.string.allow_notification_access),
                            onFix = { ask(PermissionTopic.Notifications) },
                        )
                    }
                }
            }
            item { SectionTitle(stringResource(R.string.section_optional)) }
            item {
                PanelCard {
                    Column {
                        FeatureRow(
                            Icons.Outlined.Sms, stringResource(R.string.feature_sms),
                            stringResource(R.string.feature_sms_text),
                            checked = switches.sms, onChecked = { toggle(Feature.Sms, it) },
                            missing = if (permissions.sms) null else stringResource(R.string.allow_sms_access),
                            onFix = { ask(PermissionTopic.Sms) },
                            note = note(PermissionTopic.Sms),
                        )
                        FeatureRow(
                            Icons.Outlined.Call, stringResource(R.string.feature_calls),
                            stringResource(R.string.feature_calls_text),
                            checked = switches.calls, onChecked = { toggle(Feature.Calls, it) },
                            missing = if (permissions.calls) null else stringResource(R.string.allow_phone_access),
                            onFix = { ask(PermissionTopic.Calls) },
                            note = note(PermissionTopic.Calls),
                        )
                        FeatureRow(
                            Icons.Outlined.Folder, stringResource(R.string.feature_storage),
                            stringResource(R.string.feature_storage_text),
                            checked = switches.storage, onChecked = { toggle(Feature.Storage, it) },
                            missing = if (permissions.storage) null else stringResource(R.string.allow_all_files),
                            onFix = { ask(PermissionTopic.Storage) },
                            note = note(PermissionTopic.Storage),
                        )
                    }
                }
            }
            item {
                PanelCard {
                    Column {
                        Text(
                            stringResource(R.string.section_phone_screen),
                            style = MaterialTheme.typography.titleSmall,
                            color = tokens.sectionText,
                            modifier = Modifier.padding(start = 16.dp, top = 12.dp, end = 16.dp).semantics { heading() },
                        )
                        FeatureRow(
                            Icons.AutoMirrored.Outlined.ScreenShare, stringResource(R.string.feature_screen),
                            stringResource(R.string.feature_screen_text),
                            checked = switches.screen, onChecked = { toggle(Feature.Screen, it) },
                            missing = null, onFix = {},
                        )
                        FeatureRow(
                            Icons.Outlined.TouchApp, stringResource(R.string.feature_remote),
                            stringResource(R.string.feature_remote_text),
                            checked = switches.remoteControl, onChecked = { toggle(Feature.RemoteControl, it) },
                            missing = if (permissions.remoteControl) null else stringResource(R.string.enable_remote_control),
                            onFix = actions::openRemoteControl,
                        )
                        Column(Modifier.padding(horizontal = 16.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(stringResource(R.string.pc_keyboard), style = MaterialTheme.typography.titleSmall)
                            Text(
                                if (permissions.pcKeyboard) stringResource(R.string.pc_keyboard_selected)
                                else stringResource(R.string.pc_keyboard_enable),
                                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                TextButton(onClick = actions::enablePcKeyboard) { Text(stringResource(R.string.enable_keyboard)) }
                                TextButton(onClick = actions::selectPcKeyboard) { Text(stringResource(R.string.select_keyboard)) }
                            }
                            Text(
                                if (permissions.deviceAudio) stringResource(R.string.device_audio_requested)
                                else stringResource(R.string.device_audio_unsupported),
                                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            if (switches.remoteControl && !permissions.remoteControl) {
                                Text(
                                    stringResource(R.string.restricted_settings_hint),
                                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            if (screen.detail.isNotEmpty()) Text(screen.detail, style = MaterialTheme.typography.bodyMedium)
                            if (screen.active) OutlinedButton(onClick = actions::stopScreen) { Text(stringResource(R.string.stop_sharing)) }
                        }
                    }
                }
            }
            if (!permissions.postNotifications) {
                item {
                    PanelCard {
                        Column(Modifier.padding(16.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    stringResource(R.string.post_notifications_text),
                                    style = MaterialTheme.typography.bodySmall,
                                    modifier = Modifier.weight(1f),
                                )
                                TextButton(onClick = { ask(PermissionTopic.PostNotifications) }) { Text(stringResource(R.string.allow)) }
                            }
                            note(PermissionTopic.PostNotifications)?.let {
                                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
            item { SectionTitle(stringResource(R.string.section_pc_clipboard)) }
            if (state.pcClipboard.isEmpty()) {
                item {
                    Text(
                        if (connected) stringResource(R.string.pc_clipboard_empty) else stringResource(R.string.pc_clipboard_offline),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 4.dp),
                    )
                }
            } else {
                itemsIndexed(state.pcClipboard, key = { index, clip -> "$index-${clip.time}-${clip.text.hashCode()}" }) { _, clip ->
                    PanelCard(
                        Modifier.clickable(onClickLabel = stringResource(R.string.copy_to_clipboard), role = Role.Button) {
                            actions.copyPcClip(clip.text)
                        },
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
                            Icon(Icons.Outlined.ContentCopy, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
                        }
                    }
                }
            }
            item {
                Spacer(Modifier.height(8.dp))
                TextButton(onClick = { confirmUnpair = true }, modifier = Modifier.fillMaxWidth()) {
                    Icon(Icons.Outlined.LinkOff, null, tint = MaterialTheme.colorScheme.error)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.unpair_button), color = MaterialTheme.colorScheme.error)
                }
            }
        }
    }
    if (confirmUnpair) {
        AlertDialog(
            onDismissRequest = { confirmUnpair = false },
            title = { Text(stringResource(R.string.unpair_title)) },
            text = { Text(stringResource(R.string.unpair_text, state.pcName.ifEmpty { stringResource(R.string.your_pc) })) },
            confirmButton = {
                TextButton(onClick = {
                    confirmUnpair = false
                    actions.unpair()
                }) { Text(stringResource(R.string.unpair_confirm), color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { confirmUnpair = false }) { Text(stringResource(R.string.cancel)) } },
        )
    }
    consent?.let { topic ->
        PermissionConsentDialog(
            topic = topic,
            onContinue = {
                consent = null
                launchPermission(topic)
            },
            onDismiss = { consent = null },
        )
    }
    if (chooseSharing) SharingChoiceDialog(switches, actions)
}

@StringRes private fun permissionLabel(topic: PermissionTopic): Int = when (topic) {
    PermissionTopic.Notifications -> R.string.permission_notification_access
    PermissionTopic.Photos -> R.string.permission_photos
    PermissionTopic.SaveFiles -> R.string.permission_storage
    PermissionTopic.Sms -> R.string.permission_sms
    PermissionTopic.Calls -> R.string.permission_phone
    PermissionTopic.Storage -> R.string.permission_files
    PermissionTopic.PostNotifications -> R.string.permission_notifications
    PermissionTopic.LocalNetwork -> R.string.permission_local_network
}

/** Says what a feature reads and which Android permissions it needs, before Android asks. */
@Composable
private fun PermissionConsentDialog(topic: PermissionTopic, onContinue: () -> Unit, onDismiss: () -> Unit) {
    val (title, reads, permissions) = when (topic) {
        PermissionTopic.Notifications -> Triple(R.string.consent_notifications_title, R.string.consent_notifications_reads, R.string.consent_notifications_permissions)
        PermissionTopic.Photos -> Triple(R.string.consent_photos_title, R.string.consent_photos_reads, R.string.consent_photos_permissions)
        PermissionTopic.Sms -> Triple(R.string.consent_sms_title, R.string.consent_sms_reads, R.string.consent_sms_permissions)
        PermissionTopic.Calls -> Triple(R.string.consent_calls_title, R.string.consent_calls_reads, R.string.consent_calls_permissions)
        else -> Triple(R.string.consent_storage_title, R.string.consent_storage_reads, R.string.consent_storage_permissions)
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(stringResource(reads))
                Text(stringResource(R.string.consent_pc_only))
                Text(stringResource(R.string.consent_permissions_label), style = MaterialTheme.typography.titleSmall)
                Text(stringResource(permissions), color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        confirmButton = { TextButton(onClick = onContinue) { Text(stringResource(R.string.continue_label)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.not_now)) } },
    )
}

/**
 * Shown once after pairing, before any permission is requested: the user decides what
 * this phone shares with the PC.
 */
@Composable
private fun SharingChoiceDialog(initial: Switches, actions: HomeActions) {
    var notifications by rememberSaveable { mutableStateOf(initial.notifications) }
    var photos by rememberSaveable { mutableStateOf(initial.photos) }
    var clipboard by rememberSaveable { mutableStateOf(initial.clipboard) }
    var receiveFiles by rememberSaveable { mutableStateOf(initial.receiveFiles) }
    var ring by rememberSaveable { mutableStateOf(initial.ring) }
    var media by rememberSaveable { mutableStateOf(initial.media) }
    AlertDialog(
        onDismissRequest = { actions.finishSharingChoice(null) },
        title = { Text(stringResource(R.string.choose_title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(stringResource(R.string.choose_text), color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.height(8.dp))
                SwitchRow(stringResource(R.string.feature_notifications), stringResource(R.string.feature_notifications_text), notifications) { notifications = it }
                SwitchRow(stringResource(R.string.feature_photos), stringResource(R.string.feature_photos_text), photos) { photos = it }
                SwitchRow(stringResource(R.string.feature_clipboard), stringResource(R.string.feature_clipboard_text), clipboard) { clipboard = it }
                SwitchRow(stringResource(R.string.feature_files), stringResource(R.string.feature_files_text), receiveFiles) { receiveFiles = it }
                SwitchRow(stringResource(R.string.feature_ring), stringResource(R.string.feature_ring_text), ring) { ring = it }
                SwitchRow(stringResource(R.string.feature_media), stringResource(R.string.feature_media_text), media) { media = it }
                Spacer(Modifier.height(8.dp))
                Text(
                    stringResource(R.string.choose_later),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = {
                actions.finishSharingChoice(SharingChoices(notifications, photos, clipboard, receiveFiles, ring, media))
            }) { Text(stringResource(R.string.choose_save)) }
        },
    )
}

/** A pinned card for a pending screen-share request, so it cannot be missed or scrolled away. */
@Composable
private fun ScreenRequestCard(screen: ScreenUiState, pcName: String, actions: HomeActions) {
    var secondsLeft by remember(screen.expiresAt) { mutableLongStateOf(-1L) }
    LaunchedEffect(screen.expiresAt) {
        if (screen.expiresAt <= 0) return@LaunchedEffect
        while (true) {
            secondsLeft = ((screen.expiresAt - SystemClock.elapsedRealtime() + 999) / 1000).coerceAtLeast(0)
            delay(1000)
        }
    }
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        ),
        shape = PanelShape,
        modifier = Modifier.fillMaxWidth().border(1.dp, MaterialTheme.colorScheme.primary, PanelShape),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(stringResource(R.string.screen_request_title), style = MaterialTheme.typography.titleMedium)
            Text(
                stringResource(R.string.screen_request_text, pcName.ifEmpty { stringResource(R.string.your_pc_title) }),
                style = MaterialTheme.typography.bodyMedium,
            )
            if (secondsLeft >= 0) {
                Text(
                    pluralStringResource(R.plurals.screen_request_expires, secondsLeft.toInt(), secondsLeft.toInt()),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                Button(onClick = actions::approveScreen) { Text(stringResource(R.string.share_screen)) }
                TextButton(onClick = actions::stopScreen) { Text(stringResource(R.string.decline)) }
            }
        }
    }
}

@Composable
private fun StatusCard(state: LinkState, actions: HomeActions) {
    val connected = state.status == LinkStatus.Connected
    val reconnecting = !connected && state.reconnecting
    val (dot, label) = when {
        connected -> tokens.online to stringResource(R.string.status_connected)
        reconnecting -> tokens.waiting to stringResource(R.string.status_reconnecting)
        state.status == LinkStatus.Connecting -> tokens.waiting to stringResource(R.string.status_connecting)
        else -> tokens.waiting to stringResource(R.string.status_offline)
    }
    PanelCard {
        Row(Modifier.padding(16.dp).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.defaultMinSize(minWidth = 44.dp, minHeight = 44.dp).background(tokens.accentContainer, RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Outlined.Computer, null, tint = MaterialTheme.colorScheme.primary)
            }
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    state.pcName.ifEmpty { stringResource(R.string.your_pc_title) },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(8.dp).background(dot, CircleShape))
                    Spacer(Modifier.width(6.dp))
                    Text(label, style = MaterialTheme.typography.bodySmall, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
                }
                val hint = if (connected) "" else state.detail.ifEmpty { stringResource(R.string.status_hint_default) }
                if (hint.isNotEmpty()) {
                    Text(hint, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            if (!connected) {
                OutlinedButton(onClick = actions::reconnect) { Text(stringResource(R.string.retry)) }
            }
        }
    }
}

@Composable
private fun QuickAction(icon: ImageVector, label: String, modifier: Modifier, onClick: () -> Unit) {
    FilledTonalButton(
        onClick = onClick,
        modifier = modifier.heightIn(min = 60.dp).border(1.dp, tokens.border, PanelShape),
        shape = PanelShape,
    ) {
        Icon(icon, null, modifier = Modifier.size(20.dp))
        Spacer(Modifier.width(8.dp))
        Text(label, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        color = tokens.sectionText,
        modifier = Modifier.padding(start = 4.dp, top = 8.dp).semantics { heading() },
    )
}

/** A plain labelled switch where the whole row toggles. */
@Composable
private fun SwitchRow(title: String, text: String, checked: Boolean, onChecked: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .toggleable(value = checked, role = Role.Switch, onValueChange = onChecked)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Spacer(Modifier.width(8.dp))
        Switch(checked = checked, onCheckedChange = null)
    }
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
    note: String? = null,
) {
    Column {
        // The whole row is the switch, and TalkBack reads the title and description together.
        Row(
            Modifier
                .fillMaxWidth()
                .toggleable(value = checked, role = Role.Switch, onValueChange = onChecked)
                .padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.defaultMinSize(minWidth = 32.dp, minHeight = 32.dp).background(tokens.raised, RoundedCornerShape(7.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(18.dp))
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(4.dp))
            Switch(checked = checked, onCheckedChange = null)
        }
        if (checked && missing != null) {
            Column(Modifier.padding(start = 58.dp, end = 14.dp, bottom = 12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                OutlinedButton(onClick = onFix, shape = RoundedCornerShape(8.dp)) { Text(missing) }
                if (note != null) {
                    Text(note, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        HorizontalDivider(color = tokens.border, thickness = 1.dp)
    }
}
