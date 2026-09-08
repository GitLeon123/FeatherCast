# FeatherCast 0.9.1

## Changes

- Prewarms Direct2D and DirectComposition surfaces so the first overlay reveal
  does not spend its animation budget creating graphics resources.
- Coalesces animation-frame requests and search-result batches to keep the UI
  responsive under backpressure and avoid presenting obsolete intermediate
  frames.
- Handles swap-chain backpressure without blocking the UI thread, and clips
  transient DWM blur to the animated panel bounds during open, close, and
  resize transitions.
- Keeps visible icons stable through the opening reveal, then promotes the
  decoded icon batch together at the presentation boundary.
- Guards pointer input while panels close and keeps explicit `--show`
  activations visible when Windows temporarily refuses foreground activation.
- Adds motion tests for normalized reveal progress and coalesced frame
  scheduling.

## Verification

- Release build with warnings as errors succeeded.
- All 18 Release CTest suites passed in the normal Windows user context.
- Search and file-search benchmarks completed successfully.
- Application and plugin-host self-tests completed successfully.

The GitHub Actions workflow creates this release as a draft so the final
mixed-DPI, accessibility, suspend/resume, capture, and visual interaction
checks can be completed before publication.
