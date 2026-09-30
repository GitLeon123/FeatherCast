# FeatherCast 0.11.0

FeatherCast can now connect to an Android phone over the local network. Phone
Connection is off by default; enable it and pair with the QR code in the Phone
window. The companion APK is included in both Windows packages and is available
as a separate release download.

## Changes

- Browse phone notifications, photos, clipboard, media, messages, and files from
  the launcher or Phone window. Reply to notifications, transfer files both ways,
  ring the phone, and control media. SMS, call controls, and storage browsing are
  optional features with separate Android permissions.
- Encrypt phone sessions with AES-GCM and store pairing keys with Windows DPAPI
  and Android Keystore. Pairing invites expire after five minutes and are single use.
- Show phone photos in a thumbnail grid with keyboard navigation.
- Preserve motion velocity during scrolling and resizing, improve interrupted
  transitions, and align animation timing with the compositor when available.
- Fix phone socket ownership during shutdown, revoke pending handshakes when
  unpairing, and prevent concurrent requests from reusing a pairing invite.
- Limit connection workers and file reads; keep queued transfers attached to
  their original session and clear phone data when Phone Connection is disabled.
- Test Android protocol and release lint in CI, sign release APKs with the
  persistent release key, and include the companion in Windows release packages.

## Downloads

- `FeatherCast-0.11.0-win64.exe`: Windows installer.
- `FeatherCast-0.11.0-win64.zip`: portable Windows package.
- `FeatherCast-Phone.apk`: signed Android companion (Android 8 or newer).
- SHA-256 sidecars for each download.

The Windows binaries are **unsigned** and require manual installation. Builds
without Authenticode signer pins open the release page for updates. The Android
APK is signed with the persistent FeatherCast Phone release key.

## Verification

Local Windows Release build passed with warnings treated as errors. All 23 CTest
targets passed, including phone crypto vectors, loopback sessions, concurrent
pairing, pending-handshake revocation, restart, and phone-store coverage.
Android protocol tests and release lint passed; APK signature verification passed.
Local ZIP and NSIS packaging succeeded; extracted binaries passed startup
self-tests and the ZIP companion matched the signed APK.

Search p95 was 6.62 ms at 5,000 items and 20.31 ms at 50,000 items. Warm file
search p95 was 28.26 ms at 50,000 documents, within the 75 ms limit.

Physical Android device testing, Narrator/Accessibility Insights sessions, and
mixed-DPI display checks were not performed in this automated review because
those devices and interactive Windows testing surfaces were unavailable. Native
accessibility, interaction, and layout tests passed. Local installer execution was
left to the isolated CI runner to avoid changing this machine's install registration.

See [Phone Connection](phone-link.md) for setup and feature permissions and
[code review](code-review-0.11.0.md) for the review findings.
