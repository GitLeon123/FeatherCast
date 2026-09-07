#include "capability_catalog.hpp"
#include "accessibility_projection.hpp"
#include "command_catalog.hpp"
#include "settings_catalog.hpp"
#include "result_icons.hpp"
#include "test_framework.hpp"
#include "ui_state.hpp"
#include "ui_renderer.hpp"
#include "window_activation.hpp"

#include <algorithm>
#include <array>

int main() {
  feathercast::ui::HitRegions geometry;
  geometry.push_back(
      {{0, 0, 100, 100}, feathercast::app::HitType::Gear});
  geometry.push_back(
      {{20, 20, 40, 40}, feathercast::app::HitType::ConfirmAction});
  assert(geometry.At(30, 30));
  assert(geometry.At(30, 30)->type ==
         feathercast::app::HitType::ConfirmAction);
  assert(geometry.At(90, 90)->type == feathercast::app::HitType::Gear);
  assert(geometry.At(120, 120) == nullptr);

  std::wstring error;
  assert(feathercast::commands::ValidateCatalog(&error));
  assert(feathercast::capabilities::ValidateCatalog(&error));
  assert(feathercast::settings_catalog::ValidateCatalog(&error));
  const auto commands = feathercast::commands::BuildCommandItems();
  assert(!commands.empty());
  assert(std::all_of(commands.begin(), commands.end(),
                     [](const auto& item) { return item.isCommand; }));
  const auto* shutdown =
      feathercast::commands::Find(feathercast::app::CommandKind::ShutDown);
  assert(shutdown && shutdown->confirmation);
  const auto* discover = feathercast::commands::Find(
      feathercast::app::CommandKind::DiscoverFeatherCast);
  assert(discover && discover->stableId == L"discover-feathercast");
  for (const std::wstring keyword : {L"help", L"features", L"shortcuts",
                                     L"how to"}) {
    assert(std::find(discover->keywords.begin(), discover->keywords.end(),
                     keyword) != discover->keywords.end());
  }
  for (const auto& [kind, stableId] : {
           std::pair{feathercast::app::CommandKind::ScreenshotFullscreen,
                     std::wstring_view{L"screenshot-fullscreen"}},
           std::pair{feathercast::app::CommandKind::ScreenshotRegion,
                     std::wstring_view{L"screenshot-region"}},
           std::pair{feathercast::app::CommandKind::RecordFullscreen,
                     std::wstring_view{L"record-fullscreen"}},
           std::pair{feathercast::app::CommandKind::RecordRegion,
                     std::wstring_view{L"record-region"}}}) {
    const auto* descriptor = feathercast::commands::Find(kind);
    assert(descriptor && descriptor->stableId == stableId);
  }

  const auto capabilityMatches =
      feathercast::capabilities::Search(L"math expression");
  assert(!capabilityMatches.empty());
  assert(capabilityMatches.front()->stableId == L"calculator");
  const auto capabilityItem =
      feathercast::capabilities::Display(*capabilityMatches.front());
  assert(capabilityItem.isCapability);
  assert(capabilityItem.Key() == L"capability:calculator");
  assert(capabilityItem.capability.action.kind ==
         feathercast::app::CapabilityActionKind::SeedQuery);

  using feathercast::ui::ResultIcon;
  feathercast::app::DisplayItem gameFallback;
  gameFallback.app.name = L"Game without a local icon";
  gameFallback.app.path = L"E:\\Games\\Fixture";
  gameFallback.app.isGame = true;
  assert(gameFallback.IconKey().empty());
  assert(feathercast::ui::ResolveResultIcon(gameFallback) ==
         ResultIcon::Gamepad);

  const std::array capabilityIcons{
      std::pair{L"timers", ResultIcon::Clock},
      std::pair{L"setting-search", ResultIcon::Gear},
      std::pair{L"clipboard-favorites", ResultIcon::Clipboard},
      std::pair{L"apps", ResultIcon::AppGrid},
      std::pair{L"games", ResultIcon::Gamepad},
      std::pair{L"windows", ResultIcon::Windows},
      std::pair{L"window-management", ResultIcon::WindowLayout},
      std::pair{L"actions", ResultIcon::Actions},
      std::pair{L"calculator", ResultIcon::Calculator},
      std::pair{L"conversions", ResultIcon::Convert},
      std::pair{L"time-utilities", ResultIcon::Clock},
      std::pair{L"uuid", ResultIcon::Code},
      std::pair{L"web-search", ResultIcon::WebSearch},
      std::pair{L"emoji", ResultIcon::Smile},
      std::pair{L"symbols", ResultIcon::Symbols},
      std::pair{L"files", ResultIcon::FolderSearch},
      std::pair{L"clipboard", ResultIcon::Clipboard},
      std::pair{L"snippets", ResultIcon::Document},
      std::pair{L"quicklinks", ResultIcon::Link},
      std::pair{L"system-commands", ResultIcon::Terminal},
      std::pair{L"windows-settings", ResultIcon::Gear},
      std::pair{L"plugins", ResultIcon::Puzzle},
      std::pair{L"shortcut", ResultIcon::Keyboard},
  };
  assert(capabilityIcons.size() == feathercast::capabilities::Catalog().size());
  for (const auto& [stableId, expected] : capabilityIcons) {
    const auto found = std::find_if(
        feathercast::capabilities::Catalog().begin(),
        feathercast::capabilities::Catalog().end(),
        [&](const auto& descriptor) { return descriptor.stableId == stableId; });
    assert(found != feathercast::capabilities::Catalog().end());
    assert(feathercast::ui::ResolveResultIcon(
               feathercast::capabilities::Display(*found)) == expected);
    assert(expected != ResultIcon::App);
  }

  using feathercast::app::CommandKind;
  const std::array commandIcons{
      std::pair{CommandKind::Settings, ResultIcon::Gear},
      std::pair{CommandKind::Games, ResultIcon::Gamepad},
      std::pair{CommandKind::Quit, ResultIcon::Exit},
      std::pair{CommandKind::Restart, ResultIcon::Refresh},
      std::pair{CommandKind::RefreshApps, ResultIcon::Refresh},
      std::pair{CommandKind::ClearIconCache, ResultIcon::Trash},
      std::pair{CommandKind::ClearRecents, ResultIcon::HistoryOff},
      std::pair{CommandKind::OpenDataFolder, ResultIcon::FolderGear},
      std::pair{CommandKind::OpenLocalDataFolder, ResultIcon::Database},
      std::pair{CommandKind::ReloadExtensions, ResultIcon::PuzzleRefresh},
      std::pair{CommandKind::LockPC, ResultIcon::Lock},
      std::pair{CommandKind::SleepPC, ResultIcon::Moon},
      std::pair{CommandKind::MuteAudio, ResultIcon::SpeakerOff},
      std::pair{CommandKind::ShutDown, ResultIcon::Power},
      std::pair{CommandKind::RestartPC, ResultIcon::PowerRefresh},
      std::pair{CommandKind::EmptyRecycleBin, ResultIcon::Trash},
      std::pair{CommandKind::ClearClipboardHistory, ResultIcon::ClipboardOff},
      std::pair{CommandKind::OpenSnippetsFile, ResultIcon::Document},
      std::pair{CommandKind::ReloadSnippets, ResultIcon::DocumentRefresh},
      std::pair{CommandKind::OpenThemeFile, ResultIcon::Palette},
      std::pair{CommandKind::ReloadTheme, ResultIcon::PaletteRefresh},
      std::pair{CommandKind::CheckForUpdates, ResultIcon::Download},
      std::pair{CommandKind::ClipboardHistory, ResultIcon::Clipboard},
      std::pair{CommandKind::EmojiPicker, ResultIcon::Smile},
      std::pair{CommandKind::DiscoverFeatherCast, ResultIcon::Compass},
      std::pair{CommandKind::VolumeControl, ResultIcon::Sliders},
      std::pair{CommandKind::VolumeUp, ResultIcon::SpeakerPlus},
      std::pair{CommandKind::VolumeDown, ResultIcon::SpeakerMinus},
      std::pair{CommandKind::MediaPlayPause, ResultIcon::PlayPause},
      std::pair{CommandKind::MediaNext, ResultIcon::NextTrack},
      std::pair{CommandKind::MediaPrevious, ResultIcon::PreviousTrack},
      std::pair{CommandKind::ShowDesktop, ResultIcon::Monitor},
      std::pair{CommandKind::GenerateUuid, ResultIcon::Code},
      std::pair{CommandKind::ScreenshotFullscreen, ResultIcon::Copy},
      std::pair{CommandKind::ScreenshotRegion, ResultIcon::Copy},
      std::pair{CommandKind::RecordFullscreen, ResultIcon::Monitor},
      std::pair{CommandKind::RecordRegion, ResultIcon::Monitor},
      std::pair{CommandKind::Timers, ResultIcon::Clock},
  };
  assert(commandIcons.size() == feathercast::commands::Catalog().size());
  for (const auto& [kind, expected] : commandIcons) {
    feathercast::app::DisplayItem item;
    item.isCommand = true;
    item.command = kind;
    assert(feathercast::commands::Find(kind));
    assert(feathercast::ui::ResolveResultIcon(item) == expected);
    assert(expected != ResultIcon::App);
  }

  using feathercast::app::ActionKind;
  const std::array actionIcons{
      std::pair{ActionKind::None, ResultIcon::Actions},
      std::pair{ActionKind::Open, ResultIcon::ExternalLink},
      std::pair{ActionKind::RunAsAdmin, ResultIcon::Shield},
      std::pair{ActionKind::OpenLocation, ResultIcon::Folder},
      std::pair{ActionKind::CopyPath, ResultIcon::Copy},
      std::pair{ActionKind::Pin, ResultIcon::Pin},
      std::pair{ActionKind::Unpin, ResultIcon::PinOff},
      std::pair{ActionKind::Hide, ResultIcon::EyeOff},
      std::pair{ActionKind::Unhide, ResultIcon::Eye},
      std::pair{ActionKind::Switch, ResultIcon::Windows},
      std::pair{ActionKind::Minimize, ResultIcon::Minimize},
      std::pair{ActionKind::MaximizeRestore, ResultIcon::Maximize},
      std::pair{ActionKind::CloseWindow, ResultIcon::Close},
      std::pair{ActionKind::MoveWindowLeftHalf, ResultIcon::WindowLeft},
      std::pair{ActionKind::MoveWindowRightHalf, ResultIcon::WindowRight},
      std::pair{ActionKind::MoveWindowTopHalf, ResultIcon::WindowTop},
      std::pair{ActionKind::MoveWindowBottomHalf, ResultIcon::WindowBottom},
      std::pair{ActionKind::CenterWindow, ResultIcon::Center},
      std::pair{ActionKind::MoveWindowNextDisplay, ResultIcon::MultiMonitor},
      std::pair{ActionKind::CopyText, ResultIcon::Copy},
      std::pair{ActionKind::PasteText, ResultIcon::Clipboard},
  };
  for (const auto& [kind, expected] : actionIcons) {
    feathercast::app::DisplayItem item;
    item.isAction = true;
    item.action = kind;
    assert(feathercast::ui::ResolveResultIcon(item) == expected);
  }

  feathercast::app::DisplayItem internal;
  internal.isCalculator = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Calculator);
  internal = {};
  internal.isConversion = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Convert);
  internal = {};
  internal.isWebSearch = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::WebSearch);
  internal = {};
  internal.isExtension = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Puzzle);
  internal = {};
  internal.isSnippet = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Document);
  internal = {};
  internal.isClipboard = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Clipboard);
  internal = {};
  internal.isRunCommand = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Terminal);
  for (const auto [kind, expected] : {
           std::pair{feathercast::app::UtilityKind::LocalTime, ResultIcon::Clock},
           std::pair{feathercast::app::UtilityKind::LocalDate, ResultIcon::Calendar},
           std::pair{feathercast::app::UtilityKind::IsoWeek, ResultIcon::CalendarWeek},
           std::pair{feathercast::app::UtilityKind::UnixTime, ResultIcon::Code}}) {
    internal = {};
    internal.utility = feathercast::app::UtilityResult{kind};
    assert(feathercast::ui::ResolveResultIcon(internal) == expected);
  }
  internal = {};
  internal.app.source = L"quicklink";
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Link);
  internal = {};
  internal.app.source = L"file";
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::File);
  internal.app.fileIsDirectory = true;
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::Folder);
  internal = {};
  assert(feathercast::ui::ResolveResultIcon(internal) == ResultIcon::App);

  feathercast::settings_catalog::CatalogContext context;
  for (const auto& [hit, requirement] : {
           std::pair{
               feathercast::app::HitType::ClearScreenshotFullscreenShortcut,
               feathercast::settings_catalog::Requirement::
                   ExistingScreenshotFullscreenShortcut},
           std::pair{
               feathercast::app::HitType::ClearScreenshotRegionShortcut,
               feathercast::settings_catalog::Requirement::
                   ExistingScreenshotRegionShortcut},
           std::pair{
               feathercast::app::HitType::ClearFullscreenShortcut,
               feathercast::settings_catalog::Requirement::
                   ExistingRecordFullscreenShortcut},
           std::pair{
               feathercast::app::HitType::ClearRegionShortcut,
               feathercast::settings_catalog::Requirement::
                   ExistingRecordRegionShortcut}}) {
    const auto* descriptor = feathercast::settings_catalog::Find(hit);
    assert(descriptor && descriptor->requirement == requirement);
    assert(!feathercast::settings_catalog::Enabled(*descriptor, context));
  }
  context.hasScreenshotFullscreenShortcut = true;
  assert(feathercast::settings_catalog::Enabled(
      *feathercast::settings_catalog::Find(
          feathercast::app::HitType::ClearScreenshotFullscreenShortcut),
      context));
  assert(!feathercast::settings_catalog::Enabled(
      *feathercast::settings_catalog::Find(
          feathercast::app::HitType::ClearScreenshotRegionShortcut),
      context));
  context.clipboardEnabled = false;
  auto privacy = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::Privacy, context);
  assert(std::find(privacy.begin(), privacy.end(),
                   feathercast::app::HitType::ClipboardLimitDown) ==
         privacy.end());
  context.clipboardEnabled = true;
  privacy = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::Privacy, context);
  assert(std::find(privacy.begin(), privacy.end(),
                   feathercast::app::HitType::ClipboardLimitDown) !=
         privacy.end());
  const auto* clipboardLimit = feathercast::settings_catalog::Find(
      feathercast::app::HitType::ClipboardLimitDown);
  assert(clipboardLimit);
  context.storageIdle = false;
  assert(!feathercast::settings_catalog::Enabled(*clipboardLimit, context));
  feathercast::app::Settings settingValues;
  settingValues.compactMode = true;
  assert(feathercast::settings_catalog::Checked(
      feathercast::app::HitType::CompactToggle, settingValues));
  const auto* animation = feathercast::settings_catalog::Find(
      feathercast::app::HitType::AnimationLevel);
  assert(animation);
  assert(animation->kind ==
         feathercast::settings_catalog::ControlKind::Slider);
  assert(feathercast::settings_catalog::Role(*animation) ==
         feathercast::settings_catalog::AccessibleRole::Slider);
  assert(feathercast::settings_catalog::AccessibleValue(
             feathercast::app::HitType::AnimationLevel, settingValues) ==
         L"Full");
  const auto* compactDescriptor = feathercast::settings_catalog::Find(
      feathercast::app::HitType::CompactToggle);
  assert(compactDescriptor &&
         feathercast::settings_catalog::Role(*compactDescriptor) ==
             feathercast::settings_catalog::AccessibleRole::CheckButton);
  assert(feathercast::settings_catalog::AccessibleValue(
             feathercast::app::HitType::CompactToggle, settingValues) == L"On");
  assert(feathercast::settings_catalog::AccessibleValue(
             feathercast::app::HitType::OverlayWidthDown, settingValues) ==
         L"720 DIP");
  const auto generalControls = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::General, context);
  assert(std::count(generalControls.begin(), generalControls.end(),
                    feathercast::app::HitType::AnimationLevel) == 1);
  const auto libraryControls = feathercast::settings_catalog::FocusOrder(
      feathercast::app::SettingsCategory::Library, context);
  assert(libraryControls.size() == 2);
  assert(libraryControls[0] == feathercast::app::HitType::ManageSnippets);
  assert(libraryControls[1] == feathercast::app::HitType::ManageQuicklinks);
  const auto snippetsCapability = std::find_if(
      feathercast::capabilities::Catalog().begin(),
      feathercast::capabilities::Catalog().end(), [](const auto& item) {
        return item.stableId == L"snippets";
      });
  assert(snippetsCapability != feathercast::capabilities::Catalog().end());
  assert(snippetsCapability->action.kind ==
         feathercast::app::CapabilityActionKind::OpenSettings);
  assert(snippetsCapability->action.settingsCategory ==
         feathercast::app::SettingsCategory::Library);

  feathercast::ui::OverlayState overlay;
  overlay.status = feathercast::app::StatusMessage{
      feathercast::app::StatusSeverity::Error, L"Previous error"};
  const auto resetEffects = feathercast::ui::OverlayController::ResetForShow(
      overlay, feathercast::app::View::Search);
  assert((resetEffects & feathercast::ui::Effect(
                             feathercast::ui::UiEffect::RequestSearch)) != 0);
  assert(!overlay.status);

  feathercast::ui::OverlayState resumed;
  feathercast::ui::OverlayController::ResetForShow(
      resumed, feathercast::app::View::Search, L"2 + 2", true);
  assert(resumed.query == L"2 + 2" && resumed.caret == resumed.query.size());
  const auto resumedSelection =
      feathercast::ui::OverlayController::SelectionRange(resumed);
  assert(resumedSelection && resumedSelection->first == 0 &&
         resumedSelection->second == resumed.query.size());
  // The first new text starts a fresh expression by replacing the resumed one.
  feathercast::ui::OverlayController::InsertText(resumed, L"3");
  assert(resumed.query == L"3");

  // Moving the caret first opts into continuing the resumed expression.
  feathercast::ui::OverlayController::SetQuery(resumed, L"2 + 2");
  feathercast::ui::OverlayController::MoveCaret(
      resumed, resumed.query.size(), false);
  feathercast::ui::OverlayController::InsertText(resumed, L" + 3");
  assert(resumed.query == L"2 + 2 + 3");

  overlay.status = feathercast::app::StatusMessage{
      feathercast::app::StatusSeverity::Error, L"Previous error"};
  feathercast::ui::OverlayController::SetQuery(overlay, L"terminal");
  assert(overlay.query == L"terminal" && overlay.caret == 8);
  assert(!overlay.status);
  feathercast::ui::OverlayController::MoveCaret(overlay, 4, true);
  assert(overlay.selectionAnchor && *overlay.selectionAnchor == 8);
  feathercast::ui::OverlayController::InsertText(overlay, L"\U0001F600");
  assert(overlay.query == L"term\U0001F600");
  assert(!overlay.selectionAnchor && overlay.selected == 0 &&
         overlay.scroll == 0);
  feathercast::ui::OverlayController::SetQuery(overlay, L"terminal");
  feathercast::ui::OverlayController::Select(overlay, 99, 3);
  assert(overlay.selected == 2);
  feathercast::ui::OverlayController::SetScroll(overlay, 90, 50);
  assert(overlay.scroll == 50);
  feathercast::app::DisplayItem actionTarget;
  actionTarget.app.id = L"terminal";
  actionTarget.app.name = L"Terminal";
  feathercast::ui::OverlayController::EnterAction(
      overlay, actionTarget, L"app:terminal");
  assert(overlay.actionMode && overlay.actionTarget.app.id == L"terminal");
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(!overlay.actionMode && overlay.selected == 2 && overlay.scroll == 50);
  feathercast::ui::OverlayController::EnterBrowse(
      overlay, feathercast::app::BrowseView::Capabilities, L"cmd:guide");
  assert(overlay.browseView == feathercast::app::BrowseView::Capabilities);
  feathercast::ui::OverlayController::SetQuery(overlay, L"calculator");
  feathercast::ui::OverlayController::EnterBrowse(
      overlay, feathercast::app::BrowseView::Emoji, L"capability:emoji");
  assert(overlay.browseView == feathercast::app::BrowseView::Emoji);
  assert(overlay.navigationStack.size() == 2);
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(overlay.browseView == feathercast::app::BrowseView::Capabilities);
  assert(overlay.query == L"calculator");
  assert(overlay.pendingNavigationRestore);
  assert(overlay.pendingNavigationRestore->selectedKey ==
         L"capability:emoji");
  feathercast::ui::OverlayController::RestoreNavigation(overlay);
  assert(overlay.browseView == feathercast::app::BrowseView::None);
  assert(overlay.query == L"terminal");

  feathercast::app::ConfirmationDialog confirmation;
  confirmation.title = L"Confirm";
  feathercast::ui::OverlayController::ShowConfirmation(
      overlay, std::move(confirmation));
  assert(overlay.confirmation && overlay.confirmationFocus == 0);
  feathercast::ui::OverlayController::CloseConfirmation(overlay);
  assert(!overlay.confirmation);

  feathercast::ui::SettingsState settings;
  feathercast::ui::SettingsController::SelectCategory(
      settings, feathercast::app::SettingsCategory::Privacy);
  assert(settings.category == feathercast::app::SettingsCategory::Privacy);
  feathercast::ui::SettingsController::MoveFocus(settings, 5, 3);
  assert(settings.focusIndex == 2);
  feathercast::ui::SettingsController::CycleFocus(settings, 1, 3);
  assert(settings.focusIndex == 0);
  feathercast::ui::SettingsController::CycleFocus(settings, -1, 3);
  assert(settings.focusIndex == 2);
  feathercast::ui::SettingsController::BeginShortcutRecording(settings);
  assert(settings.recordingShortcut && settings.pendingShortcut.empty());
  feathercast::ui::SettingsController::SetPendingShortcut(settings,
                                                           L"Ctrl+Space");
  assert(!settings.recordingShortcut &&
         settings.pendingShortcut == L"Ctrl+Space");
  feathercast::ui::SettingsController::CancelShortcutRecording(settings);
  assert(settings.pendingShortcut.empty());

  feathercast::ui::OverlayState history;
  for (int index = 0; index < 40; ++index) {
    feathercast::ui::OverlayController::InsertText(history, L"x");
  }
  assert(history.query.size() == 40 && history.queryUndo.size() == 32);
  history.imeComposition = L"pending";
  for (int index = 0; index < 32; ++index) {
    const auto effects =
        feathercast::ui::OverlayController::UndoQueryEdit(history);
    assert(feathercast::ui::HasEffect(
        effects, feathercast::ui::UiEffect::RequestSearch));
  }
  assert(history.query.size() == 8 && history.imeComposition.empty());
  feathercast::ui::OverlayController::RedoQueryEdit(history);
  assert(history.query.size() == 9);
  feathercast::ui::OverlayController::InsertText(history, L"new");
  assert(!feathercast::ui::OverlayController::CanRedoQueryEdit(history));
  const auto programmedHistorySize = history.queryUndo.size();
  feathercast::ui::OverlayController::SetQuery(history, L"programmatic");
  assert(history.queryUndo.size() == programmedHistorySize);

  const auto discoverEmpty = feathercast::capabilities::EmptyStateDisplay(
      feathercast::capabilities::EmptyStateAction::Discover);
  assert(discoverEmpty.isCapability &&
         discoverEmpty.capability.action.kind ==
             feathercast::app::CapabilityActionKind::OpenBrowse);
  const auto privacyEmpty = feathercast::capabilities::EmptyStateDisplay(
      feathercast::capabilities::EmptyStateAction::FilesPrivacy);
  assert(privacyEmpty.capability.action.kind ==
             feathercast::app::CapabilityActionKind::OpenSettings &&
         privacyEmpty.capability.action.settingsCategory ==
             feathercast::app::SettingsCategory::Privacy);

  using feathercast::window_activation::ExistingInstanceResult;
  HWND fakeWindow = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1));
  unsigned observedTimeout = 0;
  feathercast::window_activation::ExistingInstanceAdapter instanceAdapter{
      [&] { return fakeWindow; }, [](HWND) { return true; },
      [&](HWND, unsigned timeout) {
        observedTimeout = timeout;
        return true;
      }};
  assert(feathercast::window_activation::ActivateExisting(instanceAdapter,
                                                           5000) ==
         ExistingInstanceResult::Activated);
  assert(observedTimeout == 1000);
  instanceAdapter.validWindow = [](HWND) { return false; };
  assert(feathercast::window_activation::ActivateExisting(instanceAdapter) ==
         ExistingInstanceResult::StaleWindow);
  instanceAdapter.findWindow = [] { return static_cast<HWND>(nullptr); };
  assert(feathercast::window_activation::ActivateExisting(instanceAdapter) ==
         ExistingInstanceResult::NotFound);

  HWND foreground = nullptr;
  bool attached = false;
  feathercast::window_activation::FocusAdapter focusAdapter{
      [](HWND) { return true; }, [](HWND) { return true; }, [](HWND) {},
      [&] { return foreground; }, [](HWND) { return DWORD{2}; },
      [] { return DWORD{1}; },
      [&](DWORD, DWORD, bool enable) {
        attached = enable;
        return true;
      },
      [](HWND) {},
      [&](HWND target) {
        if (attached) foreground = target;
        return attached;
      }};
  assert(feathercast::window_activation::FocusVerified(fakeWindow,
                                                        focusAdapter));
  focusAdapter.validWindow = [](HWND) { return false; };
  assert(!feathercast::window_activation::FocusVerified(fakeWindow,
                                                         focusAdapter));

  using namespace feathercast::accessibility_projection;
  static_assert(SearchChild() == 1 && StatusChild() == 2);
  static_assert(ResultChild(0) == 3 && PreviewChild(4) == 7);
  const auto loadingStatus =
      ProjectLiveStatus(false, true, L"", false, false, std::nullopt);
  assert(loadingStatus.visible && loadingStatus.kind == LiveStatusKind::Loading &&
         loadingStatus.value == L"Searching...");
  const auto emptyStatus =
      ProjectLiveStatus(false, false, L"No results", false, false, std::nullopt);
  assert(emptyStatus.visible && emptyStatus.kind == LiveStatusKind::Empty);
  const auto errorStatus = ProjectLiveStatus(
      false, false, L"", false, false,
      feathercast::app::StatusMessage{
          feathercast::app::StatusSeverity::Error, L"Failed"});
  assert(errorStatus.visible && errorStatus.alert &&
         errorStatus.kind == LiveStatusKind::Error);
  const auto previewStatus =
      ProjectLiveStatus(false, false, L"", true, true, std::nullopt);
  assert(previewStatus.kind == LiveStatusKind::Preview &&
         previewStatus.value == L"Preview ready");
  return 0;
}
