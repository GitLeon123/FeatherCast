#include "settings_catalog.hpp"

#include "core.hpp"

#include <set>

namespace feathercast::settings_catalog {

const std::vector<CategoryDescriptor>& Categories() {
  using app::HitType;
  using app::SettingsCategory;
  static const std::vector<CategoryDescriptor> categories = {
      {SettingsCategory::Shortcut, HitType::SettingsShortcutCategory,
       L"Shortcut", L"Shortcut settings"},
      {SettingsCategory::General, HitType::SettingsGeneralCategory,
       L"General", L"General settings"},
      {SettingsCategory::Results, HitType::SettingsResultsCategory,
       L"Results", L"Results settings"},
      {SettingsCategory::Library, HitType::SettingsLibraryCategory,
       L"Library", L"Snippet and quicklink library"},
      {SettingsCategory::Privacy, HitType::SettingsPrivacyCategory,
       L"Privacy", L"Privacy settings"},
      {SettingsCategory::Extensions, HitType::SettingsExtensionsCategory,
       L"Extensions", L"Extensions settings"},
      {SettingsCategory::Appearance, HitType::SettingsAppearanceCategory,
       L"Appearance", L"Appearance settings"},
      {SettingsCategory::Maintenance, HitType::SettingsMaintenanceCategory,
       L"Maintenance", L"Maintenance settings"},
  };
  return categories;
}

const std::vector<SettingDescriptor>& Catalog() {
  using app::HitType;
  using app::SettingsCategory;
  static const std::vector<SettingDescriptor> controls = {
      {L"shortcut.record", SettingsCategory::Shortcut, HitType::RecordShortcut,
       ControlKind::Custom, L"Record shortcut", L"Capture a new global shortcut.",
       L"Record global shortcut"},
      {L"shortcut.save", SettingsCategory::Shortcut, HitType::SaveShortcut,
       ControlKind::Action, L"Save shortcut", L"Activate the recorded shortcut.",
       L"Save global shortcut", Requirement::PendingShortcut},
      {L"shortcut.clear", SettingsCategory::Shortcut, HitType::ClearShortcut,
       ControlKind::Action, L"Clear shortcut", L"Disable the global shortcut.",
       L"Clear global shortcut", Requirement::ExistingShortcut},
      {L"shortcut.screenshot-fullscreen.record", SettingsCategory::Shortcut,
       HitType::RecordScreenshotFullscreenShortcut, ControlKind::Action,
       L"Record or change full screen screenshot shortcut",
       L"Capture a shortcut for full screen screenshots.",
       L"Record or change full screen screenshot shortcut"},
      {L"shortcut.screenshot-fullscreen.clear", SettingsCategory::Shortcut,
       HitType::ClearScreenshotFullscreenShortcut, ControlKind::Action,
       L"Clear full screen screenshot shortcut",
       L"Disable the full screen screenshot shortcut.",
       L"Clear full screen screenshot shortcut",
       Requirement::ExistingScreenshotFullscreenShortcut},
      {L"shortcut.screenshot-region.record", SettingsCategory::Shortcut,
       HitType::RecordScreenshotRegionShortcut, ControlKind::Action,
       L"Record or change region screenshot shortcut",
       L"Capture a shortcut for region screenshots.",
       L"Record or change region screenshot shortcut"},
      {L"shortcut.screenshot-region.clear", SettingsCategory::Shortcut,
       HitType::ClearScreenshotRegionShortcut, ControlKind::Action,
       L"Clear region screenshot shortcut",
       L"Disable the region screenshot shortcut.",
       L"Clear region screenshot shortcut",
       Requirement::ExistingScreenshotRegionShortcut},
      {L"shortcut.record-fullscreen.record", SettingsCategory::Shortcut,
       HitType::RecordFullscreenShortcut, ControlKind::Action,
       L"Record or change full screen recording shortcut",
       L"Capture a shortcut for full screen recording.",
       L"Record or change full screen recording shortcut"},
      {L"shortcut.record-fullscreen.clear", SettingsCategory::Shortcut,
       HitType::ClearFullscreenShortcut, ControlKind::Action,
       L"Clear full screen recording shortcut",
       L"Disable the full screen recording shortcut.",
       L"Clear full screen recording shortcut",
       Requirement::ExistingRecordFullscreenShortcut},
      {L"shortcut.record-region.record", SettingsCategory::Shortcut,
       HitType::RecordRegionShortcut, ControlKind::Action,
       L"Record or change region recording shortcut",
       L"Capture a shortcut for region recording.",
       L"Record or change region recording shortcut"},
      {L"shortcut.record-region.clear", SettingsCategory::Shortcut,
       HitType::ClearRegionShortcut, ControlKind::Action,
       L"Clear region recording shortcut",
       L"Disable the region recording shortcut.",
       L"Clear region recording shortcut", Requirement::ExistingRecordRegionShortcut},
      {L"general.startup", SettingsCategory::General, HitType::StartupToggle,
       ControlKind::Toggle, L"Launch at sign-in",
       L"Launch FeatherCast when you log into Windows.", L"Start on startup"},
      {L"general.updates", SettingsCategory::General, HitType::UpdateChecksToggle,
       ControlKind::Toggle, L"Automatic Update Checks",
       L"Check GitHub Releases once per day.", L"Automatic update checks"},
      {L"general.compact", SettingsCategory::General, HitType::CompactToggle,
       ControlKind::Toggle, L"Compact Mode",
       L"Show only the search bar at rest; results expand below.", L"Compact mode"},
      {L"general.animations", SettingsCategory::General, HitType::AnimationLevel,
       ControlKind::Slider, L"Animation",
       L"Choose how much interface motion FeatherCast uses.", L"Animation level"},
      {L"results.windows", SettingsCategory::Results, HitType::ShowWindowsToggle,
       ControlKind::Toggle, L"Open Window Results",
       L"Include currently open windows in search results.", L"Open window results"},
      {L"results.store-apps", SettingsCategory::Results, HitType::ShowStoreAppsToggle,
       ControlKind::Toggle, L"Store and system apps",
       L"Include AppsFolder, Store, and system alias entries.", L"Store and system apps"},
      {L"results.width.down", SettingsCategory::Results, HitType::OverlayWidthDown,
       ControlKind::Decrement, L"Overlay Width", L"Width of the search overlay window.",
       L"Decrease overlay width"},
      {L"results.width.up", SettingsCategory::Results, HitType::OverlayWidthUp,
       ControlKind::Increment, L"Overlay Width", L"Width of the search overlay window.",
       L"Increase overlay width"},
      {L"results.maximum.down", SettingsCategory::Results, HitType::MaxResultsDown,
       ControlKind::Decrement, L"Max Results", L"Maximum number of results to show.",
       L"Decrease maximum results"},
      {L"results.maximum.up", SettingsCategory::Results, HitType::MaxResultsUp,
       ControlKind::Increment, L"Max Results", L"Maximum number of results to show.",
       L"Increase maximum results"},
      {L"library.snippets", SettingsCategory::Library, HitType::ManageSnippets,
       ControlKind::Action, L"Manage Snippets",
       L"Create, edit, and delete reusable text snippets.",
       L"Manage snippets"},
      {L"library.quicklinks", SettingsCategory::Library, HitType::ManageQuicklinks,
       ControlKind::Action, L"Manage Quicklinks",
       L"Create, edit, and delete keyword shortcuts for URLs, files, and folders.",
       L"Manage quicklinks"},
      {L"library.command-aliases", SettingsCategory::Library, HitType::ManageCommandAliases,
       ControlKind::Action, L"Manage Command Aliases",
       L"Assign custom keywords to built-in commands.",
       L"Manage command aliases"},
      {L"privacy.clipboard", SettingsCategory::Privacy, HitType::ClipboardHistoryToggle,
       ControlKind::Toggle, L"Clipboard History",
       L"Store copied text locally for launcher search and paste.", L"Clipboard history"},
      {L"privacy.clipboard-limit.down", SettingsCategory::Privacy, HitType::ClipboardLimitDown,
       ControlKind::Decrement, L"Clipboard Retention",
       L"Maximum number of text entries retained locally.",
       L"Decrease clipboard history retention", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-limit.up", SettingsCategory::Privacy, HitType::ClipboardLimitUp,
       ControlKind::Increment, L"Clipboard Retention",
       L"Maximum number of text entries retained locally.",
       L"Increase clipboard history retention", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-days.down", SettingsCategory::Privacy, HitType::ClipboardRetentionDaysDown,
       ControlKind::Decrement, L"Clipboard Retention (Days)",
       L"Maximum age in days before clipboard entries expire (0 for unlimited).",
       L"Decrease clipboard retention days", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-days.up", SettingsCategory::Privacy, HitType::ClipboardRetentionDaysUp,
       ControlKind::Increment, L"Clipboard Retention (Days)",
       L"Maximum age in days before clipboard entries expire (0 for unlimited).",
       L"Increase clipboard retention days", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-exclude-app", SettingsCategory::Privacy, HitType::AddClipboardExcludedApp,
       ControlKind::Action, L"Exclude App from Clipboard",
       L"Prevent clipboard capture when a specific application is active.",
       L"Exclude app from clipboard", Requirement::ClipboardEnabled},
      {L"privacy.clipboard-remove-excluded-app", SettingsCategory::Privacy, HitType::RemoveClipboardExcludedApp,
       ControlKind::Action, L"Remove Excluded App",
       L"Remove an excluded app rule.",
       L"Remove excluded app from clipboard", Requirement::ClipboardEnabled},
      {L"privacy.file-index", SettingsCategory::Privacy, HitType::FileIndexToggle,
       ControlKind::Toggle, L"Files & Folders Index",
       L"Index selected local folders for launcher search.", L"Files and folders index"},
      {L"privacy.file-content", SettingsCategory::Privacy,
       HitType::FileContentIndexToggle, ControlKind::Toggle,
       L"Search File Contents",
       L"Build a local searchable token index for supported text files.",
       L"Search file contents", Requirement::FileIndexEnabled},
      {L"privacy.file-limit.down", SettingsCategory::Privacy, HitType::FileIndexLimitDown,
       ControlKind::Decrement, L"File Index Limit",
       L"Maximum number of files and folders stored locally.",
       L"Decrease file index limit", Requirement::FileIndexEnabled},
      {L"privacy.file-limit.up", SettingsCategory::Privacy, HitType::FileIndexLimitUp,
       ControlKind::Increment, L"File Index Limit",
       L"Maximum number of files and folders stored locally.",
       L"Increase file index limit", Requirement::FileIndexEnabled},
      {L"privacy.add-file-pattern", SettingsCategory::Privacy, HitType::AddFileIndexPattern,
       ControlKind::Action, L"Add Exclusion Pattern",
       L"Exclude matching files or folders using glob patterns.",
       L"Add file index exclusion pattern", Requirement::FileIndexEnabled},
      {L"privacy.remove-file-pattern", SettingsCategory::Privacy, HitType::RemoveFileIndexPattern,
       ControlKind::Action, L"Remove Exclusion Pattern",
       L"Remove a configured glob exclusion pattern.",
       L"Remove file index exclusion pattern", Requirement::FileIndexEnabled},
      {L"privacy.add-root", SettingsCategory::Privacy, HitType::AddFileRoot,
       ControlKind::Action, L"Add Indexed Folder", L"Add a folder to the local index.",
       L"Add file index folder", Requirement::FileIndexEnabled},
      {L"privacy.remove-root", SettingsCategory::Privacy, HitType::RemoveFileRoot,
       ControlKind::Action, L"Remove Indexed Folder",
       L"Remove one configured folder from the local index.",
       L"Remove indexed folder", Requirement::FileIndexEnabled},
      {L"privacy.default-roots", SettingsCategory::Privacy, HitType::ClearFileRoots,
       ControlKind::Action, L"Use Default Folders",
       L"Index Desktop, Documents, and Downloads.", L"Use default file index folders",
       Requirement::FileIndexEnabled},
      {L"privacy.rebuild-files", SettingsCategory::Privacy,
       HitType::RebuildFileIndex, ControlKind::Action, L"Rebuild File Index",
       L"Reconcile selected folders and rebuild searchable content.",
       L"Rebuild file index", Requirement::FileIndexEnabled},
      {L"privacy.diagnostics", SettingsCategory::Privacy, HitType::DiagnosticsToggle,
       ControlKind::Toggle, L"Diagnostics",
       L"Write bounded troubleshooting logs without queries or clipboard text.",
       L"Enable diagnostics"},
      {L"privacy.clear-clipboard", SettingsCategory::Privacy, HitType::ClearClipboardData,
       ControlKind::Action, L"Delete Clipboard Data", L"Delete saved clipboard entries.",
       L"Delete clipboard data", Requirement::StorageIdle},
      {L"privacy.clear-files", SettingsCategory::Privacy, HitType::ClearFileIndexData,
       ControlKind::Action, L"Delete File Index", L"Delete the disposable local file index.",
       L"Delete file index", Requirement::StorageIdle},
      {L"privacy.open-data", SettingsCategory::Privacy, HitType::OpenLocalDataFolder,
       ControlKind::Action, L"Open Local Data", L"Open logs, cache, database, and updates.",
       L"Open local data folder"},
      {L"extensions.reload", SettingsCategory::Extensions, HitType::ReloadExtensions,
       ControlKind::Action, L"Reload Extensions", L"Restart and reload installed plugins.",
       L"Reload extensions", Requirement::ExtensionsIdle},
      {L"extensions.open-folder", SettingsCategory::Extensions, HitType::OpenPluginsFolder,
       ControlKind::Action, L"Open Plugins Folder", L"Open the trusted native plugin folder.",
       L"Open plugins folder"},
      {L"appearance.system-accent", SettingsCategory::Appearance, HitType::AccentToggle,
       ControlKind::Toggle, L"Sync Accent Color",
       L"Match the Windows accent color automatically.", L"Sync accent color"},
      {L"appearance.custom-accent", SettingsCategory::Appearance, HitType::AccentColor,
       ControlKind::Custom, L"Accent Color", L"Choose a custom launcher accent color.",
       L"Pick accent color", Requirement::CustomAccent},
      {L"maintenance.clear-recents", SettingsCategory::Maintenance, HitType::ClearRecents,
       ControlKind::Action, L"Clear Recents", L"Forget recent apps and usage ranking.",
       L"Clear recents"},
      {L"maintenance.clear-icons", SettingsCategory::Maintenance, HitType::ClearIconCache,
       ControlKind::Action, L"Clear Icon Cache", L"Delete cached Windows Shell icons.",
       L"Clear icon cache"},
      {L"maintenance.check-updates", SettingsCategory::Maintenance, HitType::CheckUpdates,
       ControlKind::Action, L"Check Updates", L"Check GitHub Releases now.",
       L"Check for updates"},
  };
  return controls;
}

std::vector<const SettingDescriptor*> Search(const std::wstring& query) {
  const auto trimmed = core::Trim(query);
  if (trimmed.empty()) return {};

  std::vector<core::SearchItem> items;
  items.reserve(Catalog().size());
  for (const auto& descriptor : Catalog()) {
    core::SearchItem item;
    item.id = std::wstring(descriptor.stableId);
    item.name = std::wstring(descriptor.label);
    item.keywords = {std::wstring(descriptor.description),
                     std::wstring(descriptor.accessibleName),
                     std::wstring(descriptor.stableId)};
    if (const auto* category = FindCategory(descriptor.category)) {
      item.keywords.push_back(std::wstring(category->label));
      item.keywords.push_back(std::wstring(category->accessibleName));
    }
    items.push_back(std::move(item));
  }

  const auto matches = core::Search(trimmed, items);
  std::vector<const SettingDescriptor*> results;
  results.reserve(matches.size());
  for (const auto index : matches) results.push_back(&Catalog()[index]);
  return results;
}

const SettingDescriptor* Find(app::HitType hit) {
  for (const auto& descriptor : Catalog()) {
    if (descriptor.hit == hit) return &descriptor;
  }
  return nullptr;
}

const CategoryDescriptor* FindCategory(app::SettingsCategory category) {
  for (const auto& descriptor : Categories()) {
    if (descriptor.category == category) return &descriptor;
  }
  return nullptr;
}

const CategoryDescriptor* FindCategory(app::HitType hit) {
  for (const auto& descriptor : Categories()) {
    if (descriptor.hit == hit) return &descriptor;
  }
  return nullptr;
}

bool Enabled(const SettingDescriptor& descriptor,
             const CatalogContext& context) {
  switch (descriptor.requirement) {
    case Requirement::Always: return true;
    case Requirement::PendingShortcut: return context.hasPendingShortcut;
    case Requirement::ExistingShortcut:
      return !context.hasPendingShortcut && context.hasExistingShortcut;
    case Requirement::ExistingScreenshotFullscreenShortcut:
      return context.hasScreenshotFullscreenShortcut;
    case Requirement::ExistingScreenshotRegionShortcut:
      return context.hasScreenshotRegionShortcut;
    case Requirement::ExistingRecordFullscreenShortcut:
      return context.hasRecordFullscreenShortcut;
    case Requirement::ExistingRecordRegionShortcut:
      return context.hasRecordRegionShortcut;
    case Requirement::ClipboardEnabled:
      return context.clipboardEnabled && context.storageIdle;
    case Requirement::FileIndexEnabled:
      return context.fileIndexEnabled && context.storageIdle;
    case Requirement::StorageIdle: return context.storageIdle;
    case Requirement::ExtensionsIdle: return context.extensionsIdle;
    case Requirement::CustomAccent: return context.customAccent;
  }
  return false;
}

bool Checked(app::HitType hit, const app::Settings& settings) {
  using app::HitType;
  switch (hit) {
    case HitType::StartupToggle: return settings.startOnStartup;
    case HitType::UpdateChecksToggle: return settings.updateChecksEnabled;
    case HitType::CompactToggle: return settings.compactMode;
    case HitType::ShowWindowsToggle: return settings.showOpenWindows;
    case HitType::ShowStoreAppsToggle: return settings.showStoreApps;
    case HitType::ClipboardHistoryToggle: return settings.clipboardHistoryEnabled;
    case HitType::FileIndexToggle: return settings.fileIndexEnabled;
    case HitType::FileContentIndexToggle:
      return settings.fileContentIndexEnabled;
    case HitType::DiagnosticsToggle: return settings.diagnosticsEnabled;
    case HitType::AccentToggle: return settings.syncAccentColor;
    default: return false;
  }
}

AccessibleRole Role(const SettingDescriptor& descriptor) {
  if (descriptor.kind == ControlKind::Toggle) {
    return AccessibleRole::CheckButton;
  }
  if (descriptor.kind == ControlKind::Slider) return AccessibleRole::Slider;
  return AccessibleRole::PushButton;
}

std::wstring AccessibleValue(app::HitType hit,
                             const app::Settings& settings) {
  if (const auto* descriptor = Find(hit);
      descriptor && descriptor->kind == ControlKind::Toggle) {
    return Checked(hit, settings) ? L"On" : L"Off";
  }
  using app::HitType;
  switch (hit) {
    case HitType::AnimationLevel:
      return std::wstring(
          feathercast::settings::AnimationLevelLabel(settings.animationLevel));
    case HitType::OverlayWidthDown:
    case HitType::OverlayWidthUp:
      return std::to_wstring(settings.overlayWidth) + L" DIP";
    case HitType::MaxResultsDown:
    case HitType::MaxResultsUp:
      return std::to_wstring(settings.maxResults);
    case HitType::ClipboardLimitDown:
    case HitType::ClipboardLimitUp:
      return std::to_wstring(settings.clipboardHistoryLimit) + L" entries";
    case HitType::ClipboardRetentionDaysDown:
    case HitType::ClipboardRetentionDaysUp:
      return settings.clipboardRetentionDays == 0
                 ? L"Unlimited"
                 : (std::to_wstring(settings.clipboardRetentionDays) + L" days");
    case HitType::FileIndexLimitDown:
    case HitType::FileIndexLimitUp:
      return std::to_wstring(settings.fileIndexMaxEntries) + L" entries";
    case HitType::AccentColor: return settings.customAccentColor;
    default: return L"";
  }
}

std::vector<app::HitType> FocusOrder(app::SettingsCategory category,
                                     const CatalogContext& context) {
  std::vector<app::HitType> order;
  for (const auto& control : Catalog()) {
    if (control.category == category && Enabled(control, context)) {
      order.push_back(control.hit);
    }
  }
  return order;
}

bool ValidateCatalog(std::wstring* error) {
  std::set<std::wstring_view> ids;
  std::set<app::HitType> hits;
  for (const auto& descriptor : Catalog()) {
    if (descriptor.stableId.empty() || descriptor.label.empty() ||
        descriptor.description.empty() || descriptor.accessibleName.empty() ||
        !ids.insert(descriptor.stableId).second ||
        !hits.insert(descriptor.hit).second) {
      if (error) *error = L"Setting metadata and stable identifiers must be complete and unique.";
      return false;
    }
  }
  return Categories().size() == 8;
}

}  // namespace feathercast::settings_catalog
