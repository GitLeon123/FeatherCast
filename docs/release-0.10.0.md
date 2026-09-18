# FeatherCast 0.10.0

## Changes

- **Custom Text Size Scaling & Typography**:
  - Added user-configurable font scaling (`90%`, `100%`, `110%`, `120%`) in Settings with live layout recalculation and DirectWrite font regeneration.
  - Standardized font hierarchy and metric tokens across all surfaces with proper single-line ellipsis trimming and wrapped descriptions.

- **Contrast Normalization & Native Design Contract**:
  - Established native UI design baseline tokens and geometry contracts (`DESIGN.md`, `native/src/layout_contract.hpp`).
  - Added alpha-composited contrast calculations guaranteeing WCAG AA (4.5:1) compliance for body/secondary text and 3.0:1 for interactive controls and status badges.
  - Enhanced High Contrast mode to preserve readability and map system window, button, and highlight roles cleanly.

- **Accessible Screen Recording Toolbar**:
  - Added non-activating keyboard focus and MSAA accessibility to recording controls (pause, resume, stop) so shortcuts and screen readers work seamlessly without stealing foreground focus.

- **Enhanced Global Shortcut & Windows-Key Reliability**:
  - Added low-level keyboard hook handling for Windows-key shortcut chords (e.g. `Win+Space`), preventing the Windows Start menu from intercepting or stealing the shortcut.
  - Improved key chord detection and replay for smooth modifier handling.

- **Clipboard Persistence & Cryptographic Hash Fix**:
  - Fixed clipboard content deduplication by replacing certificate hashing with standard CryptoAPI SHA-256 hashing.
  - Added DPAPI fallback support for isolated test environments.

- **App Discovery & Shortcut Disambiguation**:
  - Improved executable path validation and shortcut disambiguation to prevent malformed or duplicate Start Menu shortcuts from polluting search results.

- **Testing & Verification**:
  - Expanded unit and smoke tests covering typography scaling, contrast validation, shortcut runtime behaviors, and screen recording accessibility.

## Release Notes

This release includes both the standard NSIS installer (`FeatherCast-0.10.0-win64.exe`) and the standalone portable package (`FeatherCast-0.10.0-win64.zip`), along with their respective SHA-256 checksum sidecars. If repository signing is not configured, the generated binaries are unsigned and intended for manual installation.
