#include "file_index_service.hpp"

#include "filesystem_semantics.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cwctype>
#include <deque>
#include <filesystem>
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

bool IsGeneratedDirectory(std::wstring_view rawName) {
  if (rawName.size() < 3 || rawName.size() > 12) return false;
  wchar_t lower[16];
  for (std::size_t i = 0; i < rawName.size(); ++i) {
    const wchar_t ch = rawName[i];
    if (ch >= L'A' && ch <= L'Z') {
      lower[i] = static_cast<wchar_t>(ch - L'A' + L'a');
    } else if (ch < 128) {
      lower[i] = ch;
    } else {
      return false;
    }
  }
  const std::wstring_view name(lower, rawName.size());
  static constexpr std::wstring_view kGenerated[] = {
      L".cache", L".git", L".hg", L".svn", L"bin", L"build",
      L"build-native", L"dist", L"node_modules", L"obj", L"out", L"target"};
  return std::binary_search(std::begin(kGenerated), std::end(kGenerated), name);
}

// Full rescans while a configured root is unavailable. Later retries only
// probe the missing roots and rescan once one of them is back.
constexpr std::size_t kMaxRescanRetries = 4;
constexpr DWORD kSkippedAttributes =
    FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT;
constexpr auto kWorkerIdleTimeout = std::chrono::seconds(20);

// Key of %LOCALAPPDATA%\FeatherCast, or empty when it cannot be resolved.
// FeatherCast writes its database, the file index itself and the icon cache
// there, so a root containing it would rescan after every one of those writes.
std::wstring OperationalDataKey() {
  wchar_t stackBuffer[512];
  DWORD length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", stackBuffer, static_cast<DWORD>(std::size(stackBuffer)));
  if (length == 0) return {};
  std::wstring localAppData;
  if (length < std::size(stackBuffer)) {
    localAppData.assign(stackBuffer, length);
  } else {
    localAppData.resize(length);
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(), length);
    if (written == 0 || written >= length) return {};
    localAppData.resize(written);
  }
  return filesystem_semantics::PathKey(
      (std::filesystem::path(std::move(localAppData)) / L"FeatherCast").wstring());
}

bool IsOperationalDataDirectory(const std::wstring& path,
                                const std::wstring& dataKey) {
  return !dataKey.empty() && filesystem_semantics::PathKey(path) == dataKey;
}

bool PathKeyContainsOrEquals(const std::wstring& parentKey,
                             const std::wstring& childKey) {
  if (parentKey.empty() || childKey.size() < parentKey.size()) return false;
  if (childKey == parentKey) return true;
  if (!childKey.starts_with(parentKey)) return false;
  return parentKey.back() == L'\\' || childKey[parentKey.size()] == L'\\';
}

std::wstring MakeFindPattern(const std::wstring& directory) {
  std::wstring pattern;
  const bool needsExtended =
      directory.size() >= 246 && !directory.starts_with(L"\\\\?\\");
  if (needsExtended) {
    if (directory.starts_with(L"\\\\")) {
      pattern = L"\\\\?\\UNC\\";
      pattern.append(directory, 2, std::wstring::npos);
    } else {
      pattern = L"\\\\?\\";
      pattern += directory;
    }
  } else {
    pattern = directory;
  }
  if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  return pattern;
}

