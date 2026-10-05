# Search optimization validation

Validated on 2026-10-05 with a native x64 Release build, warnings treated as errors, Windows 11 Pro 10.0.26200, an AMD Ryzen 7 8845HS (16 logical processors), Radeon 780M and approximately 16 GB RAM. The baseline includes the repository's existing uncommitted improvements. Session snapshots and measurement output are retained locally under `build-native/optimization`.

Search now keeps corpus matches as indices through filtering and categorization, copying result payloads only when selecting section results. Scoped queries filter eligible entries before scoring and retain the requested number of best matches. Prepared names and aliases are reused, fields that cannot improve the match class are skipped, and a full-range search avoids building an unnecessary heap before sorting. Equal scores and names use corpus order consistently across capped and parallel searches. Cancellation is checked before search and during corpus filtering.

Common ASCII preparation bypasses Windows Unicode calls. Carets and grave accents continue through the Unicode implementation because Windows classifies them as diacritics. Regression tests compare all 128 ASCII code points, including embedded NULs, against the Windows normalization path and retain the existing Unicode, accent and boundary tests.

## Measurements

The following values come from a consecutive baseline/optimized benchmark pair after compilation and the native test suite finished. Each query is warmed before recording samples. The scoring benchmarks record 20 or 30 samples; the pipeline benchmarks record 20 samples at 5,000 entries and 12 at 50,000 entries. Preparation is one construction measurement, including synthetic item creation, rather than a startup measurement.

The pipeline fixture contains 98% file entries and 2% launchable apps, half of which are games. The query is `application`; the request limit is 100 and the worker cap is two. Root search preserves the existing per-section limits and expansion behavior. Apps scope searches the 1,000 eligible entries in the 50,000-entry fixture.

| Measurement | Baseline | Optimized |
| --- | ---: | ---: |
| Root pipeline p95, 5,000 entries | 52.52 ms | 8.28 ms |
| Root pipeline p95, 50,000 entries, collapsed | 492.40 ms | 46.74 ms |
| Root pipeline p95, 50,000 entries, expanded | 485.45 ms | 49.47 ms |
| Apps scope pipeline p95, 50,000 entries | 48.80 ms | 5.23 ms |
| Scoring p95, 50,000 entries, best 100 | 22.34 ms | 17.50 ms |
| Typo scoring p95, 50,000 entries, best 100 | 20.20 ms | 20.76 ms |
| Scoring/sorting p95, 50,000 entries, full result range | 23.48 ms | 20.33 ms |
| Preparing 50,000 synthetic entries | 410.95 ms | 309.62 ms |

The earlier comparison also measured collapsed root search at 526.93 ms before and 52.54 ms after. Both comparisons produced identical checksums for all four pipeline scenarios, covering section titles, ordered result keys and hidden-result counts. These synthetic timings exclude physical input, icon loading, rendering and monitor presentation. They do not establish an improvement in shortcut-to-visible latency or idle memory.

## Verification

- Complete native Release build with `/W4 /WX`: passed.
- Native CTest suite: 26/26 passed in 13.36 seconds, including search, aliases, clipboard privacy, section expansion, lifecycle, accessibility smoke and phone protocol tests.
- New core regression checks: Windows normalization parity, stronger literal matches after name matches, deterministic capped/parallel ties, eligible-index filtering, invalid/empty candidate lists, zero limits and stale blank queries.
- Updated search benchmark: all scoring and pipeline budgets passed; all four result checksums matched the saved baseline on both comparisons.
- Warm file-content search over 50,000 files: p95 20.80 ms, within the existing 75 ms budget.
- Launcher closed cleanly, rebuilt and restarted from `build-native/FeatherCast.exe`; the new process entered its message loop and remained running through verification.

The running launcher is PID 34020, started at 18:13:24 CEST. Its SHA-256 is `84FDC6158444E1670729CF2A3CC17DFF3396CD8A74CC175A8EF21D912E02F69C`. Local evidence includes `running-build.json`, `ctest.txt`, `search-pipeline-before.txt`, `search-pipeline-after.txt`, both `*-repeat.txt` reports, `file-search-after.txt`, `benchmark-validation.json` and `environment.json` under `build-native/optimization`.
