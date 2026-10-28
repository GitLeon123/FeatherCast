#pragma once

// Pure helpers for keyword web searches and the "Search the web" fallback.
// Nothing here opens a connection; the caller hands the final URL to the
// default browser only when the user activates the result.

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::web_search {

using EngineMap = std::map<std::wstring, std::wstring>;
using Engine = std::pair<const std::wstring, std::wstring>;

// Replaces every "%s" in the template with the already encoded terms. A
// template without a placeholder ("https://example.com/?q=") gets the terms
// appended, matching how keyword searches have always behaved.
inline std::wstring ExpandTemplate(std::wstring_view engineTemplate,
                                   std::wstring_view encodedTerms) {
  std::wstring url;
  url.reserve(engineTemplate.size() + encodedTerms.size());
  bool replaced = false;
  for (std::size_t index = 0; index < engineTemplate.size(); ++index) {
    if (engineTemplate[index] == L'%' && index + 1 < engineTemplate.size() &&
        engineTemplate[index + 1] == L's') {
      url += encodedTerms;
      replaced = true;
      ++index;
    } else {
      url.push_back(engineTemplate[index]);
    }
  }
  if (!replaced) url += encodedTerms;
  return url;
}

// Host of an http(s)-style template, lowercased, without credentials or port.
inline std::wstring TemplateHost(std::wstring_view engineTemplate) {
  const std::size_t scheme = engineTemplate.find(L"://");
  if (scheme == std::wstring_view::npos) return L"";
  std::wstring_view rest = engineTemplate.substr(scheme + 3);
  const std::size_t end = rest.find_first_of(L"/?#");
  if (end != std::wstring_view::npos) rest = rest.substr(0, end);
  if (const std::size_t at = rest.rfind(L'@'); at != std::wstring_view::npos) {
    rest = rest.substr(at + 1);
  }
  if (const std::size_t colon = rest.find(L':'); colon != std::wstring_view::npos) {
    rest = rest.substr(0, colon);
  }
  std::wstring host(rest);
  for (auto& ch : host) ch = static_cast<wchar_t>(std::towlower(ch));
  return host;
}

// A readable engine name for result labels and settings: "Google" for
// https://www.google.com/search?q=%s. Hosts without a well-known name use their
// registrable label ("docs.example.co.uk" -> "Example"); templates without a
// host fall back to the keyword.
inline std::wstring EngineName(const std::wstring& keyword,
                               std::wstring_view engineTemplate) {
  const std::wstring host = TemplateHost(engineTemplate);
  std::vector<std::wstring> labels;
  std::size_t begin = 0;
  while (begin <= host.size()) {
    const std::size_t dot = host.find(L'.', begin);
    const std::size_t end = dot == std::wstring::npos ? host.size() : dot;
    if (end > begin) labels.push_back(host.substr(begin, end - begin));
    if (dot == std::wstring::npos) break;
    begin = dot + 1;
  }
  if (labels.empty()) return keyword;

  std::wstring label = labels.front();
  if (labels.size() >= 2) {
    // "example.co.uk" and "example.com.au" register below a short
    // second-level label; everything else registers below the TLD.
    std::size_t registrable = labels.size() - 2;
    static constexpr std::wstring_view kSecondLevel[] = {
        L"ac", L"co", L"com", L"edu", L"gov", L"net", L"org"};
    if (labels.size() >= 3 && labels.back().size() == 2 &&
        std::find(std::begin(kSecondLevel), std::end(kSecondLevel),
                  labels[registrable]) != std::end(kSecondLevel)) {
      --registrable;
    }
    label = labels[registrable];
  }

  static const std::map<std::wstring, std::wstring, std::less<>> kKnown = {
      {L"amazon", L"Amazon"},       {L"baidu", L"Baidu"},
      {L"bing", L"Bing"},           {L"brave", L"Brave Search"},
      {L"duckduckgo", L"DuckDuckGo"}, {L"ecosia", L"Ecosia"},
      {L"github", L"GitHub"},       {L"google", L"Google"},
      {L"kagi", L"Kagi"},           {L"npmjs", L"npm"},
      {L"qwant", L"Qwant"},         {L"reddit", L"Reddit"},
      {L"stackoverflow", L"Stack Overflow"}, {L"startpage", L"Startpage"},
      {L"wikipedia", L"Wikipedia"}, {L"yahoo", L"Yahoo"},
      {L"yandex", L"Yandex"},       {L"youtube", L"YouTube"},
  };
  if (const auto known = kKnown.find(label); known != kKnown.end()) {
    return known->second;
  }
  if (label.empty()) return keyword;
  label.front() = static_cast<wchar_t>(std::towupper(label.front()));
  return label;
}

// The configured engine, or the first engine in keyword order when the
// configured keyword was removed. Returns nullptr only when no engine exists.
inline const Engine* ResolveEngine(const EngineMap& engines,
                                   const std::wstring& keyword) {
  if (engines.empty()) return nullptr;
  if (const auto exact = engines.find(keyword); exact != engines.end()) {
    return &*exact;
  }
  std::wstring lowered = keyword;
  for (auto& ch : lowered) ch = static_cast<wchar_t>(std::towlower(ch));
  if (const auto folded = engines.find(lowered); folded != engines.end()) {
    return &*folded;
  }
  return &*engines.begin();
}

// The keyword `step` engines away from the current one, wrapping around, for
// the settings stepper.
inline std::wstring StepEngine(const EngineMap& engines,
                               const std::wstring& current, int step) {
  const Engine* resolved = ResolveEngine(engines, current);
  if (!resolved) return current;
  const auto count = static_cast<long long>(engines.size());
  long long position = 0;
  for (const auto& engine : engines) {
    if (engine.first == resolved->first) break;
    ++position;
  }
  long long next = (position + step) % count;
  if (next < 0) next += count;
  auto target = engines.begin();
  std::advance(target, static_cast<std::ptrdiff_t>(next));
  return target->first;
}

}  // namespace feathercast::web_search
