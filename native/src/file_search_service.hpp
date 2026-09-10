#pragma once

#include "app_types.hpp"
#include "core.hpp"
#include "storage.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct sqlite3;

namespace feathercast::files {

struct FileQuery {
  unsigned long long generation = 0;
  std::wstring terms;
  int limit = 200;
  bool contentEnabled = false;
  std::size_t maxWorkers = 1;
};

struct PreparedFileIndex {
  std::uint64_t generation = 0;
  std::shared_ptr<const std::vector<app::AppEntry>> files;
};

class FileSearchService {
 public:
  using ResultSink = std::function<void(app::ResultsCollection)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;
  using ProjectionSink = std::function<void(PreparedFileIndex)>;

  FileSearchService(std::filesystem::path databasePath, ResultSink sink = {},
                    ErrorSink errors = {}, ProjectionSink projection = {});
  ~FileSearchService();
  FileSearchService(const FileSearchService&) = delete;
  FileSearchService& operator=(const FileSearchService&) = delete;

  void Start();
  void Stop();
  void UpdateFiles(std::vector<app::AppEntry> files);
  // Publishes an immutable corpus from the service worker. The UI only
  // transfers ownership of the source vector and never sorts/prepares tens of
  // thousands of entries on its message thread.
  bool UpdateFilesAsync(std::vector<app::AppEntry> files,
                        std::uint64_t generation = 0);
  bool UpdateStorageFilesAsync(
      std::vector<storage::FileIndexEntry> entries,
      std::uint64_t generation);
  bool Query(FileQuery query);
  void Invalidate(unsigned long long generation);

 private:
  struct Corpus;
  static std::shared_ptr<Corpus> BuildCorpus(std::vector<app::AppEntry> files,
                                             std::uint64_t generation);
  void WorkerLoop(std::stop_token token);
  app::ResultsCollection Compute(const FileQuery& query);
  bool EnsureDatabase();
  std::vector<std::wstring> QueryContent(const std::wstring& terms,
                                         std::size_t limit);

  std::filesystem::path databasePath_;
  ResultSink sink_;
  ErrorSink errors_;
  ProjectionSink projectionSink_;
  sqlite3* database_ = nullptr;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<FileQuery> pending_;
  std::optional<std::vector<app::AppEntry>> pendingFiles_;
  std::uint64_t pendingFilesGeneration_ = 0;
  std::optional<std::vector<storage::FileIndexEntry>> pendingStorageFiles_;
  std::uint64_t pendingStorageFilesGeneration_ = 0;
  std::shared_ptr<const Corpus> corpus_;
  std::atomic<unsigned long long> generation_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::files
