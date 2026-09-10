#include "file_index_service.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <queue>
#include <set>
#include <string_view>

namespace feathercast::files {
namespace {

long long NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::wstring RootOf(const std::filesystem::path& path) {
  return path.root_path().wstring();
}

bool IsGeneratedDirectory(const std::filesystem::path& path) {
  std::wstring name = path.filename().wstring();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return name == L".git" || name == L".hg" || name == L".svn" ||
         name == L"node_modules" || name == L"build" ||
         name == L"build-native" || name == L"bin" || name == L"obj" ||
         name == L"target" || name == L"out" || name == L"dist" ||
         name == L".cache";
}

bool NewerEntry(const storage::FileIndexEntry& left,
                const storage::FileIndexEntry& right) {
  if (left.lastWriteTime != right.lastWriteTime) {
    return left.lastWriteTime > right.lastWriteTime;
  }
  return left.path < right.path;
}

struct OlderEntryFirst {
  bool operator()(const storage::FileIndexEntry& left,
                  const storage::FileIndexEntry& right) const {
    return NewerEntry(left, right);
  }
};

bool FixedLocalRoot(const std::filesystem::path& path) {
  const auto root = RootOf(path);
  return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_FIXED;
}

std::vector<std::wstring> NormalizedSegments(std::wstring_view value) {
  std::vector<std::wstring> segments;
  std::wstring segment;
  const auto finishSegment = [&] {
    if (segment.empty() || segment == L".") {
      segment.clear();
      return;
    }
    if (segment == L"..") {
      if (!segments.empty() && segments.back() != L"..") {
        segments.pop_back();
      } else {
        segments.push_back(std::move(segment));
      }
      segment.clear();
      return;
    }
    segments.push_back(std::move(segment));
    segment.clear();
  };
  for (const wchar_t ch : value) {
    if (ch == L'/' || ch == L'\\') {
      finishSegment();
    } else {
      segment.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
  }
  finishSegment();
  return segments;
}

bool MatchesSegment(std::wstring_view pattern, std::wstring_view value) {
  std::vector<bool> previous(value.size() + 1);
  std::vector<bool> current(value.size() + 1);
  previous[0] = true;
  for (const wchar_t patternCharacter : pattern) {
    std::fill(current.begin(), current.end(), false);
    if (patternCharacter == L'*') {
      current[0] = previous[0];
      for (std::size_t valueIndex = 1; valueIndex <= value.size();
           ++valueIndex) {
        current[valueIndex] = previous[valueIndex] || current[valueIndex - 1];
      }
    } else {
      for (std::size_t valueIndex = 1; valueIndex <= value.size();
           ++valueIndex) {
        current[valueIndex] = previous[valueIndex - 1] &&
                              patternCharacter == value[valueIndex - 1];
      }
    }
    previous.swap(current);
  }
  return previous[value.size()];
}

bool MatchesSegments(const std::vector<std::wstring>& pattern,
                     const std::vector<std::wstring>& path) {
  std::vector<std::vector<bool>> matches(
      pattern.size() + 1, std::vector<bool>(path.size() + 1));
  matches[0][0] = true;
  for (std::size_t patternIndex = 1; patternIndex <= pattern.size();
       ++patternIndex) {
    if (pattern[patternIndex - 1] == L"**") {
      matches[patternIndex][0] = matches[patternIndex - 1][0];
      for (std::size_t pathIndex = 1; pathIndex <= path.size(); ++pathIndex) {
        matches[patternIndex][pathIndex] =
            matches[patternIndex - 1][pathIndex] ||
            matches[patternIndex][pathIndex - 1];
      }
      continue;
    }
    for (std::size_t pathIndex = 1; pathIndex <= path.size(); ++pathIndex) {
      matches[patternIndex][pathIndex] =
          matches[patternIndex - 1][pathIndex - 1] &&
          MatchesSegment(pattern[patternIndex - 1], path[pathIndex - 1]);
    }
  }
  return matches[pattern.size()][path.size()];
}

class RelativePathExclusionMatcher {
 public:
  explicit RelativePathExclusionMatcher(
      const std::vector<std::wstring>& exclusionPatterns) {
    patterns_.reserve(exclusionPatterns.size());
    for (const auto& pattern : exclusionPatterns) {
      auto segments = NormalizedSegments(pattern);
      if (!segments.empty()) patterns_.push_back(std::move(segments));
    }
  }

  bool Matches(std::wstring_view relativePath) const {
    const auto pathSegments = NormalizedSegments(relativePath);
    if (pathSegments.empty()) return false;
    return std::any_of(
        patterns_.begin(), patterns_.end(), [&](const auto& pattern) {
          return MatchesSegments(pattern, pathSegments);
        });
  }

 private:
  std::vector<std::vector<std::wstring>> patterns_;
};

bool EntryMatchesExclusion(
    const storage::FileIndexEntry& entry,
    const RelativePathExclusionMatcher& exclusionMatcher) {
  if (entry.path.empty() || entry.root.empty()) {
    return false;
  }
  const auto relative = std::filesystem::path(entry.path).lexically_relative(
      std::filesystem::path(entry.root));
  if (relative.empty()) return false;
  const auto relativeText = relative.generic_wstring();
  if (relativeText == L".." || relativeText.starts_with(L"../")) return false;
  return exclusionMatcher.Matches(relativeText);
}

}  // namespace

bool IsFixedLocalIndexRoot(const std::filesystem::path& path) {
  return FixedLocalRoot(path);
}

bool MatchesRelativePathExclusion(
    std::wstring_view relativePath,
    const std::vector<std::wstring>& exclusionPatterns) {
  return RelativePathExclusionMatcher(exclusionPatterns).Matches(relativePath);
}

std::vector<storage::FileIndexEntry> MergeFileIndexEntries(
    const std::vector<storage::FileIndexEntry>& previous,
    std::vector<storage::FileIndexEntry> scanned,
    const std::vector<std::wstring>& configuredRoots,
    const std::vector<std::wstring>& availableRoots, std::size_t limit,
    const std::vector<std::wstring>& exclusionPatterns) {
  auto normalized = [](const std::wstring& value) {
    std::wstring result =
        std::filesystem::path(value).lexically_normal().wstring();
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) {
                     return static_cast<wchar_t>(std::towlower(ch));
                   });
    return result;
  };
  std::set<std::wstring> configured;
  std::set<std::wstring> available;
  const RelativePathExclusionMatcher exclusionMatcher(exclusionPatterns);
  for (const auto& root : configuredRoots) configured.insert(normalized(root));
  for (const auto& root : availableRoots) available.insert(normalized(root));
  std::erase_if(scanned, [&](const storage::FileIndexEntry& entry) {
    return EntryMatchesExclusion(entry, exclusionMatcher);
  });

