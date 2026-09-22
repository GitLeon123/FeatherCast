#pragma once

#include "extension_protocol.hpp"
#include "filesystem_semantics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace feathercast::theme {

struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Theme {
  std::wstring fontFamily = L"Segoe UI Variable Text";
  // Obsidian dark panel at ~92 % opacity over real DirectComposition transparency.
  Color overlayBackground{0.063f, 0.063f, 0.071f, 0.92f};
  Color settingsBackground{0.063f, 0.063f, 0.071f, 0.92f};
  Color border{1.0f, 1.0f, 1.0f, 0.08f};
  Color divider{1.0f, 1.0f, 1.0f, 0.08f};
  // surface: #18181B – modern Tailwind Zinc-900 slate grey.
  Color surface{0.094f, 0.094f, 0.106f, 1.0f};
  Color surfaceHover{1.0f, 1.0f, 1.0f, 0.12f};
  // selectedBase: dark opaque base that the Windows accent is tinted into for a readable,
  // accent-colored selection pill.
  Color selectedBase{0.11f, 0.11f, 0.13f, 1.0f};
  Color iconTile{0.23f, 0.23f, 0.28f, 1.0f};
  Color textPrimary{0.95f, 0.95f, 0.96f, 1.0f};
  Color textMuted{0.60f, 0.60f, 0.64f, 1.0f};
  Color textDim{0.53f, 0.53f, 0.58f, 1.0f};
  Color sectionText{0.62f, 0.64f, 0.74f, 1.0f};
  Color danger{1.0f, 0.36f, 0.36f, 1.0f};
  Color dangerText{1.0f, 0.36f, 0.36f, 1.0f};
  Color success{0.30f, 0.78f, 0.48f, 1.0f};
  Color recording{0.95f, 0.18f, 0.20f, 1.0f};
  Color accentFallback{0.36f, 0.42f, 1.0f, 1.0f};
  float overlayRadius = 10.0f;
  float settingsRadius = 10.0f;
  // rowRadius: 6.0f keeps selected rows subtly rounded inside the 10px outer panel.
  float rowRadius = 6.0f;
  float controlRadius = 8.0f;
};

inline Color ClampColor(Color color) noexcept {
  color.r = std::clamp(color.r, 0.0f, 1.0f);
  color.g = std::clamp(color.g, 0.0f, 1.0f);
  color.b = std::clamp(color.b, 0.0f, 1.0f);
  color.a = std::clamp(color.a, 0.0f, 1.0f);
  return color;
}