long long FileTimeToTicks(const FILETIME& ft) {
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return static_cast<long long>(value.QuadPart);
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

bool RootAvailable(const std::wstring& configured) {
  const std::filesystem::path root(configured);
  std::error_code ec;
  return FixedLocalRoot(root) && std::filesystem::is_directory(root, ec) &&
         !ec;
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
  // Only '*' is special within a segment. Remember the most recent star
  // and extend it on a mismatch instead of allocating two DP rows per name.
  std::size_t patternIndex = 0;
  std::size_t valueIndex = 0;
  std::size_t star = std::wstring_view::npos;
  std::size_t starEnd = 0;
  while (valueIndex < value.size()) {
    if (patternIndex < pattern.size() && pattern[patternIndex] == L'*') {
      star = patternIndex++;
      starEnd = valueIndex;
    } else if (patternIndex < pattern.size() &&
               pattern[patternIndex] == value[valueIndex]) {
      ++patternIndex;
      ++valueIndex;
    } else if (star != std::wstring_view::npos) {
      patternIndex = star + 1;
      valueIndex = ++starEnd;
    } else {
      return false;
    }
  }
  while (patternIndex < pattern.size() && pattern[patternIndex] == L'*') {
    ++patternIndex;
  }
  return patternIndex == pattern.size();
}

bool MatchesSegments(const std::vector<std::wstring>& pattern,
                     const std::vector<std::wstring>& path) {
  std::vector<bool> previous(path.size() + 1);
  std::vector<bool> current(path.size() + 1);
  previous[0] = true;
  for (std::size_t patternIndex = 1; patternIndex <= pattern.size();
       ++patternIndex) {
    std::fill(current.begin(), current.end(), false);
    if (pattern[patternIndex - 1] == L"**") {
      current[0] = previous[0];
      for (std::size_t pathIndex = 1; pathIndex <= path.size(); ++pathIndex) {
        current[pathIndex] = previous[pathIndex] || current[pathIndex - 1];
      }
      previous.swap(current);
      continue;
    }
    for (std::size_t pathIndex = 1; pathIndex <= path.size(); ++pathIndex) {
      current[pathIndex] =
          previous[pathIndex - 1] &&
          MatchesSegment(pattern[patternIndex - 1], path[pathIndex - 1]);
    }
    previous.swap(current);
  }
  return previous[path.size()];
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

  bool Empty() const noexcept { return patterns_.empty(); }

  bool Matches(std::wstring_view relativePath) const {
    if (patterns_.empty()) return false;
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
  if (exclusionMatcher.Empty() || entry.path.empty() || entry.root.empty()) {
    return false;
  }
  const auto relative = std::filesystem::path(entry.path).lexically_relative(
      std::filesystem::path(entry.root));
  if (relative.empty()) return false;
  const auto relativeText = relative.generic_wstring();
  if (relativeText == L".." || relativeText.starts_with(L"../")) return false;
  return exclusionMatcher.Matches(relativeText);
}

// Whether a change reported below `root` can alter the index. It applies the
// same rules as Scan: nothing below hidden, system, generated or excluded
// folders, or below FeatherCast's own data folder, is ever indexed, so changes
// there must not trigger a rescan. A path that no longer exists still counts,
// because it may have been indexed.
bool ChangeAffectsIndex(const std::filesystem::path& root,
                        std::wstring_view relative,
                        const RelativePathExclusionMatcher& exclusionMatcher,
                        const std::wstring& dataKey) {
  std::filesystem::path current = root;
  std::wstring prefix;
  std::size_t start = 0;
  while (start < relative.size()) {
    const auto end = std::min(relative.find(L'\\', start), relative.size());
    const auto segment = relative.substr(start, end - start);
    start = end + 1;
    if (segment.empty()) continue;
    const bool leaf = end >= relative.size();
    current /= std::wstring(segment);
    if (!exclusionMatcher.Empty()) {
      if (!prefix.empty()) prefix.push_back(L'/');
      prefix.append(segment);
      if (exclusionMatcher.Matches(prefix)) return false;
    }
    const DWORD attributes = GetFileAttributesW(current.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (exists && (attributes & kSkippedAttributes) != 0) return false;
    // Every ancestor of a reported path is a directory, even after a delete.
    const bool directory =
        !leaf || (exists && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if (directory && (IsGeneratedDirectory(segment) ||
                      IsOperationalDataDirectory(current.wstring(), dataKey))) {
      return false;
    }
  }
  return true;
}

bool NotificationsAffectIndex(const std::byte* buffer, DWORD length,
                              const std::filesystem::path& root,
                              const RelativePathExclusionMatcher& exclusionMatcher,
                              const std::wstring& dataKey) {
  DWORD offset = 0;
  for (;;) {
    if (offset > length ||
        length - offset < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
      return true;
    }
    const auto* record =
        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    const DWORD nameBytes = record->FileNameLength;
    if (length - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName) <
        nameBytes) {
      return true;
    }
    const std::wstring_view relative(record->FileName,
                                     nameBytes / sizeof(wchar_t));
    if (ChangeAffectsIndex(root, relative, exclusionMatcher, dataKey)) {
      return true;
    }
    if (record->NextEntryOffset == 0) return false;
    offset += record->NextEntryOffset;
  }
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
  // Roots arrive in the user's spelling ("D:/Notes/", "d:\notes"), so
  // compare path keys rather than the stored text.
  const auto normalized = [](const std::wstring& value) {
    return filesystem_semantics::PathKey(value);
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

struct FileIndexService::WatcherGroup {
  struct WatchSlot {
    std::wstring root;
    std::filesystem::path path;
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    alignas(DWORD) std::array<std::byte, 64 * 1024> buffer{};
    bool ioPending = false;
    bool broken = false;
    bool delivered = false;
    std::size_t overflows = 0;
    bool waitingOverflow = false;
    std::chrono::steady_clock::time_point resumeAt{};
  };

  static constexpr ULONG_PTR kStopKey = 0;
  HANDLE iocp = nullptr;
  std::vector<std::unique_ptr<WatchSlot>> slots;
  std::jthread thread;

  ~WatcherGroup() {
    if (thread.joinable()) {
      thread.request_stop();
      if (iocp) PostQueuedCompletionStatus(iocp, 0, kStopKey, nullptr);
      thread.join();
    }
    for (auto& slot : slots) {
      if (slot && slot->directory != INVALID_HANDLE_VALUE) {
        CloseHandle(slot->directory);
        slot->directory = INVALID_HANDLE_VALUE;
      }
    }
    if (iocp) {
      CloseHandle(iocp);
      iocp = nullptr;
    }
  }
};

FileIndexService::FileIndexService(ResultSink sink, ErrorSink errors)
    : sink_(std::move(sink)), errors_(std::move(errors)) {}

FileIndexService::~FileIndexService() { Stop(); }

void FileIndexService::Start() {
  std::lock_guard lock(mutex_);
  stopping_ = false;
  stoppingAtomic_.store(false, std::memory_order_release);
  started_ = true;
}

void FileIndexService::EnsureWorker() {
  for (;;) {
    std::jthread toJoin;
    {
      std::lock_guard lock(mutex_);
      if (!started_ || stopping_ || workerActive_) return;
      if (worker_.joinable()) {
        toJoin = std::move(worker_);
      } else {
        workerActive_ = true;
        worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
        return;
      }
    }
    toJoin.join();
  }
}

void FileIndexService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!started_ && !worker_.joinable()) {
      stopping_ = false;
      stoppingAtomic_.store(false, std::memory_order_release);
    } else {
      stopping_ = true;
      stoppingAtomic_.store(true, std::memory_order_release);
      started_ = false;
      request_.reset();
      if (worker_.joinable()) worker_.request_stop();
    }
  }
  cv_.notify_all();
  StopWatchers();
  std::jthread toJoin;
  {
    std::lock_guard lock(mutex_);
    toJoin = std::move(worker_);
  }
  if (toJoin.joinable()) toJoin.join();
  StopWatchers();
  std::lock_guard lock(mutex_);
  stopping_ = false;
  stoppingAtomic_.store(false, std::memory_order_release);
  workerActive_ = false;
  rebuildPending_ = false;
  restartWatchersPending_ = false;
  ResetRetries();
  watchFailures_.clear();
  ForgetContentRecords();
}

void FileIndexService::Pause() {
  {
    std::lock_guard lock(mutex_);
    paused_ = true;
    request_.reset();
    rebuildPending_ = false;
    restartWatchersPending_ = false;
    ResetRetries();
    watchFailures_.clear();
    ForgetContentRecords();
  }
  StopWatchers();
  cv_.notify_all();
}

void FileIndexService::ForgetContentRecords() {
  contentRecords_.clear();
  ++contentEpoch_;
}

void FileIndexService::ResetRetries() {
  retryDelay_ = std::chrono::seconds(2);
  retryAttempts_ = 0;
  probeOnly_ = false;
}

void FileIndexService::SetInteractive(bool interactive) {
  interactive_.store(interactive, std::memory_order_release);
  cv_.notify_all();
}

bool FileIndexService::Reconfigure(IndexRequest request) {
  currentGeneration_.store(request.generation, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !started_) return false;
    request_ = std::move(request);
    paused_ = false;
    restartWatchersPending_ = true;
    rebuildPending_ = true;
    rebuildAfter_ = std::chrono::steady_clock::now();
    ResetRetries();
    watchFailures_.clear();
    ForgetContentRecords();
  }
  EnsureWorker();
  cv_.notify_all();
  return true;
}

bool FileIndexService::Rebuild() {
  {
    std::lock_guard lock(mutex_);
    if (!request_ || stopping_ || !started_) return false;
    rebuildPending_ = true;
    rebuildAfter_ = std::chrono::steady_clock::now();
    // A rebuild is an explicit request to read every file again. It also
    // restarts the watchers, which gives broken watches a fresh budget.
    restartWatchersPending_ = true;
    ResetRetries();
    watchFailures_.clear();
    ForgetContentRecords();
  }
  EnsureWorker();
  cv_.notify_all();
  return true;
}

bool FileIndexService::IsCurrent(std::uint64_t generation) const {
  return !paused_ && currentGeneration_.load(std::memory_order_acquire) == generation;
}

void FileIndexService::ScheduleWatchRefresh(bool restartWatchers) {
  bool shouldWake = false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !started_ || paused_ || !request_) return;
    rebuildPending_ = true;
    probeOnly_ = false;
    restartWatchersPending_ = restartWatchersPending_ || restartWatchers;
    rebuildAfter_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(250);
    shouldWake = true;
  }
  if (shouldWake) {
    EnsureWorker();
    cv_.notify_all();
  }
}

