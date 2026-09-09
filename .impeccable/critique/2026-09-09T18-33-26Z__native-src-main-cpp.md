---
target: FeatherCast native search overlay and Settings surface
total_score: 38
max_score: 40
na_heuristics:
p0_count: 0
p1_count: 0
p2_count: 2
target_identity: "file:C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
target_fingerprint: "sha256:545f28acc98946ff796133ac5ea5dc195ff57fdb99923af249170c5f49db20df"
target_path: "C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
timestamp: 2026-09-09T18-33-26Z
slug: native-src-main-cpp
---
⚠️ DEGRADED: single-context (spawn_agent unavailable in this session)

## Design Health Score

| # | Heuristic | Score | Key Issue |
|---|---|---:|---|
| 1 | Visibility of System Status | 4/4 | Search, loading, empty, privacy, settings, and accessibility status states are explicit and actionable. |
| 2 | Match Between System and Real World | 4/4 | Settings and result copy now use plain Windows verbs such as “Launch,” “Open,” “Apply,” and “Launch at sign-in.” |
| 3 | User Control and Freedom | 4/4 | Layered Escape/back behavior, query editing, browse exits, confirmations, and protected persistence provide strong recovery paths. |
| 4 | Consistency and Standards | 4/4 | Settings has a keyboard/accessibility route, command results share the action-mode contract, and projection IDs are centralized. |
| 5 | Error Prevention | 4/4 | Dependent controls disable correctly, aliases validate, privacy features are opt-in, and risky local-data actions use confirmations. |
| 6 | Recognition Rather Than Recall | 4/4 | Searchable settings, in-settings filtering, “More tools,” scope hints, and explicit action hints reduce hidden-mode discovery cost. |
| 7 | Flexibility and Efficiency of Use | 4/4 | Scopes, aliases, invocation favorites, previews, settings quick-jump, keyboard navigation, and native actions support both novice and power-user paths. |
| 8 | Aesthetic and Minimalist Design | 3/4 | The surface remains restrained and focused, but its visual grammar is still broadly launcher-like rather than unmistakably FeatherCast. |
| 9 | Help Users Recognize, Diagnose, and Recover from Errors | 3/4 | Runtime status and recovery messaging are strong; destructive local-data paths still deserve a clearer impact/recovery model. |
| 10 | Help and Documentation | 4/4 | Shortcut hints, setting descriptions, accessible names/actions, and the feature guide now describe the primary routes consistently. |
| **Total** | | **38/40** | **Strong release candidate; remaining work is polish and live assistive-technology validation, not a blocking interaction defect.** |

## Design Specificity Verdict

**LLM assessment:** The updated surface is authored for FeatherCast rather than a generic launcher. Native Windows focus restoration, local privacy boundaries, scoped fuzzy search, command aliases, invocation favorites, capture tools, timers, stable accessibility projection, a discoverable Settings shortcut, settings filtering, and the intentionally capped empty state form a coherent product model. The remaining visual opportunity is signature: the interaction model is specific, while the dark glass/selected-row language is still familiar from other launchers.

**Deterministic scan:** `impeccable detect --json native/src/main.cpp` is not applicable to this native Direct2D/Win32 surface. The browser-oriented detector cannot evaluate native geometry, contrast, focus routing, or accessibility behavior, so no detector result is treated as evidence of quality or failure.

## Resolved Priority Issues

- The launcher now exposes an accessible “Open settings” button, supports `Ctrl+,`, and includes the settings route in focus/notification handling.
- The custom Search and Settings filter controls expose editable values and `put_accValue` support through the native accessibility model.
- Command rows can enter action mode while `Enter` still executes the command directly; a regression test covers the command action transition.
- The empty view is intent-first: capped Favorites, Recent, Apps, and Open windows lead into a single “More tools” capability while the full corpus remains searchable.
- Settings has a native filter/quick-jump field, IME-aware editing, keyboard navigation, and a reserved footer hint strip.
- Action hints and result source labels now use consistent verb grammar and distinguish open/apply/run behavior.

## Remaining P2 Issues

### [P2] FeatherCast still needs a more distinctive visual signature

The behavior and copy are now product-specific, but the dark glass panel, generic result cards, selected-row treatment, and muted source labels remain recognizable as the broader Raycast/Spotlight family. A restrained icon/verb grammar or a stronger FeatherCast empty-state treatment would improve recognition without adding ornament.

### [P2] Real assistive-technology and display-mode validation remains outstanding

The source projection and smoke tests are strong, but live Narrator/Accessibility Insights checks at 150–200% scaling, High Contrast, and mixed-DPI monitor transitions were not possible in this session. The running process was confirmed, but its native layered tray overlay exposes no normal `MainWindowHandle` to the available UI automation surface, so visual inspection should still be done manually on the user’s machine.

## Verification

- Release build: passed with the repository’s Visual Studio environment workaround for the duplicate `PATH`/`Path` environment entries.
- CTest: 19/19 passed.
- CPack ZIP: generated `FeatherCast-0.9.1-win64.zip` successfully.
- CPack NSIS: unavailable because `makensis` is not installed; this is a packaging-environment limitation, not an application build failure.
- Manual UI automation: not completed against the layered overlay because the process is tray/native-window based and not exposed as a targetable normal window. No visual result is claimed from that path.
