# Phone Connection

FeatherCast can link with an Android phone over your local Wi-Fi. Once paired, the
**Phone** window on the PC shows the phone's notifications, newest photos, clipboard,
text messages, what is playing, and battery level. From the PC you can reply to
notifications and texts, send files to the phone, make it ring, control its media,
handle incoming calls, and browse its storage.

Nothing goes through the internet or a cloud service.

## Set up

1. On the PC, open **Phone**: tray icon → **Phone**, search for `Phone`, or
   **Settings → Privacy → Phone Window**. Turn on **Phone Connection** if asked.
   `FeatherCast.exe --phone` opens the window directly.
2. **Install the app.** Scan the *Install app* QR code with the phone camera. It
   downloads `FeatherCast-Phone.apk` straight from the PC. Allow the browser to
   install unknown apps when Android asks.
3. **Pair.** Open FeatherCast on the phone, tap **Scan QR code**, and scan the
   *Pair* code. Pairing codes are valid for 5 minutes and can only be used once.
4. On the phone, allow **notification access** and **photo access** from the
   buttons on the home screen.

If Windows asks, allow FeatherCast on **private networks**. The phone and the PC must
be on the same network. Afterwards the phone reconnects automatically. It also finds
the PC again when the PC gets a new IP address.

## What is shared