// DirectComposition surfaces are premultiplied and may contain translucent
// theme colors. Contrast must therefore be measured after compositing the
// foreground and the surface over the same opaque backdrop.
inline Color CompositeOver(Color foreground, Color background) noexcept {
  foreground = ClampColor(foreground);
  background = ClampColor(background);
  const float inverse = 1.0f - foreground.a;
  const float alpha = foreground.a + background.a * inverse;
  if (alpha <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
  return ClampColor({
      (foreground.r * foreground.a + background.r * background.a * inverse) /
          alpha,
      (foreground.g * foreground.a + background.g * background.a * inverse) /
          alpha,
      (foreground.b * foreground.a + background.b * background.a * inverse) /
          alpha,
      alpha,
  });
}

inline float SrgbToLinear(float channel) {
  channel = std::clamp(channel, 0.0f, 1.0f);
  return channel <= 0.04045f
             ? channel / 12.92f
             : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

inline float RelativeLuminance(const Color& color) {
  return 0.2126f * SrgbToLinear(color.r) +
         0.7152f * SrgbToLinear(color.g) +
         0.0722f * SrgbToLinear(color.b);
}

inline float ContrastRatio(const Color& first, const Color& second) {
  const float firstLuminance = RelativeLuminance(first);
  const float secondLuminance = RelativeLuminance(second);
  const float lighter = std::max(firstLuminance, secondLuminance);
  const float darker = std::min(firstLuminance, secondLuminance);
  return (lighter + 0.05f) / (darker + 0.05f);
}

inline float CompositedContrastRatio(const Color& foreground,
                                     const Color& background,
                                     const Color& canvas = {}) {
  const Color opaqueCanvas{canvas.r, canvas.g, canvas.b, 1.0f};
  const Color effectiveBackground =
      CompositeOver(background, opaqueCanvas);
  const Color effectiveForeground =
      CompositeOver(foreground, effectiveBackground);
  return ContrastRatio(effectiveForeground, effectiveBackground);
}

// Preserve a theme's hue as far as possible, but never let secondary text fall
// below the requested contrast against the panel it is rendered on.
inline Color EnsureContrast(Color foreground, const Color& background,
                            float minimumRatio = 4.5f,
                            const Color& canvas = {}) {
  foreground = ClampColor(foreground);
  foreground.a = 1.0f;
  if (CompositedContrastRatio(foreground, background, canvas) >=
      minimumRatio) {
    return foreground;
  }

  const Color effectiveBackground = CompositeOver(
      background, Color{canvas.r, canvas.g, canvas.b, 1.0f});
  const bool moveTowardWhite = RelativeLuminance(foreground) >
                                RelativeLuminance(effectiveBackground);
  const Color endpoint = moveTowardWhite
                             ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                             : Color{0.0f, 0.0f, 0.0f, 1.0f};
  Color candidate = foreground;
  for (int iteration = 0; iteration < 12; ++iteration) {
    const float amount =
        0.5f + 0.5f * static_cast<float>(iteration) / 11.0f;
    candidate.r = foreground.r + (endpoint.r - foreground.r) * amount;
    candidate.g = foreground.g + (endpoint.g - foreground.g) * amount;
    candidate.b = foreground.b + (endpoint.b - foreground.b) * amount;
    if (CompositedContrastRatio(candidate, background, canvas) >=
        minimumRatio) {
      return candidate;
    }
  }
  return endpoint;
}

template <std::size_t Count>
inline Color EnsureContrastOnSurfaces(Color foreground,
                                     const std::array<Color, Count>& surfaces,
                                     float minimumRatio) {
  foreground = ClampColor(foreground);
  foreground.a = 1.0f;
  const auto minimumContrast = [&](const Color& candidate) {
    float result = 100.0f;
    for (const auto& surface : surfaces) {
      result = std::min(result,
                        CompositedContrastRatio(candidate, surface));
    }
    return result;
  };
  if (minimumContrast(foreground) >= minimumRatio) return foreground;

  Color best = foreground;
  float bestRatio = minimumContrast(best);
  const std::array<Color, 2> endpoints = {
      Color{1.0f, 1.0f, 1.0f, 1.0f},
      Color{0.0f, 0.0f, 0.0f, 1.0f},
  };
  for (const auto& endpoint : endpoints) {
    for (int step = 1; step <= 32; ++step) {
      const float amount = static_cast<float>(step) / 32.0f;
      Color candidate = foreground;
      candidate.r += (endpoint.r - foreground.r) * amount;
      candidate.g += (endpoint.g - foreground.g) * amount;
      candidate.b += (endpoint.b - foreground.b) * amount;
      const float ratio = minimumContrast(candidate);
      if (ratio > bestRatio) {
        best = candidate;
        bestRatio = ratio;
      }
      if (ratio >= minimumRatio) return candidate;
    }
  }
  return best;
}

inline std::array<Color, 7> ThemeTextSurfaces(const Theme& theme) {
  const Color canvas{0.0f, 0.0f, 0.0f, 1.0f};
  const Color overlay = CompositeOver(theme.overlayBackground, canvas);
  const Color settings = CompositeOver(theme.settingsBackground, canvas);
  const Color surface = CompositeOver(theme.surface, overlay);
  const Color hover = CompositeOver(theme.surfaceHover, surface);
  const Color selected = CompositeOver(theme.selectedBase, overlay);
  const Color icon = CompositeOver(theme.iconTile, overlay);
  return {overlay, settings, surface, hover, selected, icon,
          CompositeOver(theme.surface, settings)};
}

inline Theme NormalizeTheme(Theme theme) {
  theme.overlayBackground = ClampColor(theme.overlayBackground);
  theme.settingsBackground = ClampColor(theme.settingsBackground);
  theme.border = ClampColor(theme.border);
  theme.divider = ClampColor(theme.divider);
  theme.surface = ClampColor(theme.surface);
  theme.surfaceHover = ClampColor(theme.surfaceHover);
  theme.selectedBase = ClampColor(theme.selectedBase);
  theme.iconTile = ClampColor(theme.iconTile);
  theme.textPrimary = ClampColor(theme.textPrimary);
  theme.textMuted = ClampColor(theme.textMuted);
  theme.textDim = ClampColor(theme.textDim);
  theme.sectionText = ClampColor(theme.sectionText);
  theme.danger = ClampColor(theme.danger);
  theme.dangerText = ClampColor(theme.dangerText);
  theme.success = ClampColor(theme.success);
  theme.recording = ClampColor(theme.recording);
  theme.accentFallback = ClampColor(theme.accentFallback);

  const auto surfaces = ThemeTextSurfaces(theme);
  theme.textPrimary = EnsureContrastOnSurfaces(theme.textPrimary, surfaces, 4.5f);
  theme.textMuted = EnsureContrastOnSurfaces(theme.textMuted, surfaces, 4.5f);
  theme.textDim = EnsureContrastOnSurfaces(theme.textDim, surfaces, 4.5f);
  theme.sectionText = EnsureContrastOnSurfaces(theme.sectionText, surfaces, 4.5f);
  theme.dangerText =
      EnsureContrastOnSurfaces(theme.dangerText, surfaces, 4.5f);

  // Status colors and the accent are used as control fills, focus rings, and
  // icon treatments. Three-to-one is the appropriate non-body-text floor.
  theme.danger = EnsureContrastOnSurfaces(theme.danger, surfaces, 3.0f);
  theme.success = EnsureContrastOnSurfaces(theme.success, surfaces, 3.0f);
  theme.recording = EnsureContrastOnSurfaces(theme.recording, surfaces, 3.0f);
  theme.accentFallback = EnsureContrastOnSurfaces(theme.accentFallback, surfaces,
                                                  3.0f);
  return theme;
}

inline Color NormalizeAccent(Color accent, const Theme& theme,
                             float minimumRatio = 3.0f) {
  const auto surfaces = ThemeTextSurfaces(theme);
  return EnsureContrastOnSurfaces(ClampColor(accent), surfaces, minimumRatio);
}

inline Theme HighContrastTheme(Theme theme, Color window, Color button,
                               Color highlight, Color windowText) {
  theme.overlayBackground = window;
  theme.settingsBackground = window;
  theme.surface = button;
  theme.surfaceHover = highlight;
  theme.selectedBase = highlight;
  theme.iconTile = button;
  theme.border = windowText;
  theme.divider = windowText;
  theme.textPrimary = windowText;
  // COLOR_GRAYTEXT is a disabled-control role. Ordinary secondary copy must
  // remain readable as normal window text in High Contrast mode.
  theme.textMuted = windowText;
  theme.textDim = windowText;
  theme.sectionText = windowText;
  theme.danger = highlight;
  theme.dangerText = windowText;
  theme.success = highlight;
  theme.recording = highlight;
  theme.accentFallback = highlight;
  return theme;
}

inline int HexNibble(wchar_t ch) {
  if (ch >= L'0' && ch <= L'9') return ch - L'0';
  ch = static_cast<wchar_t>(std::towlower(ch));
  if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
  return -1;
}

inline std::optional<Color> ParseHexColor(const std::wstring& value, float fallbackAlpha = 1.0f) {
  if (value.size() != 7 && value.size() != 9) return std::nullopt;
  if (value.front() != L'#') return std::nullopt;
  auto byteAt = [&](size_t index) -> std::optional<int> {
    const int hi = HexNibble(value[index]);
    const int lo = HexNibble(value[index + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    return hi * 16 + lo;
  };
  const auto r = byteAt(1);
  const auto g = byteAt(3);
  const auto b = byteAt(5);
  if (!r || !g || !b) return std::nullopt;
  float alpha = std::clamp(fallbackAlpha, 0.0f, 1.0f);
  if (value.size() == 9) {
    const auto a = byteAt(7);
    if (!a) return std::nullopt;
    alpha = *a / 255.0f;
  }
  return Color{*r / 255.0f, *g / 255.0f, *b / 255.0f, alpha};
}

inline void ApplyColor(const std::string& json, const char* key, Color& target) {
  if (const auto raw = feathercast::extensions::JsonString(json, key)) {
    if (const auto parsed = ParseHexColor(feathercast::extensions::Utf8ToWide(*raw), target.a)) {
      target = *parsed;
    }
  }
}

inline void ApplyFloat(const std::string& json, const char* key, float& target, float minValue, float maxValue) {
  if (const auto raw = feathercast::extensions::JsonNumber(json, key)) {
    target = std::clamp(static_cast<float>(*raw), minValue, maxValue);
  }
}

inline Theme ParseThemeJson(const std::string& json, const Theme& defaults = Theme{}) {
  Theme theme = defaults;
  if (const auto font = feathercast::extensions::JsonString(json, "fontFamily")) {
    const std::wstring parsed = feathercast::extensions::Utf8ToWide(*font);
    if (!parsed.empty()) theme.fontFamily = parsed;
  }

  ApplyColor(json, "overlayBackground", theme.overlayBackground);
  ApplyColor(json, "settingsBackground", theme.settingsBackground);
  ApplyColor(json, "border", theme.border);
  ApplyColor(json, "divider", theme.divider);
  ApplyColor(json, "surface", theme.surface);
  ApplyColor(json, "surfaceHover", theme.surfaceHover);
  ApplyColor(json, "selectedBase", theme.selectedBase);
  ApplyColor(json, "iconTile", theme.iconTile);
  ApplyColor(json, "textPrimary", theme.textPrimary);
  ApplyColor(json, "textMuted", theme.textMuted);
  ApplyColor(json, "textDim", theme.textDim);
  ApplyColor(json, "sectionText", theme.sectionText);
  ApplyColor(json, "danger", theme.danger);
  theme.dangerText = theme.danger;
  ApplyColor(json, "dangerText", theme.dangerText);
  ApplyColor(json, "success", theme.success);
  ApplyColor(json, "recording", theme.recording);
  ApplyColor(json, "accentFallback", theme.accentFallback);

  ApplyFloat(json, "overlayRadius", theme.overlayRadius, 0.0f, 32.0f);
  ApplyFloat(json, "settingsRadius", theme.settingsRadius, 0.0f, 32.0f);
  ApplyFloat(json, "rowRadius", theme.rowRadius, 0.0f, 20.0f);
  ApplyFloat(json, "controlRadius", theme.controlRadius, 0.0f, 20.0f);
  return theme;
}

inline Theme LoadTheme(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return Theme{};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return ParseThemeJson(buffer.str());
}

inline bool WriteDefaultTheme(const std::filesystem::path& path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  const auto presence =
      feathercast::filesystem_semantics::ClassifyPresence(exists, ec);
  if (presence == feathercast::filesystem_semantics::Presence::Error) {
    return false;
  }
  if (presence == feathercast::filesystem_semantics::Presence::Present) {
    return true;
  }
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return false;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file <<
      "{\n"
      "  \"fontFamily\": \"Segoe UI Variable Text, Segoe UI Variable, Inter, Segoe UI\",\n"
      "  \"overlayBackground\": \"#101012EB\",\n"
      "  \"settingsBackground\": \"#101012EB\",\n"
      "  \"border\": \"#FFFFFF14\",\n"
      "  \"divider\": \"#FFFFFF14\",\n"
      "  \"surface\": \"#18181B\",\n"
      "  \"surfaceHover\": \"#FFFFFF1F\",\n"
      "  \"selectedBase\": \"#1C1C20\",\n"
      "  \"iconTile\": \"#3B3B47\",\n"
      "  \"textPrimary\": \"#F2F2F5\",\n"
      "  \"textMuted\": \"#9999A3\",\n"
      "  \"textDim\": \"#878793\",\n"
      "  \"sectionText\": \"#9EA3BD\",\n"
      "  \"danger\": \"#FF5C5C\",\n"
      "  \"dangerText\": \"#FF5C5C\",\n"
      "  \"success\": \"#4DC77A\",\n"
      "  \"recording\": \"#F22E33\",\n"
      "  \"accentFallback\": \"#5B6CFF\",\n"
      "  \"overlayRadius\": 10,\n"
      "  \"settingsRadius\": 10,\n"
      "  \"rowRadius\": 6,\n"
      "  \"controlRadius\": 8\n"
      "}\n";
  return true;
}

}  // namespace feathercast::theme
