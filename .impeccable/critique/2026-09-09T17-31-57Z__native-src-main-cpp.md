---
target: FeatherCast native search overlay and Settings surface
total_score: 28
max_score: 40
na_heuristics:
p0_count: 0
p1_count: 3
target_identity: "file:C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
target_fingerprint: "sha256:541f7bd79e0d843b607f30fcb5f4cda1435a24cca7064d380fa9b7df979c80f2"
target_path: "C:\\Users\\LeonK\\Documents\\Git\\LeanCast\\native\\src\\main.cpp"
timestamp: 2026-09-09T17-31-57Z
slug: native-src-main-cpp
---
⚠️ DEGRADED: single-context (spawn_agent unavailable in this session)

## Design Health Score

| # | Heuristic | Score | Key Issue |
|---|---|---:|---|
| 1 | Visibility of System Status | 3/4 | Search, loading, empty, and settings states are represented, but some feedback is easy to miss in the dense overlay. |
| 2 | Match Between System and Real World | 3/4 | Mostly plain Windows language; some product/system terms still assume prior knowledge. |
| 3 | User Control and Freedom | 3/4 | Escape, undo/redo, browse-back, and confirmation cancel work well; the settings route is an exception. |
| 4 | Consistency and Standards | 3/4 | The visual system is cohesive, but the pointer-only gear and multiple hint patterns break the keyboard-first promise. |
| 5 | Error Prevention | 3/4 | Dependency disabling and confirmation dialogs are strong; destructive local-data actions do not appear undoable. |
| 6 | Recognition Rather Than Recall | 2/4 | Result subtitles and hints help, but scopes, actions, and the feature catalog still require learning hidden conventions. |
| 7 | Flexibility and Efficiency of Use | 3/4 | Excellent keyboard/search accelerators overall, weakened by no keyboard route to Settings from the launcher. |
| 8 | Aesthetic and Minimalist Design | 3/4 | Clean and compact, but dense, generic launcher conventions and helper copy dilute the premium feel. |
| 9 | Help Users Recognize, Diagnose, and Recover from Errors | 3/4 | Status text is generally concrete and actionable; destructive data flows lack a recovery path. |
| 10 | Help and Documentation | 2/4 | Feature guide, descriptions, and shortcut hints exist, but there is no contextual help or settings search. |
| **Total** | | **28/40** | **Good — solid foundation, with major gains available in hierarchy, accessibility, and identity.** |

## Design Specificity Verdict

**LLM assessment:** The interaction model is clearly authored for FeatherCast: global activation, fuzzy search, app/window focus, scopes, local utilities, previews, native Windows behavior, and focus restoration are coherent and product-specific. The visual world is less distinctive. Dark acrylic, a rounded selected-row pill, small uppercase section labels, a top-right gear, and two-line result cards could be exchanged with another Raycast/Spotlight-style launcher with little change. The product’s character currently lives more in its feature breadth and native implementation than in a memorable visual grammar.

**Deterministic scan:** `impeccable detect --json native/src/main.cpp` exited 0 and returned `[]`. No applicable markup findings were reported; the detector does not evaluate native Direct2D geometry, contrast, focus routing, or Win32 accessibility behavior. No false positives were found.

## Overall Impression

This is a capable, carefully engineered launcher with a credible premium foundation. The core loop is fast and legible once learned, but the interface is trying to be an app launcher, command palette, utility browser, settings gateway, clipboard/file tool, and feature catalog at the same time. The single biggest opportunity is to make the first screen feel more intentional: fewer competing result types, a clearer primary path, and one recognizable FeatherCast signature.

## What’s Working

- The search overlay has a strong hierarchy: a large input, selected result treatment, compact two-line rows, section grouping, and a restrained footer. The 50px rows and 26px section headers create a sensible scan rhythm.
- Keyboard interaction is unusually thoughtful for a custom-drawn native surface. Escape has layered exits, query editing supports selection and undo/redo, result navigation is deep, and previews/actions are available without abandoning the overlay.
- The implementation takes accessibility and system preferences seriously: high-contrast colors, reduced-motion handling, DPI-aware geometry, explicit status projection, MSAA items, disabled dependent controls, and confirmation dialogs are all present.

## Priority Issues

### [P1] The visible Settings gear is not keyboard-first or fully exposed to assistive technology

**Why it matters:** FeatherCast promises keyboard-first operation, yet the gear is added as a hit region and opened in the pointer-up path only. It is not part of the launcher’s accessible item list, and there is no obvious keyboard equivalent. A keyboard-only or screen-reader user can reach results and settings controls after opening Settings, but may not be able to discover or invoke the route into Settings.

**Fix:** Add the gear as an accessible push button named “Open settings,” include it in the launcher focus order, and provide a stable shortcut such as `Ctrl+,` or an explicit `Alt+S` path. Give the custom search field a standard editable accessibility value/action contract rather than only exposing a read-only custom text role.

**Suggested command:** `$impeccable audit`

