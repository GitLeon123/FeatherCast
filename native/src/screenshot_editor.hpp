#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::screenshot {

struct Point {
  int x = 0;
  int y = 0;

  constexpr bool operator==(const Point&) const = default;
};

// Screenshot geometry is stored in physical pixels.  The editor can be
// painted at a different per-monitor DPI, so all screen-facing layout code
// goes through these small, deterministic conversions instead of maintaining
// a second rounded copy of the draft geometry.
struct DipPoint {
  float x = 0.0f;
  float y = 0.0f;

  constexpr bool operator==(const DipPoint&) const = default;
};

struct DipRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  constexpr float Width() const noexcept { return right - left; }
  constexpr float Height() const noexcept { return bottom - top; }
  constexpr bool Empty() const noexcept {
    return right <= left || bottom <= top;
  }
  constexpr bool operator==(const DipRect&) const = default;
};

inline float ValidDpiScale(float scale) noexcept {
  return std::max(0.01f, scale);
}

inline int DipToPixel(float dip, float scale) noexcept {
  return static_cast<int>(std::lround(dip * ValidDpiScale(scale)));
}

inline float PixelToDip(int pixel, float scale) noexcept {
  return static_cast<float>(pixel) / ValidDpiScale(scale);
}

inline DipPoint PixelToDip(Point point, float scale) noexcept {
  return {PixelToDip(point.x, scale), PixelToDip(point.y, scale)};
}

inline DipPoint PixelToDip(Point point, Point origin, float scale) noexcept {
  return PixelToDip({point.x - origin.x, point.y - origin.y}, scale);
}

inline Point DipToPixel(DipPoint point, float scale) noexcept {
  return {DipToPixel(point.x, scale), DipToPixel(point.y, scale)};
}

struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  constexpr int Width() const noexcept { return right - left; }
  constexpr int Height() const noexcept { return bottom - top; }
  constexpr bool Empty() const noexcept {
    return right <= left || bottom <= top;
  }
  constexpr bool operator==(const Rect&) const = default;
};

inline DipRect PixelToDip(Rect rect, Point origin, float scale) noexcept {
  return {PixelToDip(rect.left - origin.x, scale),
          PixelToDip(rect.top - origin.y, scale),
          PixelToDip(rect.right - origin.x, scale),
          PixelToDip(rect.bottom - origin.y, scale)};
}

inline Rect DipToPixel(DipRect rect, Point origin, float scale) noexcept {
  return {origin.x + DipToPixel(rect.left, scale),
          origin.y + DipToPixel(rect.top, scale),
          origin.x + DipToPixel(rect.right, scale),
          origin.y + DipToPixel(rect.bottom, scale)};
}

inline Rect Normalize(Rect rect) noexcept {
  if (rect.left > rect.right) std::swap(rect.left, rect.right);
  if (rect.top > rect.bottom) std::swap(rect.top, rect.bottom);
  return rect;
}

inline Rect ClampRect(Rect rect, Rect bounds, int minimumSize = 2) noexcept {
  bounds = Normalize(bounds);
  rect = Normalize(rect);
  if (bounds.Empty()) return bounds;
  const int boundedMinimum = std::max(1, std::min(minimumSize, bounds.Width()));
  const int boundedMinimumHeight =
      std::max(1, std::min(minimumSize, bounds.Height()));
  const int width = std::clamp(rect.Width(), boundedMinimum, bounds.Width());
  const int height =
      std::clamp(rect.Height(), boundedMinimumHeight, bounds.Height());
  const int maxLeft = bounds.right - width;
  const int maxTop = bounds.bottom - height;
  rect.left = std::clamp(rect.left, bounds.left, maxLeft);
  rect.top = std::clamp(rect.top, bounds.top, maxTop);
  rect.right = rect.left + width;
  rect.bottom = rect.top + height;
  return rect;
}