| Feature | Direction | Notes |
| --- | --- | --- |
| Notifications | Phone → PC | Every app's notifications (not ongoing ones such as music players). Dismissing one on the PC dismisses it on the phone. Optional toasts on the PC. |
| Photos | Phone → PC | The newest 60 pictures as thumbnails; clicking one saves the full picture to `Downloads\FeatherCast` and opens it. |
| Clipboard | Both | Text copied on the PC is copied on the phone (**Clipboard sync**). Android only lets the visible app read the clipboard, so use **Send clipboard** in the app, its notification, or the *Clipboard to PC* quick-settings tile. |
| Files | Phone → PC | **Share → Send to PC** from any app, or **Send photo or file** in the app. Saved to `Downloads\FeatherCast` (max. 40 MB per file). |
| Files | PC → Phone | **Send File to Phone** in the launcher, **Send to Phone** on a file result (Tab), Explorer's **Send to → FeatherCast Phone**, dropping files on the Phone window, or `FeatherCast.exe --send-to-phone <files>`. Saved to `Download/FeatherCast` on the phone (max. 40 MB per file). |
| Notification actions | PC → Phone | Buttons such as *Mark as read* and inline replies (*Reply*) of phone notifications. |
| Find my phone | PC → Phone | Rings at full alarm volume even in silent mode, for up to 60 seconds. Stop it on the phone or run **Find My Phone** again. |
| Media | Both | Title, artist, and cover of the playing app; play/pause, next, previous, and volume. Uses the notification access grant. |
| Text messages *(optional)* | Both | Conversations, messages, and sending SMS. Needs SMS and contacts permissions. |
| Calls *(optional)* | Phone → PC | Incoming calls appear as a toast and at the top of the launcher, with **Reject** and **Silence**. |
| Phone storage *(optional)* | Phone → PC | Browse shared storage and download files. Needs *All files access* on Android 11+. |
| Battery | Phone → PC | Level and charging state. The PC warns once when the battery drops to 20% while not charging (toggle in the Phone window's **Devices** tab). |

The optional features are off until you turn them on under **Optional** in the phone
app, which then asks for the Android permissions they need. Every feature can be
switched off in the phone app. On the PC, **Settings → Privacy →
Phone Connection** turns the whole feature off. The **Devices** tab in the Phone
window unpairs a phone.

## In the launcher

Phone data is also searchable right in the FeatherCast search panel, without the
Phone window. Start typing what you want (`notif`, `photos`, `phone`, `copied`) and the
matching views are suggested in a **Phone** section near the top, with a live summary
such as "3 notifications · latest: …". Press **Enter** on one and the panel turns into a
live list that updates as new data arrives, and **Esc** goes back:

| Command | Shows | Enter | More (Tab / Ctrl+K) |
| --- | --- | --- | --- |
| **Phone Notifications** | Recent notifications, newest first | Copy title and text | Paste text, **Dismiss on Phone** (also Ctrl+Delete) |
| **Phone Photos** | The newest photos as a thumbnail grid (arrow keys move in 2D, F5 refreshes) | Download and open | **Save to Downloads** without opening |
| **Phone Clipboard** | Text recently copied on the phone during this session | Paste into the previous app | Copy |
| **Phone Media** | What is playing, plus Play/Pause, Next, Previous, Volume rows (Space plays/pauses) | Run the control | – |
| **Phone Messages** | SMS conversations, newest first (F5 refreshes). Type a phone number for *New message to …* | Open the conversation | Copy Number |
| **Phone Files** | The phone's shared storage, folders first (Backspace goes up, F5 refreshes) | Open a folder, or download and open a file | **Save to Downloads** |
| **Find My Phone** | – | Start or stop ringing | – |
| **Send File to Phone** | A file picker | Send the chosen files | – |

**Replying.** In a conversation, and after choosing *Reply* on a notification (Tab),
the search box becomes the message: type it and press **Enter** to send, **Esc** to
cancel. Notification actions such as *Mark as read* are listed under Tab as well.

Typing filters the list. When the phone is offline, the data received so far stays
visible and the section title shows *(offline)*. The data lives in memory only and
is cleared when another phone connects or FeatherCast exits.

## Security

- The pairing QR code contains the PC's addresses, its ECDH P-256 public key, and a
  one-time token. The phone proves that it saw the code
  (`HMAC(token, "pair" ‖ phonePub ‖ pcPub)`). Both sides then derive a long-term link
  key from ECDH with HKDF-SHA256, salted with the token.
- Each connection runs a challenge–response with fresh nonces from both sides. That
  derives separate AES-256-GCM session keys for each direction, and nonces count up
  per message.
- The PC only accepts paired phones. Link keys are stored DPAPI-encrypted in
  `%APPDATA%\FeatherCast\phone-link.dat`. On the phone they are wrapped with an
  Android Keystore key.
- The listener is off until **Phone Connection** is enabled.

## Protocol reference

- TCP port **47800**. Frames are `[u32 big-endian length][body]`, with a maximum of 48 MB.
  The same port answers `GET /app.apk` with the phone app when the file
  `FeatherCast-Phone.apk` sits next to `FeatherCast.exe`.
- UDP port **47801**: the PC broadcasts `FCAST1 <pcId> <port>` every 3 seconds, so
  phones can find it again.
- Pairing (plain JSON frames): `pair{deviceId, name, pub, proof}` → `paired{proof, pcName, pcId}`
  or `error{code}`.
- Session handshake: `hello{deviceId, nonce}` → `challenge{nonce, pcName}` →
  `auth{mac}` → `welcome{mac, pcName}`. After that every frame is
  `AES-GCM([u32 jsonLength][json][binary])`.
- Phone → PC: `status{battery, charging, name, features[]}`, `ping`,
  `notification.posted{…, actions[{i, title, reply}]}` (PNG icon as binary),
  `notification.removed`, `notifications.reset`, `clipboard.set`,
  `clipboard.history.request`, `photos.list`, `photo.thumb`, `photo.full`, `file.send`,
  `file.received{id, name, ok, error?}`, `ring.state{ringing}`,
  `media.state{app, appName, title, artist, playing, pos, dur, posAt, vol, volMax}`
  (JPEG cover as binary, only when the track changes), `media.none`,
  `sms.threads{items}`, `sms.messages{thread, items}`, `sms.received`, `sms.sent{ref, ok}`,
  `call.state{state, number, name}`, `files.list{path, items, error?}`,
  `file.data{path, name, error?}` (file as binary).
- PC → phone: `pong`, `clipboard.set`, `clipboard.history`, `photos.request`,
  `photo.request`, `notification.dismiss`, `notification.action{key, i, text?}`,
  `file.send{id, name}` (file as binary), `ring.start`, `ring.stop`,
  `media.command{cmd, pos?}`, `media.volume{vol}`, `sms.threads.request`,
  `sms.messages.request{thread, limit}`, `sms.send{ref, address, body}`, `call.reject`,
  `call.silence`, `files.list.request{path}`, `file.request{path}`.
- `status.features` lists what the phone app has turned on (`notification.actions`,
  `files.receive`, `ring`, `media`, `sms`, `calls`, `storage`), so the PC can explain what
  to enable. Both sides ignore message types they do not know, so older apps keep working.

Both implementations are checked against the same test vectors. The PC side is in
`native/tests/phone_protocol_tests.cpp`, and the phone side is in
`android/protocol/src/test`.

## Building the phone app

```powershell
scripts\setup-android-sdk.ps1   # once: JDK 17, Android SDK, Gradle under %LOCALAPPDATA%\FeatherCast-dev
scripts\build-android.ps1       # tests, signed release APK, copied next to FeatherCast.exe
```

The release keystore is created on the first build at
`%LOCALAPPDATA%\FeatherCast-dev\phone-release.jks`. Keep it: Android only installs
updates that are signed with the same key.

To test the PC side without a phone, use the desktop simulator:

```powershell
cd android
.\gradlew :protocol:run --args="pair '<feathercast://pair?... URI>' sim.state"
.\gradlew :protocol:run --args="run sim.state 30"
```

The simulator answers every request with sample data: a WhatsApp notification with
*Reply* and *Mark as read*, a playing song, two SMS conversations, a small folder tree,
and an incoming call 30 seconds after it connects.
