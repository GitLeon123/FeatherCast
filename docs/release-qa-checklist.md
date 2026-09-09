# Release QA Checklist

Use this checklist for every release candidate before publishing the draft
GitHub Release. Record the build identifier, tester, machine, Windows version,
display layout, and the result of each applicable item. A skipped item needs a
written reason.

## Automated gates

Run from a Visual Studio developer PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
build-native\FeatherCastSearchBenchmarks.exe
build-native\FeatherCastFileSearchBenchmarks.exe
build-native\FeatherCast.exe --self-test
build-native\FeatherCastPluginHost.exe --self-test
```

Accept the candidate only when:

- The warnings-as-errors Release build and every CTest target pass.
- Root search remains within the existing 10 ms p95 budget at 5,000 items and
  50 ms p95 budget at 50,000 items.
- Warm full-text file search remains within its existing 75 ms p95 budget at
  50,000 synthetic documents.
- Both executable self-tests exit successfully.
- Diagnostics remain opt-in and contain only state counters, durations, and
  error codes. Search queries, clipboard text, file-preview contents, and
  indexed file contents must never be logged.

## Authenticode gate

Official release artifacts must use the configured publisher and one of the
allowed SHA-256 signer-certificate pins. Verify both application binaries, not
only the installer. Every signature must be valid and timestamped.

```powershell
scripts\verify-release-artifacts.ps1 `
  -BuildDirectory build-native `
  -Configuration Release `
  -ExpectedPublisher $env:FEATHERCAST_EXPECTED_PUBLISHER `
  -AllowedSignerThumbprints ($env:FEATHERCAST_ALLOWED_SIGNER_THUMBPRINTS -split ';') `
  -PortableZipPath .\FeatherCast-<version>-win64.zip `
  -InstallerPath .\FeatherCast-<version>-win64.exe
```

The helper validates the signature, timestamp, publisher, and certificate pin
for `FeatherCast.exe`, `FeatherCastPluginHost.exe`, the NSIS installer, and both
executables inside the portable ZIP. It also runs the executable self-tests.
It performs no network access.

When an MSIX package target exists, pass `-MsixPath` as well. The helper checks
the package signature, manifest, and presence and signatures of both payload
executables. MSIX is currently a readiness check only; it is not part of the
configured CPack output and must not be claimed as a release asset yet.

Unsigned artifacts are suitable only for clearly labeled manual-installation
fallback releases. They fail this official-release gate and cannot use the
verified in-app installation path.

## Package smoke gates

Run the existing ZIP and NSIS end-to-end smoke suite:

```powershell
scripts\package-smoke.ps1 -BuildDirectory build-native -Configuration Release
```

Confirm that:

- ZIP: both executables are present and pass `--self-test`; extraction and
  manual replacement leave `%APPDATA%\FeatherCast` and
  `%LOCALAPPDATA%\FeatherCast` untouched.
- NSIS: clean install, in-place repeat install, Start menu registration, one
  uninstall identity, both installed self-tests, and uninstall all pass.
- Upgrade: installing over the previous stable build preserves settings,
  snippets, plugins, database contents, and the existing install identity.
- MSIX, when available: install on a clean test account, launch and single
  instance activation, both packaged self-tests, upgrade, uninstall, Start
  menu identity, and user-data preservation all pass. Until an MSIX target is
  integrated, mark this item not applicable rather than simulating success.
- SHA-256 sidecars exist and match every published installer and ZIP asset.

## Accessibility model gate

The accessibility smoke test covers the stable Search, Status, Result, and
Preview child numbering; hidden/loading/empty/error/preview status projection;
accessible names, values, descriptions, roles, states, and default actions;
focus and selection forwarding; hit testing; navigation; and default-action
invocation.

The test source is `native/tests/accessibility_smoke_tests.cpp`. It must be
registered as a normal 30-second CTest target and linked with the Windows
accessibility libraries before release.

## Manual Windows gates

Automation does not replace these checks:

- Run keyboard and pointer passes at 100%, 150%, and 200% scaling, including a
  mixed-DPI layout with the primary monitor changed during the pass.
- With Narrator and Accessibility Insights, confirm Search, stable live Status,
  Results, and Preview order; visible names, roles, focus, default actions, and
  bounds; loading, empty, error, and preview announcements; and no child-ID
  movement when status text changes.
- Exercise IME composition, caret movement, selection, and candidate-window
  placement in the search box and Library editor.
- Suspend and resume while hidden, searching, indexing, running timers, and
  recording. Verify recovery, bounded background work, and clean shutdown.
- Exercise full-screen and region screenshots and recordings across mixed-DPI
  and negative-origin monitors, including pause/resume, cancellation, device
  loss, and finalization failures.

Use `docs/manual-regression-checklist.md` for the full behavioral matrix.
