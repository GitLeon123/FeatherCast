# FeatherCast

<p align="center">
  <img src="build/icon.png" alt="FeatherCast Logo" width="128" height="128" />
</p>

<h3 align="center">Ultra-Fast, Keyboard-Driven Native Windows Launcher & Productivity Toolkit</h3>

<p align="center">
  A lightweight, privacy-focused <strong>Raycast / Spotlight alternative for Windows 10 & 11</strong>.<br />
  Built from scratch in native C++23 and Direct2D — zero Electron, zero web runtimes, zero unnecessary bloat.
</p>

<p align="center">
  <a href="https://github.com/GitLeon123/FeatherCast/releases/latest"><img src="https://img.shields.io/github/v/release/GitLeon123/FeatherCast?color=2ea44f&label=Latest%20Release" alt="Latest Release" /></a>
  <a href="https://github.com/GitLeon123/FeatherCast"><img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20(x64)-0078D6?logo=windows&logoColor=white" alt="Platform: Windows 10 | 11" /></a>
  <a href="https://en.cppreference.com/w/cpp/23"><img src="https://img.shields.io/badge/Standard-C%2B%2B23-00599C?logo=c%2B%2B&logoColor=white" alt="C++23" /></a>
  <a href="#"><img src="https://img.shields.io/badge/UI-Direct2D%20%2F%20DirectWrite-7928CA" alt="Direct2D & DirectWrite" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT" /></a>
</p>

---

## 📑 Table of Contents

