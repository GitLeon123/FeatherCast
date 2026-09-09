#pragma once

#include "settings.hpp"
#include "snippets.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::library {

enum class ItemKind {
  Snippet,
  Quicklink,
  AppAlias,
  CommandAlias,
  WebSearch,
};

struct AppAlias {
  std::wstring appId;
  std::wstring appName;
  std::wstring alias;
};

struct AppChoice {
  std::wstring id;
  std::wstring name;
};

struct CommandAlias {
  std::wstring stableId;
  std::wstring commandName;
  std::wstring alias;
};

struct CommandChoice {
  std::wstring stableId;
  std::wstring name;
};

struct WebSearch {
  std::wstring keyword;
  std::wstring urlTemplate;
};

struct OperationResult {
  bool succeeded = false;
  std::wstring message;
};

std::wstring NormalizeKeyword(std::wstring value);

std::optional<std::wstring> ValidateSnippet(
    const snippets::Snippet& candidate,
    const std::vector<snippets::Snippet>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateQuicklink(
    const settings::Quicklink& candidate,
    const std::vector<settings::Quicklink>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateAppAlias(
    const AppAlias& candidate, const std::vector<AppAlias>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::optional<std::wstring> ValidateCommandAlias(
    const CommandAlias& candidate,
    const std::vector<CommandAlias>& existing,
    const std::vector<AppAlias>& appAliases,
    const std::vector<snippets::Snippet>& snippets,
    const std::vector<settings::Quicklink>& quicklinks,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::vector<CommandAlias> BuildCommandAliases(
    const std::map<std::wstring, std::wstring>& aliases,
    const std::vector<CommandChoice>& commands);
std::optional<std::map<std::wstring, std::wstring>> ToCommandAliasMap(
    const std::vector<CommandAlias>& aliases,
    const std::vector<AppAlias>& appAliases,
    const std::vector<snippets::Snippet>& snippets,
    const std::vector<settings::Quicklink>& quicklinks,
    std::wstring* error = nullptr);

std::optional<std::wstring> ValidateWebSearch(
    const WebSearch& candidate, const std::vector<WebSearch>& existing,
    std::optional<std::size_t> editingIndex = std::nullopt);

std::vector<std::size_t> SortedSnippetIndices(
    const std::vector<snippets::Snippet>& snippets);
std::vector<std::size_t> SortedQuicklinkIndices(
    const std::vector<settings::Quicklink>& quicklinks);
std::vector<std::size_t> SortedAppAliasIndices(
    const std::vector<AppAlias>& aliases);
std::vector<std::size_t> SortedCommandAliasIndices(
    const std::vector<CommandAlias>& aliases);
std::vector<std::size_t> SortedWebSearchIndices(
    const std::vector<WebSearch>& searches);

}  // namespace feathercast::library