inline bool Contains(Rect rect, Point point) noexcept {
  rect = Normalize(rect);
  return point.x >= rect.left && point.x < rect.right &&
         point.y >= rect.top && point.y < rect.bottom;
}

inline bool Contains(DipRect rect, DipPoint point) noexcept {
  return point.x >= rect.left && point.x < rect.right &&
         point.y >= rect.top && point.y < rect.bottom;
}

enum class Tool {
  Select,
  Rectangle,
  Ellipse,
  Line,
  Arrow,
  Freehand,
  Text,
  Blur,
  Pixelate,
};

enum class Destination { File, Clipboard };

// This is deliberately shared with the Win32 toolbar projection.  Keeping
// the action order and bounds in a pure contract prevents paint, mouse hit
// testing, and MSAA from drifting apart at narrow widths.
enum class ToolbarAction {
  Select,
  Rectangle,
  Ellipse,
  Line,
  Arrow,
  Freehand,
  Text,
  Blur,
  Pixelate,
  Undo,
  Redo,
  ColorAccent,
  ColorRed,
  ColorYellow,
  ColorWhite,
  ColorBlack,
  StrokeDown,
  StrokeUp,
  Save,
  Copy,
  Cancel,
};

struct ToolbarButtonLayout {
  DipRect rect;
  ToolbarAction action = ToolbarAction::Select;
  const wchar_t* label = L"";
};

struct ToolbarLayout {
  DipRect bar;
  DipRect footer;
  std::vector<ToolbarButtonLayout> buttons;
  int rows = 0;
};

inline float ToolbarPreferredWidth(ToolbarAction action, bool compact) noexcept {
  switch (action) {
    case ToolbarAction::Select: return compact ? 54.0f : 62.0f;
    case ToolbarAction::Rectangle: return compact ? 66.0f : 78.0f;
    case ToolbarAction::Ellipse: return compact ? 56.0f : 64.0f;
    case ToolbarAction::Line: return compact ? 48.0f : 56.0f;
    case ToolbarAction::Arrow: return compact ? 50.0f : 58.0f;
    case ToolbarAction::Freehand: return compact ? 64.0f : 76.0f;
    case ToolbarAction::Text: return compact ? 48.0f : 54.0f;
    case ToolbarAction::Blur: return compact ? 48.0f : 54.0f;
    case ToolbarAction::Pixelate: return compact ? 60.0f : 70.0f;
    case ToolbarAction::Undo:
    case ToolbarAction::Redo: return compact ? 50.0f : 58.0f;
    case ToolbarAction::ColorAccent:
    case ToolbarAction::ColorRed:
    case ToolbarAction::ColorYellow:
    case ToolbarAction::ColorWhite:
    case ToolbarAction::ColorBlack: return 30.0f;
    case ToolbarAction::StrokeDown:
    case ToolbarAction::StrokeUp: return compact ? 58.0f : 68.0f;
    case ToolbarAction::Save:
    case ToolbarAction::Copy:
    case ToolbarAction::Cancel: return compact ? 64.0f : 76.0f;
  }
  return 48.0f;
}

inline const wchar_t* ToolbarActionLabel(ToolbarAction action) noexcept {
  switch (action) {
    case ToolbarAction::Select: return L"Select";
    case ToolbarAction::Rectangle: return L"Rectangle";
    case ToolbarAction::Ellipse: return L"Ellipse";
    case ToolbarAction::Line: return L"Line";
    case ToolbarAction::Arrow: return L"Arrow";
    case ToolbarAction::Freehand: return L"Freehand";
    case ToolbarAction::Text: return L"Text";
    case ToolbarAction::Blur: return L"Blur";
    case ToolbarAction::Pixelate: return L"Pixelate";
    case ToolbarAction::Undo: return L"Undo";
    case ToolbarAction::Redo: return L"Redo";
    case ToolbarAction::ColorAccent: return L"Accent color";
    case ToolbarAction::ColorRed: return L"Red color";
    case ToolbarAction::ColorYellow: return L"Yellow color";
    case ToolbarAction::ColorWhite: return L"White color";
    case ToolbarAction::ColorBlack: return L"Black color";
    case ToolbarAction::StrokeDown: return L"Stroke -";
    case ToolbarAction::StrokeUp: return L"Stroke +";
    case ToolbarAction::Save: return L"Save";
    case ToolbarAction::Copy: return L"Copy";
    case ToolbarAction::Cancel: return L"Cancel";
  }
  return L"";
}