void FileIndexService::ScheduleWatchRetry(const std::wstring& root) {
  bool shouldWake = false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !started_ || paused_ || !request_) return;
    const std::size_t failures = ++watchFailures_[root];
    if (failures > kMaxWatchRetries) return;
    // Changes made while the watch was down are unknown, so the restart
    // rescans as well. An earlier pending rebuild is not postponed.
    const auto retryAt =
        std::chrono::steady_clock::now() + WatchRetryDelay(failures);
    rebuildAfter_ =
        rebuildPending_ ? std::min(rebuildAfter_, retryAt) : retryAt;
    rebuildPending_ = true;
    probeOnly_ = false;
    restartWatchersPending_ = true;
    shouldWake = true;
  }
  if (shouldWake) {
    EnsureWorker();
    cv_.notify_all();
  }
}

void FileIndexService::ClearWatchFailures(const std::wstring& root) {
  std::lock_guard lock(mutex_);
  watchFailures_.erase(root);
}

void FileIndexService::RestartWatchers(const IndexRequest& request) {
  std::lock_guard watchersLock(watchersMutex_);
  watcherGroup_.reset();
  if (stoppingAtomic_.load(std::memory_order_acquire) ||
      paused_.load(std::memory_order_acquire)) {
    return;
  }
  auto group = std::make_unique<WatcherGroup>();
  group->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (!group->iocp) return;

  for (const auto& root : request.roots) {
    const std::filesystem::path path(root);
    if (!FixedLocalRoot(path)) continue;
    HANDLE directory = CreateFileW(
        root.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) continue;
    auto slot = std::make_unique<WatcherGroup::WatchSlot>();
    slot->root = root;
    slot->path = path;
    slot->directory = directory;
    if (!CreateIoCompletionPort(slot->directory, group->iocp,
                                reinterpret_cast<ULONG_PTR>(slot.get()), 0)) {
      CloseHandle(slot->directory);
      slot->directory = INVALID_HANDLE_VALUE;
      continue;
    }
    group->slots.push_back(std::move(slot));
  }
  if (group->slots.empty()) return;

  const auto dataKey = OperationalDataKey();
  WatcherGroup* rawGroup = group.get();
  group->thread = std::jthread([this, rawGroup, dataKey,
                                patterns = request.exclusionPatterns](
                                   std::stop_token token) {
    const RelativePathExclusionMatcher exclusionMatcher(patterns);
    auto issueRead = [&](WatcherGroup::WatchSlot& slot) {
      slot.overlapped = {};
      const BOOL queued = ReadDirectoryChangesW(
          slot.directory, slot.buffer.data(),
          static_cast<DWORD>(slot.buffer.size()), TRUE,
          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
          nullptr, &slot.overlapped, nullptr);
      if (queued) {
        slot.ioPending = true;
        return true;
      }
      slot.broken = true;
      if (!token.stop_requested()) ScheduleWatchRetry(slot.root);
      return false;
    };

    for (auto& slot : rawGroup->slots) {
      issueRead(*slot);
    }

    while (!token.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      DWORD timeout = INFINITE;
      bool anyActive = false;
      for (auto& slot : rawGroup->slots) {
        if (slot->broken) continue;
        if (slot->waitingOverflow) {
          if (now >= slot->resumeAt) {
            slot->waitingOverflow = false;
            if (issueRead(*slot)) anyActive = true;
          } else {
            anyActive = true;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                slot->resumeAt - now)
                                .count();
            const DWORD clamped = ms <= 0 ? 1u
                : (ms > 60'000 ? 60'000u : static_cast<DWORD>(ms));
            timeout = timeout == INFINITE ? clamped : std::min(timeout, clamped);
          }
        } else if (slot->ioPending) {
          anyActive = true;
        }
      }
      if (!anyActive) break;

      DWORD transferred = 0;
      ULONG_PTR key = 0;
      LPOVERLAPPED overlapped = nullptr;
      const BOOL completed = GetQueuedCompletionStatus(
          rawGroup->iocp, &transferred, &key, &overlapped, timeout);
      if (!completed && !overlapped) {
        if (GetLastError() == WAIT_TIMEOUT) continue;
        break;
      }
      if (key == WatcherGroup::kStopKey) break;
      auto* slot = reinterpret_cast<WatcherGroup::WatchSlot*>(key);
      slot->ioPending = false;
      if (token.stop_requested()) break;
      if (!completed) {
        slot->broken = true;
        ScheduleWatchRetry(slot->root);
        continue;
      }
      if (transferred == 0) {
        // The change buffer overflowed. The watch still works and the
        // handle keeps collecting changes, but the dropped ones need a
        // rescan. Pausing before the next read spaces out the rescans of
        // a long burst (and of a file system that keeps completing empty).
        ScheduleWatchRefresh();
        slot->waitingOverflow = true;
        slot->resumeAt = std::chrono::steady_clock::now() +
                         WatchRetryDelay(++slot->overflows);
        continue;
      }
      slot->overflows = 0;
      if (!slot->delivered) {
        // The watch works, so its root gets a fresh retry budget.
        slot->delivered = true;
        ClearWatchFailures(slot->root);
      }
      if (NotificationsAffectIndex(slot->buffer.data(), transferred, slot->path,
                                   exclusionMatcher, dataKey)) {
        ScheduleWatchRefresh();
      }
      issueRead(*slot);
    }

    // Cancel and drain all in-flight overlapped reads before returning so the
    // kernel never writes into freed WatchSlot OVERLAPPED/buffer memory.
    for (auto& slot : rawGroup->slots) {
      if (slot->ioPending && slot->directory != INVALID_HANDLE_VALUE) {
        CancelIoEx(slot->directory, &slot->overlapped);
      }
    }
    for (;;) {
      bool anyPending = false;
      for (const auto& slot : rawGroup->slots) {
        if (slot->ioPending) {
          anyPending = true;
          break;
        }
      }
      if (!anyPending) break;
      DWORD transferred = 0;
      ULONG_PTR key = 0;
      LPOVERLAPPED overlapped = nullptr;
      const BOOL ok = GetQueuedCompletionStatus(
          rawGroup->iocp, &transferred, &key, &overlapped, 5000);
      if (overlapped && key != WatcherGroup::kStopKey) {
        reinterpret_cast<WatcherGroup::WatchSlot*>(key)->ioPending = false;
      } else if (!ok && !overlapped) {
        break;
      }
    }
  });
  watcherGroup_ = std::move(group);
}

