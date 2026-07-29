#pragma once

#include "app_types.hpp"
#include "background_executor.hpp"
#include "settings_io.hpp"
#include "storage.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace feathercast::persistence {

struct StorageStartupState {
  bool opened = false;
  bool recoveredFromCorruption = false;
  std::filesystem::path quarantinedPath;
  storage::StorageError error;
  std::vector<storage::FileIndexEntry> files;
  std::vector<storage::ClipboardEntry> clipboard;
};

struct SettingsSaveCompleted {
  bool succeeded = false;
  std::wstring error;
};

struct FileIndexWriteCompleted {
  bool succeeded = false;
  storage::StorageError error;
};

struct FileIndexLoaded {
  std::uint64_t generation = 0;
  std::vector<storage::FileIndexEntry> entries;
  storage::StorageError error;
};

struct FileIndexMerged {
  std::uint64_t generation = 0;
  bool succeeded = false;
  std::vector<storage::FileIndexEntry> entries;
  storage::StorageError error;
};

struct ClipboardStored {
  std::optional<storage::ClipboardEntry> entry;
  storage::StorageError error;
};

struct ClipboardLoaded {
  std::vector<storage::ClipboardEntry> entries;
  storage::StorageError error;
};

struct ClipboardPruned {
  bool succeeded = false;
  storage::StorageError error;
};

struct StorageClearCompleted {
  app::StorageOperationKind kind =
      app::StorageOperationKind::ClearClipboard;
  bool succeeded = false;
  storage::StorageError error;
};

struct WorkerFailed {
  std::wstring operation;
};

using Event =
    std::variant<SettingsSaveCompleted, FileIndexWriteCompleted,
                 FileIndexLoaded, FileIndexMerged,
                 ClipboardStored, ClipboardLoaded, ClipboardPruned,
                 StorageClearCompleted, WorkerFailed>;

class PersistenceService {
 public:
  using EventSink = std::function<void(Event)>;

  PersistenceService(std::filesystem::path settingsPath,
                     std::filesystem::path databasePath,
                     EventSink eventSink = {});
  ~PersistenceService();

  PersistenceService(const PersistenceService&) = delete;
  PersistenceService& operator=(const PersistenceService&) = delete;

  settings_io::LoadResult LoadSettingsForStartup() const;
  bool SaveSettingsForStartup(const settings::Settings& settings,
                              std::wstring* error = nullptr) const;
  StorageStartupState LoadStorageForStartup(std::size_t fileLimit,
                                            std::size_t clipboardLimit,
                                            bool loadFiles = true);
  // Loads the persisted file index on demand.  This keeps startup cheap when
  // the Files scope is not used while preserving the existing synchronous API
  // used by the UI thread for a small, bounded result set.
  std::vector<storage::FileIndexEntry> LoadFileIndex(std::size_t limit);
  bool LoadFileIndexAsync(std::size_t limit, std::uint64_t generation);

  void Start();
  void Stop(bool drainPending = true);

  bool SaveSettings(settings::Settings settings);
  bool SaveSettingsAndWait(settings::Settings settings,
                           std::wstring* error = nullptr);
  bool PruneClipboard(std::size_t limit);
  bool ReplaceFileIndex(std::vector<storage::FileIndexEntry> entries);
  bool UpdateFileIndex(std::vector<storage::FileIndexEntry> entries);
  bool MergeFileIndex(std::vector<storage::FileIndexEntry> entries,
                      std::vector<std::wstring> configuredRoots,
                      std::vector<std::wstring> availableRoots,
                      std::size_t limit, std::uint64_t generation);
  bool StoreClipboard(std::wstring text, std::wstring preview,
                      long long capturedAt, std::size_t limit);
  bool LoadClipboard(std::size_t limit);
  bool Clear(app::StorageOperationKind kind);

 private:
  bool EnsureStorageOpen();
  void Emit(Event event) const;
  void EmitWorkerFailure(const std::wstring& operation) const;
  void SettingsSaveLoop(std::stop_token stopToken);

  std::filesystem::path settingsPath_;
  std::filesystem::path databasePath_;
  EventSink eventSink_;
  background::Executor executor_;
  storage::Storage storage_;

  std::mutex settingsMutex_;
  std::optional<settings::Settings> pendingSettings_;
  bool settingsSaveScheduled_ = false;
};

}  // namespace feathercast::persistence