inline ToolbarLayout BuildToolbarLayout(float width, float height) {
  width = std::max(0.0f, width);
  height = std::max(0.0f, height);
  const bool compact = width < 920.0f;
  const float outer = std::min(8.0f, width * 0.5f);
  const float left = outer;
  const float right = std::max(left, width - outer);
  const float inset = std::min(8.0f, width * 0.25f);
  const float innerLeft = std::min(right, left + inset);
  const float innerRight = std::max(innerLeft, right - inset);
  const float available = std::max(0.0f, innerRight - innerLeft);
  constexpr float gap = 4.0f;
  constexpr float rowHeight = 32.0f;

  constexpr std::array<ToolbarAction, 21> actions = {
      ToolbarAction::Select,       ToolbarAction::Rectangle,
      ToolbarAction::Ellipse,       ToolbarAction::Line,
      ToolbarAction::Arrow,         ToolbarAction::Freehand,
      ToolbarAction::Text,          ToolbarAction::Blur,
      ToolbarAction::Pixelate,      ToolbarAction::Undo,
      ToolbarAction::Redo,          ToolbarAction::ColorAccent,
      ToolbarAction::ColorRed,      ToolbarAction::ColorYellow,
      ToolbarAction::ColorWhite,    ToolbarAction::ColorBlack,
      ToolbarAction::StrokeDown,    ToolbarAction::StrokeUp,
      ToolbarAction::Save,          ToolbarAction::Copy,
      ToolbarAction::Cancel};

  ToolbarLayout result;
  result.buttons.reserve(actions.size());
  float x = innerLeft;
  float y = 0.0f;
  for (const ToolbarAction action : actions) {
    const float preferred = std::min(ToolbarPreferredWidth(action, compact),
                                     available);
    const float buttonWidth = std::min(preferred, available);
    if (x > innerLeft && x + buttonWidth > innerRight) {
      x = innerLeft;
      y += rowHeight + gap;
    }
    // A button is always clamped to the same client-area contract used by
    // hit testing and accessibility.  This also handles synthetic tiny
    // widths in unit tests without emitting an out-of-bounds rectangle.
    const float buttonRight = std::min(innerRight, x + buttonWidth);
    result.buttons.push_back({{x, y, buttonRight, y + rowHeight}, action,
                              ToolbarActionLabel(action)});
    x = buttonRight + gap;
  }
  // The previous button's y coordinate is no longer enough to identify rows
  // after a wrap, so derive the row count from the maximum y in the nominal
  // layout.  Rows are compressed below when a synthetic or unusually short
  // client area cannot fit the normal 32-DIP controls.
  result.rows = 0;
  for (const auto& button : result.buttons) {
    result.rows = std::max(
        result.rows,
        static_cast<int>(std::lround(button.rect.top / (rowHeight + gap))) +
            1);
  }

  const float rowTop = std::min(14.0f, height * 0.25f);
  const float bottomPadding = std::min(8.0f, height * 0.05f);
  const float desiredBarBottom =
      rowTop + result.rows * rowHeight +
      std::max(0, result.rows - 1) * gap + bottomPadding;
  const float barBottom = std::min(height, desiredBarBottom);
  const float rowArea =
      std::max(0.0f, barBottom - rowTop - bottomPadding);
  const float actualGap = result.rows > 1
                              ? std::min(gap, rowArea / result.rows)
                              : 0.0f;
  const float actualRowHeight =
      result.rows > 0
          ? std::max(0.0f,
                     (rowArea - (result.rows - 1) * actualGap) / result.rows)
          : 0.0f;
  if (!result.buttons.empty()) {
    for (auto& button : result.buttons) {
      const int row = static_cast<int>(
          std::lround(button.rect.top / (rowHeight + gap)));
      button.rect.top = std::min(height, rowTop +
                                            row * (actualRowHeight + actualGap));
      button.rect.bottom = std::min(
          height, button.rect.top + actualRowHeight);
    }
  }
  result.bar = {std::min(width, left), std::min(height, 6.0f),
                std::min(width, right), std::max(std::min(height, 6.0f),
                                                 barBottom)};
  const float footerTop = std::min(
      height, std::max(result.bar.bottom + 8.0f, height - 34.0f));
  result.footer = {std::min(width, 16.0f), footerTop,
                   std::max(std::min(width, 16.0f), width - 16.0f),
                   std::min(height, std::max(footerTop, height - 10.0f))};
  return result;
}