- [Overview](#overview)
- [Why FeatherCast?](#why-feathercast)
- [Key Features](#key-features)
  - [Instant App & Game Discovery](#instant-app--game-discovery)
  - [Window Management & Multitasking](#window-management--multitasking)
  - [Scoped Search & Instant File Previews](#scoped-search--instant-file-previews)
- [Clipboard History & Snippets](#clipboard-history--snippets)
  - [Productivity Utilities & Quick Calculations](#productivity-utilities--quick-calculations)
  - [Screen Capture & Silent Screen Recording](#screen-capture--silent-screen-recording)
  - [Accessibility, Typography & Theming](#accessibility-typography--theming)
  - [Native Plugin Architecture](#native-plugin-architecture)
  - [Phone Connection](#phone-connection)
- [Search Scopes & Cheat Sheet](#search-scopes--cheat-sheet)
- [Keyboard Shortcuts](#keyboard-shortcuts)
- [Download & Installation](#download--installation)
- [Building from Source](#building-from-source)
- [Data Storage, Privacy & Security](#data-storage-privacy--security)
- [Documentation & Links](#documentation--links)
- [License](#license)

---

## 🌟 Overview

**FeatherCast** brings the speed and elegance of modern keyboard-first launchers (such as Raycast or macOS Spotlight) natively to Windows. Open the centered search overlay with a single keystroke (`Alt+Space` by default or custom `Win` key combinations), immediately launch apps and games, jump between open windows, inspect local files, capture annotated screenshots, or compute conversions in real time.

FeatherCast uses **C++23**, **Direct2D** hardware acceleration, **DirectWrite** typography, and native Win32 APIs.

> [!NOTE]
> **Local Architecture**: Search runs locally, with no telemetry or mandatory cloud account. Optional update checks, currency rates, and phone connections use the network; their behavior is described below.

---

## ⚡ Why FeatherCast?

| Attribute | FeatherCast |
| :--- | :--- |
| **UI framework** | Direct2D, DirectWrite and Win32 |
| **Search** | Local fuzzy search with separate, reproducible search benchmarks |
| **Background work** | Inactive rendering is suspended; enabled features can still perform work |
| **Privacy** | No telemetry or mandatory accounts; clipboard history, file indexing and query learning are opt-in |

Memory, CPU and latency depend on hardware, indexed data and enabled features. See the [performance measurement procedure](docs/performance.md) for separate search, idle, startup and shortcut measurements.

---

## ✨ Key Features

### 🚀 Instant App & Game Discovery
- **Comprehensive App Indexing**: Automatically scans Start Menu shortcuts (system & per-user) and Windows `AppsFolder` packages with intelligent deduplication and shell icon caching.
- **Zero-Login Game Launcher**: Detects installed games across **Steam, Epic Games, GOG Galaxy, EA app, Ubisoft Connect, Battle.net, and Xbox / Microsoft Store** directly from local registry keys and launcher manifests. No accounts, no credentials, and no external API requests needed.
- **Dedicated Games Hub**: Type `@games` or launch the dedicated Games browser to view your complete local library with provider badges and cached cover art.

### 🪟 Window Management & Multitasking
- **Fuzzy Window Switching**: Search all active top-level windows and switch immediately—even restoring minimized windows smoothly into the foreground.
- **Snap & Move Actions**: Open result actions (`Tab`, `Right`, or `Ctrl+K`) on any window result to tile it to the left or right half, center it on screen, or move it to the next connected monitor.

### 🔍 Scoped Search & Instant File Previews
- **Modular Search Scopes**: Narrow down results instantly by typing leading tokens:
  - `@apps` — Installed applications and system utilities
  - `@games` — Locally installed PC games
  - `@files` — Indexed local files and folders
  - `@windows` — Currently open desktop windows
  - `@commands` — System actions, settings, and media commands
  - `@clipboard` — Secure clipboard history entries
  - `@snippets` — User-defined text expansions
- **Recursive Local File Indexer**: Explicit opt-in indexing for selected local directories (defaults to Desktop, Documents, Downloads). Ignores system, hidden, and reparse points to keep disk I/O low.
- **Visible index coverage**: The default limit retains the newest 5,000 eligible entries. Settings reports when that limit is reached; an empty `@files` search offers the relevant indexing settings.
- **Privacy-Safe Full-Text Search (FTS5)**: Fast content matching for text and source code (up to 2 MiB per file, capped at 256 MiB total token database). Stores only search tokens, never raw file contents or text excerpts.
- **Instant Preview Pane (`Ctrl+Space`)**: Inspect file metadata, highlighted source code, or images (BMP, GIF, ICO, JPEG, PNG, TIFF up to 25 MiB and 40 MP) without launching external programs.

### 📋 Clipboard History & Snippets
- **Encrypted history**: Optional clipboard history uses user-scoped Windows DPAPI encryption at rest. DPAPI protection is tied to the Windows user account; hardware-backed protection is not guaranteed. User-authored snippets remain readable JSON.
- **Favorites & Pinning**: Pin up to 100 frequent snippets or credentials. Pinned items stay at the top and survive automatic history cleanup.
- **Custom Snippets & Quicklinks**: Manage text expansions and custom URL shortcuts in the native Library manager (**Settings > Library**) or edit `%APPDATA%\FeatherCast\snippets.json`.
- **Search preferences**: Exact aliases and exact names take priority. Optional local query learning remembers up to 256 choices; **Reset Ranking** forgets an item's learned choices and recent usage.
- **Scripts, workspaces and command shortcuts**: Search **Manage Automation** to add PowerShell files, groups of launch targets, or global command shortcuts. See [automation](docs/automation.md).

### ⚡ Productivity Utilities & Quick Calculations
- **Natural Language Calculator & Conversions**: Real-time math evaluation, unit conversion, and cached currency conversion (rates fetched from `open.er-api.com`). When reopening the search overlay, your last expression is preserved and pre-selected.
- **Background Timers & Persistent Stopwatch**:
  - Run named timers via simple syntax: `timer 10m Tea` or `timer 1h 30m Break`.
  - Timers continue tracking across Windows sleep cycles and launcher restarts. Overdue timers trigger native Windows toast notifications upon return.
  - Stopwatch auto-saves state when FeatherCast closes and resumes upon launch.
- **System & Media Controls**: Mute, adjust volume, play/pause media playback, show desktop, or deep-link directly into specific Windows Settings pages.
- **Developer Helpers**: Instant generation of UUIDs, Unix timestamps, ISO week numbers, and date/time calculations.

### 📸 Screen Capture & Silent Screen Recording
- **Native Screenshot Studio**: Press `Print Screen` or your configured shortcut to trigger a cross-monitor region selector.
  - Built-in vector markup editor: draw boxes, ovals, lines, arrows, freehand markers, and text.
  - Privacy tools: Non-destructive blur and pixelate tools for redacting sensitive information.
  - Full undo/redo (`Ctrl+Z`, `Ctrl+Y`), 8-handle resizing, and clean PNG output to `Pictures\FeatherCast`.
  - Clipboard-only mode: Press `Ctrl+C` to copy the annotated screenshot directly to the clipboard without saving any file to disk.
- **Silent Screen Recording**: High-performance, hardware-accelerated H.264 MP4 screen recordings saved to `Videos\FeatherCast`. Includes an accessible, non-activating floating toolbar (Pause, Resume, Stop) that stays excluded from the recording canvas.

### ♿ Accessibility, Typography & Theming
- **WCAG AA Contrast Guarantee**: Fully contrast-normalized Direct2D palette guaranteeing 4.5:1 contrast for body text and 3.0:1 for interactive badges and controls.
- **Custom Typography Scaling**: Adjust font sizing from `90%` to `200%` in 10% steps with live DirectWrite layout recalculation.
- **Native Windows Accommodations**: Full support for Windows High Contrast mode, UI Automation / Narrator screen readers, Windows accent color synchronization, custom color overrides, and reduced-motion preferences.

### 🧩 Native Plugin Architecture
- **Out-of-Process Isolation**: Plugins run in an isolated native companion process (`FeatherCastPluginHost.exe`). If a plugin crashes or hangs, FeatherCast remains unaffected.
- **C/C++ Plugin API**: Write lightweight native DLLs with complete type safety. See [docs/plugin-development.md](docs/plugin-development.md) for full specifications.

---

### 📱 Phone Connection
- **Pair in seconds**: Open **Phone** from the tray menu or search, install the Android app by scanning the download QR code, then scan the pairing QR code in the app.
- **See your phone on the PC**: Notifications (messages, mail, …), your newest photos, the phone clipboard, and battery level in one window.
- **Right in the search panel**: Search **Phone Notifications**, **Phone Photos** (thumbnail grid), or **Phone Clipboard** to browse, filter, copy, paste, or open phone data without leaving the launcher.
- **Both directions**: Text copied on the PC lands on the phone; share text, photos, or files from the phone with **Send to PC** (saved to `Downloads\FeatherCast`).
- **Control your phone from the keyboard**: Reply to notifications and text messages, send files with **Send to → FeatherCast Phone**, make a lost phone ring with **Find My Phone**, pause or skip music with **Phone Media**, reject or silence incoming calls, and browse and download phone files with **Phone Files**.
- **Local and encrypted**: Wi-Fi only, no cloud. Pairing uses ECDH keys from the QR code and every message is AES-256-GCM encrypted. See [Phone Connection](docs/phone-link.md).

---

## 🎯 Search Scopes & Cheat Sheet

Type `@` in the search bar or use any of the dedicated scope tokens to filter your query:

| Scope Prefix | Filter Target | Example Queries |
| :--- | :--- | :--- |
| *(None / Root)* | Unified search across apps, games, windows, math, and commands | `Spotify`, `250 USD in EUR`, `timer 25m Pomodoro` |
| `@apps` | Installed desktop applications & Store apps | `@apps Terminal`, `@apps VS Code` |
| `@games` | Installed games (Steam, Epic, GOG, EA, Battle.net, Xbox) | `@games Cyberpunk`, `@games Hades` |
| `@files` | Indexed local files, folders & FTS text contents | `@files .bashrc`, `@files budget 2026` |
| `@windows` | Open desktop windows | `@windows Chrome`, `@windows Discord` |
| `@commands` | System actions, volume, media & Windows settings | `@commands Display Settings`, `@commands Volume Up` |
| `@clipboard` | DPAPI-encrypted clipboard history entries | `@clipboard invoice`, `@clipboard auth token` |
| `@snippets` | Custom text expansions & templates | `@snippets email-signature`, `@snippets meeting-notes` |

---

## ⌨️ Keyboard Shortcuts

### Search & Navigation
| Action | Key Combination |
| :--- | :--- |
| **Open / Close Overlay** | `Alt+Space` *(Default, customizable; supports `Win` combinations)* |
| **Move Selection** | `Up` / `Down` |
| **Launch App / Focus Window** | `Enter` |
| **Run as Administrator** | `Ctrl+Shift+Enter` |
| **Open Action Panel** | `Tab`, `Right`, or `Ctrl+K` |
| **Return / Step Back** | `Left` or `Esc` (clears active scope first) |
| **Dismiss Overlay** | `Esc` |
| **Open Settings** | Gear icon or System Tray menu |
| **Interactive Feature Guide** | Search `help` or `Discover FeatherCast` |

### File Previews
| Action | Key Combination |
| :--- | :--- |
| **Toggle File Preview** | `Ctrl+Space` |
| **Scroll Preview Content** | `Ctrl+PageUp` / `Ctrl+PageDown` (or Mouse Wheel) |

### Screenshot Editor
| Action | Key Combination |
| :--- | :--- |
| **Launch Region Capture** | `Print Screen` *(or custom configured shortcut)* |
| **Copy Annotated Image to Clipboard** | `Ctrl+C` |
| **Save PNG to Disk** | `Ctrl+S` *(Saved to `Pictures\FeatherCast`)* |
| **Undo / Redo Markup** | `Ctrl+Z` / `Ctrl+Y` |
| **Cancel Drawing Gesture / Close** | `Esc` |

> [!TIP]
> **Windows Print Screen conflict**: If Windows opens the default Snipping Tool when pressing `Print Screen`, navigate to **Windows Settings > Accessibility > Keyboard** and disable *"Use the Print Screen button to open screen snipping"*.

---

## 📦 Download & Installation

### Option 1: Official GitHub Release (Recommended)
Download the latest verified release from the [FeatherCast Releases page](https://github.com/GitLeon123/FeatherCast/releases/latest):
- **Standard Installer**: `FeatherCast-<version>-win64.exe` (NSIS installer with Start Menu integration). In-app update installation requires a signed installer and matching signer pins compiled into FeatherCast. Unsigned builds can open the release page for manual updates.
- **Portable ZIP**: `FeatherCast-<version>-win64.zip` (Extract anywhere and run `FeatherCast.exe` without installation).

### System Requirements
- **Operating System**: Windows 10 (version 1809 or higher) or Windows 11 (64-bit).
- **Hardware Architecture**: x64 (AMD64 / Intel 64).
- **Dependencies**: None. The executable links statically to the MSVC C++ runtime—no Visual C++ Redistributable or .NET runtime installation required.

---

## 🛠️ Building from Source

### Prerequisites
1. **Windows 10 / 11 (64-bit)**
2. **Visual Studio 2022 (v17.0+) or Visual Studio 2026** with the *Desktop development with C++* workload
3. **CMake 3.22 or newer**
4. *(Optional)* **NSIS 3.x** if generating installer packages via CPack

### Build Steps

Clone the repository and build via CMake presets:

```powershell
# 1. Clone the repository
git clone https://github.com/GitLeon123/FeatherCast.git
cd FeatherCast

# 2. Configure build using the x64 Windows preset
cmake --preset windows-x64

# 3. Compile the Release binaries
cmake --build --preset release

# 4. Run automated unit and core search test suites
ctest --preset release

# 5. (Optional) Build standalone installer and portable ZIP packages
cpack --config build-native/CPackConfig.cmake -C Release
```

The compiled standalone executable will be located at:
`build-native/Release/FeatherCast.exe`

### Icon Assets Generation
Application icon files (`build/icon.ico` and `build/icon.png`) are automatically compiled into native resources. To regenerate them:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/gen-icons.ps1
```

---

## 🔒 Data Storage, Privacy & Security

FeatherCast stores all data locally under your Windows user profile following native platform conventions:

### Roaming User Configuration (`%APPDATA%\FeatherCast`)
- `settings.json` — Preferences, custom shortcuts, UI appearance, and update settings.
- `snippets.json` — Custom text expansions and user-authored snippets.
- `theme.json` — Optional custom color schemes and UI theme overrides.
- `plugins/` — Installed native plugins (`.dll`).
- `phone-link.dat` — Paired phones and their link keys (DPAPI-encrypted), only when Phone Connection is used.

### Local Machine Cache & Database (`%LOCALAPPDATA%\FeatherCast`)
- `feathercast.db` — SQLite database with WAL journaling:
  - **Clipboard History**: Encrypted with Windows DPAPI (user-scoped master key).
  - **File Index**: SQLite FTS5 contentless token index (no plain-text excerpts saved).
- `icon-cache-native/` — Resolved application and shell PNG icons.
- `updates/` — Downloaded and Authenticode-verified release installers.

### Privacy Guarantees
- **No telemetry**: FeatherCast uses the internet for update checks and currency conversion:
  1. Checking GitHub Releases for app updates (verified via Authenticode certificate thumbprint pinning).
  2. Fetching public foreign exchange conversion rates from `open.er-api.com`.
- **Phone Connection stays in your network**: When enabled, FeatherCast listens on TCP port 47800 in your local network and only talks to phones you paired.
- **Zero Process / Game Polling**: FeatherCast does not poll running processes in loops. App and game discovery run on-demand or upon shell notifications.
- **Resource Suspension**: When the search overlay is closed, query execution, indexing pipelines, and thumbnail decoding are immediately paused or canceled.

---

## 📚 Documentation & Links

- 🏛️ [Architecture & Technical Design](docs/architecture.md) — Deep dive into Win32 threading, Direct2D rendering, and lifetime guarantees.
- 🔌 [Plugin Development Guide](docs/plugin-development.md) — How to build out-of-process C/C++ plugins.
- 🚀 [Release Packaging & Publishing](docs/releasing.md) — Signing, updater manifest creation, and distribution workflow.
- 📱 [Phone Connection](docs/phone-link.md) — Pairing your Android phone, what is shared, and the protocol.
- ✅ [Manual Regression & QA Checklist](docs/manual-regression-checklist.md) — Testing protocols for scaling, accessibility, and input hooks.

---

## 📄 License

FeatherCast is open-source software licensed under the **MIT License**. See the [LICENSE](LICENSE) file for complete details.