void FileIndexService::StopWatchers() {
  std::lock_guard watchersLock(watchersMutex_);
  watcherGroup_.reset();
}

void FileIndexService::WorkerLoop(std::stop_token token) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    IndexRequest request;
    bool restartWatchers = false;
    bool probeOnly = false;
    std::vector<std::wstring> missingRoots;
    {
      std::unique_lock lock(mutex_);
      if (!stopping_ && !token.stop_requested() && !rebuildPending_ &&
          !restartWatchersPending_) {
        const bool woke = cv_.wait_for(lock, kWorkerIdleTimeout, [&] {
          return stopping_ || token.stop_requested() || rebuildPending_ ||
                 restartWatchersPending_;
        });
        if (!woke && !stopping_ && !token.stop_requested() &&
            !rebuildPending_ && !restartWatchersPending_) {
          workerActive_ = false;
          return;
        }
      }
      if (stopping_ || token.stop_requested()) {
        workerActive_ = false;
        return;
      }
      if (paused_) {
        restartWatchersPending_ = false;
        lock.unlock();
        StopWatchers();
        continue;
      }
      while (rebuildPending_ && std::chrono::steady_clock::now() < rebuildAfter_) {
        cv_.wait_until(lock, rebuildAfter_);
        if (stopping_ || token.stop_requested()) {
          workerActive_ = false;
          return;
        }
      }
      if (!request_) continue;
      request = *request_;
      rebuildPending_ = false;
      restartWatchers = restartWatchersPending_;
      restartWatchersPending_ = false;
      probeOnly = probeOnly_;
      probeOnly_ = false;
      missingRoots = unavailableRoots_;
    }

    try {
      // A root that was missing during the previous scan has no watcher yet.
      const bool rootReturned =
          std::any_of(missingRoots.begin(), missingRoots.end(), RootAvailable);
      if (probeOnly && !rootReturned) {
        // Nothing came back: probe again later instead of rescanning.
        std::lock_guard lock(mutex_);
        if (!stopping_ && !rebuildPending_ && request_ &&
            request_->generation == request.generation) {
          rebuildPending_ = true;
          probeOnly_ = true;
          rebuildAfter_ = std::chrono::steady_clock::now() + retryDelay_;
          retryDelay_ = std::min(retryDelay_ * 2, std::chrono::seconds(60));
        }
        continue;
      }
      if (rootReturned) restartWatchers = true;
      ContentRecords previous;
      std::uint64_t epoch = 0;
      {
        std::lock_guard lock(mutex_);
        previous = std::move(contentRecords_);
        contentRecords_.clear();
        epoch = contentEpoch_;
      }
      if (restartWatchers && !token.stop_requested()) {
        RestartWatchers(request);
      }
      ContentRecords current;
      auto status = Scan(request, token, previous, current);
      previous.clear();
      const bool needsRetry = status.unavailableRoots > 0;
      missingRoots.clear();
      for (const auto& root : status.configuredRoots) {
        if (std::none_of(status.availableRoots.begin(),
                         status.availableRoots.end(),
                         [&](const std::wstring& available) {
                           return filesystem_semantics::SamePath(available,
                                                                 root);
                         })) {
          missingRoots.push_back(root);
        }
      }
      bool delivered = false;
      if (!token.stop_requested() && IsCurrent(request.generation) && sink_) {
        sink_(std::move(status));
        delivered = true;
      }
      std::lock_guard lock(mutex_);
      if (!stopping_ && request_ &&
          request_->generation == request.generation) {
        // Records describe what the sink received. A scan that was not
        // delivered leaves none, so the next scan reads every file again.
        if (delivered && contentEpoch_ == epoch) {
          contentRecords_ = std::move(current);
        }
        unavailableRoots_ = std::move(missingRoots);
        if (needsRetry) {
          // A rebuild that is already pending (a watcher refresh or an
          // explicit request) rescans anyway and runs this check again.
          if (!rebuildPending_) {
            const bool probe = retryAttempts_ >= kMaxRescanRetries;
            if (!probe) ++retryAttempts_;
            rebuildPending_ = true;
            probeOnly_ = probe;
            if (!probe) restartWatchersPending_ = true;
            rebuildAfter_ = std::chrono::steady_clock::now() + retryDelay_;
            retryDelay_ = std::min(retryDelay_ * 2, std::chrono::seconds(60));
            cv_.notify_all();
          }
        } else {
          ResetRetries();
        }
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
    }
  }
}