inline bool ToolbarContains(const ToolbarButtonLayout& button, DipPoint point) {
  return Contains(button.rect, point);
}

inline std::vector<int> ToolbarFocusableIndices(
    const ToolbarLayout& layout, const std::vector<bool>& enabled) {
  std::vector<int> result;
  result.reserve(layout.buttons.size());
  for (std::size_t index = 0;
       index < layout.buttons.size() && index < enabled.size(); ++index) {
    if (enabled[index]) result.push_back(static_cast<int>(index));
  }
  return result;
}

enum class Handle {
  None,
  Move,
  TopLeft,
  Top,
  TopRight,
  Right,
  BottomRight,
  Bottom,
  BottomLeft,
  Left,
};

enum class Phase {
  Idle,
  Preparing,
  Selecting,
  Editing,
  Saving,
  Copying,
};

struct Color {
  std::uint8_t red = 255;
  std::uint8_t green = 92;
  std::uint8_t blue = 92;
  std::uint8_t alpha = 255;

  constexpr bool operator==(const Color&) const = default;
};

struct Annotation {
  Tool tool = Tool::Rectangle;
  Rect bounds;
  std::vector<Point> points;
  std::wstring text;
  Color color;
  std::uint32_t strokeWidth = 4;
  std::uint32_t fontSize = 24;
  std::wstring fontFamily = L"Segoe UI";
};

inline Rect AnnotationTextBounds(const Annotation& annotation) noexcept {
  const Rect rect = Normalize(annotation.bounds);
  constexpr int padding = 2;
  return {rect.left + padding, rect.top + padding,
          std::max(rect.left + padding, rect.right - padding),
          std::max(rect.top + padding, rect.bottom - padding)};
}

// A screenshot draft is deliberately CPU-owned and immutable once it reaches
// the UI. It is never backed by a file or by the system clipboard.
struct Draft {
  Rect sourceBounds;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;

  [[nodiscard]] bool Valid() const noexcept {
    return sourceBounds.Width() == static_cast<int>(width) &&
           sourceBounds.Height() == static_cast<int>(height) && width > 0 &&
           height > 0 && static_cast<std::uint64_t>(stride) >=
                              static_cast<std::uint64_t>(width) * 4u && pixels &&
           pixels->size() >= static_cast<std::size_t>(stride) * height;
  }
};

struct EditorState {
  Phase phase = Phase::Idle;
  Rect sourceBounds;
  std::optional<Rect> selection;
  Tool tool = Tool::Select;
  Color color;
  std::uint32_t strokeWidth = 4;
  std::uint32_t fontSize = 24;
  std::wstring fontFamily = L"Segoe UI";
  std::vector<Annotation> annotations;
  std::vector<Annotation> redo;
  std::optional<Handle> activeHandle;
  std::optional<Point> gestureStart;
  std::vector<Point> gesturePoints;
  int focusIndex = 0;
  int hoverIndex = -1;
};

