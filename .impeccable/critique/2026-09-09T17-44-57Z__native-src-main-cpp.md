---
target_identity: "file:C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
target_fingerprint: "sha256:cef2022bbc2d97b4bb7fa03fa5772c2452622638461f3223889edd2e345881b9"
target_path: "C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
timestamp: 2026-09-09T17-44-57Z
slug: native-src-main-cpp
closed: true
---
⚠️ DEGRADED: single-context (spawn_agent unavailable in this session)

## Design Health Score

| # | Heuristic | Score | Key Issue |
|---|---|---:|---|
| 1 | Visibility of System Status | 4/4 | Search, loading, empty, privacy, and settings states now have explicit status projection and actionable messages. |
| 2 | Match Between System and Real World | 3/4 | Most copy is plain Windows language, but “file index,” “extensions,” and command terminology still assume some launcher knowledge. |
| 3 | User Control and Freedom | 4/4 | Layered Escape/back behavior, query undo/redo, browse exits, confirmations, and protected persistence give users strong recovery paths. |
| 4 | Consistency and Standards | 3/4 | Native manager patterns and descriptor-backed settings are consistent; the custom overlay still has a pointer-only entry affordance and mixed action conventions. |
| 5 | Error Prevention | 4/4 | Dependent controls disable correctly, aliases validate, privacy features are opt-in, and risky system actions use confirmation flows. |
| 6 | Recognition Rather Than Recall | 3/4 | Searchable settings, the Discover guide, scope suggestions, and action labels help; the main action mode still depends on knowing hidden keys. |
| 7 | Flexibility and Efficiency of Use | 4/4 | Scopes, aliases, per-invocation favorites, previews, keyboard navigation, and native actions provide a strong power-user ceiling. |
| 8 | Aesthetic and Minimalist Design | 3/4 | The surface is restrained and polished, but the default catalog and generic launcher grammar dilute the premium focus. |
| 9 | Help Users Recognize, Diagnose, and Recover from Errors | 3/4 | Live status and recovery messaging are strong; destructive local-data paths still deserve a clearer recovery/impact model. |
| 10 | Help and Documentation | 3/4 | README, the feature guide, shortcut hints, setting descriptions, and accessibility QA are unusually complete; contextual guidance is still split across states. |
| **Total** | | **34/40** | **Strong foundation — now needs prioritization, one reliable Settings route, and a more distinctive visual grammar.** |

## Design Specificity Verdict

**LLM assessment:** The interaction model is increasingly authored for FeatherCast. Native Windows focus restoration, fuzzy launcher search, local privacy controls, scoped search, command aliases, per-invocation favorites, capture tools, timers, and stable accessibility projection form a coherent product rather than a generic CRUD shell. The visual language is still broadly interchangeable with a polished Raycast/Spotlight-style launcher: dark glass, a top-right gear, uppercase section labels, a selected-row pill, and two-line result cards. The product’s strongest identity remains its native behavior and feature model, not yet a memorable visual grammar.

**Deterministic scan:** `impeccable detect --json native/src/main.cpp` exited 0 and returned `[]`. No applicable markup findings were reported. The detector does not evaluate native Direct2D geometry, contrast, focus routing, or Win32 accessibility behavior, so this is a neutral result rather than evidence that the native surface is free of issues.

## Overall Impression

The GitHub version is a meaningful step forward from the previous revision. Settings are now descriptor-backed and searchable from the launcher, privacy and local-data boundaries are explicit, command aliases and invocation favorites have a real persistence model, and the accessibility contract has a stable Search/Status/Result/Preview structure with a smoke test. The remaining problem is product prioritization: the launcher is trying to be an app launcher, command palette, utility browser, clipboard/file tool, settings gateway, capture surface, and feature catalog at once. The first screen needs a stronger point of view, and the new command-personalization path needs to be reachable in the place users expect.

## What’s Working

