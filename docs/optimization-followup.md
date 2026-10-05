# FeatherCast optimization follow-up

This review extends commit `19f0205` and the earlier
[search](optimization-validation.md) and
[resource](resource-optimization-validation.md) work. It covers the native
launcher and services, Android companion, and build/test tooling. Changes focus
on repeated work, transient memory, cancellation, and allocation costs while
preserving result ordering, privacy boundaries, settings, and phone framing.

## Review and changes

| Area | Finding and action |
| --- | --- |
| Search and result assembly | Root search sorted every corpus match before selecting at most 20–80 unique results per category. It now scores once and ranks the required prefixes within each section. If earlier sections or duplicate keys consume a prefix, selection continues into the remaining matches. Exact names, aliases, learned choices, app/phone priority, section caps, and hidden counts retain their previous behavior. |
| Static catalogs | Emoji, symbols, Settings, and Discover searches rebuilt normalized fields on every query. They now retain prepared fields and use bounded prepared search. Emoji and symbol corpora remain lazy and are released through their existing memory cleanup. The first query pays preparation costs; subsequent queries reuse them. |
| Fuzzy scoring | Missing keyword/process/path fields are omitted instead of retaining empty field objects. Short edit-distance comparisons use fixed stack rows; larger comparisons use one reusable buffer. Only the active band and its boundaries are overwritten per row. Feature gates reuse query words and joined text within one result request. |
| Search scheduling | One scoring range runs on the caller instead of creating another thread while the caller waits. Corpora of 5,000–19,999 eligible entries may use two workers, while larger corpora retain the four-worker automatic cap. Explicit caps and the performance governor still take precedence. Single-worker searches move their score buffer into the result instead of allocating and copying a second buffer. |
| File search and SQLite | Content search is skipped when metadata already fills the result limit. The read-only FTS statement is prepared once, reset and rebound between queries, and finalized before the database closes. SQLite VM progress checks interrupt obsolete queries or shutdown work. Zero and negative result limits return an empty result. Temporary result vectors are moved into sections after flat results are assembled. |
| File indexing | Segment wildcard matching no longer allocates dynamic-programming rows per filename. Recursive path matching retains two rows instead of a matrix. Empty exclusion lists skip path tokenization. Existing crawl limits, exclusions, content reuse, watchers, and unavailable-root handling remain in place. |
| Memory accounting | Snapshot estimates now include original and normalized aliases. The estimate remains conservative; this corrects missing retained text rather than measuring process memory. |
| Android transfers and screen sharing | Streaming sends the used prefix of its reusable chunk buffer directly to payload packing, removing the intermediate chunk copy. The resulting payload owns its bytes before the next read. Length-prefixed AVC packets use one exact-size output copy, and video queue overflow checks use one pass without a temporary filtered list. Encryption, wire bytes, limits, keyframe recovery, and permission checks retain their existing behavior. |
| Rendering, motion, icons, and previews | Existing bounded text layouts, deferred shell icons, UI-thread GPU ownership, demand-loaded previews, and frame gating were retained. Rendering parity, accessibility, and interaction suites remain part of validation. |
| Discovery, persistence, extensions, capture, and lifecycle | Existing coalesced discovery, serialized durable writes, plugin isolation, bounded capture work, opt-in features, and orderly worker shutdown were retained. The review did not justify replacing these mechanisms without workload measurements. |
| Build and generated assets | The emoji generator gained `-ReuseData` to regenerate search code offline while preserving the current data. All 1,914 entries are retained. A new native regression target is registered with CTest; native runtime dependencies and Android signing are unchanged. |

## Regression coverage

`FeatherCastSearchOptimizationTests` compares prepared catalog searches against
the uncached, fully sorted API, including Unicode, punctuation, empty queries,
zero limits, and cleanup/reinitialization. Its root-search reference checks
ordered keys and payloads across collapsed/expanded sections, aliases, exact
names, learned preferences, duplicate-heavy corpora, fallback results, and
5,000/21,000-entry parallel cases. Exclusion checks compare 12,285 pattern/name
pairs with a reference matrix and retain recursive-path cases.

Core distance tests compare 64,516 short-string bounded results with the full
reference matrix, plus edits around the 63/64-character token boundary and larger
tokens. File-search tests cover nonpositive limits, full metadata limits, FTS
rebinding, invalidation, and service restart. Android tests verify partial-buffer
payload equivalence and ownership, multiple AVC units, malformed framing, and
unchanged Annex B input.

## Measurements and validation

Final measurement reports and build identity are retained locally under
`build-native/optimization-followup`. The baseline source and test snapshots,
original benchmark executables, executable hashes, all intermediate timing
reports, and complete build/test logs are kept there as well.

Measurements were taken on October 5, 2026, with a Release build on Windows 11
Pro 10.0.26200, Ryzen 7 8845HS (8 cores/16 logical processors), 15.26 GiB of
reported usable RAM, Radeon 780M, a 2880 × 1620 120 Hz display, and Balanced
power mode. Existing preferences were preserved. These are synthetic warm
search timings, excluding input delivery, painting, icons, and display scanout.