enum class EditorKey { Left, Up, Right, Down, Enter, Escape };

enum class KeyboardResult { Ignored, Moved, Confirmed, Cancelled };

class EditorController {
 public:
  static bool Begin(EditorState& state, Rect sourceBounds,
                    std::optional<Rect> initialSelection = std::nullopt) {
    if (state.phase != Phase::Idle || sourceBounds.Width() < 2 ||
        sourceBounds.Height() < 2) {
      return false;
    }
    state = {};
    state.sourceBounds = Normalize(sourceBounds);
    if (initialSelection) {
      state.selection = ClampRect(*initialSelection, state.sourceBounds);
      state.phase = Phase::Editing;
    } else {
      state.phase = Phase::Selecting;
    }
    return true;
  }

  static bool BeginPreparing(EditorState& state, Rect sourceBounds) {
    if (state.phase != Phase::Idle || sourceBounds.Width() < 2 ||
        sourceBounds.Height() < 2) {
      return false;
    }
    state = {};
    state.sourceBounds = Normalize(sourceBounds);
    state.phase = Phase::Preparing;
    return true;
  }

  static bool SetReady(EditorState& state,
                       std::optional<Rect> initialSelection = std::nullopt) {
    if (state.phase != Phase::Preparing) return false;
    if (initialSelection) {
      state.selection = ClampRect(*initialSelection, state.sourceBounds);
      state.phase = Phase::Editing;
    } else {
      state.phase = Phase::Selecting;
    }
    return true;
  }

  static bool BeginSelection(EditorState& state, Point point) {
    if (state.phase != Phase::Selecting) return false;
    state.gestureStart = ClampPoint(point, state.sourceBounds);
    state.gesturePoints = {*state.gestureStart};
    return true;
  }

  static bool UpdateSelection(EditorState& state, Point point) {
    if (state.phase != Phase::Selecting || !state.gestureStart) return false;
    state.gesturePoints = {*state.gestureStart,
                           ClampPoint(point, state.sourceBounds)};
    return true;
  }

  static bool FinishSelection(EditorState& state, Point point) {
    if (!UpdateSelection(state, point)) return false;
    const Point end = state.gesturePoints.back();
    const Rect candidate{std::min(state.gestureStart->x, end.x),
                         std::min(state.gestureStart->y, end.y),
                         std::max(state.gestureStart->x, end.x),
                         std::max(state.gestureStart->y, end.y)};
    if (candidate.Width() < 2 || candidate.Height() < 2) return false;
    state.selection = ClampRect(candidate, state.sourceBounds);
    state.gestureStart.reset();
    state.gesturePoints.clear();
    state.phase = Phase::Editing;
    state.tool = Tool::Select;
    return true;
  }

  static bool EnsureKeyboardSelection(EditorState& state) {
    if (state.phase != Phase::Selecting && state.phase != Phase::Editing) {
      return false;
    }
    if (state.selection && !state.selection->Empty()) return true;
    const Rect bounds = Normalize(state.sourceBounds);
    if (bounds.Width() < 2 || bounds.Height() < 2) return false;
    const int width = std::min(bounds.Width(), std::max(2, std::min(320,
                                                                      bounds.Width())));
    const int height = std::min(bounds.Height(), std::max(2, std::min(200,
                                                                        bounds.Height())));
    const int left = bounds.left + (bounds.Width() - width) / 2;
    const int top = bounds.top + (bounds.Height() - height) / 2;
    state.selection = ClampRect({left, top, left + width, top + height}, bounds);
    return true;
  }

