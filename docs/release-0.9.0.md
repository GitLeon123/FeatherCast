# FeatherCast 0.9.0 release candidate

## Changes

- Ordinary global shortcuts use RegisterHotKey. Special combinations, fallback
  handling, and shortcut recording install the keyboard hook only when needed.
- Hidden overlays cancel pending query, preview, and icon work and pause file
  indexing without joining workers. Watchers wait for changes or cancellation.
  Discovery and network maintenance start on use. One low-priority icon worker
  reuses bounded caches; retained search snapshots have a 16 MiB budget.
- Search FeatherCast settings and press Enter to focus the matching control.
  Result labels truncate cleanly and reserve separate space for action hints.
- Named timers support combined h/m/s durations, pause/resume/restart/delete,
  persisted UTC deadlines, and grouped native expiry notifications. The stopwatch
  counts sleep time and is saved paused at normal exit.
- Clipboard favorites retain their IDs when copied again, survive automatic
  cleanup, and have a separate 100-entry limit. Explicit history clearing includes
  favorites. SQLite v4 migrates existing encrypted history with a backup.
- Includes the existing native screenshot and silent screen-recording work.

## Local verification (2026-09-07)

Release build with warnings as errors; all 18 CTest suites passed. Added tests
cover duration parsing, timer actions and restore, simultaneous expiries,
failed-write rollback, migration, favorite retention and limits, setting search,
snapshot memory accounting, timer cancellation, and index pause/resume.
The existing silent graphics test exercises 500 lifecycle cycles, including
96/120/144/192 DPI. ZIP and NSIS packaging and portable app/plugin-host self-tests
passed.

| Measurement | Before | 0.9.0 |
| --- | ---: | ---: |
| Hidden cold-start process CPU, 60 seconds | 0 ms | 0 ms |
| Private bytes at end | 6,463,488 | 6,262,784 |
| Working set at end | 35,090,432 | 21,975,040 |
| Process read/write/other operations during sample | not captured | 0 / 0 / 0 |
| Search p95, 5k entries | not captured in this run | 3.72 ms |
| Search p95, 50k entries | not captured in this run | 16.78 ms |
| Typo search p95, 5k / 50k | not captured in this run | 3.66 / 17.15 ms |
| Warm file-content search p95, 50k documents | not captured in this run | 14.82 ms |

Reproduce hidden sampling with `python scripts/measure-idle.py <pid>`. Start
FeatherCast without `--show`, let initialization settle, and avoid opening it
during the sample. These are single-machine process counters, not physical disk
throughput or scheduler wakeup counts. Working-set differences are sensitive to
Windows memory management and are not a guaranteed saving. A separate sample
started with `--show` was excluded from the hidden comparison.

## Publication checks still requiring manual validation

The desktop automation interface did not expose the tool-window overlay for a
complete live keyboard/visual pass. Physical mixed-DPI monitors, small screens,
Narrator, real suspend/resume, active-recording performance, and game frame-time /
1%-low comparisons have not been verified here. Automated clock/restore and
rendering tests do not replace those checks. ETW wakeup and open/reopen latency
traces were not captured. Keep the GitHub release as a draft until these checks
pass; do not advertise zero gaming impact on all hardware.

Local packages are unsigned. CI signing depends on the repository signing
configuration. Unsigned packages require manual installation; automatic installer
execution remains disabled without the existing signer trust configuration.