  long long writeGeneration = 0;
  for (const auto& entry : scanned) {
    writeGeneration = std::max(writeGeneration, entry.indexedAt);
  }
  for (const auto& entry : previous) {
    const auto root = normalized(entry.root);
    if (!configured.contains(root) || available.contains(root) ||
        EntryMatchesExclusion(entry, exclusionMatcher)) {
      continue;
    }
    scanned.push_back(entry);
    writeGeneration = std::max(writeGeneration, entry.indexedAt);
  }
  if (writeGeneration == 0) writeGeneration = NowMilliseconds();
  for (auto& entry : scanned) entry.indexedAt = writeGeneration;
  std::sort(scanned.begin(), scanned.end(), NewerEntry);
  if (scanned.size() > limit) scanned.resize(limit);
  return scanned;
}

struct FileIndexService::Watcher {
  std::wstring root;
  HANDLE directory = INVALID_HANDLE_VALUE;
  HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::jthread thread;

  ~Watcher() {
    if (thread.joinable()) {
      thread.request_stop();
      SetEvent(stopEvent);
      if (directory != INVALID_HANDLE_VALUE) CancelIoEx(directory, nullptr);
      thread.join();
    }
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (stopEvent) CloseHandle(stopEvent);
  }
};

FileIndexService::FileIndexService(ResultSink sink, ErrorSink errors)
    : sink_(std::move(sink)), errors_(std::move(errors)) {}

FileIndexService::~FileIndexService() { Stop(); }

void FileIndexService::Start() {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  stopping_ = false;
  worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
}

void FileIndexService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    request_.reset();
    worker_.request_stop();
  }
  cv_.notify_all();
  StopWatchers();
  worker_.join();
  std::lock_guard lock(mutex_);
  stopping_ = false;
  rebuildPending_ = false;
  restartWatchersPending_ = false;
  retryDelay_ = std::chrono::seconds(2);
}

void FileIndexService::Pause() {
  {
    std::lock_guard lock(mutex_);
    paused_ = true;
    request_.reset();
    rebuildPending_ = false;
    restartWatchersPending_ = true;
  }
  cv_.notify_all();
}

void FileIndexService::SetInteractive(bool interactive) {
  interactive_.store(interactive, std::memory_order_release);
  cv_.notify_all();
}

bool FileIndexService::Reconfigure(IndexRequest request) {
  currentGeneration_.store(request.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable()) return false;
  }
  // Watch first so changes made while the initial crawl is running are
  // retained by the watcher and reconciled by the coalesced follow-up scan.
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable()) return false;
    request_ = std::move(request);
    paused_ = false;
    restartWatchersPending_ = true;
    rebuildPending_ = true;
    rebuildAfter_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
  return true;
}

