#include "library.hpp"
#include "snippets_io.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <string>

int main() {
  using feathercast::library::SortedQuicklinkIndices;
  using feathercast::library::SortedAppAliasIndices;
  using feathercast::library::SortedCommandAliasIndices;
  using feathercast::library::SortedSnippetIndices;
  using feathercast::library::SortedWebSearchIndices;
  using feathercast::library::ValidateAppAlias;
  using feathercast::library::ValidateCommandAlias;
  using feathercast::library::ValidateQuicklink;
  using feathercast::library::ValidateSnippet;
  using feathercast::library::ValidateWebSearch;
  using feathercast::settings::Quicklink;
  using feathercast::snippets::Snippet;

  std::vector<Snippet> snippets = {
      {L"sig", L"Signature", L"Kind regards\nLeon"},
      {L"addr", L"Address", L"Vienna"},
  };
  assert(!ValidateSnippet(snippets[0], snippets, 0));
  assert(ValidateSnippet({L"SIG", L"Duplicate", L"text"}, snippets));
  assert(ValidateSnippet({L"", L"Missing", L"text"}, snippets));
  assert(ValidateSnippet({L"new", L"", L"text"}, snippets));
  assert(ValidateSnippet({L"new", L"Name", L"  "}, snippets));
  const auto snippetOrder = SortedSnippetIndices(snippets);
  assert(snippetOrder.size() == 2 && snippetOrder[0] == 1 &&
         snippetOrder[1] == 0);

  std::vector<Quicklink> quicklinks = {
      {L"docs", L"Documentation", L"https://example.com/docs"},
      {L"repo", L"", L"C:\\work\\repo"},
  };
  assert(!ValidateQuicklink(quicklinks[0], quicklinks, 0));
  assert(ValidateQuicklink({L"DOCS", L"Other", L"https://example.com"},
                           quicklinks));
  assert(ValidateQuicklink({L"new", L"", L""}, quicklinks));
  const auto linkOrder = SortedQuicklinkIndices(quicklinks);
  assert(linkOrder.size() == 2 && linkOrder[0] == 0 && linkOrder[1] == 1);

  using feathercast::library::AppAlias;
  std::vector<AppAlias> aliases = {
      {L"app:code", L"Visual Studio Code", L"code"},
      {L"app:terminal", L"Terminal", L"term"},
  };
  assert(!ValidateAppAlias(aliases[0], aliases, 0));
  assert(ValidateAppAlias({L"app:other", L"Other", L"CODE"}, aliases));
  assert(ValidateAppAlias({L"", L"Missing", L"alias"}, aliases));
  assert(ValidateAppAlias({L"app:other", L"Other", std::wstring(65, L'x')},
                          aliases));
  const auto aliasOrder = SortedAppAliasIndices(aliases);
  assert(aliasOrder.size() == 2 && aliasOrder[0] == 1 && aliasOrder[1] == 0);

  using feathercast::library::CommandAlias;
  const std::vector<feathercast::library::CommandChoice> commandChoices = {
      {L"settings", L"Settings"},
      {L"volume-up", L"Volume Up"},
  };
  const std::map<std::wstring, std::wstring> commandAliasMap = {
      {L"settings", L"prefs"},
      {L"removed-command", L"legacy"},
  };
  auto commandAliases = feathercast::library::BuildCommandAliases(
      commandAliasMap, commandChoices);
  assert(commandAliases.size() == 2);
  assert(commandAliases[0].stableId == L"removed-command" &&
         commandAliases[0].commandName.empty());
  assert(commandAliases[1].stableId == L"settings" &&
         commandAliases[1].commandName == L"Settings");
  assert(!ValidateCommandAlias(commandAliases[0], commandAliases, aliases,
                               snippets, quicklinks, 0));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"PREFS"}, commandAliases, aliases,
      snippets, quicklinks));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"code"}, commandAliases, aliases,
      snippets, quicklinks));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"sig"}, commandAliases, aliases,
      snippets, quicklinks));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"docs"}, commandAliases, aliases,
      snippets, quicklinks));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"@volume"}, commandAliases, aliases,
      snippets, quicklinks));
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"two\nlines"}, commandAliases,
      aliases, snippets, quicklinks));
  commandAliases.push_back({L"volume-up", L"Volume Up", L"louder"});
  assert(ValidateCommandAlias(
      {L"volume-up", L"Volume Up", L"different"}, commandAliases,
      aliases, snippets, quicklinks));
  std::wstring commandAliasError;
  const auto serializedAliases = feathercast::library::ToCommandAliasMap(
      commandAliases, aliases, snippets, quicklinks, &commandAliasError);
  assert(serializedAliases && commandAliasError.empty());
  assert(serializedAliases->at(L"settings") == L"prefs");
  assert(serializedAliases->at(L"volume-up") == L"louder");
  const auto commandAliasOrder = SortedCommandAliasIndices(commandAliases);
  assert(commandAliasOrder.size() == 3 && commandAliasOrder[0] == 0 &&
         commandAliasOrder[1] == 1 && commandAliasOrder[2] == 2);

  using feathercast::library::WebSearch;
  std::vector<WebSearch> webSearches = {
      {L"wiki", L"https://en.wikipedia.org/wiki/Special:Search?search=%s"},
      {L"g", L"https://www.google.com/search?q=%s"},
  };
  assert(!ValidateWebSearch(webSearches[0], webSearches, 0));
  assert(ValidateWebSearch({L"WIKI", L"https://example.com/?q=%s"},
                           webSearches));
  assert(ValidateWebSearch({L"bad key", L"https://example.com/?q=%s"},
                           webSearches));
  assert(ValidateWebSearch({L"bad", L"ftp://example.com/%s"}, webSearches));
  assert(ValidateWebSearch({L"none", L"https://example.com/"}, webSearches));
  assert(ValidateWebSearch({L"twice", L"https://example.com/%s/%s"},
                           webSearches));
  const auto webOrder = SortedWebSearchIndices(webSearches);
  assert(webOrder.size() == 2 && webOrder[0] == 1 && webOrder[1] == 0);

  const auto root = std::filesystem::temp_directory_path() /
                    L"feathercast-library-tests";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root);
  const auto path = root / L"snippets.json";

  const auto missing = feathercast::snippets_io::Load(path);
  assert(missing.status == feathercast::snippets_io::LoadStatus::Missing);
  assert(missing.fingerprint.inspected && !missing.fingerprint.exists);
  const auto saved = feathercast::snippets_io::Save(
      path, {{L"unicode", L"Grüße", L"First line\nSecond U0001F680"}},
      missing.fingerprint);
  assert(saved.succeeded);
  const auto loaded = feathercast::snippets_io::Load(path);
  assert(loaded.status == feathercast::snippets_io::LoadStatus::Valid);
  assert(loaded.snippets.size() == 1);
  assert(loaded.snippets[0].name == L"Grüße");
  assert(loaded.snippets[0].text == L"First line\nSecond U0001F680");

  const auto originalWriteTime = loaded.fingerprint.writeTime;
  const auto originalSize = loaded.fingerprint.size;
  {
    std::ofstream external(path, std::ios::binary | std::ios::trunc);
    std::string sameSize(originalSize, ' ');
    external.write(sameSize.data(), static_cast<std::streamsize>(sameSize.size()));
  }
  std::filesystem::last_write_time(path, originalWriteTime, ec);
  assert(!ec);
  const auto conflict = feathercast::snippets_io::Save(
      path, loaded.snippets, loaded.fingerprint);
  assert(!conflict.succeeded);

  {
    std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
    invalid << "{\"snippets\":[{\"keyword\":\"bad\"}]}";
  }
  const auto invalid = feathercast::snippets_io::Load(path);
  assert(invalid.status == feathercast::snippets_io::LoadStatus::Invalid);
  assert(!invalid.Writable());

  std::filesystem::remove_all(root, ec);
  return 0;
}
