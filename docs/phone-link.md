# Phone Connection

FeatherCast can link with an Android phone over your local Wi-Fi. Once paired, the
**Phone** window on the PC shows the phone's notifications, newest photos, clipboard,
text messages, what is playing, and battery level. From the PC you can reply to
notifications and texts, send files to the phone, make it ring, control its media,
handle incoming calls, and browse its storage.
**Phone Screen** opens a separate, resizable window for live screen sharing,
mouse control, PC typing, and device audio.

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
| Phone Screen *(optional)* | Both | Live H.264 screen, mouse gestures, navigation, PC keyboard, and AAC device audio. Every session requires Android approval. |

The optional features are off until you turn them on in the phone app, which then
asks for the Android permissions they need. Screen sharing and Remote control
are under **Phone screen**; the other optional features are under **Optional**. Every feature can be
switched off in the phone app. On the PC, **Settings → Privacy →
Phone Connection** turns the whole feature off. The **Devices** tab in the Phone
window unpairs a phone.

## Phone Screen

1. Install the updated **FeatherCast-Phone.apk** next to your PC's EXE. Older
   companions continue to work for the existing phone features.
2. In the Android app, turn on **Screen sharing**. For mouse control or PC typing,
   also turn on **Remote control**. For mouse control, choose **Enable FeatherCast Remote Control** to
   enable its Android accessibility service. If Android blocks this after
   sideloading, open FeatherCast's **App info → Allow restricted settings** first.
3. Choose **Enable keyboard**, enable **FeatherCast PC Keyboard** in Android's
   keyboard settings, then use **Select keyboard** to select it. This supplies
   Unicode text and editing keys to the currently focused phone input field.
4. Open **Phone Screen** in the PC's Phone window or launcher. In FeatherCast on
   the phone, tap **Share screen**, allow audio if wanted, and approve Android's
   **Entire screen** prompt. A background request also creates a notification;
   requests expire after 60 seconds. Declining audio still allows video.

Click to tap, hold for a long press, drag to swipe, and use the mouse wheel to
scroll. **Back**, **Home**, and **Recent apps** provide Android navigation.
Type while the screen area is focused; **Ctrl+V** pastes up to 16 KB of text,
**Ctrl+A** selects all, and Enter, Delete, Backspace, Tab, and arrow keys go to
the phone. **Ctrl+Tab** focuses the PC toolbar. **F11** switches full screen;
Escape leaves full screen, otherwise it sends Back.

**Mute**, the volume slider, and **Stop** affect this session. Closing the
window, turning off screen sharing, locking or turning off the phone screen,
losing the connection, or Android revoking projection stops the session.
Reconnecting never resumes sharing: start again and approve a new prompt.
FeatherCast returns to the previous Android keyboard when sharing ends; if
Android cannot switch back, use the keyboard picker.

Video works on Android 8 and later. Playback audio needs Android 10 or later,
the audio permission, and an app that permits playback capture. Calls,
microphone audio, DRM-protected content, and secure screens cannot be shared
through these APIs. Viewing remains available without remote control or the
PC keyboard. This feature transmits live media only; it does not save video
or audio. File transfers use the primary connection independently.

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
| **Phone Screen** | A live, separate screen window | Request Android approval | – |

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
- Screen media and input use a second connection on the same TCP port. The
  encrypted primary connection delivers a random 32-byte, single-use screen key.
  Its lease is bound to that primary connection, phone, and screen session and
  expires after 60 seconds. The second connection derives fresh directional
  AES-GCM keys; the normal pairing key cannot authenticate it. Inputs carry the
  session ID and display generation so rotation and stopped sessions reject
  stale input. Media queues are bounded and recover video at a new keyframe.
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
- Screen sessions use `hello{deviceId, nonce, screen}` on a second socket,
  authenticated with the single-use key delivered by `screen.start`. The
  challenge/auth/welcome exchange and encrypted framing remain the same.
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
- Primary connection, PC → phone: `screen.start{session, key, audio}` and
  `screen.stop{session}`. Phone → PC: `screen.state{session, state, generation,
  detail, control, keyboard, audio}`.
- Screen connection, phone → PC: `screen.state`,
  `screen.video.config{session, generation, width, height}` (Annex-B H.264
  configuration), `screen.video{session, generation, pts, keyframe}` (H.264),
  `screen.audio.config{session, generation}` (AAC AudioSpecificConfig), and
  `screen.audio{session, generation, pts}` (AAC-LC). Timestamps are microseconds.
- Screen connection, PC → phone: `screen.input{session, generation, action,
  x, y, value, text}`. Coordinates are millionths of the mirrored screen;
  UTF-8 text is limited to 16 KB. Media packets are capped at 2 MB, audio
  packets at 64 KB, codec headers at 64 KB for video and 64 bytes for audio.
- `status.features` lists what the phone app has turned on (`notification.actions`,
  `files.receive`, `ring`, `media`, `sms`, `calls`, `storage`, `screen`,
  `screen.control`, `screen.keyboard`, `screen.audio`), so the PC can explain what
  to enable. Both sides ignore message types they do not know, so older apps keep working.
  Active-session input and audio readiness are reported separately in `screen.state`.

Screen fixtures and queue bounds are covered by `.../ScreenTest.kt` and the native
protocol suite. `native/tests/phone_screen_tests.cpp` adds real Windows codec
verification and the Phone Screen development harness.

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
