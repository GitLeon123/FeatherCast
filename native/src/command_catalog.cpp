#include "command_catalog.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace feathercast::commands {
namespace {

app::DisplayItem CommandItem(const CommandDescriptor& descriptor,
                             const std::wstring* alias = nullptr) {
  app::DisplayItem item;
  item.isCommand = true;
  item.command = descriptor.kind;
  item.commandStableId = descriptor.stableId;
  item.commandName = descriptor.label;
  item.commandDetail = descriptor.detail;
  item.commandKeywords = descriptor.keywords;
  if (alias) {
    const auto validation = core::ValidateAlias(*alias);
    if (validation.valid) item.commandKeywords.push_back(validation.value);
  }
  return item;
}

app::DisplayItem ActionItem(app::ActionKind kind, std::wstring label,
                            std::wstring detail,
                            const app::DisplayItem& target) {
  app::DisplayItem item;
  item.isAction = true;
  item.action = kind;
  item.commandKeywords = {label, detail};
  item.commandName = std::move(label);
  item.commandDetail = std::move(detail);
  if (target.isAction) {
    item.actionTarget = target.actionTarget;
  } else {
    item.actionTarget = target.isWindow ? app::ActionTarget{target.window}
                                        : app::ActionTarget{target.app};
  }
  return item;
}

app::DisplayItem TextActionItem(app::ActionKind kind, std::wstring label,
                                std::wstring detail, std::wstring value) {
  app::DisplayItem item;
  item.isAction = true;
  item.action = kind;
  item.commandKeywords = {label, detail};
  item.commandName = std::move(label);
  item.commandDetail = std::move(detail);
  item.actionTarget = app::TextActionPayload{std::move(value)};
  return item;
}

app::DisplayItem InvocationActionItem(app::ActionKind kind,
                                      std::wstring label,
                                      std::wstring detail,
                                      app::AliasTarget target) {
  app::DisplayItem item;
  item.isAction = true;
  item.action = kind;
  item.commandKeywords = {label, detail, target.currentAlias};
  item.commandName = std::move(label);
  item.commandDetail = std::move(detail);
  item.actionTarget = std::move(target);
  return item;
}

std::optional<app::AliasTarget> InvocationTarget(
    const app::DisplayItem& item, const app::Settings& settings) {
  app::AliasTarget target;
  target.invocationKey = item.InvocationKey();
  if (target.invocationKey.empty()) return std::nullopt;

  if (item.isCommand) {
    const CommandDescriptor* descriptor = !item.commandStableId.empty()
        ? Find(item.commandStableId)
        : Find(item.command);
    if (!descriptor) return std::nullopt;
    target.stableId = descriptor->stableId;
    if (const auto alias = settings.commandAliases.find(target.stableId);
        alias != settings.commandAliases.end()) {
      target.currentAlias = alias->second;
    }
  } else if (item.isSnippet) {
    target.stableId = item.snippet.keyword;
    target.currentAlias = item.snippet.keyword;
  } else if (item.app.source == L"quicklink") {
    constexpr std::wstring_view prefix = L"quicklink:";
    target.stableId = item.app.id.rfind(prefix, 0) == 0
        ? item.app.id.substr(prefix.size())
        : (!item.app.keywords.empty() ? item.app.keywords.front()
                                      : item.app.name);
    target.currentAlias = target.stableId;
  } else {
    return std::nullopt;
  }
  return target;
}

void AppendInvocationActions(std::vector<app::DisplayItem>& actions,
                             const app::DisplayItem& item,
                             const app::Settings& settings) {
  auto target = InvocationTarget(item, settings);
  if (!target) return;

  const bool canonicalKeyword = item.isSnippet ||
                                item.app.source == L"quicklink";
  actions.push_back(InvocationActionItem(
      app::ActionKind::EditAlias,
      canonicalKeyword ? L"Edit Keyword"
                       : (target->currentAlias.empty() ? L"Add Alias"
                                                       : L"Edit Alias"),
      canonicalKeyword
          ? L"Change the canonical keyword used to find this item"
          : L"Choose another search name for this command",
      *target));

  const bool pinned = std::find(settings.pinnedItems.begin(),
                                settings.pinnedItems.end(),
                                target->invocationKey) !=
                      settings.pinnedItems.end();
  actions.push_back(InvocationActionItem(
      pinned ? app::ActionKind::UnpinInvocation
             : app::ActionKind::PinInvocation,
      pinned ? L"Remove from Favorites" : L"Add to Favorites",
      pinned ? L"Remove this item from the launcher favorites"
             : L"Show this item in launcher favorites",
      std::move(*target)));
}

bool HasAppKey(const std::vector<std::wstring>& values,
               const app::AppEntry& app) {
  for (const auto& key : {app.id, app.path, app.launchTarget,
                          app.targetPath}) {
    if (!key.empty() &&
        std::find(values.begin(), values.end(), key) != values.end()) {
      return true;
    }
  }
  return false;
}

}  // namespace

