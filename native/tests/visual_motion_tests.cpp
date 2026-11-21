#include "motion.hpp"
#include "test_framework.hpp"
#include "text_layout_cache.hpp"
#include "visual_craft.hpp"

#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

bool Near(double a, double b, double tolerance = 1e-4) {
  return std::abs(a - b) <= tolerance;
}

struct TestRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

bool OnPixelGrid(float dip, float scale) {
  const float pixels = dip * scale;
  return std::abs(pixels - std::round(pixels)) < 1e-3f;
}

void TestCubicBezier() {
  using feathercast::motion::CubicBezier;
  using feathercast::motion::kDecelerateCurve;
  using feathercast::motion::kPointToPointCurve;

  for (const CubicBezier& curve : {kDecelerateCurve, kPointToPointCurve}) {
    assert(curve(0.0) == 0.0);
    assert(curve(1.0) == 1.0);
    assert(curve(-1.0) == 0.0);
    assert(curve(2.0) == 1.0);
    double previous = 0.0;
    for (int i = 1; i <= 200; ++i) {
      const double value = curve(i / 200.0);
      assert(value + 1e-9 >= previous);
      assert(value >= 0.0 && value <= 1.0);
      previous = value;
    }
  }

  // (0, 0, 0, 1): x(t) = t^3, y(t) = 3t^2 - 2t^3, so y = 3p^(2/3) - 2p.
  for (const double p : {0.01, 0.1, 0.25, 0.5, 0.75, 0.9}) {
    const double expected = 3.0 * std::cbrt(p * p) - 2.0 * p;
    assert(Near(kDecelerateCurve(p), expected));
  }
  // Decelerating: most of the distance is covered early.
  assert(kDecelerateCurve(0.25) > 0.65);
  assert(kDecelerateCurve(0.5) > 0.85);

  // Point-to-point starts gently and still settles softly.
  assert(kPointToPointCurve(0.1) < 0.2);
  assert(kPointToPointCurve(0.9) > 0.97);

  // Linear control points reproduce the identity.
  const CubicBezier linear{1.0 / 3.0, 1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0};
  for (int i = 0; i <= 10; ++i) {
    assert(Near(linear(i / 10.0), i / 10.0));
  }
  // Out-of-range x control points are clamped, keeping x(t) monotonic.
  const CubicBezier clamped{-2.0, 0.0, 3.0, 1.0};
  assert(Near(clamped(0.5), CubicBezier(0.0, 0.0, 1.0, 1.0)(0.5)));

  using feathercast::motion::Ease;
  using feathercast::motion::Easing;
  assert(Near(Ease(Easing::Decelerate, 0.3), kDecelerateCurve(0.3)));
  assert(Near(Ease(Easing::PointToPoint, 0.3), kPointToPointCurve(0.3)));
  for (const Easing easing : {Easing::Decelerate, Easing::PointToPoint}) {
    assert(Ease(easing, 0.0) == 0.0);
    assert(Ease(easing, 1.0) == 1.0);
  }

  feathercast::motion::ScalarAnimation animation;
  animation.Snap(0.96);
  animation.Retarget(1.0, 0.2, true, Easing::Decelerate);
  animation.Update(0.05);
  assert(Near(animation.Value(), 0.96 + 0.04 * kDecelerateCurve(0.25), 1e-6));
  for (int i = 0; i < 10; ++i) animation.Update(1.0 / 60.0);
  assert(!animation.Active());
  assert(animation.Value() == 1.0);
}

void TestMotionTokens() {
  namespace tokens = feathercast::motion::tokens;
  for (const double seconds :
       {tokens::kMicroFadeSeconds, tokens::kEntranceSeconds,
        tokens::kExitSeconds, tokens::kPointToPointSeconds,
        tokens::kSelectionSpringResponseSeconds,
        tokens::kSelectionPointerResponseSeconds}) {
    assert(seconds > 0.0);
    assert(seconds <= tokens::kHardCeilingSeconds);
  }
  assert(tokens::kMicroFadeSeconds <= 0.083 + 1e-9);
  assert(tokens::kEntranceSeconds >= 0.200 && tokens::kEntranceSeconds <= 0.250);
  assert(tokens::kExitSeconds < tokens::kEntranceSeconds);
  assert(tokens::kPointToPointSeconds >= 0.120 &&
         tokens::kPointToPointSeconds <= 0.167);
  // Never a "zoom from nothing": scale stays within a few percent.
  assert(tokens::kEntranceScale >= 0.96 && tokens::kEntranceScale < 1.0);
  assert(tokens::kExitScale >= tokens::kEntranceScale &&
         tokens::kExitScale < 1.0);
  assert(tokens::kSelectionPointerResponseSeconds <
         tokens::kSelectionSpringResponseSeconds);
}

