# FeatherCast 0.10.1

## Changes

- **Elevated Input Broker Architecture (`InputBroker.exe`)**:
  - Introduced a dedicated input broker process running at elevated integrity (`requireAdministrator`) to reliably intercept the Windows key even when elevated applications have foreground focus.
  - Communicates with the unprivileged FeatherCast launcher over a secure local named pipe (`\\.\pipe\FeatherCastInputBroker`).
  - Gracefully falls back to local keyboard hooks when the broker is unavailable.

- **Windows-Key Shortcut Interception**:
  - Improved Windows-key chord suppression and dummy key injection to prevent the Windows Start menu from opening during launcher activation.
  - Added dynamic hook management and messaging between the launcher and broker (`WM_BROKER_STATE_CHANGED`, `WM_APP_WINKEY_TRIGGER`).

- **Packaging & Tooling**:
  - Packaged `InputBroker.exe` alongside `FeatherCast.exe` and `FeatherCastPluginHost.exe` in both the standard NSIS installer and portable ZIP distributions.
  - Added `--self-test` support across all executables for automated CI and packaging verification.

## Release Notes

This release includes both the standard NSIS installer (`FeatherCast-0.10.1-win64.exe`) and the standalone portable package (`FeatherCast-0.10.1-win64.zip`), along with their respective SHA-256 checksum sidecars. If repository signing is not configured, the generated binaries are unsigned and intended for manual installation.

## Local verification

- 2026-09-22: Release build completed and all 21 CTest targets passed on Windows.
- Accessibility coverage includes MSAA plus the native UI Automation bridge and
  Invoke, Value, RangeValue, Toggle, and SelectionItem provider patterns.
- Text scaling and layout contracts are covered through 200%, including the
  44-DIP minimum targets for launcher settings, settings close, and recording
  controls.
- The manual Windows gates in `docs/release-qa-checklist.md` remain required on
  the packaged release candidate; this source-tree verification does not claim
  Narrator, Accessibility Insights, signing, or installer results.
