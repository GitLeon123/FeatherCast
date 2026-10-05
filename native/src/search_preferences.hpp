#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <string>

namespace feathercast::search_preferences {

inline constexpr std::size_t kMaximumChoices = 256;

inline void Remember(app::Settings& settings, const std::wstring& query,
                     const app::DisplayItem& item) {
  if (!settings.searchLearningEnabled || item.isAction ||
      (!item.isCommand && (item.app.id.empty() || item.app.source == L"file"))) {
    return;
  }
  const auto normalized = core::Normalize(core::Trim(query));
  const auto key = item.InvocationKey();
  if (normalized.empty() || normalized.size() > 64 || key.empty() ||
      normalized.front() == L'@' || normalized.front() == L'>' ||
      normalized.front() == L':' || normalized.find_first_of(L"\r\n") != std::wstring::npos) {
    return;
  }
  if (!settings.learnedQueryActions.contains(normalized) &&
      settings.learnedQueryActions.size() >= kMaximumChoices) {
    settings.learnedQueryActions.erase(settings.learnedQueryActions.begin());
  }
  settings.learnedQueryActions[normalized] = key;
}

inline void Reset(app::Settings& settings, const app::DisplayItem& item) {
  const auto invocation = item.InvocationKey();
  std::erase_if(settings.learnedQueryActions, [&](const auto& choice) {
    return choice.second == invocation;
  });
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
