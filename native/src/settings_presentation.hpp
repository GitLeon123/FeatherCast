#pragma once

// Pure presentation helpers for the Settings window: how stored values are
// shown to people, and device-pixel snapping for crisp hairlines. Nothing in
// here touches Win32 or Direct2D so it can be unit tested directly.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::settings_ui {

// U+2026 HORIZONTAL ELLIPSIS, used for "in progress" and "more follows" labels.
inline constexpr wchar_t kEllipsis = static_cast<wchar_t>(0x2026);

namespace detail {

inline std::wstring_view TrimView(std::wstring_view value) {
  while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
  while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
  return value;
}

inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::towlower(left[index]) != std::towlower(right[index])) return false;
  }
  return true;
}

// Stored shortcuts use the hook's portable names ("Control", "Super").
// Windows users read and type "Ctrl" and "Win", so only the label changes.
inline std::wstring_view DisplayToken(std::wstring_view token) {
  if (EqualsIgnoreCase(token, L"control") || EqualsIgnoreCase(token, L"ctrl")) {
    return L"Ctrl";
  }
  if (EqualsIgnoreCase(token, L"super") || EqualsIgnoreCase(token, L"win")) {
    return L"Win";
  }
  if (EqualsIgnoreCase(token, L"alt")) return L"Alt";
  if (EqualsIgnoreCase(token, L"shift")) return L"Shift";
  if (EqualsIgnoreCase(token, L"return")) return L"Enter";
  if (EqualsIgnoreCase(token, L"escape")) return L"Esc";
  if (EqualsIgnoreCase(token, L"pageup")) return L"Page Up";
  if (EqualsIgnoreCase(token, L"pagedown")) return L"Page Down";
  return token;
}

}  // namespace detail

inline bool ShortcutAssigned(std::wstring_view stored) {
  stored = detail::TrimView(stored);
  return !stored.empty() && !detail::EqualsIgnoreCase(stored, L"none");
}

// "Control+Shift+Space" -> "Ctrl+Shift+Space"; unassigned -> "Not set".
inline std::wstring ShortcutDisplayLabel(std::wstring_view stored) {
  stored = detail::TrimView(stored);
  if (!ShortcutAssigned(stored)) return L"Not set";
  std::wstring out;
  std::size_t start = 0;
  while (start <= stored.size()) {
    std::size_t end = stored.find(L'+', start);
    // A trailing "+" belongs to the key itself (e.g. "Control++").
    if (end == start && end + 1 == stored.size()) end = std::wstring_view::npos;
    const std::wstring_view token = detail::TrimView(
        stored.substr(start, end == std::wstring_view::npos ? stored.size() - start
                                                            : end - start));
    if (!out.empty()) out += L'+';
    out += detail::DisplayToken(token);
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return out;
}

// Live preview while recording: shows the modifiers that are held down so
// people can see the chord forming before they press the final key.
inline std::wstring RecordingPreviewLabel(bool ctrl, bool alt, bool shift,
                                          bool win) {
  std::wstring out;
  const auto add = [&out](std::wstring_view part) {
    out += part;
    out += L'+';
  };
  if (ctrl) add(L"Ctrl");
  if (alt) add(L"Alt");
  if (shift) add(L"Shift");
  if (win) add(L"Win");
  if (out.empty()) return std::wstring(L"Press keys") + kEllipsis;
  out += kEllipsis;
  return out;
}

struct ShortcutAssignment {
  std::wstring owner;
  std::wstring shortcut;
};

// Returns the action that already uses candidate, so a conflict can name it
// where the choice is made instead of in a generic warning dialog.
inline std::optional<std::wstring> ShortcutConflictOwner(
    std::wstring_view candidate,
    const std::vector<ShortcutAssignment>& assignments) {
  candidate = detail::TrimView(candidate);
  if (!ShortcutAssigned(candidate)) return std::nullopt;
  for (const auto& assignment : assignments) {
    if (!ShortcutAssigned(assignment.shortcut)) continue;
    if (detail::EqualsIgnoreCase(detail::TrimView(assignment.shortcut),
                                 candidate)) {
      return assignment.owner;
    }
  }
  return std::nullopt;
}

inline std::wstring ShortcutConflictMessage(std::wstring_view storedShortcut,
                                            std::wstring_view owner) {
  return ShortcutDisplayLabel(storedShortcut) + L" is already used by " +
         std::wstring(owner) + L". Pick another shortcut or clear that one first.";
}

// Thousands separators keep large counts scannable: 12345 -> "12,345".
inline std::wstring FormatCount(std::size_t value) {
  std::wstring digits = std::to_wstring(value);
  for (std::size_t index = digits.size(); index > 3; index -= 3) {
    digits.insert(index - 3, 1, L',');
  }
  return digits;
}

// "1 app" / "3 apps" / "12,000 apps".
inline std::wstring CountLabel(std::size_t value, std::wstring_view singular,
                               std::wstring_view plural) {
  return FormatCount(value) + L' ' +
         std::wstring(value == 1 ? singular : plural);
}

// Device-pixel snapping. Geometry is authored in DIPs; at fractional scales
// (125%, 150%) unsnapped edges straddle two device pixels and look soft.
inline float SnapToDevicePixel(float dip, float scale) {
  scale = std::max(0.01f, scale);
  return std::round(dip * scale) / scale;
}

// Centre of the device pixel row/column containing dip, so a stroke exactly
// one device pixel wide lands on a single pixel instead of blurring over two.
inline float SnapHairlineCenter(float dip, float scale) {
  scale = std::max(0.01f, scale);
  return (std::floor(dip * scale) + 0.5f) / scale;
}

// Stroke width (in DIPs) of exactly one device pixel.
inline float HairlineWidth(float scale) { return 1.0f / std::max(0.01f, scale); }

}  // namespace feathercast::settings_ui