void TestKeyRepeatCadence() {
  using feathercast::motion::KeyboardRepeatIntervalSeconds;
  using feathercast::motion::KeyRepeatCadence;

  assert(Near(KeyboardRepeatIntervalSeconds(0), 1.0 / 2.5));
  assert(Near(KeyboardRepeatIntervalSeconds(31), 1.0 / 30.0));
  assert(Near(KeyboardRepeatIntervalSeconds(1000), 1.0 / 30.0));
  assert(KeyboardRepeatIntervalSeconds(15) < KeyboardRepeatIntervalSeconds(5));

  assert(Near(KeyRepeatCadence::ThresholdForRepeatInterval(1.0 / 30.0), 0.0667,
              1e-3));
  assert(KeyRepeatCadence::ThresholdForRepeatInterval(0.001) == 0.060);
  assert(KeyRepeatCadence::ThresholdForRepeatInterval(0.4) == 0.120);

  KeyRepeatCadence cadence(0.090);
  assert(!cadence.Step(1000));  // first press never counts as a repeat
  assert(cadence.Step(1033));   // auto-repeat cadence
  assert(cadence.Step(1066));
  assert(!cadence.Step(1300));  // a deliberate second press animates
  assert(!cadence.Step(1390));  // exactly at the threshold is not a repeat
  assert(cadence.Step(1479));

  cadence.Reset();
  assert(!cadence.Step(1500));

  // GetMessageTime() wraps after ~49.7 days.
  KeyRepeatCadence wrap(0.090);
  assert(!wrap.Step(0xFFFFFFF0u));
  assert(wrap.Step(0x00000010u));
  // A message time from the past (out-of-order) is treated as a fresh press.
  assert(!wrap.Step(0x00000000u));

  KeyRepeatCadence adjustable(0.090);
  adjustable.SetThreshold(0.060);
  assert(!adjustable.Step(0));
  assert(!adjustable.Step(70));
  assert(adjustable.Step(120));
}

void TestPixelSnapping() {
  using namespace feathercast::visual;
  assert(SafeScale(0.0f) == 1.0f);
  assert(SafeScale(-2.0f) == 1.0f);
  assert(SafeScale(NAN) == 1.0f);

  for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f}) {
    for (const float dip : {0.0f, 0.3f, 7.5f, 60.0f, 109.4f, 333.3f}) {
      const float snapped = SnapToPixelEdge(dip, scale);
      assert(OnPixelGrid(snapped, scale));
      assert(std::abs(snapped - dip) <= 0.5f / scale + 1e-4f);
    }
    // Hairlines are whole device pixels: one below 200%, two from 200%.
    const float pixels = HairlinePixels(scale);
    assert(pixels == (scale >= 2.0f ? std::floor(scale) : 1.0f));
    assert(Near(HairlineWidth(scale) * scale, pixels, 1e-5));

    // Odd widths are centred on half pixels, even widths on boundaries.
    const float center = SnapStrokeCenter(60.0f, scale, pixels);
    const float leading = center - pixels * 0.5f / scale;
    assert(OnPixelGrid(leading, scale));

    const TestRect rect = SnapRectToPixels(TestRect{8.2f, 60.4f, 511.7f, 110.6f},
                                           scale);
    assert(OnPixelGrid(rect.left, scale) && OnPixelGrid(rect.top, scale));
    assert(OnPixelGrid(rect.right, scale) && OnPixelGrid(rect.bottom, scale));

    // A 24 DIP icon keeps one pixel size wherever it lands.
    const float expectedSide = std::round(24.0f * scale);
    for (const float y : {13.0f, 13.3f, 13.5f, 13.8f, 77.1f}) {
      const TestRect icon =
          SnapSizedRectToPixels(TestRect{20.0f, y, 44.0f, y + 24.0f}, scale);
      assert(OnPixelGrid(icon.left, scale) && OnPixelGrid(icon.top, scale));
      assert(Near((icon.right - icon.left) * scale, expectedSide, 1e-3));
      assert(Near((icon.bottom - icon.top) * scale, expectedSide, 1e-3));
    }

    // Inside strokes cover exactly the innermost pixels of the panel.
    const TestRect panel{0.0f, 0.0f, 640.0f, 400.0f};
    const TestRect inside = InsideStrokeRect(panel, scale, pixels);
    assert(Near(inside.left - pixels * 0.5f / scale, 0.0, 1e-5));
    assert(Near(inside.right + pixels * 0.5f / scale, 640.0, 1e-4));
  }
  // At 100% a 1 px stroke at y = 60 is centred on 60.5.
  assert(Near(SnapStrokeCenter(60.0f, 1.0f, 1.0f), 60.5));
  // At 150% the rule starts on device row 90 (60 DIP).
  assert(Near(SnapStrokeCenter(60.0f, 1.5f, 1.0f) * 1.5f, 90.5, 1e-4));
}