IndexStatus FileIndexService::Scan(const IndexRequest& request,
                                   std::stop_token token,
                                   const ContentRecords& previous,
                                   ContentRecords& current) const {
  IndexStatus status;
  status.generation = request.generation;
  status.configuredRoots = request.roots;
  status.entryLimit = request.limit;
  const long long scan = NowMilliseconds();
  const RelativePathExclusionMatcher exclusionMatcher(
      request.exclusionPatterns);
  const bool hasExclusions = !exclusionMatcher.Empty();
  const auto dataKey = OperationalDataKey();
  // Keep only the newest `limit` entries while traversing in a bounded vector
  // heap so entries are moved out in-place instead of copied.
  std::vector<storage::FileIndexEntry> discovered;
  if (request.limit > 0) {
    discovered.reserve(std::min<std::size_t>(request.limit, 1024));
  }
  std::size_t interactionYieldCounter = 0;
  const auto YieldDuringInteraction = [&] {
    if (!interactive_.load(std::memory_order_acquire)) return;
    if ((++interactionYieldCounter & 63u) == 0) Sleep(1);
  };

  // A root nested inside another is walked on its own, so the outer walk
  // stops at it; a root listed twice is walked once. Either way no file is
  // indexed twice.
  std::set<std::wstring> rootKeys;
  for (const auto& configured : request.roots) {
    rootKeys.insert(filesystem_semantics::PathKey(configured));
  }
  std::set<std::wstring> walkedRoots;

  struct PendingDir {
    std::wstring path;
    std::wstring relative;
  };

  for (const auto& configured : request.roots) {
    if (token.stop_requested() || !IsCurrent(request.generation)) return status;
    YieldDuringInteraction();
    const std::wstring rootKey = filesystem_semantics::PathKey(configured);
    if (!walkedRoots.insert(rootKey).second) continue;
    const std::filesystem::path root(configured);
    const DWORD rootAttrs = GetFileAttributesW(root.c_str());
    if (!FixedLocalRoot(root) || rootAttrs == INVALID_FILE_ATTRIBUTES ||
        (rootAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      ++status.unavailableRoots;
      continue;
    }
    const std::wstring rootNorm = root.lexically_normal().wstring();
    status.availableRoots.push_back(rootNorm);

    const bool checkDataKey = PathKeyContainsOrEquals(rootKey, dataKey);
    bool hasNestedRoots = false;
    for (const auto& otherKey : rootKeys) {
      if (otherKey != rootKey && PathKeyContainsOrEquals(rootKey, otherKey)) {
        hasNestedRoots = true;
        break;
      }
    }

    std::deque<PendingDir> pending;
    pending.push_back({rootNorm, L""});
    WIN32_FIND_DATAW findData{};
    while (!pending.empty()) {
      if (token.stop_requested() || !IsCurrent(request.generation)) return status;
      PendingDir directory = std::move(pending.front());
      pending.pop_front();
      const std::wstring pattern = MakeFindPattern(directory.path);
      HANDLE handle = FindFirstFileExW(
          pattern.c_str(), FindExInfoBasic, &findData,
          FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (handle == INVALID_HANDLE_VALUE) continue;
      do {
        if (token.stop_requested() || !IsCurrent(request.generation)) {
          FindClose(handle);
          return status;
        }
        const wchar_t* fileName = findData.cFileName;
        if (fileName[0] == L'.' &&
            (fileName[1] == L'\0' ||
             (fileName[1] == L'.' && fileName[2] == L'\0'))) {
          continue;
        }
        YieldDuringInteraction();
        const DWORD attributes = findData.dwFileAttributes;
        if ((attributes & kSkippedAttributes) != 0) continue;

        const std::wstring_view nameView(fileName);
        std::wstring relative;
        if (hasExclusions) {
          relative.reserve(directory.relative.size() + 1 + nameView.size());
          if (!directory.relative.empty()) {
            relative.append(directory.relative);
            relative.push_back(L'/');
          }
          relative.append(nameView);
          if (exclusionMatcher.Matches(relative)) continue;
        }

        const bool directoryEntry = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directoryEntry && IsGeneratedDirectory(nameView)) continue;

        std::wstring childPath;
        childPath.reserve(directory.path.size() + 1 + nameView.size());
        childPath.append(directory.path);
        if (!childPath.empty() && childPath.back() != L'\\') {
          childPath.push_back(L'\\');
        }
        childPath.append(nameView);

        if (directoryEntry) {
          if (checkDataKey && IsOperationalDataDirectory(childPath, dataKey)) {
            continue;
          }
          if (!hasNestedRoots ||
              !rootKeys.contains(filesystem_semantics::PathKey(childPath))) {
            pending.push_back({childPath, std::move(relative)});
          }
        }

        ++status.discoveredEntries;
        if (request.limit == 0) continue;

        const long long writeTicks = FileTimeToTicks(findData.ftLastWriteTime);
        const bool shouldKeep =
            discovered.size() < request.limit ||
            writeTicks > discovered.front().lastWriteTime ||
            (writeTicks == discovered.front().lastWriteTime &&
             childPath < discovered.front().path);
        if (!shouldKeep) continue;

        storage::FileIndexEntry entry;
        entry.path = std::move(childPath);
        entry.name.assign(nameView);
        entry.isDirectory = directoryEntry;
        entry.iconKey = entry.path;
        entry.indexedAt = scan;
        entry.scanGeneration = scan;
        entry.root = rootNorm;
        entry.lastWriteTime = writeTicks;
        if (!directoryEntry) {
          entry.size = (static_cast<long long>(findData.nFileSizeHigh) << 32) |
                       static_cast<long long>(findData.nFileSizeLow);
        }
        if (discovered.size() < request.limit) {
          discovered.push_back(std::move(entry));
          std::push_heap(discovered.begin(), discovered.end(), OlderEntryFirst{});
        } else {
          std::pop_heap(discovered.begin(), discovered.end(), OlderEntryFirst{});
          discovered.back() = std::move(entry);
          std::push_heap(discovered.begin(), discovered.end(), OlderEntryFirst{});
        }
      } while (FindNextFileW(handle, &findData));
      FindClose(handle);
    }
  }

  std::sort(discovered.begin(), discovered.end(), NewerEntry);
  if (request.contentEnabled) {
    for (auto& entry : discovered) {
      if (token.stop_requested() || !IsCurrent(request.generation)) return status;
      YieldDuringInteraction();
      if (entry.isDirectory) continue;
      constexpr int kIndexed = static_cast<int>(file_content::State::Indexed);
      constexpr int kBinary = static_cast<int>(file_content::State::Binary);
      const auto known = previous.find(entry.path);
      if (entry.lastWriteTime != 0 && known != previous.end() &&
          known->second.lastWriteTime == entry.lastWriteTime &&
          known->second.size == entry.size) {
        // Unchanged since the last delivered scan: reuse the result instead
        // of reading the file again. Storage keeps the indexed text.
        entry.contentState = known->second.contentState;
        if (entry.contentState == kIndexed) {
          if (status.indexedContentBytes + known->second.contentBytes >
              file_content::kTotalSourceQuotaBytes) {
            entry.contentState =
                static_cast<int>(file_content::State::TooLarge);
            continue;
          }
          entry.contentBytes = known->second.contentBytes;
          entry.contentUnchanged = true;
          status.indexedContentBytes += entry.contentBytes;
          ++status.indexedContentFiles;
        }
        current.insert_or_assign(entry.path, known->second);
        continue;
      }
      auto extraction = file_content::Extract(entry.path,
                                              file_content::kMaxIndexedBytes,
                                              token);
      entry.contentState = static_cast<int>(extraction.state);
      // Only results that needed the whole file are worth remembering; the
      // other states come from cheap checks that run again every scan.
      if (entry.lastWriteTime != 0 && entry.contentState == kBinary) {
        current.insert_or_assign(
            entry.path,
            ContentRecord{entry.lastWriteTime, entry.size, kBinary, 0});
      }
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
      if (entry.lastWriteTime != 0) {
        current.insert_or_assign(
            entry.path, ContentRecord{entry.lastWriteTime, entry.size,
                                      kIndexed, entry.contentBytes});
      }
    }
  }

  status.entries = std::move(discovered);
  status.limitReached = status.discoveredEntries > request.limit;
  status.live = status.unavailableRoots == 0;
  status.message = status.live ? L"File index is live."
                               : L"Some indexed folders are unavailable.";
  if (status.limitReached) {
    status.message += L" Limit reached: " + std::to_wstring(request.limit) +
        L" of " + std::to_wstring(status.discoveredEntries) +
        L" entries retained (newest first). Increase the limit or narrow folders in Privacy.";
  }
  return status;
}

}  // namespace feathercast::files
