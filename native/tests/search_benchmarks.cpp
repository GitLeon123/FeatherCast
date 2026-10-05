#include "core.hpp"
#include "search_pipeline.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double P95(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[std::min(samples.size() - 1, (samples.size() * 95) / 100)];
}

double RunCorpus(size_t count, int iterations,
                 const std::vector<std::wstring>& queries,
                 size_t limit = 100) {
  std::vector<feathercast::core::PreparedSearchItem> items;
  items.reserve(count);
  const auto prepareStart = Clock::now();
  for (size_t i = 0; i < count; ++i) {
    feathercast::core::SearchItem item;
    item.id = L"app:" + std::to_wstring(i);
    item.name = L"Application " + std::to_wstring(i) + (i % 7 == 0 ? L" Visual Studio Code" : L"");
    item.processName = L"process" + std::to_wstring(i % 113);
    item.targetPath = L"C:\\Programs\\Application" + std::to_wstring(i) + L"\\app.exe";
    item.keywords = {L"application", L"tool", i % 7 == 0 ? L"code editor" : L"utility"};
    item.usageCount = static_cast<int>(i % 100);
    items.push_back(feathercast::core::PrepareSearchItem(item));
  }
  if (limit == 100 && queries.front() == L"visual code") {
    std::cout << "search_prepare_" << count << "_ms="
              << Milliseconds(prepareStart, Clock::now()) << "\n";
  }

  feathercast::core::SearchOptions options;
  options.limit = limit;
  options.now = 1750000000;
  for (const auto& query : queries) {
    if (feathercast::core::SearchPrepared(query, items, {}, options).empty()) {
      return -1.0;
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto start = Clock::now();
    const auto results = feathercast::core::SearchPrepared(
        queries[static_cast<size_t>(i) % queries.size()], items, {}, options);
    const auto end = Clock::now();
    if (results.empty()) return -1.0;
    samples.push_back(Milliseconds(start, end));
  }
  return P95(std::move(samples));
}

std::uint64_t ResultChecksum(
    const feathercast::app::ResultsCollection& result) {
  std::uint64_t checksum = 14695981039346656037ULL;
  const auto append = [&](const std::wstring& text) {
    for (const wchar_t ch : text) {
      checksum ^= static_cast<std::uint16_t>(ch);
      checksum *= 1099511628211ULL;
    }
    checksum ^= 0xffff;
    checksum *= 1099511628211ULL;
  };
  for (const auto& section : result.sections) {
    append(section.title);
    for (const auto& item : section.items) {
      append(item.Key());
      append(std::to_wstring(item.hiddenResultCount));
    }
  }
  return checksum;
}

double RunPipeline(size_t count, int iterations,
                   feathercast::search_scope::Scope scope, bool expanded) {
  using namespace feathercast;
  auto snapshot = std::make_shared<app::SearchSnapshot>();
  snapshot->pool.reserve(count);
  snapshot->searchItems.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    app::DisplayItem display;
    display.app.id = L"item:" + std::to_wstring(i);
    display.app.name = L"Application " + std::to_wstring(i);
    display.app.path = L"C:\\Fixture\\Application" + std::to_wstring(i) + L".exe";
    display.app.source = i % 50 == 0 ? L"start-menu" : L"file";
    display.app.isGame = i % 100 == 0;
    core::SearchItem item;
    item.id = display.app.id;
    item.name = display.app.name;
    item.targetPath = display.app.path;
    item.keywords = {L"application", L"tool"};
    item.usageCount = static_cast<int>(i % 100);
    snapshot->pool.push_back(std::move(display));
    snapshot->searchItems.push_back(core::PrepareSearchItem(item));
  }
  app::QueryRequest request;
  request.snapshot = snapshot;
  request.limit = 100;
  request.maxWorkers = 2;
  request.now = 1750000000;
  request.query = L"application";
  request.scope = scope;
  if (expanded) {
    request.expandedSections = {L"Apps", L"Games", L"Files & Folders"};
  }
  const auto expected = search_pipeline::ComputeResults(request);
  if (expected.flatItems.empty()) return -1.0;
  const auto checksum = ResultChecksum(expected);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto start = Clock::now();
    const auto result = search_pipeline::ComputeResults(request);
    const auto end = Clock::now();
    assert(ResultChecksum(result) == checksum);
    samples.push_back(Milliseconds(start, end));
  }
  std::cout << "search_pipeline_" << count << "_"
            << (scope == search_scope::Scope::All ? "root" : "apps")
            << (expanded ? "_expanded" : "_collapsed")
            << "_checksum=" << checksum << "\n";
  return P95(std::move(samples));
}

}  // namespace

