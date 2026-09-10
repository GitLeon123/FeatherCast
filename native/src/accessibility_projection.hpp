#pragma once

#include "app_types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace feathercast::accessibility_projection {

enum class FocusKind {
  Search,
  Result,
  Settings,
  CloseSettings,
};

struct FocusTarget {
  FocusKind kind = FocusKind::Search;
  std::wstring key;

  bool operator==(const FocusTarget&) const = default;
};

inline FocusTarget SearchFocus() { return {FocusKind::Search, {}}; }
inline FocusTarget ResultFocus(std::wstring key) {
  return {FocusKind::Result, std::move(key)};
}
inline FocusTarget SettingsFocus() { return {FocusKind::Settings, {}}; }
inline FocusTarget CloseSettingsFocus() {
  return {FocusKind::CloseSettings, {}};
}

constexpr int SearchChild() { return 1; }
constexpr int StatusChild() { return 2; }
constexpr int SettingsChild() { return 3; }
constexpr int ResultChild(std::size_t index) {
  return static_cast<int>(index) + 4;
}
constexpr int PreviewChild(std::size_t resultCount) {
  return static_cast<int>(resultCount) + 4;
}

enum class LiveStatusKind { Hidden, Loading, Empty, Error, Preview };

struct LiveStatusProjection {
  LiveStatusKind kind = LiveStatusKind::Hidden;
  std::wstring value;
  std::wstring description;
  bool visible = false;
  bool alert = false;
};

inline LiveStatusProjection ProjectLiveStatus(
    bool fileIndexLoading, bool searchPending, std::wstring emptyMessage,
    bool previewOpen, bool previewReady,
    const std::optional<app::StatusMessage>& explicitStatus) {
  LiveStatusProjection projection;
  if (explicitStatus) {
    projection.kind = explicitStatus->severity == app::StatusSeverity::Error
                          ? LiveStatusKind::Error
                          : LiveStatusKind::Loading;
    projection.value = explicitStatus->text;
    projection.description = explicitStatus->text;
    projection.visible = true;
    projection.alert =
        explicitStatus->severity == app::StatusSeverity::Error;
  } else if (fileIndexLoading) {
    projection.kind = LiveStatusKind::Loading;
    projection.value = L"Loading file index...";
    projection.description =
        L"The saved file index is loading in the background.";
    projection.visible = true;
  } else if (searchPending) {
    projection.kind = LiveStatusKind::Loading;
    projection.value = L"Searching...";
    projection.description =
        L"Results cannot be activated until this search finishes.";
    projection.visible = true;
  } else if (!emptyMessage.empty()) {
    projection.kind = LiveStatusKind::Empty;
    projection.value = std::move(emptyMessage);
    projection.description = projection.value;
    projection.visible = true;
  } else if (previewOpen) {
    projection.kind = LiveStatusKind::Preview;
    projection.value = previewReady ? L"Preview ready" : L"Loading preview...";
    projection.description = projection.value;
    projection.visible = true;
  }
  return projection;
}

}  // namespace feathercast::accessibility_projection
