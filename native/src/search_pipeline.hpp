#pragma once

#include "app_types.hpp"

namespace feathercast::search_pipeline {

app::ResultsCollection ComputeResults(const app::QueryRequest& request);

// True when every query word starts a word of the command name or keywords.
bool MatchesPhoneSuggestion(const std::wstring& query,
                            const app::DisplayItem& item);

}  // namespace feathercast::search_pipeline