The table compares the original session executable with the range from two
final optimized runs. Both optimized runs pass the existing budgets without
changing thresholds or applying CI scaling. The original executable exceeds
the 5,000-entry typo and 50,000-entry full-result budgets in this comparison;
its failures and other baseline runs are retained.

| Search measurement (p95, ms) | Session baseline | Final optimized, two runs |
| --- | ---: | ---: |
| Scoring, 5,000 entries | 9.26 | 2.66–3.43 |
| Typo scoring, 5,000 entries | 11.55 | 3.31–4.21 |
| Scoring, 50,000 entries | 16.89 | 16.40–17.49 |
| Typo scoring, 50,000 entries | 18.33 | 18.82–27.28 |
| All ranked results, 50,000 entries | 58.66 | 20.56–23.76 |
| Root assembly, 5,000 entries | 8.13 | 2.25–3.09 |
| Collapsed root assembly, 50,000 entries | 59.63 | 28.81–53.90 |
| Expanded root assembly, 50,000 entries | 61.09 | 30.11–33.15 |
| Apps scope over 50,000 entries | 4.60 | 6.60–8.32 |

Expanded root search takes about half the baseline time in these runs. Timing
variance is substantial: the Apps scope and 50,000-entry typo measurements do
not establish a speedup. Apps scope still filters before scoring and stays
within its 25 ms budget; only 1,000 fixture entries qualify, so the new
5,000-entry worker threshold does not change that path. All four ordered-result
checksums match the original executable in both final runs and the separate
worker-threshold baseline, recorded in `benchmark-final-comparison.json`.

The new catalog regression fixture compares both paths in the same final
executable, with the same result payload copying and 24 warm samples. Emoji
search measures 5.1035 ms uncached versus 1.6148 ms prepared (about 3.2 times
faster). Preparation is excluded from these warm timings. Warm file-content
search p95 is 20.35 ms against a 75 ms budget; the earlier baseline was 19.96 ms,
so no repeatable FTS latency gain is claimed. Statement reuse, skipped work,
and cancellation are verified behavior changes rather than measured speedups.

The final native Release build succeeds with `/W4 /WX`; all 28 CTest targets
pass in 14.35 seconds. The existing rendering fixture also passes pixel parity
at all tested DPI/text-size combinations. Android passes 19 protocol tests and
5 app tests; release lint reports zero errors and nine existing warnings. The
release APK is built, signed, verified, and copied beside the native executable.
Native phone loopback coverage passes; physical-phone validation remains open.

FeatherCast was restarted from `build-native/FeatherCast.exe`. The expected
native window belongs to the new process, its message loop responds, and only
one FeatherCast instance runs. `running-build.json` records the process identity
and SHA-256 hashes of the native executable and APK.

A 30-second hidden-window sample after restart records 36.1 MiB private bytes,
58.2 MiB working set, and 78.125 ms of CPU time (0.26% of one logical core),
with no process file reads or writes during the sample. The enabled phone and
clipboard features and other preference flags are recorded alongside it. This
is a resource snapshot of the current configuration, without a before/after
memory or power comparison.

## Remaining opportunities

These are source-review findings to measure next, rather than established
performance gains.

| Priority | Opportunity | Evidence needed before a larger change |
| --- | --- | --- |
| High | Share immutable file data between `FileSearchService` and launcher snapshots; rebuild only changed corpus partitions. `BuildSnapshot` currently copies file entries and prepares the complete pool again when another source or setting changes. | Snapshot build duration, retained bytes, and cancellation under frequent window/index updates with 20,000–50,000 indexed files. Preserve source ordering and favorite/alias resolution. |
| Medium | Reuse prepared snapshot data in Games and clipboard browse searches. Those paths still call the uncached API. | Query latency and memory with realistic catalogs and maximum clipboard retention. Avoid retaining extra copies of private clipboard text. |
| Medium | Select unique indices and hidden counts before materializing collapsed section payloads. Current selection still constructs each category's capped result list before collapsing it to five rows. | Allocation counts and assembly time separately from scoring; regression coverage for duplicate keys consumed by earlier sections. |
| Medium | Bound Android storage enumeration before building a whole `listFiles()` array, and use provider-side photo/SMS limits where supported. | Large-directory/device tests, provider compatibility, unread conversation semantics, and permission/revocation behavior. |
| Medium | Replace active plugin pipe polling and audio buffer retry waits with cancellable event-based waits where practical. | Active-query/audio wakeup traces, deadline behavior, disconnect recovery, and Windows device compatibility. Idle service waits are already event driven. |
| Medium | Reduce incremental build cost by extracting non-template implementation and additional controllers from `main.cpp` and heavily included headers. | Compile-time measurements and carefully scoped ownership changes. Keep live window mutation on the UI thread and preserve existing ABI boundaries. |
| Validation | Measure real shortcut-to-visible latency and phone behavior on hardware. Synthetic search and codec tests do not cover physical input, compositor scanout, mixed-monitor presentation, or a complete live-phone session. | Existing opt-in diagnostics, controlled DPI/refresh configurations, and real-device phone QA. |

SQLite cancellation applies while its VM executes; the existing busy timeout
still governs lock waits. Prepared catalogs retain additional derived metadata
while in use. Idle process memory and battery savings are not inferred from the
synthetic search timings or the count of removed allocations.
