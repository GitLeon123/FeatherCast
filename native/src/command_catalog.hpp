#pragma once

#include "app_types.hpp"

#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::commands {

struct ConfirmationDescriptor {
  std::wstring title;
  std::wstring message;
  std::wstring actionLabel;
};

struct CommandDescriptor {
  std::wstring stableId;
  app::CommandKind kind = app::CommandKind::Settings;
  std::wstring label;
  std::wstring detail;
  std::vector<std::wstring> keywords;
  std::optional<ConfirmationDescriptor> confirmation;
};

struct ActionDescriptor {
  app::ActionKind kind = app::ActionKind::None;
  std::wstring label;
  std::wstring detail;
};

const std::vector<CommandDescriptor>& Catalog();
const CommandDescriptor* Find(app::CommandKind kind);
const CommandDescriptor* Find(std::wstring_view stableId);
std::vector<app::DisplayItem> BuildCommandItems();
std::vector<app::DisplayItem> BuildCommandItems(
    const std::map<std::wstring, std::wstring>& aliases);
core::SearchItem BuildSearchItem(
    const app::DisplayItem& command,
    const std::map<std::wstring, std::wstring>& aliases = {});
std::vector<app::DisplayItem> BuildActions(
    const app::DisplayItem& target, const app::Settings& settings);
// One Unhide action per hidden app key, resolved against the known apps so
// the list shows real names; keys with no matching app are still listed so
// they can be removed. An app matched by several keys is listed once.
std::vector<app::DisplayItem> BuildHiddenAppActions(
    const std::vector<app::AppEntry>& apps, const app::Settings& settings);
// False for FeatherCast itself and for processes Windows needs to keep
// running, so End Task is never offered for them.
bool CanEndTask(const app::WindowEntry& window, DWORD currentProcessId);
bool IsPersonalizableInvocation(const app::DisplayItem& item);
bool RecordsRecentActivation(const app::DisplayItem& item);
bool ValidateCatalog(std::wstring* error = nullptr);

}  // namespace feathercast::commands
