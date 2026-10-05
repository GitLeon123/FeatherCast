# Measuring FeatherCast

Measure Release builds on a recorded machine and preserve the executable's SHA-256. Record Windows build, CPU, RAM, GPU, display refresh rate, DPI, power mode, animation settings, indexed-item count and enabled features with each result. Repeat samples after changing features. A synthetic search benchmark does not measure startup or hotkey latency.

## Search

Run `FeatherCastSearchBenchmarks.exe` and `FeatherCastFileSearchBenchmarks.exe` from the build directory. They use reproducible synthetic corpora, warm runs and p95 budgets. Preserve their complete output alongside the environment record. These timings cover the search implementation, excluding Windows input delivery, icons, painting and display scanout.

The search benchmark also measures full result assembly with a mixed app/file corpus, collapsed and expanded sections, and the Apps scope. It verifies repeatable result checksums and applies Release p95 budgets of 25 ms for 5,000-entry root search, 100 ms for 50,000-entry root search, and 25 ms for Apps scope over 50,000 entries. These budgets use the same CI scaling as the scoring benchmarks. See [search optimization validation](optimization-validation.md) for the measured comparison against the session baseline.

`FeatherCastSearchOptimizationTests.exe` compares prepared built-in catalogs and
section selection against the uncached, fully sorted search path. It also
reports warm emoji-search p95 for both paths, including result materialization.
These comparative timings have no additional machine-specific pass threshold;
the existing search budgets remain unchanged. See the
[optimization follow-up](optimization-followup.md) for the broader review,
validation, and remaining measurement priorities.

## Text layout and rendering

Run `FeatherCastRenderingPerformanceTests.exe` to check bounded text-layout
caching and compare repeated layout work against the uncached DirectWrite path.
The fixture uses 32 labels over 100 frames and 12 warm samples. Its timings
exclude painting and monitor presentation; they are not complete frame times.

Pass an existing output directory to also save the original and cached render
fixtures as PNGs. The test compares pixels at 96, 144 and 192 DPI, with 100%
and 200% text size, including Unicode, wrapping, clipping and ellipsis.
It also checks exact dimensions, font changes, LRU eviction, aggregate text
bounds, embedded NULs and oversized strings. The app clears layouts when its
displayed data or text formats change and when the launcher or Phone window
closes. See [resource optimization validation](resource-optimization-validation.md)
for the measured results and remaining device checks.

## Idle resources

Let startup work settle, hide all FeatherCast windows and wait 30 seconds. Run:

```powershell
$launcher = Get-Process FeatherCast
python scripts/measure-idle.py $launcher.Id --seconds 60 --label hidden-idle --binary build-native/FeatherCast.exe --output build-native/measurements/idle.json
```

The script reads process counters without changing preferences. CPU percent is relative to **one logical core**, so 100% means one fully occupied core. Private bytes and working set measure different kinds of memory; sampled peaks are not allocation high-water marks. Scenario labels describe operator setup and are not automatically verified.

Repeat with phone connected, clipboard history enabled, indexing active and indexing settled. Record actual enabled features; do not treat one configuration's idle result as a universal memory claim. Avoid compiling, updating or scanning files during an idle sample. Use labels such as `phone-connected-idle` or `file-indexing-active`.

## Startup and shortcut to presentation

Enable **Settings > Privacy > Diagnostics**, restart FeatherCast, and collect `%LOCALAPPDATA%\FeatherCast\performance-debug.log`. Diagnostics are opt-in, bounded and contain durations/counters rather than queries or clipboard text.

- `startup_ready_us`: application constructor body entry to entry into the message loop. Discovery is deferred. This excludes executable loading and should not be called process-start or ready-to-search latency.
- `shortcut_to_present_us`: native shortcut receipt to the first successful overlay presentation. Hook shortcuts include queueing and Windows-key release deferral; registered shortcuts begin when `WM_HOTKEY` reaches the app. This excludes physical key actuation, compositor scanout and monitor response.
- Existing `frame_us` records cover render work. They do not measure the complete input-to-display interval.

Hide the overlay, trigger the configured shortcut, dismiss it, and wait at least one second before repeating. Collect at least 30 successful opens; separate first-open from subsequent opens, and report median/p95 with the shortcut type and display configuration. Test mixed DPI and reduced motion separately. Turn diagnostics off after collecting evidence because logging affects measurements.

For actual key-to-visible latency, use an external high-frame-rate recording showing both input and display, or Windows graphics/input tracing. Keep that evidence separate from the in-app measurements. This repository does not claim a universal sub-16-ms response or compare unmeasured third-party launchers.
