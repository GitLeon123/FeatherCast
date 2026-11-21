#pragma once

#include "core.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::symbols {

struct Symbol {
  std::wstring value;
  std::wstring label;
  std::vector<std::wstring> keywords;
};

namespace detail {

struct RawSymbol {
  const wchar_t* value;
  const wchar_t* label;
  const wchar_t* keywords;  // space-separated
};

inline std::vector<std::wstring> SplitKeywords(std::wstring_view text,
                                               wchar_t sep = L' ') {
  std::vector<std::wstring> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find(sep, start);
    if (end == std::wstring_view::npos) {
      if (start < text.size()) out.emplace_back(text.substr(start));
      break;
    }
    if (end > start) out.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

inline constexpr RawSymbol kRawSymbols[] = {
    {L"\U0001F600", L"Grinning Face", L"face smile happy grin"},
    {L"\U0001F642", L"Slightly Smiling Face", L"face smile happy"},
    {L"\U0001F601", L"Beaming Face", L"face smile happy teeth"},
    {L"\U0001F602", L"Face With Tears of Joy", L"face laugh lol joy"},
    {L"\U0001F609", L"Winking Face", L"face wink"},
    {L"\U0001F60D", L"Smiling Face With Hearts", L"face love heart"},
    {L"\U0001F914", L"Thinking Face", L"face think hmm"},
    {L"\U0001F44D", L"Thumbs Up", L"thumb yes approve like"},
    {L"\U0001F44E", L"Thumbs Down", L"thumb no disapprove"},
    {L"\U0001F44F", L"Clapping Hands", L"clap applause hands"},
    {L"\U0001F64F", L"Folded Hands", L"please thanks pray hands"},
    {L"\U0001F525", L"Fire", L"hot flame"},
    {L"\u2705", L"Check Mark Button", L"check done success yes"},
    {L"\u274C", L"Cross Mark", L"x cross error no"},
    {L"\u26A0", L"Warning", L"alert caution warn"},
    {L"\u2139", L"Information", L"info note"},
    {L"\u2192", L"Right Arrow", L"arrow right next"},
    {L"\u2190", L"Left Arrow", L"arrow left back"},
    {L"\u2191", L"Up Arrow", L"arrow up"},
    {L"\u2193", L"Down Arrow", L"arrow down"},
    {L"\u2194", L"Left Right Arrow", L"arrow horizontal"},
    {L"\u21D2", L"Double Right Arrow", L"arrow implies right"},
    {L"\u21D4", L"Double Left Right Arrow", L"arrow iff equivalent"},
    {L"\u2022", L"Bullet", L"bullet dot list"},
    {L"\u2023", L"Triangular Bullet", L"bullet triangle list"},
    {L"\u25E6", L"White Bullet", L"bullet circle list"},
    {L"\u2605", L"Black Star", L"star favorite rating"},
    {L"\u2606", L"White Star", L"star favorite rating"},
    {L"\u2726", L"Black Four Pointed Star", L"star sparkle"},
    {L"\u00B1", L"Plus Minus", L"math plus minus"},
    {L"\u00D7", L"Multiplication Sign", L"math multiply times"},
    {L"\u00F7", L"Division Sign", L"math divide"},
    {L"\u2260", L"Not Equal", L"math neq not"},
    {L"\u2264", L"Less Than or Equal", L"math lte less"},
    {L"\u2265", L"Greater Than or Equal", L"math gte greater"},
    {L"\u2248", L"Almost Equal", L"math approx about"},
    {L"\u221E", L"Infinity", L"math infinite"},
    {L"\u221A", L"Square Root", L"math root sqrt"},
    {L"\u03C0", L"Greek Small Letter Pi", L"math pi greek"},
    {L"\u03B1", L"Greek Small Letter Alpha", L"greek alpha"},
    {L"\u03B2", L"Greek Small Letter Beta", L"greek beta"},
    {L"\u03BB", L"Greek Small Letter Lambda", L"greek lambda"},
    {L"\u00A9", L"Copyright Sign", L"legal copyright"},
    {L"\u00AE", L"Registered Sign", L"legal registered"},
    {L"\u2122", L"Trademark Sign", L"legal trademark tm"},
    {L"\u00A7", L"Section Sign", L"legal section"},
    {L"\u00B6", L"Pilcrow Sign", L"text paragraph pilcrow"},
    {L"\u2014", L"Em Dash", L"text dash punctuation"},
    {L"\u2013", L"En Dash", L"text dash punctuation"},
    {L"\u2026", L"Ellipsis", L"text dots punctuation"},
    {L"\u00B0", L"Degree Sign", L"temperature degree"},
    {L"\u20AC", L"Euro Sign", L"currency euro"},
    {L"\u00A3", L"Pound Sign", L"currency pound"},
    {L"\u00A5", L"Yen Sign", L"currency yen"},
    {L"\u20B9", L"Rupee Sign", L"currency rupee"},
    {L"\u2713", L"Check Mark", L"check tick done"},
    {L"\u2717", L"Ballot X", L"x cross no"},
};

}  // namespace detail

inline const std::vector<Symbol>& AllSymbols() {
  static const std::vector<Symbol> kSymbols = [] {
    std::vector<Symbol> list;
    list.reserve(std::size(detail::kRawSymbols));
    for (const auto& raw : detail::kRawSymbols) {
      list.push_back({raw.value, raw.label, detail::SplitKeywords(raw.keywords)});
    }
    return list;
  }();
  return kSymbols;
}

inline std::unique_ptr<std::vector<feathercast::core::PreparedSearchItem>> g_SymbolsSearchItems;
inline std::mutex g_SymbolsMutex;

inline void FreeSymbolsMemory() {
  std::lock_guard<std::mutex> lock(g_SymbolsMutex);
  g_SymbolsSearchItems.reset();
}

inline std::vector<Symbol> SearchSymbols(std::wstring query, size_t limit = 32) {
  if (limit == 0) return {};
  query = feathercast::core::Trim(std::move(query));
  if (!query.empty() && query.front() == L':') {
    query.erase(query.begin());
    query = feathercast::core::Trim(std::move(query));
  }

  const auto& all = AllSymbols();
  std::vector<Symbol> out;
  if (query.empty()) {
    for (const auto& symbol : all) {
      out.push_back(symbol);
      if (out.size() >= limit) break;
    }
    return out;
  }

  std::lock_guard<std::mutex> lock(g_SymbolsMutex);
  if (!g_SymbolsSearchItems) {
    g_SymbolsSearchItems = std::make_unique<std::vector<feathercast::core::PreparedSearchItem>>();
    g_SymbolsSearchItems->reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
      feathercast::core::SearchItem item;
      item.id = std::to_wstring(i);
      item.kind = L"symbol";
      item.source = L"symbol";
      item.name = all[i].label;
      item.keywords = all[i].keywords;
      item.keywords.push_back(all[i].value);
      g_SymbolsSearchItems->push_back(feathercast::core::PrepareSearchItem(std::move(item)));
    }
  }

  feathercast::core::SearchOptions options;
  options.limit = limit;
  const auto order = feathercast::core::SearchPrepared(query, *g_SymbolsSearchItems, {}, options);
  out.reserve(order.size());
  for (const auto index : order) {
    out.push_back(all[index]);
    if (out.size() >= limit) break;
  }
  return out;
}

}  // namespace feathercast::symbols