### [P1] Everything is presented as a result, so the main task competes with the catalog

**Why it matters:** App launches, open windows, files, snippets, clipboard entries, calculators, capture tools, timers, system settings, extensions, and feature discovery all share the same two-line row model. The section headers help, but the user still has to interpret many result types, source subtitles, and row-specific verbs. This is the main cognitive-load risk in a 720×470 surface where only a handful of rows are visible.

**Fix:** Make the default state intent-first: prioritize apps/windows/files, cap secondary categories, and put utilities/feature discovery behind explicit grouped sections or a “More” expansion. Keep scopes and actions available, but reveal them contextually after the user chooses an intent or presses the action key. Use a small, consistent type marker or icon grammar so “launch,” “run,” “paste,” “copy,” and “switch” do not depend on reading the subtitle.

**Suggested command:** `$impeccable distill`

### [P1] The visual language is polished but category-interchangeable

**Why it matters:** The dark glass panel, accent-tinted selection pill, gear affordance, uppercase micro-labels, and generic two-line rows read as “good launcher UI,” not yet as FeatherCast. That limits brand recall and makes the premium positioning depend on comparison with other launchers instead of a clear point of view.

**Fix:** Establish one quiet but unmistakable FeatherCast signature: a refined use of the FeatherCast mark in the empty/search state, a distinctive result-type color or icon grammar, and a stronger relationship between the Windows accent and the selected result. Keep the native, unobtrusive tone; the goal is recognition, not decoration.

**Suggested command:** `$impeccable bolder`

### [P2] Settings exposes too much structure before the user has a task

**Why it matters:** The sidebar shows eight peer categories—Shortcut, General, Results, Library, Privacy, Extensions, Appearance, and Maintenance—while the pages themselves contain long, scrollable lists. The grouping is understandable, but it makes Settings feel like a control panel rather than a focused configuration tool, especially for first-time users.

**Fix:** Either add a settings search/quick-jump field or regroup the eight categories into four higher-level groups such as Behavior, Search & Library, Privacy & Data, and Advanced. Keep destructive data actions visibly separated from ordinary toggles. Preserve the current direct category navigation for power users.

**Suggested command:** `$impeccable layout`

### [P2] Help text and action hints compete with the task

**Why it matters:** The overlay can show a compact hint line, a footer containing five keyboard instructions, and a selected-row action hint. Settings also permanently shows “Drag this bar to move.” Each string is individually useful, but together they flatten the hierarchy and spend precious pixels on instructions that experienced users stop reading.

**Fix:** Use one context-aware instruction strip. Show the highest-value hint for the current state, reveal the rest on focus or an explicit help action, and remove the implementation-oriented drag sentence from the default title area. Keep result verbs, but shorten them to a consistent pattern such as “Open · Actions” and expose the exact shortcuts in a tooltip or help view.

**Suggested command:** `$impeccable clarify`

## Persona Red Flags

**Alex (Power User):** The launch/search loop is fast, but Alex cannot use a keyboard shortcut to reach the top-right Settings gear. They must know the hidden feature/action vocabulary (`Tab`, `Ctrl+K`, `Ctrl+Space`, and scopes) to get beyond simple launches. The lack of an explicit keyboard settings route is the sharpest efficiency break.

**Jordan (First-Timer):** The placeholder “Search apps, files, commands, and more...” is broad but does not explain the next useful action. The shortcuts are split across a low-salience compact hint, footer, and row hints, while the gear is icon-only. Jordan can search an app, but may not discover previews, scopes, utilities, or Settings without already knowing launcher conventions.

**Sam (Accessibility-Dependent User):** The app invests in custom accessibility projection, focus events, high-contrast colors, and keyboard navigation, which is a strong base. However, the gear is missing from the launcher’s accessible item projection, and the custom search field does not implement a writable accessibility value. At 150–200% scaling, the 10px section labels and muted 12px descriptions also deserve a contrast and readability check on real displays.

## Minor Observations

- “Start on Startup” reads awkwardly; “Start at sign-in” or “Launch at startup” is clearer.
- “Store/System Apps” and “AppsFolder” mix user language with implementation language. Prefer “Store and system apps” in the visible UI.
- The empty state “try Discover FeatherCast for examples” assumes that “Discover FeatherCast” is already known as a reachable feature. Make it a visible action or use plain guidance.
- The title-bar drag helper is useful during discoverability testing but feels like debug copy in a premium settings window.
- The muted 10px section labels and 12px subtitles should be checked against acrylic backgrounds, Windows accent variations, and High Contrast—not only against the default dark theme.

## Questions to Consider

- What if the default result view showed only the four highest-confidence intents and treated the rest as an explicit “More tools” layer?
- What is the smallest visual cue that would make someone recognize FeatherCast without adding ornament or slowing the search loop?
- Should Settings be reachable from the launcher with a first-class keyboard command, rather than relying on a mouse-only gear or a discovered command result?
