# FeatherCast 0.13.0

FeatherCast 0.13.0 focuses on performance, lower CPU and RAM usage, and a smaller
executable without compromising on features, ranking accuracy, or visuals,
alongside UI polish and Android companion 1.3.1.

## Changes since 0.12.0

- **Faster search and file search**: Optimized fuzzy scoring (`core.hpp`) with
  batched Unicode classification, zero-copy token spans, bitmask/length pruning,
  stack-allocated Damerau-Levenshtein matching, and a reusable bounded search
  worker pool. Improved search p95 latency by 3.7x–4.5x across 5k–50k items with
  bit-identical ranking checksums, and cut warm 50k `@files` FTS p95 latency by
  45%.
- **Near-zero idle CPU and smaller memory footprint**: Converted background
  workers to lazy-spawn threads with automatic idle exit, consolidated file
  index watchers into a single IOCP watcher thread, replaced the timer thread
  with a Windows threadpool timer, switched the InputBroker client to overlapped
  pipe I/O with zero idle polling, and added idle DXGI/DirectComposition surface
  and cache trimming.
- **Smaller binary and deferred system DLLs**: Enabled Release `/Gy /Gw /GF /GL`
  and `/LTCG`, converted large static emoji/symbol/command catalogs into
  `constexpr` tables, removed `<regex>` from game discovery, and delay-loaded
  feature-gated DLLs (`mfplat`, `mfreadwrite`, `winhttp`, `wintrust`, `ws2_32`,
  `iphlpapi`, `powrprof`), shrinking `FeatherCast.exe` by 28.6% (7.14 MB to
  5.10 MB).
- **Rendering and accessibility efficiency**: Replaced the solid-color brush
  cache map with a small rotating pool, bounded Direct2D opacity layers and
  cached rounded photo-tile geometry and ellipsis trimming signs, preserved text
  layouts across result updates, cached accent/DPI/shortcut-hint lookups, added
  O(1) MSAA/UIA child access, and demand-armed the Phone window tick timer.
- **UI polish and Android companion 1.3.1**: Includes launcher/Settings/Phone UI
  polish, broker registration acknowledgement, and Android companion 1.3.1
  (version code 6).

## Downloads

- `FeatherCast-0.13.0-win64.exe`: Windows installer.
- `FeatherCast-0.13.0-win64.zip`: portable Windows package.
- `FeatherCast-Phone.apk`: signed Android companion, also included in both
  Windows packages.
- SHA-256 sidecars for each download.

## Verification

Local Windows Debug and Release builds passed with warnings treated as errors
(`/WX`), all 31 CTest targets passed, all 4 search pipeline checksums matched
baseline bit-for-bit, rendering pixel parity passed, and live hidden-idle
profiling measured 0.0 ms idle CPU and 0 I/O ops over a 15-second window.
