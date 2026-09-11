#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::ui {

enum class UiEffect : std::uint32_t {
  None = 0,
  RequestSearch = 1u << 0,
  Invalidate = 1u << 1,
  EnsureSelectionVisible = 1u << 2,
  PersistSettings = 1u << 3,
  RefreshData = 1u << 4,
  FocusWindow = 1u << 5,
  CloseView = 1u << 6,
};

using UiEffects = std::uint32_t;

constexpr UiEffects Effect(UiEffect effect) {
  return static_cast<UiEffects>(effect);
}

constexpr UiEffects operator|(UiEffect left, UiEffect right) {
  return Effect(left) | Effect(right);
}

constexpr bool HasEffect(UiEffects effects, UiEffect effect) {
  return (effects & Effect(effect)) != 0;
}

struct NavigationState {
  app::View view = app::View::Search;
  bool actionMode = false;
  app::BrowseView browseView = app::BrowseView::None;
  std::wstring query;
  std::size_t caret = 0;
  std::optional<std::size_t> selectionAnchor;
  int selected = 0;
  int scroll = 0;
  std::wstring selectedKey;
};

struct QuerySnapshot {
  std::wstring query;
  std::size_t caret = 0;
  std::optional<std::size_t> selectionAnchor;
};

struct OverlayState {
  app::View view = app::View::Search;
  std::wstring query;
  std::wstring imeComposition;
  std::size_t caret = 0;
  std::optional<std::size_t> selectionAnchor;
  int selected = 0;
  int scroll = 0;
  bool actionMode = false;
  app::DisplayItem actionTarget;
  app::BrowseView browseView = app::BrowseView::None;
  std::optional<app::ConfirmationDialog> confirmation;
  std::optional<app::StatusMessage> status;
  int confirmationFocus = 0;
  int confirmationHover = -1;
  std::vector<NavigationState> navigationStack;
  std::optional<NavigationState> pendingNavigationRestore;
  std::vector<QuerySnapshot> queryUndo;
  std::vector<QuerySnapshot> queryRedo;
};

struct SettingsState {
  std::optional<app::HitType> searchTarget;
  std::wstring filter;
  std::wstring filterImeComposition;
  std::size_t filterCaret = 0;
  std::optional<std::size_t> filterSelectionAnchor;
  app::SettingsCategory category = app::SettingsCategory::General;
  int focusIndex = 0;
  float scroll = 0.0f;
  bool recordingShortcut = false;
  int recordingCaptureShortcut = -1;
  std::wstring pendingShortcut;
  int hover = -1;
};

enum class CapturePhase {
  Idle,
  SelectingScreenshot,
  SelectingRecording,
  StartingScreenshot,
  StartingRecording,
  Recording,
  Paused,
  Stopping,
};

enum class CaptureShortcutTarget {
  None,
  ScreenshotFullscreen,
  ScreenshotRegion,
  RecordFullscreen,
  RecordRegion,
};

struct PixelPoint {
  int x = 0;
  int y = 0;

  constexpr bool operator==(const PixelPoint&) const = default;
};

struct PixelRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  constexpr int Width() const { return right - left; }
  constexpr int Height() const { return bottom - top; }
  constexpr bool operator==(const PixelRect&) const = default;
};

inline PixelRect Normalize(PixelRect rect) noexcept {
  if (rect.left > rect.right) std::swap(rect.left, rect.right);
  if (rect.top > rect.bottom) std::swap(rect.top, rect.bottom);
  return rect;
}

inline PixelRect Clamp(PixelRect rect, PixelRect bounds,
                       int minimumSize = 2) noexcept {
  bounds = Normalize(bounds);
  rect = Normalize(rect);
  if (bounds.Width() <= 0 || bounds.Height() <= 0) return bounds;
  const int minWidth = std::max(1, std::min(minimumSize, bounds.Width()));
  const int minHeight = std::max(1, std::min(minimumSize, bounds.Height()));
  const int width = std::clamp(rect.Width(), minWidth, bounds.Width());
  const int height = std::clamp(rect.Height(), minHeight, bounds.Height());
  rect.left = std::clamp(rect.left, bounds.left, bounds.right - width);
  rect.top = std::clamp(rect.top, bounds.top, bounds.bottom - height);
  rect.right = rect.left + width;
  rect.bottom = rect.top + height;
  return rect;
}

