# FeatherCast 0.9.6

## Changes

- Adds a native Volume Control with live default-output detection, percentage
  adjustment, mute/unmute, keyboard and mouse interaction, and delayed writes
  that commit the final slider position cleanly.
- Improves volume accessibility by exposing separate slider and mute controls,
  focus state, mute state, and useful output-device descriptions to MSAA
  clients.
- Stabilizes fresh launcher selection so reused result snapshots cannot restore
  a stale selection, preview, or selection pill after a new overlay opens.
- Improves screenshot toolbar layout across wide and virtual multi-monitor
  capture surfaces.
- Brings the Library manager and editor in line with the active theme and
  high-contrast mode, including inline save/validation status instead of
  interrupting message boxes.
- Raises secondary-text contrast and adds contrast enforcement for readable
  themes, with regression coverage for the new UI behavior.
- Keeps CPack output isolated under the build package directory and expands
  release/manual regression coverage for volume, accessibility, selection, and
  packaging behavior.

## Release notes

This release is built and published by the tagged GitHub Actions workflow. If
the repository signing configuration is not enabled, the generated Windows
packages are unsigned and are intended for manual installation.
