#include "file_search_service.hpp"

#include "extension_protocol.hpp"
#include "discovery.hpp"
#include "sqlite3.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <unordered_set>
#include <utility>

namespace feathercast::files {
namespace {

constexpr auto kIdleWorkerTimeout = std::chrono::seconds(30);

wchar_t NormalizePathChar(wchar_t ch) noexcept {
  if (ch == L'/') return L'\\';
  if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch + (L'a' - L'A'));
  if (ch < 0x80) return ch;
  return static_cast<wchar_t>(std::towlower(ch));
}

std::uint64_t NormalizedPathHash(std::wstring_view path) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (const wchar_t ch : path) {
    hash ^= static_cast<std::uint64_t>(NormalizePathChar(ch));
    hash *= 1099511628211ull;
  }
  return hash;
}

bool NormalizedPathEquals(std::wstring_view left, std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (NormalizePathChar(left[i]) != NormalizePathChar(right[i])) return false;
  }
  return true;
}

app::DisplayItem Display(app::AppEntry entry, bool contentMatch = false) {
  entry.fileContentMatch = contentMatch;
  app::DisplayItem item;
  item.app = std::move(entry);
  item.commandDetail = contentMatch ? L"Content match" : L"File or folder";
  return item;
}

app::AppEntry ProjectStorageEntry(const storage::FileIndexEntry& entry) {
  app::AppEntry app;
  app.id = L"file:" + entry.path;
  app.name = entry.name;
  app.path = entry.path;
  app.source = L"file";
  app.launchType = app::LaunchType::Exe;
  app.launchTarget = entry.path;
  app.adminSupported = discovery::SupportsAdminLaunchTarget(entry.path);
  app.iconKey = entry.iconKey.empty() ? entry.path : entry.iconKey;
  app.fileIsDirectory = entry.isDirectory;
  app.fileLastWriteTime = entry.lastWriteTime;
  app.fileSize = entry.size;
  app.fileIndexedAt = entry.indexedAt;
  return app;
}

core::SearchItem Searchable(const app::AppEntry& entry) {
  core::SearchItem item;
  item.id = entry.id;
  item.path = entry.path;
  item.name = entry.name;
  item.source = L"file";
  item.kind = L"file";
  item.targetPath = entry.path;
  return item;
}

std::string FtsExpression(const std::wstring& terms) {
  std::string out;
  std::wstring token;
  auto flush = [&] {
    if (token.empty()) return;
    if (!out.empty()) out += " AND ";
    out += '"';
    for (const char ch : extensions::WideToUtf8(token)) {
      if (ch == '"') out += "\"\"";
      else out += ch;
    }
    out += "\"*";
    token.clear();
  };
  for (const wchar_t ch : terms) {
    if (std::iswalnum(ch) || ch == L'_') token.push_back(ch);
    else flush();
  }
  flush();
  return out;
}

struct QueryCancellation {
  const std::atomic<unsigned long long>& latest;
  unsigned long long generation;
  std::stop_token token;

  bool Cancelled() const {
    return token.stop_requested() ||
           latest.load(std::memory_order_acquire) != generation;
  }
};

class ScopedContentQuery {
 public:
  ScopedContentQuery(sqlite3* database, sqlite3_stmt* statement,
                      QueryCancellation& cancellation)
      : database_(database), statement_(statement) {
    sqlite3_progress_handler(database_, 1000, [](void* context) {
      return static_cast<QueryCancellation*>(context)->Cancelled() ? 1 : 0;
    }, &cancellation);
  }
  ~ScopedContentQuery() {
    sqlite3_progress_handler(database_, 0, nullptr, nullptr);
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
  }
  ScopedContentQuery(const ScopedContentQuery&) = delete;
  ScopedContentQuery& operator=(const ScopedContentQuery&) = delete;

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_;
};

}  // namespace

struct FileSearchService::Corpus {
  struct PathHashEntry {
    std::uint64_t hash = 0;
    std::size_t index = 0;
  };

  std::uint64_t generation = 0;
  std::shared_ptr<const std::vector<app::AppEntry>> files =
      std::make_shared<const std::vector<app::AppEntry>>();
  std::vector<core::PreparedSearchItem> prepared;
  std::vector<PathHashEntry> pathIndex;

