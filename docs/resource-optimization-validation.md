# Rendering and resource optimization validation

Validated on 2026-10-05 on Windows 11 Pro 10.0.26200, an AMD Ryzen 7 8845HS
(16 logical processors), and approximately 16 GB RAM. The baseline includes
the repository's existing uncommitted changes, including its earlier search
optimizations. Source snapshots, executable hashes, reports and render
fixtures are retained locally under `build-native/resource-optimization`.

## Changes

- The launcher and Phone window reuse DirectWrite layouts for short text.
  Cache hits borrow their lookup text instead of building composite string
  keys. Exact dimensions, font identity and layout options distinguish entries.
  The launcher caps entries at 256 and retained text at 32,768 UTF-16 code
  units; the Phone window uses 128 entries and 16,384 units. Strings longer
  than 1,024 units bypass retention. Model changes, format changes and window
  closure clear cached text. Formats are retained until eviction to prevent
  an address reused by a new format from selecting an old layout.
- The phone listener waits for socket events instead of checking every
  500 ms. Its screen command worker waits for work or the current request's
  deadline instead of checking every 100 ms. Screen request deadlines use a
  monotonic clock, and cancellation, authentication, disconnect and shutdown
  wake the worker. Periodic discovery beacons and session timeouts retain
  their existing behavior.
- Outbound phone frames gather their header and encrypted body in one send
  without a second frame-sized buffer. AES-GCM output reserves space for its
  tag before encryption, avoiding another ciphertext allocation and copy.
  Encryption, counters, message contents and the wire protocol are unchanged.
- Android photo thumbnails and temporary scaled album art release their pixel
  buffers immediately after compression. Metadata-owned album art remains
  available to its owner. Media updates compare structured state before JSON
  serialization and reuse the current app label. Position changes that
  represent seeks are sent; ordinary playback progress is suppressed, with
  resynchronization when playback speed or the wall clock changes.

## Measurements and checks

| Check | Result |
| --- | --- |
| Native x64 Release build, `/W4 /WX` | Passed |
| Native CTest suite | 27/27 passed after the final cache-lifetime changes; 12.31 seconds |
| Repeated layout work: 32 labels × 100 frames, 12 warm samples | Uncached p95 57.336 ms; cached p95 0.705 ms |
| Original/cached software render fixtures | Exact pixel parity for all six DPI/text-size combinations |
| Idle phone service shutdown, three runs | 0.38 / 0.32 / 0.36 ms |
| Search scoring p95, 5,000 / 50,000 entries | 6.37 / 19.98 ms; existing budgets passed |
| Root search pipeline p95, 50,000 entries | 50.00 ms collapsed; 57.34 ms expanded; budget 100 ms |
| Apps scope pipeline p95, 50,000 entries | 4.46 ms; budget 25 ms |
| Warm file-content search, 50,000 files | p95 22.80 ms; budget 75 ms |
| Android protocol tests | 17 passed |
| Android app unit tests | 5 passed, including progress, seeks, volume, metadata, reset and clock changes |
| Android release lint | 0 errors; 9 existing warnings |
| Android release build and APK signature verification | Passed; APK signature v2 verified |
| Local ZIP and NSIS installer | Generated; ZIP hashes match all three native executables and the current APK |

The layout comparison measures layout creation, shaping and metrics retrieval,
not complete rendering or input-to-display latency. Software fixtures cover
representative text roles, rather than a capture of every live app screen.
The phone loopback suite covers pairing, encrypted sessions, a separate screen
channel, concurrent authentication, pending-screen cancellation, empty files,
a 1 MiB legacy binary transfer, streaming negotiation and delivery receipts.

## Resource samples and artifacts

Read-only process samples use the existing preferences and the sampler in
`scripts/measure-idle.py`. Phone Connection and clipboard history are enabled;
file indexing, content indexing and diagnostics are disabled. Search learning
uses its disabled default when absent from the stored settings. Preferences
were not changed for measurement. CPU percentages are relative to one logical
core. Private memory differs from the process working set.

The baseline 45-second interval used 78.125 ms CPU (0.174% of one core) and
ended at 71.7 MiB private memory. The first restarted optimized build's
45-second interval used 31.25 ms CPU (0.069%) and ended at 35.9 MiB private
memory. The baseline had a longer process lifetime, and live window/connection
states were not captured; this comparison does not isolate the source changes
as the cause of the memory difference. After the final cache-lifetime changes,
another 45-second sample used 31.25 ms CPU (0.069%) and ended at 35.4 MiB
private memory and 58.2 MiB working set, with no process file-read or file-write
bytes during the interval. Final-build counters and identity are recorded in
`idle-after.json` and `running-build.json`. These are local samples, not
universal performance guarantees.

The initial optimization build was started from `build-native/FeatherCast.exe`,
PID 31088, at 18:54:42 CEST. Its SHA-256 was
`2AB1E6954A499DEC03596FF4244B1EB17183447FE2AB2ABE1FDC4DDD9B7FE595`.
The signed development companion is available at
`build-native/FeatherCast-Phone.apk`. Packaging is local; no release is published
or installer installed on the user's machine.

No physical Android device was attached. Android memory profiling, real Wi-Fi
transfers, media control on device, live Narrator checks and physical shortcut
latency remain device/manual checks. Existing accessibility, High Contrast,
motion, search and lifecycle tests pass; they do not replace those checks.

## Opaque background follow-up

The requested desktop appearance now uses solid neutral dark grey `#191919`
for overlay and settings backgrounds. Theme normalization makes these roles
opaque before computing contrast, including custom themes with an alpha byte.
Rounded edges, accents, layout, reveal/dismiss motion and the functional capture
selection overlay retain their existing behavior.

Desktop blur, its undocumented accent policy, all three backing windows, and
their per-frame positioning/region work have been removed. Runtime enumeration
confirms the main window is present, zero `FeatherCastBlurWindow` instances
exist, and launcher, settings, volume and recording windows use `DWMSBT_NONE`.
The native Release build and all 27 CTest tests pass (12.62 seconds), including
contrast, rendering, accessibility and lifecycle coverage. ZIP and NSIS packages
were refreshed; the ZIP launcher hash matches the running build.

The current build was restarted at 19:09:51 CEST, PID 36684, SHA-256
`CEB396792037502A6FF70FECCD1DE6E4FEF883EF928307BB2619F87B8CD6B3BE`.
Source snapshots, build/test logs, window state and package verification are
under `build-native/opaque-background`. This change removes compositor work;
its effect on GPU utilization, frame times and power consumption was not
measured. The idle measurements above belong to the earlier optimization build.
