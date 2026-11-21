#pragma once

// Pure helpers for pixel-exact drawing and keycap hints. Drawing happens in
// device-independent pixels (DIPs) on a Direct2D context whose DPI matches the
// window, so at 125/150/175% a DIP coordinate rarely lands on a device pixel.
// Edges and hairlines are snapped here so fills stay crisp and 1 px strokes
// never smear across two pixel rows.

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::visual {

inline float SafeScale(float scale) {
  return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

// Nearest device-pixel boundary, in DIPs.
inline float SnapToPixelEdge(float dip, float scale) {
  scale = SafeScale(scale);
  return std::round(dip * scale) / scale;
}

// Width of a hairline in device pixels: one pixel up to 199%, two from 200%.
// Fractional widths would be antialiased into a blurry double line.
inline float HairlinePixels(float scale) {
  return std::max(1.0f, std::floor(SafeScale(scale) + 0.001f));
}

inline float HairlineWidth(float scale) {
  scale = SafeScale(scale);
  return HairlinePixels(scale) / scale;
}

// Centre coordinate (DIPs) for a stroke of `widthPixels` device pixels whose
// leading side starts at the device-pixel boundary nearest to `dip`. Odd
// widths land on half-pixel centres, even widths on pixel boundaries.
inline float SnapStrokeCenter(float dip, float scale, float widthPixels) {
  scale = SafeScale(scale);
  const float edge = std::round(dip * scale);
  return (edge + widthPixels * 0.5f) / scale;
}

// Snaps every edge of a rectangle to the nearest device-pixel boundary. Size
// differences of up to one pixel are preferable to antialiased soft edges.
template <typename Rect>
Rect SnapRectToPixels(Rect rect, float scale) {
  rect.left = SnapToPixelEdge(rect.left, scale);
  rect.top = SnapToPixelEdge(rect.top, scale);
  rect.right = SnapToPixelEdge(rect.right, scale);
  rect.bottom = SnapToPixelEdge(rect.bottom, scale);
  return rect;
}

// Snaps the origin and rounds the size separately, so an icon of fixed DIP
// size keeps the same pixel size at every position (no 1 px wobble while a
// row moves) and is never resampled across a pixel boundary.
template <typename Rect>
Rect SnapSizedRectToPixels(Rect rect, float scale) {
  scale = SafeScale(scale);
  const float width = std::max(1.0f, std::round((rect.right - rect.left) * scale));
  const float height = std::max(1.0f, std::round((rect.bottom - rect.top) * scale));
  const float left = std::round(rect.left * scale);
  const float top = std::round(rect.top * scale);
  rect.left = left / scale;
  rect.top = top / scale;
  rect.right = (left + width) / scale;
  rect.bottom = (top + height) / scale;
  return rect;
}

// Rectangle for stroking a hairline border on the innermost device pixels of
// `rect` (an "inside" border). The rect is pixel-snapped first.
template <typename Rect>
Rect InsideStrokeRect(Rect rect, float scale, float widthPixels) {
  scale = SafeScale(scale);
  rect = SnapRectToPixels(rect, scale);
  const float inset = widthPixels * 0.5f / scale;
  rect.left += inset;
  rect.top += inset;
  rect.right -= inset;
  rect.bottom -= inset;
  return rect;
}

// One " · "-separated part of a shortcut hint such as
// "Enter open · Tab actions · Ctrl+Shift+Enter admin". `keys` holds the key
// names of a chord in order; a part that does not start with a key (for
// example "Downloading...") has no keys and is drawn as plain text.
struct ShortcutHintPart {
  std::vector<std::wstring> keys;
  std::wstring label;
  bool operator==(const ShortcutHintPart&) const = default;
};

inline bool IsShortcutKeyName(std::wstring_view token) {
  if (token.empty()) return false;
  if (token.size() == 1) {
    // Single printable characters: capital letters, digits and the scope
    // sigils. Lowercase words such as "a" are prose, not keys.
    const wchar_t ch = token.front();
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
           ch == L'@' || ch == L'?' || ch == L'/' ||
           ch == L'.' || ch == L',' || ch == L';' || ch == L'\'' ||
           ch == L'[' || ch == L']' || ch == L'-' || ch == L'=';
  }
  if ((token.front() == L'F' || token.front() == L'f') && token.size() <= 3) {
    const std::wstring_view digits = token.substr(1);
    if (std::all_of(digits.begin(), digits.end(),
                    [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
      const int number = std::stoi(std::wstring(digits));
      return number >= 1 && number <= 24;
    }
  }
  static constexpr std::wstring_view kNames[] = {
      L"Enter", L"Tab",   L"Esc",   L"Space", L"Backspace", L"Del",
      L"Delete", L"Ins",  L"Home",  L"End",   L"PgUp",      L"PgDn",
      L"Up",    L"Down",  L"Left",  L"Right", L"Ctrl",      L"Shift",
      L"Alt",   L"Win"};
  return std::find(std::begin(kNames), std::end(kNames), token) !=
         std::end(kNames);
}

// Splits "Ctrl+Shift+Enter" into its keys. A lone "+" is a key itself.
inline std::vector<std::wstring> SplitShortcutChord(std::wstring_view token) {
  std::vector<std::wstring> keys;
  if (token == L"+") {
    keys.emplace_back(token);
    return keys;
  }
  std::size_t start = 0;
  while (start <= token.size()) {
    const std::size_t plus = token.find(L'+', start);
    const std::size_t end = plus == std::wstring_view::npos ? token.size() : plus;
    const std::wstring_view key = token.substr(start, end - start);
    if (!IsShortcutKeyName(key)) return {};
    keys.emplace_back(key);
    if (plus == std::wstring_view::npos) break;
    start = plus + 1;
  }
  return keys;
}

inline std::wstring_view TrimHintSpaces(std::wstring_view text) {
  while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
  while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
  return text;
}

inline ShortcutHintPart ParseShortcutHintPart(std::wstring_view part) {
  part = TrimHintSpaces(part);
  ShortcutHintPart parsed;
  const std::size_t space = part.find(L' ');
  const std::wstring_view first =
      space == std::wstring_view::npos ? part : part.substr(0, space);
  parsed.keys = SplitShortcutChord(first);
  if (parsed.keys.empty()) {
    parsed.label = std::wstring(part);
    return parsed;
  }
  if (space != std::wstring_view::npos) {
    parsed.label = std::wstring(TrimHintSpaces(part.substr(space + 1)));
  }
  return parsed;
}

inline std::vector<ShortcutHintPart> ParseShortcutHints(std::wstring_view hint) {
  static constexpr std::wstring_view kSeparator = L" · ";
  std::vector<ShortcutHintPart> parts;
  std::size_t start = 0;
  while (start <= hint.size()) {
    const std::size_t separator = hint.find(kSeparator, start);
    const std::size_t end =
        separator == std::wstring_view::npos ? hint.size() : separator;
    const std::wstring_view part = TrimHintSpaces(hint.substr(start, end - start));
    if (!part.empty()) parts.push_back(ParseShortcutHintPart(part));
    if (separator == std::wstring_view::npos) break;
    start = separator + kSeparator.size();
  }
  return parts;
}

// Keycap geometry tokens (DIPs at 100% text size): 12 px caption text, a
// 4 px radius, and gaps on the 4 px grid.
struct KeycapMetrics {
  float height = 20.0f;
  float minWidth = 20.0f;
  float paddingX = 6.0f;
  float radius = 4.0f;
  float chordGap = 4.0f;
  float labelGap = 6.0f;
  float partGap = 16.0f;
};

inline KeycapMetrics ScaledKeycapMetrics(float textScale) {
  textScale = std::clamp(std::isfinite(textScale) ? textScale : 1.0f, 0.5f, 3.0f);
  KeycapMetrics metrics;
  metrics.height *= textScale;
  metrics.minWidth *= textScale;
  metrics.paddingX *= textScale;
  metrics.chordGap *= textScale;
  metrics.labelGap *= textScale;
  metrics.partGap *= textScale;
  return metrics;
}

// Width of one hint part given measured text widths for its keys and label.
inline float ShortcutHintPartWidth(const std::vector<float>& keyTextWidths,
                                   float labelWidth,
                                   const KeycapMetrics& metrics) {
  float width = 0.0f;
  for (std::size_t i = 0; i < keyTextWidths.size(); ++i) {
    if (i > 0) width += metrics.chordGap;
    width += std::max(metrics.minWidth,
                      keyTextWidths[i] + metrics.paddingX * 2.0f);
  }
  if (labelWidth > 0.0f) {
    if (!keyTextWidths.empty()) width += metrics.labelGap;
    width += labelWidth;
  }
  return width;
}

// Number of leading parts that fit into `available` DIPs. The first part
// carries the primary action, so trailing parts are dropped first.
inline std::size_t FittingHintParts(const std::vector<float>& partWidths,
                                    float available,
                                    const KeycapMetrics& metrics) {
  float used = 0.0f;
  std::size_t count = 0;
  for (const float width : partWidths) {
    const float next = used + (count > 0 ? metrics.partGap : 0.0f) + width;
    if (next > available + 0.01f) break;
    used = next;
    ++count;
  }
  return count;
}

}  // namespace feathercast::visual