struct CaptureUiState {
  CapturePhase phase = CapturePhase::Idle;
  CaptureShortcutTarget target = CaptureShortcutTarget::None;
  PixelRect sourceBounds;
  std::optional<PixelPoint> selectionStart;
  std::optional<PixelPoint> selectionEnd;
  std::uint64_t elapsedMilliseconds = 0;
  int controlFocus = 0;
  int controlHover = -1;
};

enum class CaptureKey { Left, Up, Right, Down, Enter, Escape };

enum class CaptureKeyboardResult { Ignored, Moved, Confirmed, Cancelled };

class CaptureUiController {
 public:
  static bool Begin(CaptureUiState& state, CaptureShortcutTarget target,
                    PixelRect sourceBounds = {}) {
    if (state.phase != CapturePhase::Idle ||
        target == CaptureShortcutTarget::None) {
      return false;
    }
    state.target = target;
    state.sourceBounds = Normalize(sourceBounds);
    state.selectionStart.reset();
    state.selectionEnd.reset();
    state.elapsedMilliseconds = 0;
    state.controlFocus = 0;
    state.controlHover = -1;
    switch (target) {
      case CaptureShortcutTarget::ScreenshotFullscreen:
        state.phase = CapturePhase::StartingScreenshot;
        break;
      case CaptureShortcutTarget::ScreenshotRegion:
        state.phase = CapturePhase::SelectingScreenshot;
        break;
      case CaptureShortcutTarget::RecordFullscreen:
        state.phase = CapturePhase::StartingRecording;
        break;
      case CaptureShortcutTarget::RecordRegion:
        state.phase = CapturePhase::SelectingRecording;
        break;
      case CaptureShortcutTarget::None: return false;
    }
    return true;
  }

  static void SetBounds(CaptureUiState& state, PixelRect bounds) {
    state.sourceBounds = Normalize(bounds);
    if (state.selectionStart && state.sourceBounds.Width() > 0 &&
        state.sourceBounds.Height() > 0) {
      state.selectionStart = PixelPoint{
          std::clamp(state.selectionStart->x, state.sourceBounds.left,
                     state.sourceBounds.right),
          std::clamp(state.selectionStart->y, state.sourceBounds.top,
                     state.sourceBounds.bottom)};
    }
    if (state.selectionEnd && state.sourceBounds.Width() > 0 &&
        state.sourceBounds.Height() > 0) {
      state.selectionEnd = PixelPoint{
          std::clamp(state.selectionEnd->x, state.sourceBounds.left,
                     state.sourceBounds.right),
          std::clamp(state.selectionEnd->y, state.sourceBounds.top,
                     state.sourceBounds.bottom)};
    }
  }

  static bool BeginSelection(CaptureUiState& state, PixelPoint point) {
    if (!IsSelecting(state)) return false;
    if (state.sourceBounds.Width() > 0 && state.sourceBounds.Height() > 0) {
      point.x = std::clamp(point.x, state.sourceBounds.left,
                           state.sourceBounds.right);
      point.y = std::clamp(point.y, state.sourceBounds.top,
                           state.sourceBounds.bottom);
    }
    state.selectionStart = point;
    state.selectionEnd = point;
    return true;
  }

  static bool UpdateSelection(CaptureUiState& state, PixelPoint point) {
    if (!IsSelecting(state) || !state.selectionStart) return false;
    if (state.sourceBounds.Width() > 0 && state.sourceBounds.Height() > 0) {
      point.x = std::clamp(point.x, state.sourceBounds.left,
                           state.sourceBounds.right);
      point.y = std::clamp(point.y, state.sourceBounds.top,
                           state.sourceBounds.bottom);
    }
    state.selectionEnd = point;
    return true;
  }

  static bool FinishSelection(CaptureUiState& state, PixelPoint point) {
    if (!UpdateSelection(state, point)) return false;
    state.phase = state.phase == CapturePhase::SelectingScreenshot
                      ? CapturePhase::StartingScreenshot
                      : CapturePhase::StartingRecording;
    return true;
  }

