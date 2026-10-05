#include "capability_catalog.hpp"
#include "emoji.hpp"
#include "file_index_service.hpp"
#include "search_pipeline.hpp"
#include "settings_catalog.hpp"
#include "snapshot_memory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace feathercast;

std::vector<core::SearchItem> SymbolItems(
    const std::vector<symbols::Symbol>& catalog, bool emoji) {
  std::vector<core::SearchItem> items;
  for (std::size_t i = 0; i < catalog.size(); ++i) {
    core::SearchItem item;
    item.id = std::to_wstring(i);
    item.kind = item.source = emoji ? L"emoji" : L"symbol";
    item.name = catalog[i].label;
    item.keywords = catalog[i].keywords;
    if (!emoji) item.keywords.push_back(catalog[i].value);
    items.push_back(std::move(item));
  }
  return items;
}

void CheckSymbols(bool emoji) {
  const auto catalog = emoji ? emoji::AllEmoji() : symbols::AllSymbols();
  const auto items = SymbolItems(catalog, emoji);
  for (const auto* query : {L"", L"   ", L"smile", L"flag ger", L"arrow",
                            L"heart", L"aple", L"pi", L"\u03c0", L"^"}) {
    const auto all = core::Search(query, items);
    for (const std::size_t limit : {0u, 1u, 5u, 32u, 3000u}) {
      const auto actual = emoji ? emoji::SearchEmoji(query, limit)
                                : symbols::SearchSymbols(L":" + std::wstring(query), limit);
      assert(actual.size() == std::min(all.size(), limit));
      for (std::size_t i = 0; i < actual.size(); ++i) {
        assert(actual[i].value == catalog[all[i]].value);
        assert(actual[i].label == catalog[all[i]].label);
      }
    }
  }
  // Low-memory cleanup must release the prepared corpus as well as its data,
  // and a later query must rebuild it without changing results.
  const auto before = emoji ? emoji::SearchEmoji(L"heart", 5)
                            : symbols::SearchSymbols(L":arrow", 5);
  if (emoji) {
    emoji::FreeEmojiMemory();
    assert(!emoji::g_EmojiList && !emoji::g_EmojiSearchItems);
  } else {
    symbols::FreeSymbolsMemory();
    assert(!symbols::g_SymbolsSearchItems);
  }
  const auto after = emoji ? emoji::SearchEmoji(L"heart", 5)
                           : symbols::SearchSymbols(L":arrow", 5);
  assert(before.size() == after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    assert(before[i].value == after[i].value);
  }
}

