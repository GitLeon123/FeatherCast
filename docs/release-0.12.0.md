# FeatherCast 0.12.0

FeatherCast 0.12.0 is a polish and hardening release: a more deliberate launcher
and Settings experience, a refreshed Android companion, and a large batch of
correctness fixes from code review.

## Changes since 0.11.1

- Polish the search overlay: input, hover, Esc, busy and copy feedback, shortcut
  hints, and ranking. Hover hints ("Enter open", "Tab actions") are now centred
  vertically in their row.
- Give the Settings, Phone, Phone Screen, Library, and screenshot editor windows
  a more premium feel, with consistent motion and visual tokens.
- Make the Phone windows friendlier for keyboards, screen readers, and high
  contrast.
- Improve the Android companion UI, consent flow, theme, and accessibility. Ship
  Android companion 1.3.0 (version code 5).
- Harden phone link calls, sharing, transfers, and handshakes; guard phone volume
  steps and long phone names; keep expiring phone requests when the beacon socket
  fails.
- Run `>program.exe` as a command instead of opening it as a URL, gate IME
  composition like typed characters, and reject a second alias for an app that
  already has one.
- Fix confirmed code review findings across rendering device recovery, file corpus
  rebuilds, clipboard store results, UI Automation range values, and version
  parsing.

## Downloads

- `FeatherCast-0.12.0-win64.exe`: Windows installer.
- `FeatherCast-0.12.0-win64.zip`: portable Windows package.
- `FeatherCast-Phone.apk`: signed Android companion, also included in both
  Windows packages.
- SHA-256 sidecars for each download.

## Verification

Local Windows Release build passed with warnings treated as errors, and all 30
CTest targets passed. Physical Android device testing, interactive screen-reader
checks, and mixed-monitor visual checks were not performed.
