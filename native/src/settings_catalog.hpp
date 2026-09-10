#pragma once

#include "app_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::settings_catalog {

enum class ControlKind { Toggle, Slider, Decrement, Increment, Action, Custom };
enum class AccessibleRole { CheckButton, Slider, PushButton };
enum class Requirement {
  Always,
  PendingShortcut,
  ExistingShortcut,
  ExistingScreenshotFullscreenShortcut,
  ExistingScreenshotRegionShortcut,
  ExistingRecordFullscreenShortcut,
  ExistingRecordRegionShortcut,
  ClipboardEnabled,
  FileIndexEnabled,
  ExistingClipboardExcludedApp,
  ExistingFileIndexPattern,
  ExistingFileIndexRoot,
  StorageIdle,
  ExtensionsIdle,
  CustomAccent,
};

struct SettingDescriptor {
  std::wstring_view stableId;
  app::SettingsCategory category = app::SettingsCategory::General;
  app::HitType hit = app::HitType::CompactToggle;
  ControlKind kind = ControlKind::Toggle;
  std::wstring_view label;
  std::wstring_view description;
  std::wstring_view accessibleName;
  Requirement requirement = Requirement::Always;
};

struct CategoryDescriptor {
  app::SettingsCategory category = app::SettingsCategory::General;
  app::HitType hit = app::HitType::SettingsGeneralCategory;
  std::wstring_view label;
  std::wstring_view accessibleName;
};

struct CatalogContext {
  bool hasPendingShortcut = false;
  bool hasExistingShortcut = false;
  bool hasScreenshotFullscreenShortcut = false;
  bool hasScreenshotRegionShortcut = false;
  bool hasRecordFullscreenShortcut = false;
  bool hasRecordRegionShortcut = false;
  bool clipboardEnabled = false;
  bool fileIndexEnabled = false;
  bool hasClipboardExcludedApps = false;
  bool hasFileIndexExcludePatterns = false;
  bool hasFileIndexRoots = false;
  bool storageIdle = true;
  bool extensionsIdle = true;
  bool customAccent = false;
};

const std::vector<SettingDescriptor>& Catalog();
const std::vector<CategoryDescriptor>& Categories();
std::vector<const SettingDescriptor*> Search(const std::wstring& query);
const SettingDescriptor* Find(app::HitType hit);
const CategoryDescriptor* FindCategory(app::SettingsCategory category);
const CategoryDescriptor* FindCategory(app::HitType hit);
bool Enabled(const SettingDescriptor& descriptor, const CatalogContext& context);
bool Checked(app::HitType hit, const app::Settings& settings);
AccessibleRole Role(const SettingDescriptor& descriptor);
std::wstring AccessibleValue(app::HitType hit,
                             const app::Settings& settings);
std::vector<app::HitType> FocusOrder(app::SettingsCategory category,
                                     const CatalogContext& context);
std::vector<app::HitType> AccessibilityOrder(app::SettingsCategory category);
bool ValidateCatalog(std::wstring* error = nullptr);

}  // namespace feathercast::settings_catalog