void CheckCatalogs() {
  std::vector<core::SearchItem> settingsItems;
  for (const auto& descriptor : settings_catalog::Catalog()) {
    core::SearchItem item;
    item.id = descriptor.stableId;
    item.name = descriptor.label;
    item.keywords = {std::wstring(descriptor.description),
                     std::wstring(descriptor.accessibleName),
                     std::wstring(descriptor.stableId)};
    if (const auto* category = settings_catalog::FindCategory(descriptor.category)) {
      item.keywords.push_back(std::wstring(category->label));
      item.keywords.push_back(std::wstring(category->accessibleName));
    }
    settingsItems.push_back(std::move(item));
  }
  assert(settings_catalog::Search(L"   ").empty());
  for (const auto* query : {L"animation", L"phone", L"privacy", L"retention",
                            L"shortcut", L"increse", L"results", L"é", L"."}) {
    const auto expected = core::Search(query, settingsItems);
    const auto actual = settings_catalog::Search(query);
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      assert(actual[i] == &settings_catalog::Catalog()[expected[i]]);
    }
  }
  std::vector<core::SearchItem> capabilityItems;
  for (const auto& descriptor : capabilities::Catalog()) {
    core::SearchItem item;
    item.id = descriptor.stableId;
    item.name = descriptor.title;
    item.source = descriptor.category;
    item.keywords = descriptor.keywords;
    item.keywords.push_back(descriptor.summary);
    item.keywords.push_back(descriptor.example);
    capabilityItems.push_back(std::move(item));
  }
  for (const auto* query : {L"", L"   ", L"files", L"screen", L"phone",
                            L"shortcut", L"clipboard", L"snipet", L"é", L"."}) {
    const auto expected = core::Search(query, capabilityItems);
    const auto actual = capabilities::Search(query);
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      assert(actual[i] == &capabilities::Catalog()[expected[i]]);
    }
  }

  core::SearchItem nameOnly;
  nameOnly.name = L"Application";
  const auto compact = core::PrepareSearchItem(nameOnly);
  assert(compact.fields.size() == 1 && compact.fields.capacity() == 1);
  auto previous = compact;
  previous.fields.push_back(core::PrepareField(L"", 0.82, core::SearchFieldKind::Keywords));
  previous.fields.push_back(core::PrepareField(L"", 0.7, core::SearchFieldKind::Process));
  previous.fields.push_back(core::PrepareField(L"", 0.45, core::SearchFieldKind::Path));
  for (const auto* query : {L"application", L"app", L"aplication", L"alp", L"."}) {
    const auto normalized = core::Normalize(query);
    const auto tokens = core::TokensNormalized(normalized);
    assert(core::ScorePreparedItem(normalized, tokens, compact, {}) ==
           core::ScorePreparedItem(normalized, tokens, previous, {}));
  }
  auto aliases = nameOnly;
  aliases.aliases = {L"launch", L"work"};
  assert(search::SearchItemBytes(aliases) > search::SearchItemBytes(nameOnly));
}