  // Move the selection by one logical pixel step, or resize its trailing edge
  // when Shift is held.  The coordinates remain physical pixels; Ctrl only
  // changes the increment so keyboard editing stays deterministic at every
  // monitor scale.
  static bool MoveSelection(EditorState& state, int horizontal, int vertical,
                            bool resize, int step = 1) {
    if (horizontal == 0 && vertical == 0) return false;
    if (!EnsureKeyboardSelection(state) || !state.selection) return false;
    const Rect before = Normalize(*state.selection);
    Rect next = before;
    const Rect bounds = Normalize(state.sourceBounds);
    const int increment = std::max(1, step);
    if (resize) {
      if (horizontal < 0) {
        next.right = std::max(next.left + 2, next.right - increment);
      } else if (horizontal > 0) {
        next.right = std::min(bounds.right, next.right + increment);
      }
      if (vertical < 0) {
        next.bottom = std::max(next.top + 2, next.bottom - increment);
      } else if (vertical > 0) {
        next.bottom = std::min(bounds.bottom, next.bottom + increment);
      }
    } else {
      const int dx = horizontal == 0 ? 0 : (horizontal < 0 ? -increment : increment);
      const int dy = vertical == 0 ? 0 : (vertical < 0 ? -increment : increment);
      const int width = next.Width();
      const int height = next.Height();
      next.left = std::clamp(next.left + dx, bounds.left,
                             bounds.right - width);
      next.top = std::clamp(next.top + dy, bounds.top,
                            bounds.bottom - height);
      next.right = next.left + width;
      next.bottom = next.top + height;
    }
    next = ClampRect(next, bounds);
    state.selection = next;
    return next != before;
  }

  static bool ConfirmSelection(EditorState& state) {
    if (state.phase != Phase::Selecting || !state.selection ||
        state.selection->Width() < 2 || state.selection->Height() < 2) {
      return false;
    }
    CancelGesture(state);
    state.phase = Phase::Editing;
    state.tool = Tool::Select;
    return true;
  }

  static KeyboardResult HandleKeyboard(EditorState& state, EditorKey key,
                                       bool shift = false,
                                       bool control = false) {
    if (key == EditorKey::Escape) return KeyboardResult::Cancelled;
    if (key == EditorKey::Enter) {
      return ConfirmSelection(state) ? KeyboardResult::Confirmed
                                     : KeyboardResult::Ignored;
    }
    int horizontal = 0;
    int vertical = 0;
    switch (key) {
      case EditorKey::Left: horizontal = -1; break;
      case EditorKey::Right: horizontal = 1; break;
      case EditorKey::Up: vertical = -1; break;
      case EditorKey::Down: vertical = 1; break;
      case EditorKey::Enter:
      case EditorKey::Escape: return KeyboardResult::Ignored;
    }
    if (state.phase != Phase::Selecting &&
        (state.phase != Phase::Editing || state.tool != Tool::Select)) {
      return KeyboardResult::Ignored;
    }
    return MoveSelection(state, horizontal, vertical, shift,
                         control ? 10 : 1)
               ? KeyboardResult::Moved
               : KeyboardResult::Ignored;
  }

  static std::optional<Rect> Selection(const EditorState& state) {
    return state.selection;
  }

  static Handle HitTestHandle(const EditorState& state, Point point,
                              int radius = 10) noexcept {
    if (!state.selection) return Handle::None;
    const Rect rect = Normalize(*state.selection);
    const auto isNear = [radius](int first, int second) {
      return std::abs(first - second) <= radius;
    };
    const bool left = isNear(point.x, rect.left);
    const bool right = isNear(point.x, rect.right);
    const bool top = isNear(point.y, rect.top);
    const bool bottom = isNear(point.y, rect.bottom);
    if (left && top) return Handle::TopLeft;
    if (right && top) return Handle::TopRight;
    if (left && bottom) return Handle::BottomLeft;
    if (right && bottom) return Handle::BottomRight;
    if (top && point.x >= rect.left && point.x <= rect.right) return Handle::Top;
    if (bottom && point.x >= rect.left && point.x <= rect.right) return Handle::Bottom;
    if (left && point.y >= rect.top && point.y <= rect.bottom) return Handle::Left;
    if (right && point.y >= rect.top && point.y <= rect.bottom) return Handle::Right;
    if (Contains(rect, point)) return Handle::Move;
    return Handle::None;
  }

