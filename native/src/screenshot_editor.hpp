#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::screenshot {

struct Point {
  int x = 0;
  int y = 0;

  constexpr bool operator==(const Point&) const = default;
};

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
  return point.x >= rect.left && point.x <= rect.right &&
         point.y >= rect.top && point.y <= rect.bottom;
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
};

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
  std::vector<Annotation> annotations;
  std::vector<Annotation> redo;
  std::optional<Handle> activeHandle;
  std::optional<Point> gestureStart;
  std::vector<Point> gesturePoints;
  int focusIndex = 0;
  int hoverIndex = -1;
};

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
