# What makes a product feel premium — research digest

Synthesis of five deep-research passes (motion, visual craft, interaction/responsiveness,
perception psychology, Windows-native rendering). Sources: Microsoft Learn / Fluent 2 /
WinUI theme resources, Apple HIG + WWDC spring talks, NN/g, web.dev RAIL, Emil Kowalski,
Rauno Freiberg, Raycast and Linear engineering/design posts, Superhuman, Lindgaard 2006,
Kurosu & Kashimura 1995, Reber/Schwarz/Winkielman 2004, Fredrickson & Kahneman 1993.

## Why people perceive "premium"

- **50 ms first impression** (Lindgaard 2006) colours later judgments of usability and trust
  (aesthetic-usability / halo). Looks buy tolerance for small flaws, not big ones.
- **Processing fluency**: alignment, consistency, contrast and repetition feel good because
  they are easy to process. Inconsistency (different radii, paddings, fonts between windows)
  is read as "cheap" instantly.
- **Visible care is a costly signal**: handled edge cases, considered empty/error states and
  pixel-exact alignment imply the invisible parts are good too.
- **Peak-end rule**: the summon and the dismiss are the moments that are remembered.
- **Restraint**: quiet chrome, loud content; one accent colour; spacing instead of separators.

## Speed is the first premium feature

- 100 ms is a ceiling, not a target (perception thresholds ~60 ms). Keystroke-to-paint < 50 ms.
- Window warm and pre-rendered; never show a stale or blank frame; never block on I/O.
- Keyboard-invoked, high-frequency actions get **no or near-zero motion** (Emil, Rauno,
  macOS app switcher). Typing, caret and arrow navigation are live on frame 0.
- Loading indicators only after a delay (no spinner flash under ~1 s); keep previous results
  visible instead of skeletons.

## Motion tokens

| Use | Duration | Curve |
|---|---|---|
| Micro fade (hotkey show/hide, new rows) | 0–83 ms | opacity only |
| Point-to-point (selection pill) | 120–167 ms, retargetable, snap under key repeat | (0.55,0.55,0,1) or critically damped spring |
| Press feedback | 100–160 ms | ease-out, scale ≥ 0.97 |
| Window/panel entrance | 200–250 ms | (0,0,0,1), scale 0.96→1 + opacity |
| Exit | 120–167 ms, always faster than entrance | opacity-led |
| Hard ceiling | 300 ms | — |

- Retarget from the current value and velocity; never restart from zero.
- No stagger on live result lists. Never scale from 0. Animate transform/opacity, not layout.
- Springs: bounce 0 for UI (0.2 s → k≈987, c≈63, m=1). Don't spring colour/opacity.
- Reduced motion (`SPI_GETCLIENTAREAANIMATION`): drop transform motion, keep short fades and
  busy indicators.

## Visual craft

- **Type ramp (Windows)**: Caption 12/16, Body 14/20, Body Strong 14/20 semibold, Body Large
  18/24, Subtitle 20/28 semibold. Nothing below 12 px regular.
- **Text tiers**: primary 100 %, secondary ~77 %, tertiary ~53 %, disabled ~37 % white on
  dark. Tertiary must still reach 4.5:1.
- **Tabular figures** (`tnum`) for counts, times, sizes, percentages.
- **4 px grid**; every padding and gap a multiple of 4 (2 allowed for hairline nudges).
- **Concentric radii**: inner = outer − inset (panel 12 / inset 4 / row 8; badge 4).
- **Strokes before shadows**: 1 px hairline at ~7 % white on dark, one soft shadow only on
  floating surfaces, no nested borders. Elevation via lighter layers, not heavier shadows.
- **Pixel snapping**: snap rects to device pixels at 125/150/175 %; odd-width strokes on
  half-pixel centres; icons rasterised at device size.
- **Keycaps**: 12 px text, ~4 px radius, subtle fill + hairline, consistent glyphs (Ctrl, ⇧, ↵).
- Explicit text antialias mode per surface (grayscale on translucent, ClearType on opaque).

## Interaction details

- Hover never beats the keyboard: ignore hover until the pointer actually moves; keyboard
  input clears hover; one selection state.
- Bind selection to item identity across result updates; zero unexplained row movement.
- Show the shortcut where the action is (action bar / footer hints, `?` sheet).
- Esc is two-stage: clear/back, then hide.
- Undoable toasts instead of routine confirmation dialogs.
- Every failure is a designed state: plain-language cause + one next step, keep user input.
- Status is truthful (phone: discovering → pairing → connected → degraded → disconnected,
  with last-seen); progress after 1 s, percent after 10 s.
- Sound and haptics off by default.

## Cheap tells to eliminate

Flicker or white flash, layout shift while typing, hover hijacking selection, spinners that
flash, inconsistent radii/padding/fonts across windows, 11 px text, low-contrast grey text,
off-centre icons/labels, separators and badges with no job, "Something went wrong" without
a next step, animations ≥ 300 ms on frequent paths.
