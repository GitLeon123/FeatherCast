#pragma once

#include "app_types.hpp"

namespace feathercast::search_pipeline {

app::ResultsCollection ComputeResults(const app::QueryRequest& request);

// True when the query almost spells out one phrase, or every query word almost
// spells out a word of the phrases.
bool MatchesFeatureQuery(const std::wstring& query,
                         const std::vector<std::wstring>& phrases);

// MatchesFeatureQuery over the command name and keywords.
bool MatchesPhoneSuggestion(const std::wstring& query,
                            const app::DisplayItem& item);

}  // namespace feathercast::search_pipeline
