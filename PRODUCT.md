# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows

## Users

People who want a clean, premium-feeling launcher for quickly launching apps
and other useful actions, without giving up system performance.

## Product Purpose

FeatherCast is a lightweight native Windows launcher. A global shortcut opens a
compact search overlay where people can find installed apps, open windows, and
useful local utilities, then launch, focus, or execute the selected result.
Success means making frequent desktop actions feel immediate and polished
while remaining effectively invisible when idle.

## Positioning

FeatherCast's defining position is premium launcher utility with an unusually
small performance footprint. The user-stated indicative envelope is about 0%
CPU and 40 MB RAM while running in the background ready to search, and about 1%
CPU and 60 MB RAM during active search; these figures are performance targets
or claims to validate on representative hardware, not universal guarantees.

## Operating Context

FeatherCast is used from anywhere in Windows through a configurable global
shortcut (Alt+Space by default). It normally remains available from the system
tray, opens a centered keyboard-first overlay, and returns focus to the relevant
window after an action or dismissal. People can also open Settings from the
overlay or tray to configure shortcuts, appearance, privacy choices, library
content, updates, and optional background features.

## Capabilities and Constraints

- The current native implementation searches Start Menu and AppsFolder entries,
  open windows, locally installed games, optional indexed local files, commands,
  clipboard history, and snippets through scoped and fuzzy search.
- It provides launch/focus actions, window arrangement, previews, calculator and
  conversion utilities, emoji and symbols, timers and a stopwatch, media and
  volume commands, Windows Settings shortcuts, screenshots, silent screen
  recordings, snippets, quicklinks, and a searchable feature guide.
- Settings, snippets, themes, plugins, recent usage, and privacy choices persist
  locally under the user's Windows profile. Clipboard history and file/content
  indexing are explicit opt-in features; sensitive local data is protected where
  applicable and is not sent to an AI service.
- The app is Windows-only and must remain a native C++/Win32 application using
  native Windows interaction and rendering APIs. Do not add Electron, WebView,
  Qt, or a Node runtime without explicit approval.
- The product has no accounts, AI chat, AI provider settings, or network AI
  calls. Do not reintroduce them unless explicitly requested.
- The experience is keyboard-first but must remain usable with pointer input,
  Narrator/UI Automation, High Contrast, reduced-motion settings, IME input,
  and 100%, 150%, and 200% display scaling across supported Windows displays.
- Native plugins run with the current user's permissions and are an explicit
  trusted-source choice; the plugin host provides crash/resource isolation but
  is not a security sandbox.
- Update checks and currency-rate retrieval are limited to the existing
  documented services and trust checks; no new cloud dependency should be
  assumed.

## Brand Commitments

- Product name: FeatherCast.
- The experience should feel clean, premium, quick, and unobtrusive.
- The product's credibility rests on low resource use, native Windows behavior,
  and responsive interaction rather than account-based or AI functionality.
- UI text and comments remain in English.

## Evidence on Hand

- `README.md` documents the product promise, workflows, feature set, shortcuts,
  privacy behavior, and local storage boundaries.
- `docs/architecture.md` documents the native Win32/C++23 architecture, service
  ownership, threading, persistence, and compatibility boundaries.
- `docs/manual-regression-checklist.md` documents keyboard, pointer,
  accessibility, scaling, motion, capture, persistence, and performance
  acceptance checks.
- `docs/release-0.9.0.md` contains current release-candidate measurements and
  explicitly warns that machine-specific measurements are not universal claims.
- `native/src/main.cpp`, `native/src/ui.cpp`, and related files are the current
  implementation evidence; `native/assets/icon.png` and the generated build
  icon are the current identity assets.
- No testimonials, external user research, customer logos, or other marketing
  proof are on hand. Future work must not fabricate them.

## Product Principles

1. Make frequent desktop actions immediate.
2. Keep the launcher’s idle footprint negligible and active work bounded.
3. Preserve native Windows behavior and accessibility as part of product quality.
4. Keep sensitive features local, explicit, and understandable.
5. Offer broad utility without obscuring the primary launch-and-search job.

## Accessibility & Inclusion

Keyboard-first interaction is required, with pointer parity where practical.
The product must support Narrator/UI Automation, visible focus and stable
navigation, High Contrast, reduced-motion settings, IME composition, and
mixed-DPI display scaling. Accessibility behavior is a release-gate concern,
not a later visual enhancement.