void TestShortcutHints() {
  using feathercast::visual::IsShortcutKeyName;
  using feathercast::visual::ParseShortcutHints;
  using feathercast::visual::ShortcutHintPart;
  using feathercast::visual::SplitShortcutChord;

  assert(IsShortcutKeyName(L"Enter"));
  assert(IsShortcutKeyName(L"Tab"));
  assert(IsShortcutKeyName(L"Esc"));
  assert(IsShortcutKeyName(L"F5"));
  assert(IsShortcutKeyName(L"F24"));
  assert(!IsShortcutKeyName(L"F25"));
  assert(!IsShortcutKeyName(L"F0"));
  assert(IsShortcutKeyName(L"@"));
  assert(IsShortcutKeyName(L"K"));
  assert(IsShortcutKeyName(L","));
  assert(!IsShortcutKeyName(L"a"));
  assert(!IsShortcutKeyName(L"Type"));
  assert(!IsShortcutKeyName(L"Downloading..."));
  assert(!IsShortcutKeyName(L""));

  assert((SplitShortcutChord(L"Ctrl+Shift+Enter") ==
          std::vector<std::wstring>{L"Ctrl", L"Shift", L"Enter"}));
  assert((SplitShortcutChord(L"Ctrl+,") ==
          std::vector<std::wstring>{L"Ctrl", L","}));
  assert((SplitShortcutChord(L"+") == std::vector<std::wstring>{L"+"}));
  assert(SplitShortcutChord(L"Ctrl+").empty());
  assert(SplitShortcutChord(L"play/pause").empty());

  {
    const auto parts = ParseShortcutHints(L"Enter open · Tab actions");
    assert(parts.size() == 2);
    assert((parts[0] == ShortcutHintPart{{L"Enter"}, L"open"}));
    assert((parts[1] == ShortcutHintPart{{L"Tab"}, L"actions"}));
  }
  {
    const auto parts = ParseShortcutHints(
        L"Enter open · Ctrl+Shift+Enter admin · Tab actions · @ scopes");
    assert(parts.size() == 4);
    assert((parts[1] ==
            ShortcutHintPart{{L"Ctrl", L"Shift", L"Enter"}, L"admin"}));
    assert((parts[3] == ShortcutHintPart{{L"@"}, L"scopes"}));
  }
  {
    const auto parts = ParseShortcutHints(L"Ctrl+K actions · Ctrl+, settings");
    assert(parts.size() == 2);
    assert((parts[0] == ShortcutHintPart{{L"Ctrl", L"K"}, L"actions"}));
    assert((parts[1] == ShortcutHintPart{{L"Ctrl", L","}, L"settings"}));
  }
  {
    const auto parts = ParseShortcutHints(
        L"Type and press Enter to send · F5 refresh · Esc back");
    assert(parts.size() == 3);
    assert(parts[0].keys.empty());
    assert(parts[0].label == L"Type and press Enter to send");
    assert((parts[1] == ShortcutHintPart{{L"F5"}, L"refresh"}));
    assert((parts[2] == ShortcutHintPart{{L"Esc"}, L"back"}));
  }
  {
    const auto parts = ParseShortcutHints(L"Enter run · Space play/pause");
    assert((parts[1] == ShortcutHintPart{{L"Space"}, L"play/pause"}));
  }
  {
    const auto parts = ParseShortcutHints(L"Downloading...");
    assert(parts.size() == 1);
    assert(parts[0].keys.empty() && parts[0].label == L"Downloading...");
  }
  {
    const auto parts = ParseShortcutHints(L"Esc");
    assert(parts.size() == 1);
    assert((parts[0] == ShortcutHintPart{{L"Esc"}, L""}));
  }
  assert(ParseShortcutHints(L"").empty());
  assert(ParseShortcutHints(L"  ").empty());
  assert(ParseShortcutHints(L" · ").empty());
}

