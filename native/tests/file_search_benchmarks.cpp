#include "app_types.hpp"
#include "file_content.hpp"
#include "file_search_service.hpp"
#include "storage.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace {

int SetupFailure(const std::filesystem::path& root, const char* reason) {
  std::cerr << "@files benchmark setup failed: " << reason << "\n";
  if (!root.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  return 2;
}

}  // namespace

int main() {
  constexpr std::size_t kDocuments = 50000;
  constexpr int kSamples = 40;
  wchar_t temporary[MAX_PATH]{};
  if (GetTempPathW(MAX_PATH, temporary) == 0) {
    return SetupFailure({}, "GetTempPathW failed");
  }
  const auto root = std::filesystem::path(temporary) /
                    (L"FeatherCastFileSearchBenchmark-" +
                     std::to_wstring(GetCurrentProcessId()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root, error);
  if (error) return SetupFailure(root, "cannot create the temporary directory");
  const auto databasePath = root / L"feathercast.db";

  std::vector<feathercast::storage::FileIndexEntry> stored;
  std::vector<feathercast::app::AppEntry> files;
  stored.reserve(kDocuments);
  files.reserve(kDocuments);
  for (std::size_t index = 0; index < kDocuments; ++index) {
    const auto path = root / (L"document-" + std::to_wstring(index) + L".txt");
    feathercast::storage::FileIndexEntry entry;
    entry.path = path.wstring();
    entry.name = path.filename().wstring();
    entry.iconKey = entry.path;
    entry.indexedAt = 1;
    entry.root = root.wstring();
    entry.contentState = static_cast<int>(
        feathercast::file_content::State::Indexed);
    entry.contentText = L"synthetic benchmark document group" +
                        std::to_wstring(index % 100);
    if (index % 100 == 42) entry.contentText += L" contentneedle";
    entry.contentBytes = static_cast<long long>(entry.contentText.size());
    stored.push_back(std::move(entry));

    feathercast::app::AppEntry app;
    app.id = L"file:" + path.wstring();
    app.name = path.filename().wstring();
    app.path = path.wstring();
    app.source = L"file";
    app.fileLastWriteTime = static_cast<long long>(kDocuments - index);
    files.push_back(std::move(app));
  }

  feathercast::storage::Storage storage;
  if (!storage.Open(databasePath) || !storage.UpdateFileIndex(stored)) {
    storage.Close();
    return SetupFailure(root, "cannot write the synthetic file index");
  }
  storage.Close();
  stored.clear();
  stored.shrink_to_fit();

  std::mutex mutex;
  std::condition_variable ready;
  unsigned long long completed = 0;
  feathercast::files::FileSearchService service(
      databasePath, [&](feathercast::app::ResultsCollection result) {
        {
          std::lock_guard lock(mutex);
          completed = result.generation;
        }
        ready.notify_one();
      });
  service.Start();
  service.UpdateFiles(std::move(files));

  std::vector<double> samples;
  samples.reserve(kSamples);
  for (int sample = -5; sample < kSamples; ++sample) {
    const auto generation = static_cast<unsigned long long>(sample + 6);
    const auto started = std::chrono::steady_clock::now();
    if (!service.Query({generation, L"contentneedle", 200, true})) {
      service.Stop();
      return SetupFailure(root, "the file search service rejected a query");
    }
    bool answered = false;
    {
      std::unique_lock lock(mutex);
      answered = ready.wait_for(lock, std::chrono::seconds(5),
                                [&] { return completed == generation; });
    }
    if (!answered) {
      service.Stop();
      return SetupFailure(root, "a query did not complete within 5 seconds");
    }
    const double milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    if (sample >= 0) samples.push_back(milliseconds);
  }
  service.Stop();

  std::sort(samples.begin(), samples.end());
  const auto p95Index = static_cast<std::size_t>(
      0.95 * static_cast<double>(samples.size() - 1));
  const double p95 = samples[p95Index];
  const double average =
      std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
  const double budget = 75.0 * feathercast::test::TimingBudgetScale();
  std::cout << "@files warm FTS 50k: avg=" << average << "ms p95=" << p95
            << "ms (budget " << budget << "ms)\n";

  std::filesystem::remove_all(root, error);
  if (p95 > budget) {
    std::cerr << "@files benchmark failed: p95 latency exceeded its budget\n";
    return 1;
  }
  return 0;
}
