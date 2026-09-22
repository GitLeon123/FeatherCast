# FeatherCast Native UI Design Baseline

This document records the native Windows visual and interaction contract for
FeatherCast. Win32, Direct2D, DirectWrite, DirectComposition, and the source
constants in `native/src` are authoritative. This is not a web design system:
there is no browser viewport, CSS cascade, DOM focus tree, or responsive
framework.

## Product posture

FeatherCast is a compact, dark, keyboard-first launcher. The overlay is a
short-lived command surface; settings, volume, capture selection, screenshot
editing, and recording controls are supporting native windows. Motion is
interruptible and must follow the user's animation preference, Windows client
area animation setting, reduced-motion expectations, and High Contrast mode.

## Visual tokens

The default theme is defined by `native/src/theme.hpp` and may be overridden by
`%APPDATA%\\FeatherCast\\theme.json`. Theme colors are normalized after load.
Translucent roles are compared after compositing over the same opaque canvas.

| Role | Default | Use |
| --- | --- | --- |
| Overlay/settings background | `#101012EB` | Obsidian glass panel |
| Surface | `#18181B` | Rows and controls |
| Surface hover | white at 12% alpha | Pointer/keyboard hover feedback |
| Selected base | `#1C1C21` | Selection pill under the system accent |
| Icon tile | `#3B3B47` | Missing-app and result icon tile |
| Primary text | `#F2F2F5` | Names, labels, active values |
| Muted text | `#9999A3` | Descriptions and secondary metadata |
| Dim text | `#878793` | Hints and placeholders |
| Section text | `#9EA3BD` | Settings/result section headings |
| Danger | `#FF5C5C` | Destructive/error semantics |
| Success | `#4DC77A` | Success semantics, normalized at runtime |
| Recording | `#F22E33` | Recording status |
| Accent fallback | `#5C6BFF` | Accent when DWM colorization is unavailable |

Body text is normalized to at least 4.5:1 against all known surfaces. Control,
icon, and focus roles target at least 3:1. High Contrast maps to system window,
button, highlight, and window-text roles; ordinary secondary text stays a
normal window-text role rather than `COLOR_GRAYTEXT`.

## Typography

DirectWrite uses `Segoe UI Variable Text` by default with a compact role ramp:

- Search input: 18 px
- Result rows: 14 px medium, with a 12 px subtitle
- Settings labels: 14 px semibold, with a 13 px description
- Buttons and centered controls: 13 px semibold
- Titles: 17 px semibold
- Volume value: 28 px semibold

The supported FeatherCast text-size preference ranges from 90% to 200% in 10% steps.
It scales DirectWrite role sizes and leading, then uses native DirectWrite
single-line ellipsis trimming for compact controls. Larger blocks wrap. Layout
rows and scroll ranges are recalculated after the preference changes.

## Geometry and interaction

Interactive DIP geometry is centralized in `native/src/layout_contract.hpp`.
The recording bar, volume control, launcher header, settings filter, and
settings close button use those contracts for visible bounds, pointer hit
regions, and accessibility rectangles. Physical pixels are obtained through
rounded DPI conversion. Rectangles use a half-open edge convention in the
shared contract.

The launcher, settings, volume, and recording surfaces draw rounded silhouettes
with Direct2D. Rounded-corner `WM_NCHITTEST` handling returns
`HTTRANSPARENT` outside the silhouette. No persistent window region is applied
because it conflicts with the DirectComposition alpha surface.

## Component states

- **Normal:** surface/background roles, visible hierarchy, no focus ring.
- **Hover:** surface-hover or semantic status role; pointer movement never
  changes keyboard selection unexpectedly.
- **Focused:** accent focus frame, with a logical focus model where a window is
  intentionally non-activating.
- **Selected:** accent-tinted selection pill or selected settings category;
  labels and shapes retain meaning without color alone.
- **Disabled/unavailable:** action is excluded from keyboard invocation and
  accessibility focus; the screen reader receives `STATE_SYSTEM_UNAVAILABLE`.
- **Loading/error/success:** visible status copy plus semantic roles and
  `EVENT_OBJECT_*` updates; errors do not rely on color only.
- **Empty/long content:** explicit empty-state copy; long single-line labels
  trim with ellipsis, while paragraph-like content wraps and clips only at a
  deliberate viewport boundary.

## Accessibility contract

The native `IAccessible` projection is child-based. Every actionable child has
an English name, role, value/description when useful, a screen rectangle, and
state flags. The recording bar is `WS_EX_NOACTIVATE`: it owns an explicit
logical control focus while active, wraps `Tab`/`Shift+Tab` over Pause/Stop,
invokes with `Enter`/`Space`, cancels with `Escape`, and publishes focus,
value, state, show, and hide events.

## Verification baseline

The repository's CTest registration contains 21 tests. Native release builds
use the Visual Studio x64 environment described in `AGENTS.md`. The bundled
Impeccable launcher currently exposes detector/context helpers but does not
provide the requested `document` or `polish` verbs for this Windows-native
surface, so this baseline is maintained from the source-of-truth tokens above.