- The result model has real depth without abandoning keyboard operation: scopes, browse views, previews, action menus, query undo/redo, and layered Escape/back behavior fit the launcher’s native workflow.
- Status and accessibility work are now treated as product behavior. The stable live status node, explicit roles/values, disabled dependent controls, focus notifications, and `accessibility_smoke_tests.cpp` give the custom-rendered surface a credible foundation.
- Privacy and personalization are concrete rather than decorative. Clipboard/file indexing are explicit opt-ins, local data can be cleared, command aliases persist through the settings model, and invocation-level favorites make frequent actions more direct.

## Priority Issues

### [P1] The visible Settings gear is still not keyboard-first or fully exposed to assistive technology

**Evidence:** `native/src/main.cpp:9546` adds the gear only to the pointer hit list. The launcher branch of `AccessibleItems` returns Search, Status, Results, and Preview, but no “Open settings” item; `SettingsFocusOrder()` begins with settings categories instead. The keyboard handler has no first-class Settings shortcut.

**Why it matters:** FeatherCast promises a keyboard-first launcher, yet the most visible Settings route is pointer-only. A keyboard-only or screen-reader user must already know to type the Settings command or use the tray menu, and the gear itself has no accessible name, role, or default action.

**Fix:** Add an accessible pushbutton named “Open settings,” give it a stable child/focus route, and add a discoverable shortcut such as `Ctrl+,`. Keep the searchable Settings command and tray menu as secondary routes. Treat the custom search surface as an editable control in the native accessibility contract, not only as a read-only text item.

**Suggested command:** `$impeccable audit`

### [P1] Command aliases and per-command favorites are implemented but unreachable from command results

**Evidence:** `native/src/command_catalog.cpp:363-365` builds command-specific “Edit/Add Alias” and “Add/Remove from Favorites” actions. But `native/src/main.cpp:7604-7608` immediately rejects `target.isCommand` in `EnterActionMode()`, which is the path used by `Tab`, `Right`, `Ctrl+K`, context-menu invocation, and `OpenSelectedResultActions()`.

**Why it matters:** The new feature advertises contextual personalization, but the most natural contextual entry point silently does nothing on a command row. Users can manage aliases from Settings, but the result-level affordance is effectively dead, creating a mismatch between the feature model and the interaction model.

**Fix:** Allow command targets into action mode, then preserve direct execution on `Enter`. Keep the action list small and explicit—“Edit Alias” and “Add to Favorites”—and make the selected row’s action hint reflect that the menu is available.

**Suggested command:** `$impeccable clarify`

### [P1] The empty launcher still presents a catalog instead of a clear primary intent

**Evidence:** `native/src/search_pipeline.cpp:306-323` builds the empty state from Favorites, Recent, Apps, Open windows, Snippets, Clipboard History, System Folders, System essentials, Commands, and Explore. All of these are rendered through the same result-row system in `DrawSearch()`.

**Why it matters:** The new release adds valuable capabilities, but the first view now asks the user to interpret a broad catalog before they have stated an intent. On a compact 560–980 DIP overlay, multiple section headers, source subtitles, and row-specific verbs compete with the core “find and launch” job.

**Fix:** Make the idle state intent-first: lead with Favorites/Recent and a tightly capped Apps/Open windows group, then put secondary utilities, capture, clipboard, settings, and discovery behind an explicit “More tools” or browse layer. Keep the full corpus searchable and preserve scopes for power users.

**Suggested command:** `$impeccable distill`

### [P2] Settings is better searchable from the launcher, but the Settings window is still a control-panel wall

**Evidence:** `native/src/main.cpp:10124-10132` defines eight peer categories, and `SettingsFocusOrder()` places all eight categories before the page controls. The overlay can jump to a setting, but the Settings surface itself has no search or quick-jump field.

**Why it matters:** The descriptor catalog improves metadata and keyboard focus, but first-time users still meet Shortcut, General, Results, Library, Privacy, Extensions, Appearance, and Maintenance as eight equal choices. Long privacy and library pages then require scrolling and memory of where a control lives.