int main() {
  const std::vector<std::wstring> normalQueries = {
      L"visual code", L"application 42"};
  const std::vector<std::wstring> typoQueries = {
      L"aplication", L"visual studoi"};
  const double p95At5k = RunCorpus(5000, 30, normalQueries);
  const double p95At50k = RunCorpus(50000, 20, normalQueries);
  const double typoP95At5k = RunCorpus(5000, 30, typoQueries);
  const double typoP95At50k = RunCorpus(50000, 20, typoQueries);
  const double fullP95At50k = RunCorpus(50000, 20, normalQueries, 50000);
  const double pipelineP95At5k = RunPipeline(
      5000, 20, feathercast::search_scope::Scope::All, false);
  const double pipelineP95At50k = RunPipeline(
      50000, 12, feathercast::search_scope::Scope::All, false);
  const double expandedP95At50k = RunPipeline(
      50000, 12, feathercast::search_scope::Scope::All, true);
  const double scopedP95At50k = RunPipeline(
      50000, 12, feathercast::search_scope::Scope::Apps, false);
  std::cout << "search_p95_5k_ms=" << p95At5k << "\n";
  std::cout << "search_p95_50k_ms=" << p95At50k << "\n";
  std::cout << "search_typo_p95_5k_ms=" << typoP95At5k << "\n";
  std::cout << "search_typo_p95_50k_ms=" << typoP95At50k << "\n";
  std::cout << "search_full_p95_50k_ms=" << fullP95At50k << "\n";
  std::cout << "search_pipeline_p95_5k_ms=" << pipelineP95At5k << "\n";
  std::cout << "search_pipeline_p95_50k_ms=" << pipelineP95At50k << "\n";
  std::cout << "search_pipeline_expanded_p95_50k_ms=" << expandedP95At50k << "\n";
  std::cout << "search_pipeline_apps_p95_50k_ms=" << scopedP95At50k << "\n";
  if (p95At5k < 0 || p95At50k < 0 || typoP95At5k < 0 ||
      typoP95At50k < 0 || fullP95At50k < 0 || pipelineP95At5k < 0 ||
      pipelineP95At50k < 0 || expandedP95At50k < 0 || scopedP95At50k < 0) {
    std::cerr << "search benchmark failed: a query returned no results\n";
    return 1;
  }
#ifdef NDEBUG
  const double scale = feathercast::test::TimingBudgetScale();
  const double budget5k = 10.0 * scale;
  const double budget50k = 50.0 * scale;
  const double pipelineBudget5k = 25.0 * scale;
  const double pipelineBudget50k = 100.0 * scale;
  std::cout << "search_budget_5k_ms=" << budget5k << "\n";
  std::cout << "search_budget_50k_ms=" << budget50k << "\n";
  std::cout << "search_pipeline_budget_5k_ms=" << pipelineBudget5k << "\n";
  std::cout << "search_pipeline_budget_50k_ms=" << pipelineBudget50k << "\n";
  if (p95At5k > budget5k || p95At50k > budget50k || typoP95At5k > budget5k ||
      typoP95At50k > budget50k || fullP95At50k > budget50k ||
      pipelineP95At5k > pipelineBudget5k ||
      pipelineP95At50k > pipelineBudget50k ||
      expandedP95At50k > pipelineBudget50k ||
      scopedP95At50k > pipelineBudget5k) {
    std::cerr << "search benchmark failed: p95 latency exceeded its budget\n";
    return 2;
  }
#endif
  return 0;
}
