# Improvement validation

This is an evidence record for the app-improvement implementation, not a claim that unperformed device or visual checks passed. Preserve earlier uncommitted work when reviewing the changes. The implementation baseline for this session is under `build-native/improvement-baseline` (local build data).

## Automated checks

The native improvement tests exercise short aliases in root search, explicit intent precedence, opt-in local learning and reset, settings round trips, actual capped indexing, selected-photo metadata, ordered streamed writes, failure/cancellation cleanup and a 41 MiB transfer using a reusable 256 KiB buffer. Encrypted loopback protocol tests exercise negotiation, send/receive receipts and legacy fallback.

The Kotlin protocol tests exercise stream metadata, zero/invalid/oversized lengths, invalid IDs, bounded chunks and exact sequencing. Android unit tests cover Full/Selected/Denied permission states and OS/target SDK combinations for local-network permission. The build script and CI run protocol tests, Android unit tests and release lint before copying the signed APK.

Validation completed on 2026-10-05 using a native Release build on Windows 11 Pro 10.0.26200, an AMD Ryzen 7 8845HS (8 cores / 16 logical processors), Radeon 780M and approximately 16 GB RAM. The local environment and timing records are under `build-native/measurements`.

| Check | Result |
| --- | --- |
| Native Release build with `/W4 /WX` | Passed |
| Native CTest suite | 26/26 passed; 12.70 seconds total |
| Search p95, 5,000 / 50,000 items | 6.17 / 20.76 ms; budgets 10 / 50 ms |
| Typo search p95, 5,000 / 50,000 items | 5.87 / 26.33 ms; budgets 10 / 50 ms |
| Warm file-content search, 50,000 files | p95 22.39 ms; budget 75 ms |
| Android protocol tests | 17 passed |
| Android app permission tests | 2 passed |
| Android release lint | 0 errors; 9 existing warnings |
| Android release build and APK signature verification | Passed; companion 1.2.0 (version code 3), target SDK 35, APK signature v2 verified |
| CPack ZIP and NSIS installer | Generated `FeatherCast-0.11.0-win64.zip` and `FeatherCast-0.11.0-win64.exe` |
| Fresh ZIP extraction | Launcher, PluginHost and InputBroker `--self-test` passed; launcher and companion APK hashes matched the current build |
| Restart with the successful build | Running `build-native/FeatherCast.exe`, PID 22944, started 17:17:42 CEST |

The running launcher SHA-256 is `628C561DD2457E4ED93C7643BE19DF2DAABBFB22788B8E6C1009D20D4463E38B`. This identifies the local artifact; it is not a publisher signature. The generated Windows installer is **NotSigned**. The NSIS installer was not installed or uninstalled on the user's machine.

Search benchmarks measure synthetic search work, not shortcut-to-visible-overlay latency. DPI, refresh rate and physical input/display latency were not measured. See [performance measurements](performance.md) for the meaning and limits of timings. Re-run the whole native CTest suite, both search benchmarks and `scripts/build-android.ps1` after implementation changes.

## Post-restart process sample

The read-only resource sampler observed PID 22944 for 60 seconds (61 samples), after startup had settled. At the end of the sample, private bytes were 35.4 MiB and the working set was 49.0 MiB. CPU time increased by 156.25 ms, equivalent to 0.26% of one logical core over that interval. Sampled peak private bytes were also 35.4 MiB; sampled peak working set was 49.0 MiB. No process file-read or file-write bytes were recorded during the interval.

Phone Connection and clipboard history were enabled in the existing preferences. File indexing, content indexing, search learning and diagnostics were disabled. Preferences were not changed for the sample. The phone's connection state, hidden-window state, display configuration and power mode were not inspected, so the operator label `restarted-tray-idle` does not establish those conditions. These measurements describe this one configuration and interval, not a general memory or idle-CPU guarantee.

The machine-readable evidence is `build-native/measurements/idle.json`, alongside `feature-flags.json`, `environment.json`, `running-build.json`, `search.txt` and `file-search.txt`. These local build records are not committed release evidence.

## Device and accessibility evidence still needed

No Android device was attached during this session. Native computer-use transport was unavailable, so live visual and screen-reader results were not collected. Automated accessibility and renderer smoke tests cannot replace the following checks.

| Check | Environment to record | Status |
| --- | --- | --- |
| Android Full / Selected / Denied photos, selection changes and foreground return | Android 14+ OS/build, device model, app version | Not run |
| Android notification denied: connection continues, visibility guidance is correct | Android 13+ OS/build and permissions | Not run |
| Local-network denied/granted behavior when moving target SDK to 37 | Android 17+, target 37 build | Not run; current target remains 35 |
| Transfer both ways above 40 MiB; cancellation during sending, receiving and final save; Wi-Fi loss; low disk space | Phone model, OS, network, file size/checksum | Not run on physical devices; loopback and receiver tests available |
| Narrator focus, result names, setting switches, Library editors and validation errors | Windows build, Narrator version, keyboard path | Not run interactively |
| Library tabs, multiline workspace input, Selected-photo and file-index empty states | 100%, 150%, 200% DPI and 200% text size | Not run visually |
| Mixed-DPI monitor changes, High Contrast and Reduced Motion | Display topology, scales, accessibility settings | Not run interactively |
| Scripts, workspace partial failure and command shortcut collision against another running app | Windows build and exact configured targets/chords | Not run interactively; validation is automated |
| Signed installer upgrade/uninstall and pinned update installation | Published signer, artifact hash, Windows clean machine | Not run with release credentials |

Replace a status only after collecting evidence. Record build commit/artifact hash, date, OS/device, steps, expected/observed behavior and evidence location. Use synthetic data for captures so personal notifications, clipboard entries and file paths do not appear in shared evidence.

## Release signing

The existing CI supports Authenticode signing, timestamp verification, publisher checks and SHA-256 signer-certificate pins. Signing credentials must be configured by the publisher. An unsigned development build remains suitable for local testing; it cannot install in-app updates. [verify-release-artifacts.ps1](../scripts/verify-release-artifacts.ps1) verifies supplied signed artifacts and identities. A local APK signing key does not establish Windows installer identity.