  static bool EnsureKeyboardSelection(CaptureUiState& state) {
    if (!IsSelecting(state) || state.sourceBounds.Width() < 2 ||
        state.sourceBounds.Height() < 2) {
      return false;
    }
    if (state.selectionStart && state.selectionEnd) return true;
    const PixelRect bounds = Normalize(state.sourceBounds);
    const int width = std::min(bounds.Width(), std::max(2, std::min(320,
                                                                      bounds.Width())));
    const int height = std::min(bounds.Height(), std::max(2, std::min(200,
                                                                        bounds.Height())));
    const int left = bounds.left + (bounds.Width() - width) / 2;
    const int top = bounds.top + (bounds.Height() - height) / 2;
    state.selectionStart = PixelPoint{left, top};
    state.selectionEnd = PixelPoint{left + width, top + height};
    return true;
  }

  static bool MoveKeyboardSelection(CaptureUiState& state, int horizontal,
                                    int vertical, bool resize,
                                    int step = 1) {
    if (!EnsureKeyboardSelection(state) || (horizontal == 0 && vertical == 0)) {
      return false;
    }
    const auto before = SelectionRect(state);
    if (!before) return false;
    PixelRect next = *before;
    const PixelRect bounds = Normalize(state.sourceBounds);
    const int increment = std::max(1, step);
    if (resize) {
      if (horizontal < 0) next.right = std::max(next.left + 2,
                                                 next.right - increment);
      if (horizontal > 0) next.right = std::min(bounds.right,
                                                 next.right + increment);
      if (vertical < 0) next.bottom = std::max(next.top + 2,
                                               next.bottom - increment);
      if (vertical > 0) next.bottom = std::min(bounds.bottom,
                                               next.bottom + increment);
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
    next = Clamp(next, bounds);
    state.selectionStart = PixelPoint{next.left, next.top};
    state.selectionEnd = PixelPoint{next.right, next.bottom};
    return before != next;
  }

  static bool ConfirmSelection(CaptureUiState& state) {
    if (!IsSelecting(state)) return false;
    const auto selection = SelectionRect(state);
    if (!selection || selection->Width() < 2 || selection->Height() < 2) {
      return false;
    }
    state.phase = state.phase == CapturePhase::SelectingScreenshot
                      ? CapturePhase::StartingScreenshot
                      : CapturePhase::StartingRecording;
    return true;
  }

  static CaptureKeyboardResult HandleSelectionKey(
      CaptureUiState& state, CaptureKey key, bool shift = false,
      bool control = false) {
    if (key == CaptureKey::Escape) return CaptureKeyboardResult::Cancelled;
    if (key == CaptureKey::Enter) {
      return ConfirmSelection(state) ? CaptureKeyboardResult::Confirmed
                                     : CaptureKeyboardResult::Ignored;
    }
    int horizontal = 0;
    int vertical = 0;
    switch (key) {
      case CaptureKey::Left: horizontal = -1; break;
      case CaptureKey::Right: horizontal = 1; break;
      case CaptureKey::Up: vertical = -1; break;
      case CaptureKey::Down: vertical = 1; break;
      case CaptureKey::Enter:
      case CaptureKey::Escape: return CaptureKeyboardResult::Ignored;
    }
    return MoveKeyboardSelection(state, horizontal, vertical, shift,
                                 control ? 10 : 1)
               ? CaptureKeyboardResult::Moved
               : CaptureKeyboardResult::Ignored;
  }

  static std::optional<PixelRect> SelectionRect(const CaptureUiState& state) {
    if (!state.selectionStart || !state.selectionEnd) return std::nullopt;
    return PixelRect{std::min(state.selectionStart->x, state.selectionEnd->x),
                     std::min(state.selectionStart->y, state.selectionEnd->y),
                     std::max(state.selectionStart->x, state.selectionEnd->x),
                     std::max(state.selectionStart->y, state.selectionEnd->y)};
  }

  static bool CancelSelection(CaptureUiState& state) {
    if (state.phase == CapturePhase::Idle) return true;
    if (!IsSelecting(state)) return false;
    Reset(state);
    return true;
  }

  static bool RecordingStarted(CaptureUiState& state) {
    if (state.phase == CapturePhase::Recording) return true;
    if (state.phase != CapturePhase::StartingRecording) return false;
    state.phase = CapturePhase::Recording;
    return true;
  }

  static bool Pause(CaptureUiState& state) {
    if (state.phase == CapturePhase::Paused) return true;
    if (state.phase != CapturePhase::Recording) return false;
    state.phase = CapturePhase::Paused;
    return true;
  }

  static bool Resume(CaptureUiState& state) {
    if (state.phase == CapturePhase::Recording) return true;
    if (state.phase != CapturePhase::Paused) return false;
    state.phase = CapturePhase::Recording;
    return true;
  }

  static bool Stop(CaptureUiState& state) {
    if (state.phase == CapturePhase::Stopping) return true;
    if (state.phase != CapturePhase::StartingRecording &&
        state.phase != CapturePhase::Recording &&
        state.phase != CapturePhase::Paused) {
      return false;
    }
    state.phase = CapturePhase::Stopping;
    return true;
  }

  static bool Complete(CaptureUiState& state) {
    if (state.phase == CapturePhase::Idle) return true;
    if (state.phase != CapturePhase::StartingScreenshot &&
        state.phase != CapturePhase::Stopping) {
      return false;
    }
    Reset(state);
    return true;
  }

  static bool Fail(CaptureUiState& state) {
    if (state.phase == CapturePhase::Idle) return true;
    Reset(state);
    return true;
  }

  static bool AddElapsed(CaptureUiState& state, std::uint64_t milliseconds) {
    if (state.phase != CapturePhase::Recording) return false;
    state.elapsedMilliseconds += milliseconds;
    return true;
  }

  static void SetControlFocus(CaptureUiState& state, int focus) {
    state.controlFocus = std::clamp(focus, 0, 1);
  }

  static void SetControlHover(CaptureUiState& state, int hover) {
    state.controlHover = std::clamp(hover, -1, 1);
  }

 private:
  static bool IsSelecting(const CaptureUiState& state) {
    return state.phase == CapturePhase::SelectingScreenshot ||
           state.phase == CapturePhase::SelectingRecording;
  }

  static void Reset(CaptureUiState& state) { state = {}; }
};

class OverlayController {
 public:
  static UiEffects ResetForShow(OverlayState& state, app::View view,
                                std::wstring initialQuery = L"",
                                bool selectInitialQuery = false) {
    state.view = view;
    state.query = std::move(initialQuery);
    state.imeComposition.clear();
    state.caret = state.query.size();
    state.selectionAnchor =
        selectInitialQuery && !state.query.empty()
            ? std::optional<std::size_t>(0)
            : std::nullopt;
    state.selected = 0;
    state.scroll = 0;
    state.actionMode = false;
    state.actionTarget = {};
    state.browseView = app::BrowseView::None;
    state.confirmation.reset();
    state.status.reset();
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    state.navigationStack.clear();
    state.pendingNavigationRestore.reset();
    state.queryUndo.clear();
    state.queryRedo.clear();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects SetQuery(OverlayState& state, std::wstring query) {
    state.query = std::move(query);
    state.imeComposition.clear();
    state.caret = state.query.size();
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects Select(OverlayState& state, int index, int itemCount,
                          bool ensureVisible = true) {
    state.selected = itemCount <= 0 ? 0 : std::clamp(index, 0, itemCount - 1);
    return ensureVisible
               ? Effect(UiEffect::Invalidate) |
                     Effect(UiEffect::EnsureSelectionVisible)
               : Effect(UiEffect::Invalidate);
  }

  static UiEffects SetScroll(OverlayState& state, int scroll, int maximum) {
    state.scroll = std::clamp(scroll, 0, std::max(0, maximum));
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects ResetResultPosition(OverlayState& state) {
    state.selected = 0;
    state.scroll = 0;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects RestoreResultPosition(OverlayState& state, int selected,
                                         int scroll) {
    state.selected = std::max(0, selected);
    state.scroll = std::max(0, scroll);
    return Effect(UiEffect::Invalidate);
  }

  static void ClampCaret(OverlayState& state) {
    state.caret = std::min(state.caret, state.query.size());
    if (state.selectionAnchor) {
      *state.selectionAnchor =
          std::min(*state.selectionAnchor, state.query.size());
    }
  }

  static std::optional<std::pair<std::size_t, std::size_t>> SelectionRange(
      const OverlayState& state) {
    if (!state.selectionAnchor || *state.selectionAnchor == state.caret) {
      return std::nullopt;
    }
    return std::minmax(*state.selectionAnchor, state.caret);
  }

  static UiEffects MoveCaret(OverlayState& state, std::size_t next,
                             bool extendSelection) {
    next = std::min(next, state.query.size());
    if (extendSelection) {
      if (!state.selectionAnchor) state.selectionAnchor = state.caret;
    } else {
      state.selectionAnchor.reset();
    }
    state.caret = next;
    return Effect(UiEffect::Invalidate);
  }

  static bool DeleteSelection(OverlayState& state) {
    const auto range = SelectionRange(state);
    if (!range) return false;
    RecordUserEdit(state);
    DeleteSelectionWithoutHistory(state, *range);
    return true;
  }

  static void RecordUserEdit(OverlayState& state) {
    PushSnapshot(state.queryUndo, Snapshot(state));
    state.queryRedo.clear();
  }

  static UiEffects UndoQueryEdit(OverlayState& state) {
    if (state.queryUndo.empty()) return Effect(UiEffect::Invalidate);
    PushSnapshot(state.queryRedo, Snapshot(state));
    const QuerySnapshot snapshot = std::move(state.queryUndo.back());
    state.queryUndo.pop_back();
    RestoreSnapshot(state, snapshot);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects RedoQueryEdit(OverlayState& state) {
    if (state.queryRedo.empty()) return Effect(UiEffect::Invalidate);
    PushSnapshot(state.queryUndo, Snapshot(state));
    const QuerySnapshot snapshot = std::move(state.queryRedo.back());
    state.queryRedo.pop_back();
    RestoreSnapshot(state, snapshot);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static bool CanUndoQueryEdit(const OverlayState& state) {
    return !state.queryUndo.empty();
  }

  static bool CanRedoQueryEdit(const OverlayState& state) {
    return !state.queryRedo.empty();
  }

 private:
  static constexpr std::size_t kQueryHistoryLimit = 32;

  static QuerySnapshot Snapshot(const OverlayState& state) {
    return {state.query, state.caret, state.selectionAnchor};
  }

  static void PushSnapshot(std::vector<QuerySnapshot>& history,
                           QuerySnapshot snapshot) {
    if (history.size() == kQueryHistoryLimit) history.erase(history.begin());
    history.push_back(std::move(snapshot));
  }

  static void RestoreSnapshot(OverlayState& state,
                              const QuerySnapshot& snapshot) {
    state.query = snapshot.query;
    state.caret = std::min(snapshot.caret, state.query.size());
    state.selectionAnchor = snapshot.selectionAnchor;
    if (state.selectionAnchor) {
      *state.selectionAnchor =
          std::min(*state.selectionAnchor, state.query.size());
    }
    state.imeComposition.clear();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
  }

  static void DeleteSelectionWithoutHistory(
      OverlayState& state,
      const std::pair<std::size_t, std::size_t>& range) {
    state.query.erase(range.first, range.second - range.first);
    state.caret = range.first;
    state.selectionAnchor.reset();
  }

 public:

  static UiEffects InsertText(OverlayState& state, const std::wstring& text,
                              std::size_t maxCharacters = 4096) {
    ClampCaret(state);
    RecordUserEdit(state);
    if (const auto range = SelectionRange(state)) {
      DeleteSelectionWithoutHistory(state, *range);
    }
    const std::size_t room = state.query.size() < maxCharacters
                                 ? maxCharacters - state.query.size()
                                 : 0;
    const std::wstring clipped = text.substr(0, room);
    state.query.insert(state.caret, clipped);
    state.caret += clipped.size();
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static void PushNavigation(OverlayState& state,
                             std::wstring selectedKey = {}) {
    state.navigationStack.push_back(
        {state.view, state.actionMode, state.browseView, state.query,
         state.caret, state.selectionAnchor, state.selected, state.scroll,
         std::move(selectedKey)});
  }

  static UiEffects EnterAction(OverlayState& state,
                               const app::DisplayItem& target,
                               std::wstring selectedKey = {}) {
    PushNavigation(state, std::move(selectedKey));
    state.actionMode = true;
    state.actionTarget = target;
    state.browseView = app::BrowseView::None;
    ClearQueryAndSelection(state);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects EnterBrowse(OverlayState& state, app::BrowseView view,
                               std::wstring selectedKey = {}) {
    PushNavigation(state, std::move(selectedKey));
    state.actionMode = false;
    state.browseView = view;
    ClearQueryAndSelection(state);
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects RestoreNavigation(OverlayState& state) {
    state.imeComposition.clear();
    state.status.reset();
    if (state.navigationStack.empty()) {
      state.actionMode = false;
      state.browseView = app::BrowseView::None;
      state.actionTarget = {};
      ClearQueryAndSelection(state);
      state.pendingNavigationRestore.reset();
    } else {
      NavigationState restored = std::move(state.navigationStack.back());
      state.navigationStack.pop_back();
      state.view = restored.view;
      state.actionMode = restored.actionMode;
      state.browseView = restored.browseView;
      state.query = restored.query;
      state.caret = std::min(restored.caret, state.query.size());
      state.selectionAnchor = restored.selectionAnchor;
      if (state.selectionAnchor) {
        *state.selectionAnchor =
            std::min(*state.selectionAnchor, state.query.size());
      }
      state.selected = restored.selected;
      state.scroll = restored.scroll;
      state.pendingNavigationRestore = std::move(restored);
    }
    return UiEffect::RequestSearch | UiEffect::Invalidate;
  }

  static UiEffects ShowConfirmation(
      OverlayState& state, app::ConfirmationDialog confirmation) {
    state.confirmation = std::move(confirmation);
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CloseConfirmation(OverlayState& state) {
    state.confirmation.reset();
    state.confirmationFocus = 0;
    state.confirmationHover = -1;
    return Effect(UiEffect::Invalidate);
  }

 private:
  static void ClearQueryAndSelection(OverlayState& state) {
    state.query.clear();
    state.imeComposition.clear();
    state.caret = 0;
    state.selectionAnchor.reset();
    state.selected = 0;
    state.scroll = 0;
    state.status.reset();
  }
};

class SettingsController {
 public:
  static UiEffects Open(SettingsState& state, int categoryIndex) {
    state.recordingShortcut = false;
    state.recordingCaptureShortcut = -1;
    state.pendingShortcut.clear();
    state.hover = -1;
    state.focusIndex = std::max(0, categoryIndex);
    state.scroll = 0.0f;
    return UiEffect::Invalidate | UiEffect::FocusWindow;
  }

  static UiEffects Close(SettingsState& state) {
    state.searchTarget.reset();
    state.recordingShortcut = false;
    state.recordingCaptureShortcut = -1;
    state.pendingShortcut.clear();
    return UiEffect::CloseView | UiEffect::Invalidate;
  }

  static UiEffects SelectCategory(SettingsState& state,
                                  app::SettingsCategory category) {
    state.searchTarget.reset();
    state.category = category;
    state.focusIndex = 0;
    state.scroll = 0.0f;
    state.recordingShortcut = false;
    state.recordingCaptureShortcut = -1;
    state.pendingShortcut.clear();
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects MoveFocus(SettingsState& state, int delta, int itemCount) {
    if (itemCount <= 0) {
      state.focusIndex = 0;
    } else {
      state.focusIndex =
          std::clamp(state.focusIndex + delta, 0, itemCount - 1);
    }
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetFocus(SettingsState& state, int index, int itemCount) {
    state.focusIndex = itemCount <= 0 ? 0 : std::clamp(index, 0, itemCount - 1);
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CycleFocus(SettingsState& state, int delta,
                              int itemCount) {
    if (itemCount <= 0) {
      state.focusIndex = 0;
    } else {
      state.focusIndex =
          (state.focusIndex + delta + itemCount) % itemCount;
    }
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetScroll(SettingsState& state, float scroll,
                             float maximum) {
    state.scroll = std::clamp(scroll, 0.0f, std::max(0.0f, maximum));
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects ScrollBy(SettingsState& state, float delta,
                            float maximum) {
    return SetScroll(state, state.scroll + delta, maximum);
  }

  static UiEffects BeginShortcutRecording(SettingsState& state) {
    state.pendingShortcut.clear();
    state.recordingShortcut = true;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects SetPendingShortcut(SettingsState& state,
                                      std::wstring shortcut) {
    state.pendingShortcut = std::move(shortcut);
    state.recordingShortcut = false;
    state.recordingCaptureShortcut = -1;
    return Effect(UiEffect::Invalidate);
  }

  static UiEffects CancelShortcutRecording(SettingsState& state) {
    state.recordingShortcut = false;
    state.recordingCaptureShortcut = -1;
    state.pendingShortcut.clear();
    return Effect(UiEffect::Invalidate);
  }
};

}  // namespace feathercast::ui
