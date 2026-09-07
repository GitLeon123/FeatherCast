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

struct CaptureUiState {
  CapturePhase phase = CapturePhase::Idle;
  CaptureShortcutTarget target = CaptureShortcutTarget::None;
  std::optional<PixelPoint> selectionStart;
  std::optional<PixelPoint> selectionEnd;
  std::uint64_t elapsedMilliseconds = 0;
  int controlFocus = 0;
  int controlHover = -1;
};

class CaptureUiController {
 public:
  static bool Begin(CaptureUiState& state, CaptureShortcutTarget target) {
    if (state.phase != CapturePhase::Idle ||
        target == CaptureShortcutTarget::None) {
      return false;
    }
    state.target = target;
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

  static bool BeginSelection(CaptureUiState& state, PixelPoint point) {
    if (!IsSelecting(state)) return false;
    state.selectionStart = point;
    state.selectionEnd = point;
    return true;
  }

  static bool UpdateSelection(CaptureUiState& state, PixelPoint point) {
    if (!IsSelecting(state) || !state.selectionStart) return false;
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