const std::vector<CommandDescriptor>& Catalog() {
  static const std::vector<CommandDescriptor> commands = {
      {L"timers", app::CommandKind::Timers, L"Timers & Stopwatch",
       L"Manage saved timers and a stopwatch. Try: timer 10m Tea",
       {L"timer", L"timers", L"countdown", L"stopwatch", L"alarm"}},
      {L"clipboard-history", app::CommandKind::ClipboardHistory,
       L"Clipboard History", L"Browse and paste copied items",
       {L"clipboard", L"history", L"paste", L"copy"}},
      {L"emoji-picker", app::CommandKind::EmojiPicker, L"Search Emoji",
       L"Browse and paste emoji",
       {L"emoji", L"emoticon", L"smiley", L"symbol", L"face"}},
      {L"games", app::CommandKind::Games, L"Games",
       L"Browse locally installed games",
       {L"games", L"library", L"steam", L"epic", L"xbox", L"gog"}},
      {L"discover-feathercast", app::CommandKind::DiscoverFeatherCast,
       L"Discover FeatherCast", L"Browse features, shortcuts, and examples",
       {L"help", L"features", L"shortcuts", L"how to", L"guide"}},
      {L"settings", app::CommandKind::Settings, L"Settings",
       L"Open FeatherCast settings",
       {L"preferences", L"options", L"shortcut"}},
      {L"quit", app::CommandKind::Quit, L"Quit FeatherCast",
       L"Exit the background launcher", {L"exit", L"close"}},
      {L"restart-app", app::CommandKind::Restart, L"Restart FeatherCast",
       L"Restart the native app", {L"reload"}},
      {L"refresh-apps", app::CommandKind::RefreshApps,
       L"Refresh Apps & Games", L"Rescan apps and locally installed games",
       {L"rescan", L"reload apps", L"refresh games"}},
      {L"clear-icon-cache", app::CommandKind::ClearIconCache,
       L"Clear Icon Cache", L"Delete cached shell icons",
       {L"icons", L"cache"}},
      {L"clear-recents", app::CommandKind::ClearRecents, L"Clear Recents",
       L"Forget recently used apps", {L"history", L"recent apps"}},
      {L"open-settings-folder", app::CommandKind::OpenDataFolder,
       L"Open Settings Folder", L"Open roaming FeatherCast configuration",
       {L"settings json", L"snippets", L"theme", L"plugins"}},
      {L"open-local-data", app::CommandKind::OpenLocalDataFolder,
       L"Open Local Data Folder", L"Open logs, cache, database, and updates",
       {L"logs", L"cache", L"database", L"updates"}},
      {L"reload-extensions", app::CommandKind::ReloadExtensions,
       L"Reload Extensions", L"Reload plugin manifests and helper processes",
       {L"plugins", L"extensions", L"dll"}},
      {L"check-updates", app::CommandKind::CheckForUpdates,
       L"Check for Updates", L"Find and install the latest FeatherCast release",
       {L"update", L"upgrade", L"release"}},
      {L"clear-clipboard", app::CommandKind::ClearClipboardHistory,
       L"Clear Clipboard History", L"Forget saved clipboard entries",
       {L"clipboard", L"history", L"clear"}},
      {L"open-snippets", app::CommandKind::OpenSnippetsFile,
       L"Open Snippets File", L"Edit reusable text snippets",
       {L"snippet", L"snippets json", L"text expansion"}},
      {L"reload-snippets", app::CommandKind::ReloadSnippets,
       L"Reload Snippets", L"Reload snippets.json from disk",
       {L"snippet", L"reload", L"text expansion"}},
      {L"open-theme", app::CommandKind::OpenThemeFile, L"Open Theme File",
       L"Edit theme.json styling", {L"theme", L"appearance", L"json", L"style"}},
      {L"reload-theme", app::CommandKind::ReloadTheme, L"Reload Theme",
       L"Reload theme.json styling", {L"theme", L"appearance", L"reload", L"style"}},
      {L"lock-pc", app::CommandKind::LockPC, L"Lock PC",
       L"Lock this computer", {L"lock", L"workstation", L"secure"}},
      {L"sleep-pc", app::CommandKind::SleepPC, L"Sleep PC",
       L"Put this computer to sleep", {L"sleep", L"suspend", L"standby"},
       ConfirmationDescriptor{L"Put this PC to sleep?",
                              L"Your computer will go to sleep immediately.",
                              L"Sleep"}},
      {L"mute-audio", app::CommandKind::MuteAudio, L"Mute Audio",
       L"Toggle system audio mute", {L"mute", L"sound", L"volume", L"audio"}},
      {L"shut-down", app::CommandKind::ShutDown, L"Shut Down PC",
       L"Power off this computer", {L"shutdown", L"power off", L"turn off"},
       ConfirmationDescriptor{L"Shut down this PC?",
                              L"This will close apps and turn off your computer.",
                              L"Shut Down"}},
      {L"restart-pc", app::CommandKind::RestartPC, L"Restart PC",
       L"Reboot this computer", {L"reboot", L"restart computer"},
       ConfirmationDescriptor{L"Restart this PC?",
                              L"This will close apps and restart your computer.",
                              L"Restart"}},
      {L"empty-recycle-bin", app::CommandKind::EmptyRecycleBin,
       L"Empty Recycle Bin", L"Permanently delete recycle bin contents",
       {L"trash", L"empty bin", L"recycle"},
       ConfirmationDescriptor{L"Empty the Recycle Bin?",
                              L"This permanently deletes everything in the Recycle Bin.",
                              L"Empty Recycle Bin"}},
      {L"volume-control", app::CommandKind::VolumeControl, L"Volume Control",
       L"Adjust the default audio output volume",
       {L"audio", L"sound", L"speaker", L"volume", L"level"}},
      {L"volume-up", app::CommandKind::VolumeUp, L"Volume Up",
       L"Increase the default audio output volume",
       {L"audio", L"sound", L"speaker", L"louder"}},
      {L"volume-down", app::CommandKind::VolumeDown, L"Volume Down",
       L"Decrease the default audio output volume",
       {L"audio", L"sound", L"speaker", L"quieter"}},
      {L"media-play-pause", app::CommandKind::MediaPlayPause,
       L"Play or Pause Media", L"Toggle playback in the active media session",
       {L"media", L"music", L"play", L"pause"}},
      {L"media-next", app::CommandKind::MediaNext, L"Next Media Track",
       L"Skip to the next media track", {L"media", L"music", L"next", L"skip"}},
      {L"media-previous", app::CommandKind::MediaPrevious,
       L"Previous Media Track", L"Return to the previous media track",
       {L"media", L"music", L"previous", L"back"}},
      {L"show-desktop", app::CommandKind::ShowDesktop, L"Show Desktop",
       L"Toggle all windows to reveal the desktop",
       {L"desktop", L"windows", L"minimize", L"hide"}},
      {L"generate-uuid", app::CommandKind::GenerateUuid, L"Generate UUID",
       L"Create and copy a new UUID v4",
       {L"uuid", L"guid", L"developer", L"random identifier"}},
      {L"screenshot-fullscreen", app::CommandKind::ScreenshotFullscreen,
       L"Take Fullscreen Screenshot", L"Capture the display under the cursor",
       {L"screenshot", L"screen capture", L"monitor", L"fullscreen"}},
      {L"screenshot-region", app::CommandKind::ScreenshotRegion,
       L"Take Region Screenshot", L"Select and capture part of the screen",
       {L"screenshot", L"screen capture", L"selection", L"region"}},
      {L"record-fullscreen", app::CommandKind::RecordFullscreen,
       L"Record Full Screen", L"Record the display under the cursor",
       {L"record", L"screen recording", L"video", L"monitor", L"fullscreen"}},
      {L"record-region", app::CommandKind::RecordRegion,
       L"Record Screen Region", L"Select and record part of the screen",
       {L"record", L"screen recording", L"video", L"selection", L"region"}},
  };
  return commands;
}