void TestKeycapMetrics() {
  using namespace feathercast::visual;
  const KeycapMetrics base;
  // 12 px caption on a 4 px grid; radius stays a crisp 4 px at any size.
  assert(base.height == 20.0f && base.radius == 4.0f);
  for (const float value : {base.height, base.minWidth, base.chordGap,
                            base.partGap}) {
    assert(std::fmod(value, 4.0f) == 0.0f);
  }
  const KeycapMetrics large = ScaledKeycapMetrics(1.5f);
  assert(large.height == 30.0f && large.paddingX == 9.0f);
  assert(large.radius == 4.0f);
  assert(ScaledKeycapMetrics(NAN).height == base.height);
  assert(ScaledKeycapMetrics(100.0f).height == base.height * 3.0f);

  // Narrow keys keep a square minimum cap; wide keys pad their text.
  assert(ShortcutHintPartWidth({4.0f}, 0.0f, base) == base.minWidth);
  assert(ShortcutHintPartWidth({30.0f}, 0.0f, base) == 42.0f);
  // Ctrl+K actions: 26+12 | 4 | 20 | 6 | 40.
  assert(ShortcutHintPartWidth({26.0f, 7.0f}, 40.0f, base) ==
         38.0f + 4.0f + 20.0f + 6.0f + 40.0f);
  // Plain text parts are only their label.
  assert(ShortcutHintPartWidth({}, 80.0f, base) == 80.0f);

  const std::vector<float> widths{60.0f, 70.0f, 50.0f};
  assert(FittingHintParts(widths, 1000.0f, base) == 3);
  assert(FittingHintParts(widths, 60.0f + 16.0f + 70.0f, base) == 2);
  assert(FittingHintParts(widths, 145.0f, base) == 1);
  assert(FittingHintParts(widths, 59.0f, base) == 0);
  assert(FittingHintParts({}, 100.0f, base) == 0);
}

void TestTabularFigures() {
  using feathercast::ui::TextLayoutCache;
  using feathercast::ui::TextLayoutOptions;
  using feathercast::ui::TextLayoutStyle;

  TextLayoutOptions proportional;
  TextLayoutOptions tabular;
  tabular.tabularFigures = true;
  assert(!(proportional == tabular));
  assert(proportional == TextLayoutOptions{});

  Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  if (FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
    return;
  }
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  if (FAILED(factory->CreateTextFormat(
          L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"",
          format.GetAddressOf()))) {
    return;
  }
  TextLayoutCache cache;
  const auto width = [&](const wchar_t* text, TextLayoutOptions options) {
    options.style = TextLayoutStyle::MeasureSingleLine;
    auto layout = cache.Get(factory.Get(), text, format.Get(), 1000.0f, 40.0f,
                            options);
    assert(layout);
    DWRITE_TEXT_METRICS metrics{};
    assert(SUCCEEDED(layout->GetMetrics(&metrics)));
    return metrics.widthIncludingTrailingWhitespace;
  };
  // The same text with different figure styles gets separate cache entries.
  auto first = cache.Get(factory.Get(), L"1111", format.Get(), 1000.0f, 40.0f,
                         proportional);
  auto second = cache.Get(factory.Get(), L"1111", format.Get(), 1000.0f, 40.0f,
                          tabular);
  assert(first && second && first.Get() != second.Get());
  assert(cache.Get(factory.Get(), L"1111", format.Get(), 1000.0f, 40.0f,
                   tabular)
             .Get() == second.Get());
  // With tabular figures every digit has the same advance, so values that
  // update in place do not shift.
  const float ones = width(L"1111", tabular);
  const float zeros = width(L"0000", tabular);
  const float eights = width(L"8888", tabular);
  assert(std::abs(ones - zeros) < 0.01f);
  assert(std::abs(eights - zeros) < 0.01f);
  // Empty text stays valid with the option set.
  assert(cache.Get(factory.Get(), L"", format.Get(), 100.0f, 20.0f, tabular));
}

}  // namespace

int main() {
  TestCubicBezier();
  TestMotionTokens();
  TestKeyRepeatCadence();
  TestPixelSnapping();
  TestShortcutHints();
  TestKeycapMetrics();
  TestTabularFigures();
  return 0;
}
