#pragma once

#include "file_content.hpp"
#include "storage.hpp"

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace feathercast::files {

struct IndexRequest {
  std::uint64_t generation = 0;
  std::vector<std::wstring> roots;
  std::size_t limit = 5000;
  bool contentEnabled = false;
  std::vector<std::wstring> exclusionPatterns;
};

struct IndexStatus {
  std::uint64_t generation = 0;
  std::vector<storage::FileIndexEntry> entries;
  std::size_t indexedContentFiles = 0;
  long long indexedContentBytes = 0;
  std::size_t unavailableRoots = 0;
  std::vector<std::wstring> configuredRoots;
  std::vector<std::wstring> availableRoots;
  bool live = false;
  std::wstring message;
};

bool IsFixedLocalIndexRoot(const std::filesystem::path& path);
bool MatchesRelativePathExclusion(
    std::wstring_view relativePath,
    const std::vector<std::wstring>& exclusionPatterns);

std::vector<storage::FileIndexEntry> MergeFileIndexEntries(
    const std::vector<storage::FileIndexEntry>& previous,
    std::vector<storage::FileIndexEntry> scanned,
    const std::vector<std::wstring>& configuredRoots,
    const std::vector<std::wstring>& availableRoots, std::size_t limit,
    const std::vector<std::wstring>& exclusionPatterns = {});

class FileIndexService {
 public:
  using ResultSink = std::function<void(IndexStatus)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit FileIndexService(ResultSink sink = {}, ErrorSink errors = {});
  ~FileIndexService();
  FileIndexService(const FileIndexService&) = delete;
  FileIndexService& operator=(const FileIndexService&) = delete;

  void Start();
  void Stop();
  void Pause();
  // Indexing remains cancellable while the launcher is interactive. The
  // worker uses this signal to yield between filesystem operations instead of
  // competing with keyboard and pointer input at full speed.
  void SetInteractive(bool interactive);
  bool Reconfigure(IndexRequest request);
  bool Rebuild();
  bool IsCurrent(std::uint64_t generation) const;

 private:
  struct Watcher;
  void WorkerLoop(std::stop_token token);
  IndexStatus Scan(const IndexRequest& request, std::stop_token token) const;
  void RestartWatchers(const std::vector<std::wstring>& roots);
  void StopWatchers();
  void ScheduleWatchRefresh(bool restartWatchers = false);

  ResultSink sink_;
  ErrorSink errors_;
  std::jthread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<IndexRequest> request_;
  std::vector<std::unique_ptr<Watcher>> watchers_;
  std::mutex watchersMutex_;
  std::atomic<std::uint64_t> currentGeneration_ = 0;
  std::atomic<bool> interactive_ = false;
  bool stopping_ = false;
  std::atomic<bool> paused_ = false;
  bool rebuildPending_ = false;
  bool restartWatchersPending_ = false;
  std::chrono::steady_clock::time_point rebuildAfter_{};
  std::chrono::seconds retryDelay_{2};
};

}  // namespace feathercast::files