const CommandDescriptor* Find(app::CommandKind kind) {
  const auto& commands = Catalog();
  const auto found = std::find_if(
      commands.begin(), commands.end(),
      [&](const CommandDescriptor& command) { return command.kind == kind; });
  return found == commands.end() ? nullptr : &*found;
}

const CommandDescriptor* Find(std::wstring_view stableId) {
  const auto& commands = Catalog();
  const auto found = std::find_if(
      commands.begin(), commands.end(),
      [&](const CommandDescriptor& command) {
        return command.stableId == stableId;
      });
  return found == commands.end() ? nullptr : &*found;
}

std::vector<app::DisplayItem> BuildCommandItems() {
  return BuildCommandItems({});
}

std::vector<app::DisplayItem> BuildCommandItems(
    const std::map<std::wstring, std::wstring>& aliases) {
  std::vector<app::DisplayItem> items;
  items.reserve(Catalog().size());
  for (const auto& descriptor : Catalog()) {
    const auto alias = aliases.find(descriptor.stableId);
    items.push_back(CommandItem(
        descriptor, alias == aliases.end() ? nullptr : &alias->second));
  }
  return items;
}

core::SearchItem BuildSearchItem(
    const app::DisplayItem& command,
    const std::map<std::wstring, std::wstring>& aliases) {
  core::SearchItem item;
  if (!command.isCommand) return item;
  const CommandDescriptor* descriptor = !command.commandStableId.empty()
      ? Find(command.commandStableId)
      : Find(command.command);
  if (!descriptor) return item;
  item.id = command.InvocationKey();
  if (item.id.empty()) {
    item.id = core::StableInvocationKey(L"command", descriptor->stableId);
  }
  item.kind = L"command";
  item.source = L"command";
  item.name = command.commandName;
  item.keywords = descriptor->keywords;
  item.keywords.push_back(descriptor->stableId);
  item.systemEssential = true;
  if (const auto alias = aliases.find(descriptor->stableId);
      alias != aliases.end()) {
    const auto validation = core::ValidateAlias(alias->second);
    if (validation.valid) item.aliases.push_back(validation.value);
  }
  return item;
}