bool FileIndexService::Rebuild() {
  {
    std::lock_guard lock(mutex_);
    if (!request_ || stopping_ || !worker_.joinable()) return false;
    rebuildPending_ = true;
    rebuildAfter_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
  return true;
}

bool FileIndexService::IsCurrent(std::uint64_t generation) const {
  return !paused_ && currentGeneration_.load(std::memory_order_acquire) == generation;
}

void FileIndexService::ScheduleWatchRefresh(bool restartWatchers) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || paused_ || !request_) return;
    rebuildPending_ = true;
    restartWatchersPending_ = restartWatchersPending_ || restartWatchers;
    rebuildAfter_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(250);
  }
  cv_.notify_all();
}

void FileIndexService::RestartWatchers(
    const std::vector<std::wstring>& roots) {
  std::lock_guard watchersLock(watchersMutex_);
  watchers_.clear();
  for (const auto& root : roots) {
    const std::filesystem::path path(root);
    if (!FixedLocalRoot(path)) continue;
    auto watcher = std::make_unique<Watcher>();
    watcher->root = root;
    if (!watcher->stopEvent) continue;
    watcher->directory = CreateFileW(
        root.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (watcher->directory == INVALID_HANDLE_VALUE) continue;
    Watcher* raw = watcher.get();
    watcher->thread = std::jthread([this, raw](std::stop_token token) {
      alignas(DWORD) std::array<std::byte, 64 * 1024> buffer{};
      while (!token.stop_requested()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return;
        const BOOL queued = ReadDirectoryChangesW(
            raw->directory, buffer.data(), static_cast<DWORD>(buffer.size()),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &overlapped, nullptr);
        if (!queued) {
          CloseHandle(overlapped.hEvent);
          if (!token.stop_requested()) ScheduleWatchRefresh(true);
          return;
        }
        const HANDLE events[] = {overlapped.hEvent, raw->stopEvent};
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 || token.stop_requested()) {
          CancelIoEx(raw->directory, &overlapped);
        }
        DWORD transferred = 0;
        const BOOL completed = GetOverlappedResult(
            raw->directory, &overlapped, &transferred, TRUE);
        CloseHandle(overlapped.hEvent);
        if (token.stop_requested()) return;
        if (!completed || transferred == 0) {
          ScheduleWatchRefresh(true);
          continue;
        }
        ScheduleWatchRefresh();
      }
    });
    watchers_.push_back(std::move(watcher));
  }
}

void FileIndexService::StopWatchers() {
  std::lock_guard watchersLock(watchersMutex_);
  watchers_.clear();
}

void FileIndexService::WorkerLoop(std::stop_token token) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    IndexRequest request;
    bool restartWatchers = false;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return stopping_ || token.stop_requested() || rebuildPending_ || restartWatchersPending_;
      });
      if (stopping_ || token.stop_requested()) return;
      if (paused_) {
        restartWatchersPending_ = false;
        lock.unlock();
        StopWatchers();
        continue;
      }
      while (rebuildPending_ && std::chrono::steady_clock::now() < rebuildAfter_) {
        cv_.wait_until(lock, rebuildAfter_);
        if (stopping_ || token.stop_requested()) return;
      }
      if (!request_) continue;
      request = *request_;
      rebuildPending_ = false;
      restartWatchers = restartWatchersPending_;
      restartWatchersPending_ = false;
    }

    try {
      if (restartWatchers && !token.stop_requested()) {
        RestartWatchers(request.roots);
      }
      auto status = Scan(request, token);
      const bool needsRetry = status.unavailableRoots > 0;
      if (!token.stop_requested() && IsCurrent(request.generation) && sink_) {
        sink_(std::move(status));
      }
      std::lock_guard lock(mutex_);
      if (!stopping_ && request_ &&
          request_->generation == request.generation) {
        if (needsRetry) {
          const auto retryAt = std::chrono::steady_clock::now() + retryDelay_;
          if (!rebuildPending_ || retryAt < rebuildAfter_) {
            rebuildAfter_ = retryAt;
          }
          rebuildPending_ = true;
          restartWatchersPending_ = true;
          retryDelay_ = std::min(retryDelay_ * 2, std::chrono::seconds(60));
          cv_.notify_all();
        } else {
          retryDelay_ = std::chrono::seconds(2);
        }
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
    }
  }
}