**Fix:** Add a small in-settings search/quick-jump field, or group the categories into Behavior, Search & Library, Privacy & Data, and Advanced while retaining direct keyboard navigation for power users. Keep destructive data actions visually and semantically separated from ordinary toggles.

**Suggested command:** `$impeccable layout`

### [P2] The polished surface still lacks a distinctive FeatherCast visual signature

**Evidence:** `DrawSearch()` relies on the same dark glass panel, generic two-line rows, muted source labels, small uppercase section labels, and accent selection treatment used by many launcher products. `SourceLabel()` carries much of the semantic burden for whether a row launches, switches, copies, pastes, or runs.

**Why it matters:** The feature set is specific, but the visual result is still “good launcher UI.” That makes the premium positioning depend on comparison with other launchers and makes category differences easy to miss when scanning quickly.

**Fix:** Establish one restrained signature: a consistent result-type icon/verb grammar, a stronger but quiet relationship between the Windows accent and the selected state, and a refined FeatherCast mark or empty-state treatment. Keep the native, unobtrusive tone; the goal is recognition and faster scanning, not decoration.

**Suggested command:** `$impeccable bolder`

## Persona Red Flags

**Alex (Power User):** Alex gets a strong ceiling from aliases, favorites, scopes, previews, and keyboard navigation, but `Ctrl+K`/`Tab` does not open actions for a command row, and there is still no direct keyboard route to the visible Settings gear. The new personalization model is more powerful than its discoverability.

**Jordan (First-Timer):** Jordan can find an app, but the empty state exposes many peer sections and the meaning of “actions,” scopes, browse views, and per-invocation favorites is distributed across small hints and documentation. The Discover guide helps only after Jordan notices or searches for it.

**Sam (Accessibility-Dependent User):** The stable status/result projection and smoke test are strong foundations. The launcher’s missing gear item remains a concrete gap, and the custom Search item is exposed as `ROLE_SYSTEM_TEXT` with a read value but no dedicated writable/editing route in the custom IAccessible model. Real Narrator/Accessibility Insights passes at 150–200% scaling and High Contrast remain necessary release evidence.

## Minor Observations

- “Store/System Apps” still reads partly like an implementation label; “Store and system apps” is clearer in user-facing settings.
- “Start on Startup” is awkward; “Launch at sign-in” is more natural Windows copy.
- The compact hint mentions `Tab actions`, scopes, and preview, but the command-specific action path needs to be fixed before that promise is reliable.
- Result subtitles are doing too much semantic work. A small, consistent type marker would reduce the reading burden without adding another control.
- The native Library manager is functional and appropriately standard, but its tab-heavy surface reinforces the sense that Settings is a collection of tools rather than a guided configuration experience.

## Questions to Consider

- Should the default empty view show only Favorites, Recent, and a small Apps/Open windows sample, with “More tools” as an explicit next layer?
- Should every invokable result—including commands—share one predictable action-menu contract, with direct Enter execution preserved?
- Is the intended Settings entry point the gear, the searchable command, the tray menu, or all three? If all three, which one is the accessible/keyboard canonical route?
- What is the smallest visual cue that would make FeatherCast recognizable without adding ornament or slowing the search loop?

## Targeted Questions

1. For Settings, should I prioritize a first-class `Ctrl+,` plus an accessible “Open settings” item, or a richer keyboard focus model that also makes the gear part of launcher traversal? (Recommended: both, with `Ctrl+,` as the fast path.)
2. For the empty state, should I collapse secondary sections behind “More tools,” reduce the current section caps, or keep the catalog visible and focus only on type markers? (Recommended: “More tools” plus Favorites/Recent first.)
3. For visual identity, should the next pass emphasize result-type icon/verb grammar, a stronger FeatherCast empty/search state, or a more pronounced accent/selection treatment? (Recommended: icon/verb grammar first.)