std::vector<app::DisplayItem> BuildActions(
    const app::DisplayItem& target, const app::Settings& settings) {
  std::vector<app::DisplayItem> actions;
  if (target.isAction && target.action == app::ActionKind::ArrangeWindow) {
    actions.push_back(ActionItem(app::ActionKind::MoveWindowLeftHalf,
                                 L"Left Half", L"Fill the left half", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowRightHalf,
                                 L"Right Half", L"Fill the right half", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowTopHalf,
                                 L"Top Half", L"Fill the top half", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowBottomHalf,
                                 L"Bottom Half", L"Fill the bottom half", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowLeftThird,
                                 L"Left Third", L"Fill the left third", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowCenterThird,
                                 L"Center Third", L"Fill the center third", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowRightThird,
                                 L"Right Third", L"Fill the right third", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowTopLeft,
                                 L"Top Left Quarter", L"Fill the top-left quarter", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowTopRight,
                                 L"Top Right Quarter", L"Fill the top-right quarter", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowBottomLeft,
                                 L"Bottom Left Quarter", L"Fill the bottom-left quarter", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowBottomRight,
                                 L"Bottom Right Quarter", L"Fill the bottom-right quarter", target));
    actions.push_back(ActionItem(app::ActionKind::CenterWindow,
                                 L"Center", L"Center without resizing", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowPreviousDisplay,
                                 L"Previous Display", L"Move to the previous monitor", target));
    actions.push_back(ActionItem(app::ActionKind::MoveWindowNextDisplay,
                                 L"Next Display", L"Move to the next monitor", target));
    return actions;
  }
  if (target.isCommand) {
    AppendInvocationActions(actions, target, settings);
    return actions;
  }
  if (target.isAction || target.isExtension ||
      target.isCapability || target.isRunCommand || target.isWebSearch) {
    return actions;
  }
  if (target.isWindow) {
    actions.push_back(ActionItem(app::ActionKind::Switch, L"Switch to Window",
                                 L"Focus " + target.window.name, target));
    actions.push_back(ActionItem(app::ActionKind::Minimize, L"Minimize Window",
                                 L"Minimize " + target.window.name, target));
    actions.push_back(ActionItem(app::ActionKind::MaximizeRestore,
                                 L"Maximize or Restore Window",
                                 L"Toggle window state", target));
    actions.push_back(ActionItem(app::ActionKind::ArrangeWindow,
                                 L"Arrange Window...",
                                 L"Choose a half, third, quarter, or display", target));
    actions.push_back(ActionItem(app::ActionKind::CloseWindow, L"Close Window",
                                 L"Send close request", target));
    return actions;
  }
  if (target.isCalculator || target.isConversion) {
    actions.push_back(TextActionItem(app::ActionKind::CopyText, L"Copy Result",
                                     L"Copy the result to the clipboard",
                                     target.calculationResult));
    actions.push_back(TextActionItem(
        app::ActionKind::CopyText, L"Copy Expression and Result",
        L"Copy the complete calculation",
        target.calculationExpression + L" = " + target.calculationResult));
    actions.push_back(TextActionItem(app::ActionKind::PasteText, L"Paste Result",
                                     L"Paste the result into the previous app",
                                     target.calculationResult));
    return actions;
  }
  if (target.isSnippet || target.isClipboard || target.isSymbol ||
      target.utility) {
    std::wstring value;
    if (target.isSnippet) value = target.snippet.text;
    else if (target.isClipboard) value = target.clipboard.text;
    else if (target.isSymbol) value = target.symbol.value;
    else value = target.utility->value;
    actions.push_back(TextActionItem(app::ActionKind::CopyText, L"Copy",
                                     L"Copy this value to the clipboard", value));
    actions.push_back(TextActionItem(app::ActionKind::PasteText, L"Paste",
                                     L"Paste this value into the previous app", value));
    if (target.isClipboard) {
      auto pin = TextActionItem(target.clipboard.pinned ? app::ActionKind::UnpinClipboard : app::ActionKind::PinClipboard,
                                target.clipboard.pinned ? L"Remove from Favorites" : L"Add to Favorites",
                                L"Keep this entry when older history is removed", L"");
      pin.actionTarget = target.clipboard;
      actions.push_back(std::move(pin));
    } else if (target.isSnippet) {
      AppendInvocationActions(actions, target, settings);
    }
    return actions;
  }
  if (target.app.source == L"file") {
    actions.push_back(ActionItem(app::ActionKind::Preview, L"Preview",
                                 L"Show text, image, or metadata preview",
                                 target));
  }
  actions.push_back(ActionItem(app::ActionKind::Open, L"Open",
                               L"Launch " + target.app.name, target));
  if (target.app.adminSupported) {
    actions.push_back(ActionItem(app::ActionKind::RunAsAdmin,
                                 L"Run as Administrator", L"Launch elevated",
                                 target));
  }
  if (target.app.launchType != app::LaunchType::Shell &&
      (!target.app.path.empty() || !target.app.targetPath.empty())) {
    actions.push_back(ActionItem(app::ActionKind::OpenLocation,
                                 L"Open File Location",
                                 L"Show app shortcut or target", target));
  }
  if (!target.app.path.empty() || !target.app.targetPath.empty() ||
      !target.app.launchTarget.empty()) {
    actions.push_back(ActionItem(app::ActionKind::CopyPath, L"Copy Path",
                                 L"Copy app shortcut or target path", target));
  }
  if (!target.app.id.empty() && target.app.source != L"quicklink" &&
      target.app.source != L"file") {
    actions.push_back(ActionItem(app::ActionKind::EditAppAlias,
                                 L"Add or Edit Alias",
                                 L"Choose another search name for this app", target));
  }
  if (target.app.source == L"quicklink") {
    AppendInvocationActions(actions, target, settings);
  } else {
    actions.push_back(
        HasAppKey(settings.pinnedApps, target.app)
            ? ActionItem(app::ActionKind::Unpin, L"Unpin",
                         L"Remove from pinned apps", target)
            : ActionItem(app::ActionKind::Pin, L"Pin",
                         L"Keep near the top of results", target));
  }
  actions.push_back(
      HasAppKey(settings.hiddenApps, target.app)
          ? ActionItem(app::ActionKind::Unhide, L"Unhide",
                       L"Show in launcher results", target)
          : ActionItem(app::ActionKind::Hide, L"Hide",
                       L"Remove from launcher results", target));
  return actions;
}

bool IsPersonalizableInvocation(const app::DisplayItem& item) {
  return !item.InvocationKey().empty() &&
         (item.isCommand || item.isSnippet ||
          item.app.source == L"quicklink");
}

bool RecordsRecentActivation(const app::DisplayItem& item) {
  return IsPersonalizableInvocation(item) && !item.isAction;
}

bool ValidateCatalog(std::wstring* error) {
  std::set<std::wstring> ids;
  std::set<app::CommandKind> kinds;
  for (const auto& command : Catalog()) {
    if (command.stableId.empty() || command.label.empty() ||
        !ids.insert(command.stableId).second ||
        !kinds.insert(command.kind).second) {
      if (error) *error = L"Command identifiers, kinds, and labels must be unique.";
      return false;
    }
  }
  return true;
}

}  // namespace feathercast::commands