  std::optional<std::size_t> FindByPath(std::wstring_view path) const {
    if (!files || pathIndex.empty()) return std::nullopt;
    const std::uint64_t hash = NormalizedPathHash(path);
    auto it = std::lower_bound(
        pathIndex.begin(), pathIndex.end(), hash,
        [](const PathHashEntry& entry, std::uint64_t target) {
          return entry.hash < target;
        });
    for (; it != pathIndex.end() && it->hash == hash; ++it) {
      if (NormalizedPathEquals((*files)[it->index].path, path)) {
        return it->index;
      }
    }
    return std::nullopt;
  }
};

FileSearchService::FileSearchService(std::filesystem::path databasePath,
                                     ResultSink sink, ErrorSink errors,
                                     ProjectionSink projection)
    : databasePath_(std::move(databasePath)),
      sink_(std::move(sink)),
      errors_(std::move(errors)),
      projectionSink_(std::move(projection)),
      corpus_(std::make_shared<Corpus>()) {}

FileSearchService::~FileSearchService() { Stop(); }

void FileSearchService::Start() {
  std::lock_guard lock(mutex_);
  if (started_) return;
  stopping_ = false;
  started_ = true;
}

void FileSearchService::Stop() {
  std::jthread toJoin;
  {
    std::lock_guard lock(mutex_);
    if (!started_ && !worker_.joinable()) {
      CloseDatabase();
      return;
    }
    stopping_ = true;
    started_ = false;
    pending_.reset();
    pendingFiles_.reset();
    pendingStorageFiles_.reset();
    toJoin = std::move(worker_);
    workerActive_ = false;
    if (toJoin.joinable()) toJoin.request_stop();
  }
  cv_.notify_all();
  if (toJoin.joinable()) toJoin.join();
  CloseDatabase();
  std::lock_guard lock(mutex_);
  stopping_ = false;
}

void FileSearchService::UpdateFiles(std::vector<app::AppEntry> files) {
  auto corpus = BuildCorpus(std::move(files), 0);
  std::lock_guard lock(mutex_);
  corpus_ = std::move(corpus);
}

bool FileSearchService::UpdateFilesAsync(std::vector<app::AppEntry> files,
                                         std::uint64_t generation) {
  std::jthread retired;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return false;
    // Hide-time clears pass an empty vector; short-circuit when already empty
    // so hiding the launcher never spawns an idle worker thread.
    if (files.empty() && !pendingFiles_ && !pendingStorageFiles_ &&
        (!corpus_ || !corpus_->files || corpus_->files->empty())) {
      return true;
    }
    pendingFiles_ = std::move(files);
    pendingFilesGeneration_ = generation;
    if (!workerActive_) {
      retired = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
    }
  }
  if (retired.joinable()) retired.join();
  cv_.notify_one();
  return true;
}

bool FileSearchService::UpdateStorageFilesAsync(
    std::vector<storage::FileIndexEntry> entries, std::uint64_t generation) {
  std::jthread retired;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return false;
    pendingStorageFiles_ = std::move(entries);
    pendingStorageFilesGeneration_ = generation;
    if (!workerActive_) {
      retired = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
    }
  }
  if (retired.joinable()) retired.join();
  cv_.notify_one();
  return true;
}

std::shared_ptr<FileSearchService::Corpus> FileSearchService::BuildCorpus(
    std::vector<app::AppEntry> files, std::uint64_t generation) {
  auto corpus = std::make_shared<Corpus>();
  corpus->generation = generation;
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    if (left.fileLastWriteTime != right.fileLastWriteTime) {
      return left.fileLastWriteTime > right.fileLastWriteTime;
    }
    return left.path < right.path;
  });
  auto sharedFiles = std::make_shared<std::vector<app::AppEntry>>(std::move(files));
  const std::size_t count = sharedFiles->size();
  corpus->prepared.reserve(count);
  corpus->pathIndex.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& entry = (*sharedFiles)[index];
    auto prepared = core::PrepareSearchItem(Searchable(entry));
    // FileSearchService passes empty recentIds and reads display fields from
    // `corpus->files[index]`, so drop duplicate strings on `prepared.item`.
    std::wstring{}.swap(prepared.item.id);
    std::wstring{}.swap(prepared.item.path);
    std::wstring{}.swap(prepared.item.name);
    std::wstring{}.swap(prepared.item.source);
    std::wstring{}.swap(prepared.item.kind);
    std::wstring{}.swap(prepared.item.targetPath);
    corpus->prepared.push_back(std::move(prepared));
    corpus->pathIndex.push_back({NormalizedPathHash(entry.path), index});
  }
  std::sort(corpus->pathIndex.begin(), corpus->pathIndex.end(),
            [](const Corpus::PathHashEntry& left,
               const Corpus::PathHashEntry& right) {
              if (left.hash != right.hash) return left.hash < right.hash;
              return left.index < right.index;
            });
  corpus->files = std::move(sharedFiles);
  return corpus;
}

