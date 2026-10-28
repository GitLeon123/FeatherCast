#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <string>

namespace feathercast::search_preferences {

inline constexpr std::size_t kMaximumChoices = feathercast::settings::kMaximumLearnedQueries;

// Remembers the confirmed item for this query. At the cap, the least recently
// confirmed query makes room.
inline void Remember(app::Settings& settings, const std::wstring& query,
                     const app::DisplayItem& item) {
  if (!settings.searchLearningEnabled || item.isAction ||
      (!item.isCommand && (item.app.id.empty() || item.app.source == L"file"))) {
    return;
  }
  // Normalize turns line breaks into spaces, so check the raw query.
  if (query.find_first_of(L"\r\n") != std::wstring::npos) return;
  const auto normalized = core::Normalize(core::Trim(query));
  const auto key = item.InvocationKey();
  if (normalized.empty() || normalized.size() > 64 || key.empty() ||
      normalized.front() == L'@' || normalized.front() == L'>' ||
      normalized.front() == L':') {
    return;
  }
  const bool known = settings.learnedQueryActions.contains(normalized);
  feathercast::settings::TrimLearnedQueries(settings, known ? kMaximumChoices : kMaximumChoices - 1);
  long long newest = 0;
  for (const auto& entry : settings.learnedQueryRecency) newest = (std::max)(newest, entry.second);
  settings.learnedQueryActions[normalized] = key;
  settings.learnedQueryRecency[normalized] = newest + 1;
}

inline void Reset(app::Settings& settings, const app::DisplayItem& item) {
  const auto invocation = item.InvocationKey();
  std::erase_if(settings.learnedQueryActions, [&](const auto& choice) {
    return choice.second == invocation;
  });
  feathercast::settings::TrimLearnedQueries(settings);
  std::erase(settings.recentItems, invocation);
  settings.usageStats.erase(invocation);
  for (const auto& key : {item.app.id, item.app.path,
                         item.app.launchTarget, item.app.targetPath}) {
    if (key.empty()) continue;
    settings.usageStats.erase(key);
    std::erase(settings.recentApps, key);
  }
}

}  // namespace feathercast::search_preferences
