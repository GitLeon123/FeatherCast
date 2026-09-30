# AGENTS.md

Guidance for coding agents working in this repository.

## What is FeatherCast?

FeatherCast is a Windows-only native C++ app launcher. It opens a compact Raycast/Spotlight-style overlay from a global shortcut, searches installed apps and open windows, and launches or focuses the selected result.

An optional companion Android app (`android/`) links a phone over the local network: notifications (with replies), photos, clipboard, files both ways, find my phone, media remote, SMS, calls, and storage browsing (see `docs/phone-link.md`).

AI chat and all AI provider settings have been removed. Do not reintroduce AI UI, API key storage, or network AI calls unless explicitly requested.

## Tech Stack

- C++23
- Win32 message loop, tray icon, shell APIs, low-level keyboard hook
- Direct2D + DirectWrite rendering
- WIC PNG icon cache
- CMake/CPack build
- Phone app: Kotlin, Jetpack Compose, Gradle (JDK 17, Android SDK 35, minSdk 26)

## Architecture

```
CMakeLists.txt              # Native build, tests, CPack packaging
native/
  FeatherCast.rc.in           # CMake-generated icon and manifest resource template
  app.manifest             # DPI/common-controls manifest
  src/core.hpp             # Pure fuzzy search core used by app and tests
  src/main.cpp             # Win32 app, UI, discovery, settings, icons, tray
  src/phone_protocol.hpp   # Phone link framing, payloads, pairing URI (pure, tested)
  src/phone_messages.hpp   # Session message parsing and PC->phone builders (pure, tested)
  src/phone_crypto.*       # BCrypt ECDH/HMAC/HKDF/AES-GCM and DPAPI helpers
  src/phone_service.*      # TCP 47800 server, UDP 47801 beacon, /app.apk download
  src/phone_ui.*           # "FeatherCast Phone" window (pairing QR, notifications, photos, clipboard)
  src/phone_store.hpp      # In-memory phone data behind the launcher's phone browse views (pure, tested)
  tests/core_tests.cpp     # Core fuzzy/search tests
  tests/phone_protocol_tests.cpp # Phone crypto vectors and loopback pairing/session tests
  tests/phone_store_tests.cpp    # Phone store, photo grid math, and phone search view tests
android/
  protocol/                # Pure Kotlin link protocol, shared test vectors, desktop phone simulator
  app/                     # Android app (app.feathercast.phone); one bridge per feature
                           # (NotifyListener, IncomingFiles, Ringer, MediaWatcher, SmsBridge,
                           # CallWatcher, StorageBridge), dispatched by LinkManager
third_party/qrcodegen/     # Nayuki QR code generator (MIT)
scripts/gen-icons.ps1      # Generates build/icon.ico, icon.png, and Android launcher icons
scripts/setup-android-sdk.ps1 # Installs JDK 17, Android SDK, Gradle to %LOCALAPPDATA%\FeatherCast-dev
scripts/build-android.ps1  # Builds and signs FeatherCast-Phone.apk, copies it next to FeatherCast.exe
build/                     # Generated icon assets used by native resources
```

## Behavior Notes

- Settings, snippets, themes, and user plugins are stored under `%APPDATA%\FeatherCast`.
- Operational data is stored under `%LOCALAPPDATA%\FeatherCast`; clipboard history and file indexing are explicit opt-in features backed by `feathercast.db`.
- App discovery scans Start Menu `.lnk` files and AppsFolder shell entries, then de-duplicates by id/name.
- Icons are lazy-loaded from the Windows Shell and cached as PNG files under `%LOCALAPPDATA%\FeatherCast\icon-cache-native`.
- The low-level keyboard hook handles both global shortcut monitoring and settings shortcut recording.
- The app is single-instance via a named mutex; a second launch asks the existing window to open search (`--phone` opens the Phone window instead).
- Phone Connection is opt-in (`phoneLinkEnabled`). Paired phones are stored DPAPI-encrypted in `%APPDATA%\FeatherCast\phone-link.dat`; files from the phone go to `Downloads\FeatherCast`. Keep the C++ and Kotlin protocol implementations in sync and extend both test suites when the protocol changes.

## Commands

```powershell
cmake -S . -B build-native -G "Visual Studio 18 2026" -A x64
cmake --build build-native --config Release
ctest --test-dir build-native -C Release
cpack --config build-native/CPackConfig.cmake -C Release -B build-native/packages

# Phone app (once: scripts\setup-android-sdk.ps1)
scripts\build-android.ps1
```

## Conventions

- Keep UI text and comments in English.
- Keep the app Windows-native; do not add Electron, WebView, Qt, or Node runtime dependencies without explicit approval.
- Regenerate app icons through `scripts/gen-icons.ps1`; do not hand-edit generated icon files.
- After making app changes, always rebuild and restart FeatherCast with the newest successful build so the user can test the changes immediately.