// The reference deliberately uses the uncached, fully sorted search API.
// It checks the selected payloads as well as keys: duplicate keys can have
// different names, scores and paths, and must retain the highest ranked row.
void CheckRoot(std::size_t count, bool aliases, bool exact, bool preferred,
                bool noApps, bool expanded, std::size_t workers) {
  auto snapshot = std::make_shared<app::SearchSnapshot>();
  std::vector<core::SearchItem> items;
  std::vector<std::wstring> buckets;
  for (std::size_t i = 0; i < count; ++i) {
    app::DisplayItem display;
    const auto name = exact && i == 15 ? L"Application"
        : L"Application " + std::to_wstring(i % 29);
    const auto id = std::to_wstring((i / 7) / 4);
    std::wstring bucket;
    switch (i % 7) {
      case 0: case 1:
        display.app.source = noApps ? L"file" : L"start-menu";
        display.app.isGame = i % 7 == 1;
        bucket = noApps ? L"Files & Folders" : i % 7 == 1 ? L"Games" : L"Apps";
        break;
      case 2: display.app.source = L"file"; bucket = L"Files & Folders"; break;
      case 3:
        display.isWindow = true;
        display.window.hwnd = reinterpret_cast<HWND>(i / 28 + 1);
        display.window.name = name;
        bucket = L"Open windows";
        break;
      case 4:
        display.isSnippet = true;
        display.snippet.keyword = id;
        display.snippet.name = name;
        bucket = L"Snippets";
        break;
      case 5:
        display.app.source = L"windows-settings";
        bucket = L"Windows Settings";
        break;
      default:
        display.app.source = L"system-folder";
        bucket = L"System Folders";
        break;
    }
    display.app.id = L"fixture:" + std::to_wstring(i % 7) + L":" + id;
    display.app.name = name;
    display.app.path = L"C:\\Fixture\\" + std::to_wstring(i);
    core::SearchItem item;
    item.id = display.Key();
    item.name = name;
    item.usageCount = static_cast<int>(i % 101);
    item.pinned = i % 43 == 0;
    if (aliases && i < 8) item.aliases = {L"application"};
    items.push_back(item);
    snapshot->pool.push_back(std::move(display));
    snapshot->searchItems.push_back(core::PrepareSearchItem(item));
    buckets.push_back(std::move(bucket));
  }
  app::QueryRequest request;
  request.snapshot = snapshot;
  request.query = L"application";
  request.limit = 100;
  request.maxWorkers = workers;
  if (preferred) request.preferredInvocationKey = snapshot->pool.back().InvocationKey();
  if (expanded) request.expandedSections = {L"Apps", L"Games", L"Files & Folders",
      L"Windows Settings", L"Snippets", L"System Folders", L"Open windows", L"Best match"};

  const auto order = core::Search(request.query, items);
  app::ResultsCollection expected;
  std::set<std::wstring> used;
  const auto add = [&](const std::wstring& title,
                        const std::vector<std::size_t>& indices, std::size_t limit) {
    app::Section section;
    section.title = title;
    for (const auto index : indices) {
      if (section.items.size() == limit) break;
      if (used.insert(snapshot->pool[index].Key()).second) {
        section.items.push_back(snapshot->pool[index]);
      }
    }
    if (section.items.empty()) return;
    if (!expanded && section.items.size() > 5) {
      app::DisplayItem expander;
      expander.isSectionExpander = true;
      expander.sectionTitle = title;
      expander.hiddenResultCount = section.items.size() - 5;
      section.items.resize(5);
      section.items.push_back(std::move(expander));
    }
    expected.sections.push_back(std::move(section));
  };
  std::vector<std::size_t> explicitMatches;
  if (aliases) {
    for (std::size_t i = 0; i < 8; ++i) explicitMatches.push_back(i);
    add(L"Best match", explicitMatches, SIZE_MAX);
  } else {
    for (const auto index : order) {
      if (items[index].name == L"Application") { explicitMatches = {index}; break; }
    }
    if (explicitMatches.empty() && preferred) {
      for (const auto index : order) {
        if (snapshot->pool[index].InvocationKey() == request.preferredInvocationKey) {
          explicitMatches = {index}; break;
        }
      }
    }
    if (!explicitMatches.empty()) add(L"Best match", explicitMatches, SIZE_MAX);
  }
  if (noApps && explicitMatches.empty()) add(L"Best match", {order.front()}, 1);
  for (const auto& [title, limit] : std::vector<std::pair<std::wstring, std::size_t>>{
           {L"Apps", 80}, {L"Games", 80}, {L"Windows Settings", 40},
           {L"Snippets", 20}, {L"Files & Folders", 40},
           {L"System Folders", 30}, {L"Open windows", 40}}) {
    std::vector<std::size_t> indices;
    for (const auto index : order) if (buckets[index] == title) indices.push_back(index);
    add(title, indices, limit);
  }
  const auto actual = search_pipeline::ComputeResults(request);
  assert(actual.sections.size() == expected.sections.size());
  std::size_t flat = 0;
  for (std::size_t section = 0; section < actual.sections.size(); ++section) {
    const auto& a = actual.sections[section];
    const auto& b = expected.sections[section];
    assert(a.title == b.title && a.items.size() == b.items.size());
    for (std::size_t i = 0; i < a.items.size(); ++i) {
      assert(a.items[i].Key() == b.items[i].Key());
      assert(a.items[i].Name() == b.items[i].Name());
      assert(a.items[i].app.path == b.items[i].app.path);
      assert(a.items[i].hiddenResultCount == b.items[i].hiddenResultCount);
      assert(actual.flatItems[flat++].Key() == a.items[i].Key());
    }
  }
  assert(flat == actual.flatItems.size());
}

bool ReferenceSegment(std::wstring_view pattern, std::wstring_view value) {
  std::vector<std::vector<bool>> dp(pattern.size() + 1,
                                    std::vector<bool>(value.size() + 1));
  dp[0][0] = true;
  for (std::size_t p = 1; p <= pattern.size(); ++p) {
    if (pattern[p - 1] == L'*') dp[p][0] = dp[p - 1][0];
    for (std::size_t v = 1; v <= value.size(); ++v) {
      dp[p][v] = pattern[p - 1] == L'*' ? dp[p - 1][v] || dp[p][v - 1]
          : dp[p - 1][v - 1] && pattern[p - 1] == value[v - 1];
    }
  }
  return dp.back().back();
}