  static bool BeginSelectionEdit(EditorState& state, Point point) {
    if (state.phase != Phase::Editing || !state.selection) return false;
    const Handle handle = HitTestHandle(state, point);
    if (handle == Handle::None) return false;
    state.activeHandle = handle;
    state.gestureStart = point;
    return true;
  }

  static bool UpdateSelectionEdit(EditorState& state, Point point) {
    if (state.phase != Phase::Editing || !state.selection ||
        !state.activeHandle) {
      return false;
    }
    point = ClampPoint(point, state.sourceBounds);
    Rect rect = Normalize(*state.selection);
    switch (*state.activeHandle) {
      case Handle::Move: {
        if (!state.gestureStart) return false;
        const int dx = point.x - state.gestureStart->x;
        const int dy = point.y - state.gestureStart->y;
        const int width = rect.Width();
        const int height = rect.Height();
        rect.left = std::clamp(rect.left + dx, state.sourceBounds.left,
                               state.sourceBounds.right - width);
        rect.top = std::clamp(rect.top + dy, state.sourceBounds.top,
                              state.sourceBounds.bottom - height);
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
        state.gestureStart = point;
        break;
      }
      case Handle::TopLeft:
        rect.left = std::min(point.x, rect.right - 2);
        rect.top = std::min(point.y, rect.bottom - 2);
        break;
      case Handle::Top:
        rect.top = std::min(point.y, rect.bottom - 2);
        break;
      case Handle::TopRight:
        rect.right = std::max(point.x, rect.left + 2);
        rect.top = std::min(point.y, rect.bottom - 2);
        break;
      case Handle::Right:
        rect.right = std::max(point.x, rect.left + 2);
        break;
      case Handle::BottomRight:
        rect.right = std::max(point.x, rect.left + 2);
        rect.bottom = std::max(point.y, rect.top + 2);
        break;
      case Handle::Bottom:
        rect.bottom = std::max(point.y, rect.top + 2);
        break;
      case Handle::BottomLeft:
        rect.left = std::min(point.x, rect.right - 2);
        rect.bottom = std::max(point.y, rect.top + 2);
        break;
      case Handle::Left:
        rect.left = std::min(point.x, rect.right - 2);
        break;
      case Handle::None: return false;
    }
    state.selection = ClampRect(rect, state.sourceBounds);
    return true;
  }

  static bool FinishSelectionEdit(EditorState& state) {
    if (state.phase != Phase::Editing || !state.activeHandle) return false;
    state.activeHandle.reset();
    state.gestureStart.reset();
    return true;
  }

  static bool SetTool(EditorState& state, Tool tool) {
    if (state.phase != Phase::Editing) return false;
    CancelGesture(state);
    state.tool = tool;
    return true;
  }

  static bool BeginAnnotation(EditorState& state, Point point) {
    if (state.phase != Phase::Editing || !state.selection ||
        state.tool == Tool::Select) {
      return false;
    }
    state.gestureStart = ClampPoint(point, state.sourceBounds);
    state.gesturePoints = {*state.gestureStart};
    return true;
  }

  static bool UpdateAnnotation(EditorState& state, Point point) {
    if (state.phase != Phase::Editing || !state.gestureStart ||
        state.tool == Tool::Select) {
      return false;
    }
    point = ClampPoint(point, state.sourceBounds);
    if (state.tool == Tool::Freehand) {
      if (state.gesturePoints.empty() || state.gesturePoints.back() != point) {
        state.gesturePoints.push_back(point);
      }
    } else {
      if (state.gesturePoints.size() == 1) state.gesturePoints.push_back(point);
      else state.gesturePoints.back() = point;
    }
    return true;
  }