bool FileSearchService::Query(FileQuery query) {
  generation_.store(query.generation, std::memory_order_release);
  std::jthread retired;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return false;
    pending_ = std::move(query);
    if (!workerActive_) {
      retired = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
    }
  }
  if (retired.joinable()) retired.join();
  cv_.notify_one();
  return true;
}

void FileSearchService::Invalidate(unsigned long long generation) {
  generation_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  pending_.reset();
  pendingFiles_.reset();
  pendingStorageFiles_.reset();
}

void FileSearchService::WorkerLoop(std::stop_token token) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    FileQuery query;
    std::vector<app::AppEntry> files;
    std::vector<storage::FileIndexEntry> storageFiles;
    std::uint64_t filesGeneration = 0;
    std::uint64_t storageFilesGeneration = 0;
    bool updateCorpus = false;
    bool projectStorageFiles = false;
    {
      std::unique_lock lock(mutex_);
      const bool woke = cv_.wait_for(lock, kIdleWorkerTimeout, [&] {
        return stopping_ || token.stop_requested() || pending_.has_value() ||
               pendingFiles_.has_value() || pendingStorageFiles_.has_value();
      });
      if (stopping_ || token.stop_requested() ||
          (!woke && !pending_.has_value() && !pendingFiles_.has_value() &&
           !pendingStorageFiles_.has_value())) {
        CloseDatabase();
        workerActive_ = false;
        return;
      }
      // Corpus publication has priority over a stale query. The next loop
      // iteration will coalesce and execute the newest query against it.
      if (pendingStorageFiles_) {
        storageFiles = std::move(*pendingStorageFiles_);
        pendingStorageFiles_.reset();
        storageFilesGeneration = pendingStorageFilesGeneration_;
        projectStorageFiles = true;
      } else if (pendingFiles_) {
        files = std::move(*pendingFiles_);
        pendingFiles_.reset();
        filesGeneration = pendingFilesGeneration_;
        updateCorpus = true;
      } else {
        query = std::move(*pending_);
        pending_.reset();
      }
    }
    // An exception escaping this thread would terminate the app, so corpus
    // work is guarded like queries; the previous corpus stays in place.
    try {
      if (projectStorageFiles) {
        files.reserve(storageFiles.size());
        for (const auto& entry : storageFiles) {
          if (!entry.path.empty() && !entry.name.empty()) {
            files.push_back(ProjectStorageEntry(entry));
          }
        }
        filesGeneration = storageFilesGeneration;
        updateCorpus = true;
      }
      if (updateCorpus) {
        auto corpus = BuildCorpus(std::move(files), filesGeneration);
        auto projected = corpus->files;
        {
          std::lock_guard lock(mutex_);
          corpus_ = std::move(corpus);
        }
        if (projectStorageFiles && projectionSink_) {
          projectionSink_(PreparedFileIndex{filesGeneration,
                                             std::move(projected)});
        }
        continue;
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
      if (projectStorageFiles && projectionSink_) {
        // Tell the UI thread this projection failed so it can retry.
        try {
          projectionSink_(PreparedFileIndex{storageFilesGeneration, nullptr});
        } catch (...) {
        }
      }
      continue;
    }
    try {
      auto result = Compute(query, token);
      if (!token.stop_requested() &&
          generation_.load(std::memory_order_acquire) == query.generation &&
          sink_) {
        sink_(std::move(result));
      }
    } catch (...) {
      if (errors_) errors_(std::current_exception());
    }
  }
}