void CheckExclusions() {
  std::vector<std::wstring> patterns = {L""};
  std::vector<std::wstring> values = {L"a", L"b", L"?", L"aa", L"ab", L"ba", L"bb", L"aba", L"ababa"};
  std::vector<std::wstring> level = {L""};
  for (int depth = 0; depth < 5; ++depth) {
    std::vector<std::wstring> next;
    for (const auto& prefix : level) {
      for (const auto ch : {L'a', L'b', L'*', L'?'}) {
        next.push_back(prefix + ch);
        patterns.push_back(next.back());
      }
    }
    level = std::move(next);
  }
  for (const auto& pattern : patterns) {
    for (const auto& value : values) {
      assert(files::MatchesRelativePathExclusion(value, {pattern}) ==
             (!pattern.empty() && ReferenceSegment(pattern, value)));
    }
  }
  assert(files::MatchesRelativePathExclusion(L"A/b/cache/entry.tmp", {L"**/CACHE/*.tmp"}));
  assert(files::MatchesRelativePathExclusion(L"cache/entry.tmp", {L"**/cache/*.tmp"}));
  assert(!files::MatchesRelativePathExclusion(L"cache/deep/entry.tmp", {L"**/cache/*.tmp"}));
  assert(files::MatchesRelativePathExclusion(L"cache/deep/entry.tmp", {L"cache/**/*.tmp"}));
  assert(!files::MatchesRelativePathExclusion(L"A/b/entry.txt", {L"**/*.tmp"}));
  assert(!files::MatchesRelativePathExclusion(L"A/b/entry.txt", {}));
}

template <typename Search>
double WarmP95(Search search) {
  const std::vector<std::wstring> queries = {L"smile", L"heart", L"flag ger", L"aple"};
  for (const auto& query : queries) assert(!search(query).empty());
  std::vector<double> times;
  for (int i = 0; i < 24; ++i) {
    const auto start = std::chrono::steady_clock::now();
    assert(!search(queries[static_cast<std::size_t>(i) % queries.size()]).empty());
    times.push_back(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count());
  }
  std::sort(times.begin(), times.end());
  return times[times.size() * 95 / 100];
}

}  // namespace

int main() {
  CheckSymbols(false);
  CheckSymbols(true);
  CheckCatalogs();
  for (const bool expanded : {false, true}) {
    CheckRoot(1400, false, false, false, false, expanded, 1);
    CheckRoot(1400, true, true, true, false, expanded, 1);
    CheckRoot(1400, false, true, true, false, expanded, 1);
    CheckRoot(1400, false, false, true, false, expanded, 1);
    CheckRoot(1400, false, false, false, true, expanded, 1);
  }
  CheckRoot(5000, false, false, false, false, false, 2);
  CheckRoot(21000, false, false, false, false, false, 2);
  CheckExclusions();
  const auto& catalog = emoji::AllEmoji();
  const auto items = SymbolItems(catalog, true);
  const auto uncached = WarmP95([&](const std::wstring& query) {
    auto matches = core::Search(query, items);
    if (matches.size() > 300) matches.resize(300);
    std::vector<symbols::Symbol> output;
    for (const auto index : matches) output.push_back(catalog[index]);
    return output;
  });
  const auto cached = WarmP95([](const std::wstring& query) {
    return emoji::SearchEmoji(query, 300);
  });
  std::cout << "emoji_warm_uncached_p95_ms=" << uncached << '\n'
            << "emoji_warm_prepared_p95_ms=" << cached << '\n';
  emoji::FreeEmojiMemory();
  symbols::FreeSymbolsMemory();
  return 0;
}
