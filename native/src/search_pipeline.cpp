#include "search_pipeline.hpp"

#include "calculator.hpp"
#include "capability_catalog.hpp"
#include "converter.hpp"
#include "core.hpp"
#include "emoji.hpp"
#include "extension_protocol.hpp"
#include "run_command.hpp"
#include "search_scope.hpp"
#include "symbols.hpp"
#include "settings_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

namespace feathercast::search_pipeline {
namespace {

using namespace app;

// Words folded exactly like the search core folds them (invariant case,
// diacritics), so a gate never rejects what the search itself matched.
std::vector<std::wstring> LowerWords(const std::wstring& text) {
  return core::Tokens(text);
}

}  // namespace

namespace {

std::wstring JoinWords(const std::vector<std::wstring>& words) {
  std::wstring joined;
  for (const auto& word : words) {
    if (!joined.empty()) joined.push_back(L' ');
    joined += word;
  }
  return joined;
}

// The two texts share a prefix covering at least three quarters of both, so
// "animations" still finds "Animation" while "notif" is not enough.
bool NearlyEqual(const std::wstring& left, const std::wstring& right) {
  const auto mismatch =
      std::mismatch(left.begin(), left.end(), right.begin(), right.end());
  const auto common = static_cast<std::size_t>(mismatch.first - left.begin());
  return common * 4 >= left.size() * 3 && common * 4 >= right.size() * 3;
}

struct FeatureQuery {
  explicit FeatureQuery(const std::wstring& text)
      : words(LowerWords(text)), joined(JoinWords(words)) {}
  std::vector<std::wstring> words;
  std::wstring joined;
};

bool MatchesFeatureQuery(const FeatureQuery& query,
                         const std::vector<std::wstring>& phrases) {
  const auto& queryWords = query.words;
  const auto& joinedQuery = query.joined;
  // One or two letters match too much to be worth a feature result.
  if (joinedQuery.size() < 3) return false;
  std::vector<std::wstring> phraseWords;
  for (const auto& phrase : phrases) {
    auto words = LowerWords(phrase);
    const auto joinedPhrase = JoinWords(words);
    // The whole name, keyword, or alias is (almost) spelled out.
    if (!words.empty() &&
        (joinedPhrase.starts_with(joinedQuery) ||
         NearlyEqual(joinedQuery, joinedPhrase))) return true;
    phraseWords.insert(phraseWords.end(), words.begin(), words.end());
  }
  // Several words that each (almost) spell out a word of the feature, such as
  // "increase volume" or "adjust output".
  return queryWords.size() > 1 &&
         std::all_of(queryWords.begin(), queryWords.end(), [&](const auto& queryWord) {
           return std::any_of(phraseWords.begin(), phraseWords.end(),
                              [&](const auto& word) { return NearlyEqual(queryWord, word); });
         });
}

bool MatchesPhoneSuggestion(const FeatureQuery& query,
                            const app::DisplayItem& item) {
  std::vector<std::wstring> phrases = item.commandKeywords;
  phrases.push_back(item.commandName);
  return MatchesFeatureQuery(query, phrases);
}

}  // namespace

bool MatchesFeatureQuery(const std::wstring& query,
                         const std::vector<std::wstring>& phrases) {
  return MatchesFeatureQuery(FeatureQuery(query), phrases);
}

bool MatchesPhoneSuggestion(const std::wstring& query,
                            const app::DisplayItem& item) {
  return MatchesPhoneSuggestion(FeatureQuery(query), item);
}

namespace {

DisplayItem CalculatorDisplay(const calculator::Result& calculation) {
  DisplayItem item;
  item.isCalculator = true;
  item.calculationExpression = calculation.expression;
  item.calculationResult = calculation.display;
  item.commandDetail = L"Calculator result - " + calculation.expression;
  item.commandKeywords = {L"calculator", L"calc", calculation.expression,
                          calculation.display};
  return item;
}

DisplayItem ConversionDisplay(const converter::Result& conversion) {
  DisplayItem item;
  item.isConversion = true;
  item.calculationExpression = conversion.expression;
  item.calculationResult = conversion.display;
  item.commandDetail = L"Conversion - " + conversion.expression;
  item.commandKeywords = {L"convert", L"conversion", conversion.expression,
                          conversion.display};
  return item;
}

std::string UrlEncode(const std::wstring& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::string utf8 = extensions::WideToUtf8(text);
  std::string out;
  for (const unsigned char ch : utf8) {
    if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else if (ch == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[ch >> 4]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

DisplayItem WebSearchDisplay(const std::wstring& keyword,
                             const std::wstring& engineTemplate,
                             const std::wstring& terms) {
  std::wstring url = engineTemplate;
  const std::wstring encoded = extensions::Utf8ToWide(UrlEncode(terms));
  if (const std::size_t position = url.find(L"%s");
      position != std::wstring::npos) {
    url.replace(position, 2, encoded);
  } else {
    url += encoded;
  }
  DisplayItem item;
  item.isWebSearch = true;
  item.webSearchUrl = url;
  item.webSearchLabel = L"Search " + keyword + L" for \"" + terms + L"\"";
  item.commandDetail = url;
  item.commandKeywords = {L"web", L"search", keyword, terms};
  return item;
}

DisplayItem RunCommandDisplay(const run_command::Command& command) {
  DisplayItem item;
  item.isRunCommand = true;
  item.runCommand = command;
  item.commandDetail = command.detail;
  item.commandKeywords = {L"run", L"command", L"shell", L"url",
                          command.input, command.target};
  return item;
}

DisplayItem SymbolDisplay(const symbols::Symbol& symbol) {
  DisplayItem item;
  item.isSymbol = true;
  item.symbol = symbol;
  item.commandDetail = L"Symbol - " + symbol.value;
  item.commandKeywords = symbol.keywords;
  item.commandKeywords.push_back(symbol.label);
  item.commandKeywords.push_back(symbol.value);
  return item;
}

DisplayItem UtilityDisplay(const clock_utilities::Result& utility,
                           UtilityKind kind) {
  DisplayItem item;
  item.utility = UtilityResult{kind, utility.stableId, utility.title,
                               utility.value, utility.keywords};
  item.commandDetail = L"Local utility - " + utility.value;
  item.commandKeywords = utility.keywords;
  item.commandKeywords.push_back(utility.value);
  return item;
}

UtilityKind UtilityKindFor(const std::wstring& stableId) {
  if (stableId == L"local-date") return UtilityKind::LocalDate;
  if (stableId == L"iso-week") return UtilityKind::IsoWeek;
  if (stableId == L"unix-time") return UtilityKind::UnixTime;
  return UtilityKind::LocalTime;
}

bool IsPlainApp(const DisplayItem& item) {
  return !item.isWindow && !item.isCommand && !item.isAction &&
         !item.isExtension && !item.isSnippet && !item.isClipboard &&
         !item.isCalculator && !item.isConversion && !item.isWebSearch &&
         !item.isRunCommand && !item.isSymbol && !item.isCapability &&
         !item.utility;
}

bool IsLaunchableApp(const DisplayItem& item) {
  return IsPlainApp(item) && item.app.source != L"file" &&
         item.app.source != L"system-folder" &&
         item.app.source != L"windows-settings";
}

bool MatchesScope(const DisplayItem& item, search_scope::Scope scope) {
  const bool plainApp = IsPlainApp(item);
  switch (scope) {
    case search_scope::Scope::All: return true;
    case search_scope::Scope::Apps: return IsLaunchableApp(item);
    case search_scope::Scope::Games:
      return plainApp && item.app.isGame;
    case search_scope::Scope::Windows: return item.isWindow;
    case search_scope::Scope::Files:
      return plainApp && item.app.source == L"file";
    case search_scope::Scope::Commands: return item.isCommand;
    case search_scope::Scope::Clipboard: return item.isClipboard;
    case search_scope::Scope::Snippets: return item.isSnippet;
    case search_scope::Scope::Settings:
      return plainApp && item.app.source == L"windows-settings";
  }
  return false;
}

std::wstring ScopeTitle(search_scope::Scope scope, bool empty) {
  using search_scope::Scope;
  switch (scope) {
    case Scope::Apps: return L"Apps";
    case Scope::Games: return L"Games";
    case Scope::Windows: return L"Open windows";
    case Scope::Files: return empty ? L"Recently modified" : L"Names & paths";
    case Scope::Commands: return L"Commands";
    case Scope::Clipboard: return L"Clipboard History";
    case Scope::Snippets: return L"Snippets";
    case Scope::Settings: return L"Windows Settings";
    case Scope::All: return L"Results";
  }
  return L"Results";
}

DisplayItem ScopeSuggestion(const search_scope::Descriptor& descriptor) {
  DisplayItem item;
  item.isCapability = true;
  item.capability.stableId = L"scope:" + std::wstring(descriptor.token);
  item.capability.category = L"SEARCH SCOPES";
  item.capability.title = std::wstring(descriptor.token) + L" — " +
                          std::wstring(descriptor.label);
  item.capability.summary = std::wstring(descriptor.detail);
  item.capability.action.kind = CapabilityActionKind::SeedQuery;
  item.capability.action.query = std::wstring(descriptor.token) + L" ";
  return item;
}

}  // namespace

app::ResultsCollection ComputeResults(const app::QueryRequest& request) {
  using namespace app;
  constexpr std::size_t kCollapsedSectionLimit = 5;
  ResultsCollection result;
  result.generation = request.generation;
  const auto cancelled = [&] {
    return request.latestGeneration &&
           request.latestGeneration->load(std::memory_order_acquire) !=
               request.generation;
  };
  if (cancelled()) return result;

  const SearchSnapshot emptySnapshot;
  const SearchSnapshot* snapshot =
      request.snapshot ? request.snapshot.get() : &emptySnapshot;
  std::vector<Section> sections;
  std::set<std::wstring> used;
  auto appendUnique = [&](const DisplayItem& item,
                          std::vector<DisplayItem>& output) {
    const auto key = item.Key();
    if (key.empty() || !used.insert(key).second) return;
    output.push_back(item);
  };
  auto take = [&](const std::vector<DisplayItem>& items,
                  std::size_t limit = SIZE_MAX) {
    std::vector<DisplayItem> output;
    output.reserve(std::min(items.size(), limit));
    for (const auto& item : items) {
      if (output.size() >= limit) break;
      appendUnique(item, output);
    }
    return output;
  };
  // Keep corpus matches as indices until a section selects its visible items.
  // DisplayItem owns many strings and feature payloads; copying every match
  // into a temporary list and then into a category wastes work on broad queries.
  auto takeMatches = [&](const std::vector<std::size_t>& indices,
                         std::size_t limit = SIZE_MAX) {
    std::vector<DisplayItem> output;
    output.reserve(std::min(indices.size(), limit));
    for (const auto index : indices) {
      if (output.size() >= limit) break;
      appendUnique(snapshot->pool[index], output);
    }
    return output;
  };
  auto addSection = [&](std::wstring title, std::vector<DisplayItem> items) {
    if (!items.empty()) {
      sections.push_back({std::move(title), std::move(items)});
    }
  };

  if (request.compactClear) {
    // Compact mode deliberately renders no results.
  } else if (request.browseView == BrowseView::Timers) {
    if (request.empty) {
      addSection(L"Timers & Stopwatch", take(request.timerItems));
    } else {
      // Match the label and state (see timers::Items), not the countdown.
      std::vector<core::SearchItem> searchItems;
      searchItems.reserve(request.timerItems.size());
      for (const auto& item : request.timerItems) {
        core::SearchItem searchItem;
        searchItem.id = item.Key();
        searchItem.kind = L"timer";
        if (item.commandKeywords.empty()) {
          searchItem.name = item.Name();
        } else {
          searchItem.name = item.commandKeywords.front();
          searchItem.keywords.assign(item.commandKeywords.begin() + 1,
                                     item.commandKeywords.end());
        }
        searchItems.push_back(std::move(searchItem));
      }
      std::vector<DisplayItem> hits;
      for (const auto index : core::Search(request.query, searchItems)) {
        hits.push_back(request.timerItems[index]);
      }
      addSection(L"Timers & Stopwatch", take(hits));
    }
  } else if (IsPhoneBrowseView(request.browseView)) {
    if (request.empty || IsPhoneDraftView(request.browseView)) {
      addSection(request.phoneSectionTitle, take(request.phoneItems));
    } else {
      std::vector<core::SearchItem> searchItems;
      searchItems.reserve(request.phoneItems.size());
      for (const auto& item : request.phoneItems) {
        core::SearchItem searchItem;
        searchItem.id = item.Key();
        searchItem.kind = L"phone";
        searchItem.source = L"phone";
        searchItem.name = item.phone.title;
        searchItem.keywords = {item.phone.text, item.phone.appName};
        searchItems.push_back(std::move(searchItem));
      }
      const auto order = core::Search(request.query, searchItems);
      std::vector<DisplayItem> hits;
      hits.reserve(order.size());
      for (const auto index : order) hits.push_back(request.phoneItems[index]);
      addSection(request.phoneSectionTitle, take(hits));
    }
  } else if (request.browseView == BrowseView::Clipboard) {
    if (request.empty) {
      addSection(L"Clipboard History", take(snapshot->clipboardItems));
    } else {
      const auto order = core::Search(request.query,
                                      snapshot->clipboardSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(snapshot->clipboardItems[index]);
      std::stable_partition(hits.begin(), hits.end(), [](const auto& item) { return item.clipboard.pinned; });
      addSection(L"Clipboard History", take(hits));
    }
  } else if (request.browseView == BrowseView::Emoji) {
    std::vector<DisplayItem> items;
    for (const auto& emoji : emoji::SearchEmoji(request.query, 300)) {
      items.push_back(SymbolDisplay(emoji));
    }
    addSection(L"Emoji", take(items));
  } else if (request.browseView == BrowseView::Games) {
    if (request.empty) {
      addSection(L"Games", take(snapshot->gameItems));
    } else {
      const auto order = core::Search(request.query, snapshot->gameSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(snapshot->gameItems[index]);
      addSection(L"Games", take(hits));
    }
  } else if (request.browseView == BrowseView::Capabilities) {
    std::map<std::wstring, std::vector<DisplayItem>> grouped;
    std::vector<std::wstring> categoryOrder;
    for (const auto* capability : capabilities::Search(request.query)) {
      if (!grouped.contains(capability->category)) {
        categoryOrder.push_back(capability->category);
      }
      grouped[capability->category].push_back(
          capabilities::Display(*capability));
    }
    for (const auto& category : categoryOrder) {
      addSection(category, take(grouped[category]));
    }
  } else if (request.actionMode) {
    if (request.empty) {
      addSection(L"Actions", take(request.actions));
    } else {
      const auto order = core::Search(request.query, request.actionSearchItems);
      std::vector<DisplayItem> hits;
      for (const auto index : order) hits.push_back(request.actions[index]);
      addSection(L"Actions", take(hits));
    }
  } else if (const auto suggestions = search_scope::Suggestions(request.query);
             !suggestions.empty()) {
    std::vector<DisplayItem> items;
    for (const auto* descriptor : suggestions) {
      items.push_back(ScopeSuggestion(*descriptor));
    }
    addSection(L"Search scopes", take(items));
  } else if (request.scope != search_scope::Scope::All) {
    std::vector<std::size_t> hits;
    if (request.empty) {
      for (std::size_t index = 0; index < snapshot->pool.size(); ++index) {
        if (MatchesScope(snapshot->pool[index], request.scope)) hits.push_back(index);
      }
      if (request.scope == search_scope::Scope::Files) {
        std::sort(hits.begin(), hits.end(), [&](const auto left, const auto right) {
          return snapshot->pool[left].app.fileLastWriteTime >
                 snapshot->pool[right].app.fileLastWriteTime;
        });
      }
    } else {
      // Filter before scoring so, for example, @apps does not fuzzy-match and
      // sort thousands of indexed files that cannot appear in this scope.
      std::vector<std::size_t> candidates;
      candidates.reserve(snapshot->pool.size());
      for (std::size_t index = 0; index < snapshot->pool.size(); ++index) {
        if ((index & 255u) == 0 && cancelled()) return result;
        if (MatchesScope(snapshot->pool[index], request.scope)) {
          candidates.push_back(index);
        }
      }
      core::SearchOptions options;
      options.limit = request.limit > 0 ? static_cast<std::size_t>(request.limit) : 0;
      options.candidateIndices = &candidates;
      options.maxWorkers = request.maxWorkers;
      options.now = request.now;
      options.generation = request.generation;
      options.latestGeneration = request.latestGeneration;
      hits = core::SearchPrepared(request.query, snapshot->searchItems,
                                 request.recentIds, options);
    }
    if (hits.empty() && request.offerFileRecovery &&
        request.scope == search_scope::Scope::Files) {
      DisplayItem recovery;
      recovery.settingId = request.fileIndexLimitReached
          ? L"privacy.file-limit.up" : L"privacy.file-index";
      recovery.commandName = !request.fileIndexingEnabled ? L"Set Up File Search"
          : request.fileIndexLimitReached ? L"Review File Index Limit"
                                         : L"Review Indexed Folders";
      recovery.commandDetail = !request.fileIndexingEnabled
          ? L"Choose folders and enable local indexing in Privacy settings"
          : request.fileIndexLimitReached
              ? L"The index holds the newest entries; older files may be outside its limit"
              : L"Search covers selected local folders; review coverage in Privacy settings";
      addSection(L"File Search", take({recovery}, 1));
    } else {
      addSection(ScopeTitle(request.scope, request.empty), takeMatches(hits));
    }
  } else if (request.empty) {
    addSection(L"Incoming Call", take(request.phoneCallItems));
    // pinned/recent are intentionally generic DisplayItem buckets. The
    // snapshot builder resolves stable invocation keys and supplies them in
    // deterministic settings order; take() removes overlap between favorites,
    // recents, and the type-specific sections that follow.
    addSection(L"Favorites", take(snapshot->pinned, 5));
    addSection(L"Recent", take(snapshot->recent, 5));
    std::vector<DisplayItem> appSuggestions;
    for (const auto& item : snapshot->appItems) {
      if (IsLaunchableApp(item)) appSuggestions.push_back(item);
    }
    addSection(L"Apps", take(appSuggestions, 5));
    addSection(L"Open windows", take(snapshot->windowItems, 5));
    addSection(L"More tools",
               take({capabilities::EmptyStateDisplay(
                         capabilities::EmptyStateAction::MoreTools)},
                    1));
  } else {
    addSection(L"Incoming Call", take(request.phoneCallItems));
    const std::wstring trimmed = core::Trim(request.query);
    if (trimmed.starts_with(L">")) {
      if (const auto command = run_command::Classify(trimmed)) {
        addSection(command->kind == run_command::Kind::OpenTarget ? L"Open"
                                                                  : L"Run",
                   take({RunCommandDisplay(*command)}, 1));
      }
    } else if (trimmed.starts_with(L":")) {
      std::vector<DisplayItem> items;
      for (const auto& symbol : symbols::SearchSymbols(trimmed, 40)) {
        items.push_back(SymbolDisplay(symbol));
      }
      addSection(L"Symbols", take(items, 40));
    } else {
      // Explicit invocation remains first even when the alias resembles math,
      // a timer or a web-search keyword. Other sections still offer alternatives.
      const auto explicitQuery = core::Normalize(trimmed);
      const FeatureQuery featureQuery(trimmed);
      std::vector<DisplayItem> aliasInvocations;
      for (std::size_t index = 0; index < snapshot->searchItems.size(); ++index) {
        if ((index & 255u) == 0 && cancelled()) return result;
        const auto& aliases = snapshot->searchItems[index].normalizedAliases;
        if (!snapshot->pool[index].isClipboard &&
            std::find(aliases.begin(), aliases.end(), explicitQuery) != aliases.end()) {
          aliasInvocations.push_back(snapshot->pool[index]);
        }
      }
      addSection(L"Best match", take(aliasInvocations));
      if (!trimmed.empty()) {
        const std::size_t space = trimmed.find_first_of(L" \t");
        if (space != std::wstring::npos) {
          const std::wstring keyword = core::Lower(trimmed.substr(0, space));
          const std::wstring terms = core::Trim(trimmed.substr(space + 1));
          if (!terms.empty()) {
            if (const auto engine = request.searchEngines.find(keyword);
                engine != request.searchEngines.end()) {
              addSection(L"Web Search",
                         take({WebSearchDisplay(keyword, engine->second, terms)},
                              1));
            }
          }
        }
      }

      if (const auto calculation = calculator::TryEvaluate(request.query)) {
        addSection(L"Calculator", take({CalculatorDisplay(*calculation)}, 1));
      }
      if (const auto conversion = converter::TryConvert(
              request.query, request.currencyRates, request.defaultCurrency)) {
        addSection(L"Conversion", take({ConversionDisplay(*conversion)}, 1));
      }
      if (const auto utility =
              clock_utilities::Evaluate(request.query, request.clock)) {
        addSection(L"Utilities",
                   take({UtilityDisplay(*utility,
                                        UtilityKindFor(utility->stableId))},
                        1));
      }
      if (const auto timer = timers::Parse(request.query)) {
        DisplayItem item;
        item.timerRequest = *timer;
        item.commandName = timer->action == timers::Action::Create ? L"Start timer: " + timer->name : L"Enter a timer duration";
        item.commandDetail = timer->action == timers::Action::Create ? timers::Format(timer->duration) : L"Use positive h, m, s values. Example: timer 1h 30m Tea";
        addSection(L"Timer", take({item}));
      }

      std::vector<DisplayItem> settingMatches;
      // The catalog search is shared with the Settings window; only a
      // near-complete match of the label shows up in general search.
      for (const auto* descriptorMatch : settings_catalog::Search(request.query)) {
        const auto& descriptor = *descriptorMatch;
        if (!MatchesFeatureQuery(featureQuery,
                                 {std::wstring(descriptor.label),
                                  std::wstring(descriptor.accessibleName)})) {
          continue;
        }
        DisplayItem item;
        item.settingId = descriptor.stableId;
        item.commandName = descriptor.label;
        item.commandDetail = L"FeatherCast Settings - " + std::wstring(descriptor.description);
        settingMatches.push_back(std::move(item));
        if (settingMatches.size() == 6) break;
      }

      core::SearchOptions options;
      // Score the whole corpus before bucketing so a lower-scoring app is
      // still promoted ahead of a higher-scoring setting or command. Rank
      // only the selected prefixes of each section, rather than every match.
      options.limit = request.limit > 0 ? snapshot->pool.size() : 0;
      options.maxWorkers = request.maxWorkers;
      options.now = request.now;
      options.generation = request.generation;
      options.latestGeneration = request.latestGeneration;
      const auto matches = core::SearchPreparedMatches(
          request.query, snapshot->searchItems, request.recentIds, options);
      if (cancelled()) return result;
      const auto better = [&](std::size_t left, std::size_t right) {
        return core::BetterPreparedMatch(matches[left], matches[right],
                                         snapshot->searchItems);
      };
      const auto matchedItem = [&](std::size_t position) -> const DisplayItem& {
        return snapshot->pool[matches[position].index];
      };
      const auto matchedPrepared = [&](std::size_t position)
          -> const core::PreparedSearchItem& {
        return snapshot->searchItems[matches[position].index];
      };
      const auto best = [&](const std::vector<std::size_t>& positions) {
        return *std::min_element(positions.begin(), positions.end(), better);
      };
      const auto takeRanked = [&](std::vector<std::size_t>& positions,
                                  std::size_t limit = SIZE_MAX) {
        std::vector<DisplayItem> output;
        output.reserve(std::min(positions.size(), limit));
        std::size_t begin = 0;
        while (begin < positions.size() && output.size() < limit) {
          if (cancelled()) break;
          const auto count = std::min(positions.size() - begin,
                                      limit - output.size());
          auto first = positions.begin() + begin;
          std::partial_sort(first, first + count, positions.end(), better);
          for (std::size_t offset = 0; offset < count; ++offset) {
            appendUnique(matchedItem(positions[begin + offset]), output);
          }
          begin += count;
          // If aliases or earlier sections consumed these keys, select the
          // next ranked prefix too. A fixed top-N list would underfill the
          // section when duplicate keys appear near its head.
        }
        return output;
      };
      std::vector<std::size_t> hits;
      hits.reserve(matches.size());
      for (std::size_t position = 0; position < matches.size(); ++position) {
        if ((position & 255u) == 0 && cancelled()) return result;
        const auto index = matches[position].index;
        const auto& item = snapshot->pool[index];
        // Clipboard history stays private to its own view and scope.
        if (item.isClipboard) continue;
        // Commands and Windows settings only show up for a near-complete match.
        // The windows-settings source includes classic/advanced settings too.
        if (item.isCommand || item.app.source == L"windows-settings") {
          const auto& prepared = snapshot->searchItems[index];
          const auto& searchItem = prepared.item;
          std::vector<std::wstring> phrases = searchItem.keywords;
          phrases.push_back(searchItem.name);
          phrases.insert(phrases.end(), searchItem.aliases.begin(),
                         searchItem.aliases.end());
          const bool exactAlias =
              std::find(prepared.normalizedAliases.begin(),
                        prepared.normalizedAliases.end(), explicitQuery) !=
              prepared.normalizedAliases.end();
          if (!exactAlias && !MatchesFeatureQuery(featureQuery, phrases)) continue;
        }
        hits.push_back(position);
      }
      std::vector<std::size_t> games;
      std::vector<std::size_t> apps;
      std::vector<std::size_t> windows;
      std::vector<std::size_t> system;
      std::vector<std::size_t> commands;
      std::vector<std::size_t> snippets;
      std::vector<std::size_t> files;
      std::vector<std::size_t> systemFolders;
      std::vector<std::size_t> windowsSettings;
      std::vector<std::size_t> other;
      for (const auto index : hits) {
        const auto& item = matchedItem(index);
        if (item.isSnippet) {
          snippets.push_back(index);
        } else if (item.isCommand) {
          commands.push_back(index);
        } else if (item.isWindow) {
          windows.push_back(index);
        } else if (IsLaunchableApp(item)) {
          if (item.app.isGame) {
            games.push_back(index);
          } else {
            apps.push_back(index);
          }
        } else if (item.app.source == L"file") {
          files.push_back(index);
        } else if (item.app.source == L"system-folder") {
          systemFolders.push_back(index);
        } else if (item.app.source == L"windows-settings") {
          windowsSettings.push_back(index);
        } else if (IsPlainApp(item)) {
          system.push_back(index);
        } else {
          other.push_back(index);
        }
      }

      // Phone views are suggested near the top as soon as the query looks
      // like one ("notif", "photos", "phone"). An app whose name starts with
      // the query (e.g. the Photos app) keeps the first place.
      std::vector<DisplayItem> phoneSuggestions;
      for (const auto& item : request.phoneSuggestions) {
        if (MatchesPhoneSuggestion(featureQuery, item)) phoneSuggestions.push_back(item);
      }
      const auto& queryWords = featureQuery.words;
      const bool strongAppMatch =
          !apps.empty() && !queryWords.empty() &&
          matchedPrepared(best(apps)).normalizedName.starts_with(
              queryWords.front());

      const bool hasLaunchableApps = !apps.empty() || !games.empty();
      std::vector<std::size_t> explicitMatches;
      const auto& normalizedQuery = explicitQuery;
      for (std::size_t index = 0; index < matches.size(); ++index) {
        const auto& prepared = matchedPrepared(index);
        if (std::find(prepared.normalizedAliases.begin(),
                      prepared.normalizedAliases.end(), normalizedQuery) !=
            prepared.normalizedAliases.end() &&
            !matchedItem(index).isClipboard) {
          explicitMatches.push_back(index);
        }
      }
      if (explicitMatches.empty()) {
        for (const auto index : hits) {
          if (matchedPrepared(index).normalizedName == normalizedQuery) {
            explicitMatches.push_back(index);
          }
        }
        if (!explicitMatches.empty()) explicitMatches = {best(explicitMatches)};
      }
      if (explicitMatches.empty() && !request.preferredInvocationKey.empty()) {
        for (const auto index : hits) {
          if (matchedItem(index).InvocationKey() == request.preferredInvocationKey) {
            explicitMatches.push_back(index);
          }
        }
        if (!explicitMatches.empty()) explicitMatches = {best(explicitMatches)};
      }
      if (explicitMatches.empty() && !commands.empty()) {
        const auto command = best(commands);
        const auto& name = matchedPrepared(command).normalizedName;
        if (name == normalizedQuery ||
            (!strongAppMatch && normalizedQuery.size() >= 3 && name.starts_with(normalizedQuery))) {
          explicitMatches.push_back(command);
        }
      }
      addSection(L"Best match", takeRanked(explicitMatches));
      if (!strongAppMatch) addSection(L"Phone", take(phoneSuggestions));
      if (hasLaunchableApps) {
        addSection(L"Apps", takeRanked(apps, 80));
        addSection(L"Games", takeRanked(games, 80));
      } else if (!hits.empty() && explicitMatches.empty()) {
        addSection(L"Best match", takeMatches({matches[best(hits)].index}, 1));
      }
      if (strongAppMatch) addSection(L"Phone", take(phoneSuggestions));
      addSection(L"Windows Settings", takeRanked(windowsSettings, 40));
      addSection(L"FeatherCast Settings", take(settingMatches));
      addSection(L"Extensions", take(request.extensionItems, 20));
      addSection(L"Commands", takeRanked(commands, 20));
      addSection(L"Snippets", takeRanked(snippets, 20));
      addSection(L"Files & Folders", takeRanked(files, 40));
      addSection(L"System Folders", takeRanked(systemFolders, 30));
      addSection(L"Open windows", takeRanked(windows, 40));
      addSection(L"System & Store apps", takeRanked(system, 80));
      addSection(L"Other matches", takeRanked(other, 40));
    }
  }

  for (auto& section : sections) {
    if (!request.expandedSections.contains(section.title) &&
        section.items.size() > kCollapsedSectionLimit) {
      const std::size_t hidden = section.items.size() - kCollapsedSectionLimit;
      section.items.resize(kCollapsedSectionLimit);
      DisplayItem expander;
      expander.isSectionExpander = true;
      expander.sectionTitle = section.title;
      expander.hiddenResultCount = hidden;
      section.items.push_back(std::move(expander));
    }
    result.flatItems.insert(result.flatItems.end(), section.items.begin(),
                            section.items.end());
  }
  result.sections = std::move(sections);
  return result;
}

}  // namespace feathercast::search_pipeline