IndexStatus FileIndexService::Scan(const IndexRequest& request,
                                   std::stop_token token) const {
  IndexStatus status;
  status.generation = request.generation;
  status.configuredRoots = request.roots;
  const long long scan = NowMilliseconds();
  const RelativePathExclusionMatcher exclusionMatcher(
      request.exclusionPatterns);
  // Keep only the newest `limit` entries while traversing. The previous
  // implementation retained every path and trimmed only after the complete
  // recursive scan, which made a large Documents tree consume hundreds of MB.
  std::priority_queue<storage::FileIndexEntry,
                      std::vector<storage::FileIndexEntry>, OlderEntryFirst>
      newest;
  std::size_t interactionYieldCounter = 0;
  const auto YieldDuringInteraction = [&] {
    if (!interactive_.load(std::memory_order_acquire)) return;
    if ((++interactionYieldCounter & 63u) == 0) Sleep(1);
  };

  for (const auto& configured : request.roots) {
    if (token.stop_requested() || !IsCurrent(request.generation)) return status;
    YieldDuringInteraction();
    const std::filesystem::path root(configured);
    std::error_code rootError;
    if (!FixedLocalRoot(root) ||
        !std::filesystem::is_directory(root, rootError) || rootError) {
      ++status.unavailableRoots;
      continue;
    }
    status.availableRoots.push_back(root.lexically_normal().wstring());
    std::deque<std::filesystem::path> pending{root};
    while (!pending.empty()) {
      if (token.stop_requested() || !IsCurrent(request.generation)) return status;
      auto directory = std::move(pending.front());
      pending.pop_front();
      std::error_code ec;
      for (std::filesystem::directory_iterator it(
               directory, std::filesystem::directory_options::skip_permission_denied,
               ec), end;
           !ec && it != end; it.increment(ec)) {
        if (token.stop_requested() || !IsCurrent(request.generation)) return status;
        YieldDuringInteraction();
        const auto path = it->path();
        const auto relative = path.lexically_relative(root).generic_wstring();
        if (exclusionMatcher.Matches(relative)) {
          continue;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                           FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
          continue;
        }
        const bool directoryEntry =
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directoryEntry) {
          if (IsGeneratedDirectory(path)) continue;
          pending.push_back(path);
        }
        const auto normalizedPath = path.lexically_normal();

        storage::FileIndexEntry entry;
        entry.path = normalizedPath.wstring();
        entry.name = normalizedPath.filename().wstring();
        entry.isDirectory = directoryEntry;
        entry.iconKey = entry.path;
        entry.indexedAt = scan;
        entry.scanGeneration = scan;
        entry.root = root.lexically_normal().wstring();
        const auto writeTime =
            std::filesystem::last_write_time(normalizedPath, ec);
        if (!ec) entry.lastWriteTime = writeTime.time_since_epoch().count();
        ec.clear();
        if (!directoryEntry) {
          const auto bytes = std::filesystem::file_size(normalizedPath, ec);
          if (!ec) entry.size = static_cast<long long>(bytes);
        }
        if (request.limit == 0) continue;
        if (newest.size() < request.limit) {
          newest.push(std::move(entry));
        } else if (NewerEntry(entry, newest.top())) {
          newest.pop();
          newest.push(std::move(entry));
        }
      }
    }
  }

  std::vector<storage::FileIndexEntry> discovered;
  discovered.reserve(newest.size());
  while (!newest.empty()) {
    discovered.push_back(newest.top());
    newest.pop();
  }
  std::sort(discovered.begin(), discovered.end(), [](const auto& left,
                                                       const auto& right) {
    if (left.lastWriteTime != right.lastWriteTime) {
      return left.lastWriteTime > right.lastWriteTime;
    }
    return left.path < right.path;
  });
  if (request.contentEnabled) {
    for (auto& entry : discovered) {
      if (token.stop_requested() || !IsCurrent(request.generation)) return status;
      YieldDuringInteraction();
      if (entry.isDirectory) continue;
      auto extraction = file_content::Extract(entry.path,
                                              file_content::kMaxIndexedBytes,
                                              token);
      entry.contentState = static_cast<int>(extraction.state);
      if (extraction.state != file_content::State::Indexed) continue;
      if (status.indexedContentBytes +
              static_cast<long long>(extraction.sourceBytes) >
          file_content::kTotalSourceQuotaBytes) {
        entry.contentState = static_cast<int>(file_content::State::TooLarge);
        continue;
      }
      entry.contentBytes = static_cast<long long>(extraction.sourceBytes);
      entry.contentText = std::move(extraction.text);
      status.indexedContentBytes += entry.contentBytes;
      ++status.indexedContentFiles;
    }
  }

  status.entries = std::move(discovered);
  status.live = status.unavailableRoots == 0;
  status.message = status.live ? L"File index is live."
                               : L"Some indexed folders are unavailable.";
  return status;
}

}  // namespace feathercast::files