app::ResultsCollection FileSearchService::Compute(const FileQuery& query,
                                                   std::stop_token token) {
  app::ResultsCollection result;
  result.generation = query.generation;
  const QueryCancellation cancellation{generation_, query.generation, token};
  const auto totalLimit = static_cast<std::size_t>(std::max(0, query.limit));
  if (totalLimit == 0 || cancellation.Cancelled()) return result;
  const bool empty = core::Trim(query.terms).empty();
  std::shared_ptr<const Corpus> corpus;
  {
    std::lock_guard lock(mutex_);
    corpus = corpus_;
  }
  if (!corpus || !corpus->files) return result;
  const auto& corpusFiles = *corpus->files;

  std::unordered_set<std::size_t> usedIndices;
  std::vector<app::DisplayItem> metadata;
  const std::size_t reserveCount = std::min(corpusFiles.size(), totalLimit);
  metadata.reserve(reserveCount);
  const bool trackUsed = query.contentEnabled && !empty;
  if (trackUsed) usedIndices.reserve(reserveCount);
  if (empty) {
    for (std::size_t i = 0; i < corpusFiles.size() &&
                            metadata.size() < totalLimit;
         ++i) {
      if ((i & 63u) == 0 && cancellation.Cancelled()) return result;
      metadata.push_back(Display(corpusFiles[i]));
    }
  } else {
    core::SearchOptions options;
    options.limit = totalLimit;
    options.maxWorkers = query.maxWorkers;
    options.generation = query.generation;
    options.latestGeneration = &generation_;
    const auto order = core::SearchPrepared(query.terms, corpus->prepared, {}, options);
    for (const auto index : order) {
      metadata.push_back(Display(corpusFiles[index]));
      if (trackUsed) usedIndices.insert(index);
    }
  }
  if (cancellation.Cancelled()) return result;
  if (!metadata.empty()) {
    result.flatItems.insert(result.flatItems.end(), metadata.begin(), metadata.end());
    result.sections.push_back(
        {empty ? L"Recently modified" : L"Names & paths", std::move(metadata)});
  }

  // Content cannot add a row once metadata filled the requested result limit.
  // Avoid opening SQLite and ranking FTS matches that would all be discarded.
  if (query.contentEnabled && !empty && result.flatItems.size() < totalLimit &&
      !cancellation.Cancelled()) {
    std::vector<app::DisplayItem> content;
    const auto ftsLimit = std::max<std::size_t>(40, totalLimit * 2);
    for (const auto& path : QueryContent(query, ftsLimit, token)) {
      if (result.flatItems.size() + content.size() >= totalLimit) break;
      const auto found = corpus->FindByPath(path);
      if (!found || !usedIndices.insert(*found).second) continue;
      content.push_back(Display(corpusFiles[*found], true));
    }
    if (!content.empty()) {
      result.flatItems.insert(result.flatItems.end(), content.begin(), content.end());
      result.sections.push_back({L"Content matches", std::move(content)});
    }
  }
  return result;
}

bool FileSearchService::EnsureDatabase() {
  if (database_) return true;
  const auto utf8 = extensions::WideToUtf8(databasePath_.wstring());
  if (sqlite3_open_v2(utf8.c_str(), &database_,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) !=
      SQLITE_OK) {
    if (database_) sqlite3_close(database_);
    database_ = nullptr;
    return false;
  }
  sqlite3_db_config(database_, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 512, 64);
  sqlite3_busy_timeout(database_, 3000);
  sqlite3_exec(database_, "PRAGMA cache_size=-512; PRAGMA mmap_size=0;",
               nullptr, nullptr, nullptr);
  return true;
}

void FileSearchService::CloseDatabase() {
  if (contentStatement_) {
    sqlite3_finalize(contentStatement_);
    contentStatement_ = nullptr;
  }
  if (database_) {
    sqlite3_close(database_);
    database_ = nullptr;
  }
}

std::vector<std::wstring> FileSearchService::QueryContent(
    const FileQuery& query, std::size_t limit, std::stop_token token) {
  std::vector<std::wstring> paths;
  QueryCancellation cancellation{generation_, query.generation, token};
  if (cancellation.Cancelled()) return paths;
  const std::string expression = FtsExpression(query.terms);
  if (expression.empty() || !EnsureDatabase() || cancellation.Cancelled()) {
    return paths;
  }
  if (!contentStatement_ && sqlite3_prepare_v3(
          database_,
          "SELECT f.path FROM file_content_fts c JOIN file_index f ON f.id=c.rowid "
          "WHERE file_content_fts MATCH ? ORDER BY bm25(file_content_fts) LIMIT ?;",
          -1, SQLITE_PREPARE_PERSISTENT, &contentStatement_, nullptr) != SQLITE_OK) {
    return paths;
  }
  auto* statement = contentStatement_;
  ScopedContentQuery reset(database_, statement, cancellation);
  sqlite3_bind_text(statement, 1, expression.c_str(),
                    static_cast<int>(expression.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(limit));
  while (!cancellation.Cancelled() && sqlite3_step(statement) == SQLITE_ROW) {
    const auto* raw = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
    const int bytes = sqlite3_column_bytes16(statement, 0);
    if (raw && bytes > 0) {
      paths.emplace_back(raw, raw + bytes / static_cast<int>(sizeof(wchar_t)));
    }
  }
  if (cancellation.Cancelled()) paths.clear();
  return paths;
}

}  // namespace feathercast::files