  static bool CommitAnnotation(EditorState& state, Point point,
                               std::wstring text = {}) {
    if (!UpdateAnnotation(state, point)) return false;
    if (state.gesturePoints.empty()) return false;
    const Point first = state.gesturePoints.front();
    const Point last = state.gesturePoints.back();
    Annotation annotation;
    annotation.tool = state.tool;
    annotation.color = state.color;
    annotation.strokeWidth = state.strokeWidth;
    annotation.fontSize = state.fontSize;
    annotation.fontFamily = state.fontFamily;
    annotation.text = std::move(text);
    annotation.points = state.gesturePoints;
    annotation.bounds = {std::min(first.x, last.x), std::min(first.y, last.y),
                         std::max(first.x, last.x), std::max(first.y, last.y)};
    if (state.tool == Tool::Freehand) {
      annotation.bounds = BoundsOf(annotation.points);
    }
    const bool lineLike = state.tool == Tool::Line ||
                          state.tool == Tool::Arrow ||
                          state.tool == Tool::Freehand;
    const bool tooSmall = lineLike
                              ? std::max(annotation.bounds.Width(),
                                         annotation.bounds.Height()) < 2
                              : annotation.bounds.Width() < 2 ||
                                    annotation.bounds.Height() < 2;
    if ((state.tool == Tool::Text && annotation.text.empty()) || tooSmall) {
      CancelGesture(state);
      return false;
    }
    state.annotations.push_back(std::move(annotation));
    state.redo.clear();
    CancelGesture(state);
    return true;
  }

  static bool CommitText(EditorState& state, std::wstring text) {
    if (state.tool != Tool::Text || !state.gestureStart ||
        state.gesturePoints.empty()) {
      return false;
    }
    return CommitAnnotation(state, state.gesturePoints.back(), std::move(text));
  }

  static bool CancelGesture(EditorState& state) {
    const bool active = state.gestureStart.has_value() ||
                        !state.gesturePoints.empty() || state.activeHandle;
    state.gestureStart.reset();
    state.gesturePoints.clear();
    state.activeHandle.reset();
    return active;
  }

  static bool Undo(EditorState& state) {
    if (state.phase != Phase::Editing || state.annotations.empty()) return false;
    state.redo.push_back(std::move(state.annotations.back()));
    state.annotations.pop_back();
    return true;
  }

  static bool Redo(EditorState& state) {
    if (state.phase != Phase::Editing || state.redo.empty()) return false;
    state.annotations.push_back(std::move(state.redo.back()));
    state.redo.pop_back();
    return true;
  }

  static bool BeginOutput(EditorState& state, Destination destination) {
    if (state.phase != Phase::Editing || !state.selection ||
        state.selection->Width() < 2 || state.selection->Height() < 2) {
      return false;
    }
    CancelGesture(state);
    state.phase = destination == Destination::File ? Phase::Saving
                                                   : Phase::Copying;
    return true;
  }

  static bool OutputFailed(EditorState& state) {
    if (state.phase != Phase::Saving && state.phase != Phase::Copying) {
      return false;
    }
    state.phase = Phase::Editing;
    return true;
  }

  static bool Complete(EditorState& state) {
    if (state.phase == Phase::Idle) return true;
    state = {};
    return true;
  }

  static bool Cancel(EditorState& state) {
    if (state.phase == Phase::Idle) return false;
    state = {};
    return true;
  }

 private:
  static Point ClampPoint(Point point, Rect bounds) noexcept {
    bounds = Normalize(bounds);
    return {std::clamp(point.x, bounds.left, bounds.right),
            std::clamp(point.y, bounds.top, bounds.bottom)};
  }

  static Rect BoundsOf(const std::vector<Point>& points) noexcept {
    if (points.empty()) return {};
    Rect result{points.front().x, points.front().y, points.front().x,
                points.front().y};
    for (const auto& point : points) {
      result.left = std::min(result.left, point.x);
      result.top = std::min(result.top, point.y);
      result.right = std::max(result.right, point.x);
      result.bottom = std::max(result.bottom, point.y);
    }
    return result;
  }
};

}  // namespace feathercast::screenshot
