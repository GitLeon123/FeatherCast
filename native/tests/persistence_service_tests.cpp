#include "dpapi_scope.hpp"
#include "file_content.hpp"
#include "filesystem_semantics.hpp"
#include "persistence_service.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

std::filesystem::path TestRoot() {
  wchar_t path[MAX_PATH]{};
  assert(GetTempPathW(MAX_PATH, path) > 0);
  return std::filesystem::path(path) /
         (L"FeatherCastPersistenceServiceTests-" +
          std::to_wstring(GetCurrentProcessId()));
}

}  // namespace

int main() {
  // This test account may have no user DPAPI master key.
  feathercast::dpapi::AllowMachineScopeFallbackForTests();
  namespace persistence = feathercast::persistence;

  const auto root = TestRoot();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root);

  {
    const auto retentionDatabase = root / L"clipboard-retention.db";
    feathercast::storage::Storage storage;
    assert(storage.Open(retentionDatabase));
    const auto now = static_cast<long long>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
    constexpr long long kDay = 24LL * 60LL * 60LL;
    const auto pinnedOld = storage.AddClipboardEntry(
        L"pinned old", L"pinned old", now - 30 * kDay, 10);
    assert(pinnedOld);
    assert(storage.PinClipboard(pinnedOld->id, true, 10));
    const auto expired = storage.AddClipboardEntry(
        L"expired", L"expired", now - 8 * kDay, 10);
    const auto recent = storage.AddClipboardEntry(
        L"recent", L"recent", now - kDay, 10);
    assert(expired && recent);

    assert(storage.PruneClipboardHistory(1, 7));
    const auto retained = storage.LoadClipboardHistory(10);
    assert(retained.size() == 2);
    assert(retained.front().id == pinnedOld->id && retained.front().pinned);
    assert(retained.back().id == recent->id && !retained.back().pinned);
    assert(std::none_of(retained.begin(), retained.end(), [&](const auto& item) {
      return item.id == expired->id;
    }));

    const auto newest = storage.AddClipboardEntry(
        L"newest", L"newest", now, 1, 7);
    assert(newest);
    const auto countPruned = storage.LoadClipboardHistory(10);
    assert(countPruned.size() == 2);
    assert(countPruned.front().id == pinnedOld->id);
    assert(countPruned.back().id == newest->id);

    assert(storage.ClearClipboardHistory());
    const auto countOnlyOld = storage.AddClipboardEntry(
        L"count only old", L"count only old", now - 365 * kDay, 10);
    assert(countOnlyOld);
    assert(storage.PruneClipboardHistory(10, 0));
    const auto countOnly = storage.LoadClipboardHistory(10);
    assert(countOnly.size() == 1 && countOnly.front().id == countOnlyOld->id);
    storage.Close();
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<persistence::Event> events;
  persistence::PersistenceService service(
      root / L"settings.json", root / L"feathercast.db",
      [&](persistence::Event event) {
        {
          std::lock_guard lock(mutex);
          events.push_back(std::move(event));
        }
        cv.notify_all();
      });

  const auto missing = service.LoadSettingsForStartup();
  assert(missing.status == feathercast::settings::ParseStatus::Missing);
  const auto startup = service.LoadStorageForStartup(100, 20);
  assert(startup.opened);

  service.Start();
  feathercast::settings::Settings settings;
  settings.shortcut = L"Ctrl+Shift+Space";
  assert(service.SaveSettings(settings));
  assert(service.StoreClipboard(L"secret", L"secret", 1, 20));
  assert(service.Clear(feathercast::app::StorageOperationKind::ClearClipboard));
  const auto loadQueuedAt = std::chrono::steady_clock::now();
  assert(service.LoadFileIndexAsync(100, 77));
  assert(std::chrono::steady_clock::now() - loadQueuedAt <
         std::chrono::milliseconds(100));

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] {
      bool saved = false;
      bool stored = false;
      bool cleared = false;
      bool fileIndexLoaded = false;
      for (const auto& event : events) {
        saved = saved ||
                (std::holds_alternative<persistence::SettingsSaveCompleted>(
                     event) &&
                 std::get<persistence::SettingsSaveCompleted>(event)
                     .succeeded);
        stored = stored ||
                 (std::holds_alternative<persistence::ClipboardStored>(event) &&
                  std::get<persistence::ClipboardStored>(event).entry
                      .has_value());
        cleared =
            cleared ||
            (std::holds_alternative<persistence::StorageClearCompleted>(
                 event) &&
             std::get<persistence::StorageClearCompleted>(event).succeeded);
        fileIndexLoaded =
            fileIndexLoaded ||
            (std::holds_alternative<persistence::FileIndexLoaded>(event) &&
             std::get<persistence::FileIndexLoaded>(event).generation == 77);
      }
      return saved && stored && cleared && fileIndexLoaded;
    }));
  }

  settings.quicklinks.push_back(
      {L"docs", L"Documentation", L"https://example.com/docs"});
  assert(service.SaveSettings(settings));

  {
    // Deleting one clipboard entry emits the refreshed history without it.
    {
      std::lock_guard lock(mutex);
      events.clear();
    }
    assert(service.StoreClipboard(L"keep me", L"keep me", 2, 20));
    assert(service.StoreClipboard(L"delete me", L"delete me", 3, 20));
    std::optional<long long> deleteId;
    {
      std::unique_lock lock(mutex);
      assert(cv.wait_for(lock, std::chrono::seconds(5), [&] {
        std::size_t stored = 0;
        for (const auto& event : events) {
          const auto* entry = std::get_if<persistence::ClipboardStored>(&event);
          if (!entry || !entry->entry) continue;
          ++stored;
          if (entry->entry->text == L"delete me") deleteId = entry->entry->id;
        }
        return stored == 2;
      }));
      events.clear();
    }
    assert(deleteId);
    assert(service.DeleteClipboard(*deleteId, 20));
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return std::any_of(events.begin(), events.end(), [](const auto& event) {
        return std::holds_alternative<persistence::ClipboardLoaded>(event);
      });
    }));
    const auto loaded = std::find_if(events.begin(), events.end(), [](const auto& event) {
      return std::holds_alternative<persistence::ClipboardLoaded>(event);
    });
    const auto& history = std::get<persistence::ClipboardLoaded>(*loaded);
    assert(history.error.code == SQLITE_OK);
    assert(history.entries.size() == 1);
    assert(history.entries.front().text == L"keep me");
  }

  {
    // A file the scanner skipped as unchanged has no stored text in this
    // database, so the merge reads it again before it stores the entry.
    const auto mergeRoot = root / L"merge-root";
    std::filesystem::create_directories(mergeRoot);
    const auto mergeFile = mergeRoot / L"carried.txt";
    {
      std::ofstream output(mergeFile, std::ios::binary);
      output << "carried content";
    }
    feathercast::storage::FileIndexEntry carried;
    carried.path = mergeFile.wstring();
    carried.name = L"carried.txt";
    carried.root = mergeRoot.wstring();
    carried.lastWriteTime = 5;
    carried.size = 15;
    carried.indexedAt = 1000;
    carried.contentState =
        static_cast<int>(feathercast::file_content::State::Indexed);
    carried.contentBytes = 15;
    carried.contentUnchanged = true;
    assert(service.MergeFileIndex({carried}, {mergeRoot.wstring()},
                                  {mergeRoot.wstring()}, 100, 91, {}));
    const auto mergedEvent = [&]() -> const persistence::FileIndexMerged* {
      for (const auto& event : events) {
        if (const auto* merged =
                std::get_if<persistence::FileIndexMerged>(&event)) {
          if (merged->generation == 91) return merged;
        }
      }
      return nullptr;
    };
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5),
                       [&] { return mergedEvent() != nullptr; }));
    const auto* merged = mergedEvent();
    assert(merged->succeeded && merged->entries.size() == 1);
    assert(merged->entries.front().contentText == L"carried content");
    assert(!merged->entries.front().contentUnchanged);
  }

  service.Stop(true);
  const auto loaded =
      feathercast::settings_io::LoadSettingsFile(root / L"settings.json");
  assert(loaded.status == feathercast::settings::ParseStatus::Valid);
  assert(loaded.value.shortcut == L"Ctrl+Shift+Space");
  assert(loaded.value.quicklinks.size() == 1);
  assert(loaded.value.quicklinks[0].keyword == L"docs");

  {
    // Stop(true) still completes queued writes, but abandons a queued file
    // index merge instead of reading file contents and storing them while
    // the app exits; the next start rescans anyway.
    const auto shutdownRoot = root / L"shutdown";
    const auto mergeRoot = shutdownRoot / L"merge-root";
    std::filesystem::create_directories(mergeRoot);
    const auto mergeFile = mergeRoot / L"skipped.txt";
    {
      std::ofstream output(mergeFile, std::ios::binary);
      output << "skipped content";
    }
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool blocking = false;
    bool released = false;
    bool merged = false;
    persistence::PersistenceService shutdownService(
        shutdownRoot / L"settings.json", shutdownRoot / L"feathercast.db",
        [&](persistence::Event event) {
          std::unique_lock lock(gateMutex);
          if (std::holds_alternative<persistence::ClipboardStored>(event)) {
            blocking = true;
            gateCv.notify_all();
            gateCv.wait(lock, [&] { return released; });
          } else if (std::holds_alternative<persistence::FileIndexMerged>(
                         event)) {
            merged = true;
          }
        });
    shutdownService.Start();
    // Hold the worker inside a task so the merge and the save stay queued.
    assert(shutdownService.StoreClipboard(L"hold", L"hold", 1, 20));
    {
      std::unique_lock lock(gateMutex);
      assert(gateCv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return blocking; }));
    }
    feathercast::storage::FileIndexEntry entry;
    entry.path = mergeFile.wstring();
    entry.name = L"skipped.txt";
    entry.root = mergeRoot.wstring();
    entry.lastWriteTime = 5;
    entry.size = 15;
    entry.indexedAt = 1000;
    entry.contentState =
        static_cast<int>(feathercast::file_content::State::Indexed);
    entry.contentBytes = 15;
    entry.contentUnchanged = true;
    assert(shutdownService.MergeFileIndex({entry}, {mergeRoot.wstring()},
                                          {mergeRoot.wstring()}, 100, 92, {}));
    feathercast::settings::Settings shutdownSettings;
    shutdownSettings.shortcut = L"Alt+Shift+Space";
    assert(shutdownService.SaveSettings(shutdownSettings));

    std::thread stopper([&] { shutdownService.Stop(true); });
    // Submissions are refused once shutdown has begun, which happens only
    // after the deferrable work was told to stop.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (shutdownService.LoadTimers() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(std::chrono::steady_clock::now() < deadline);
    {
      std::lock_guard lock(gateMutex);
      released = true;
    }
    gateCv.notify_all();
    stopper.join();

    assert(!merged);
    const auto shutdownLoaded =
        feathercast::settings_io::LoadSettingsFile(shutdownRoot / L"settings.json");
    assert(shutdownLoaded.status == feathercast::settings::ParseStatus::Valid);
    assert(shutdownLoaded.value.shortcut == L"Alt+Shift+Space");
    feathercast::storage::Storage check;
    assert(check.Open(shutdownRoot / L"feathercast.db"));
    assert(check.LoadFileIndex(100).empty());
    check.Close();
  }

  {
    // Durable replacement: the data is flushed, the temp file never lingers,
    // and each failure mode reports its own status.
    using feathercast::filesystem_semantics::ReplaceFileDurably;
    using feathercast::filesystem_semantics::ReplaceStatus;
    const auto target = root / L"durable.txt";
    assert(ReplaceFileDurably(target, "first") == ReplaceStatus::Replaced);
    assert(ReplaceFileDurably(target, "second, longer") ==
           ReplaceStatus::Replaced);
    std::ifstream input(target, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    input.close();
    assert(contents == "second, longer");
    assert(!std::filesystem::exists(target.wstring() + L".tmp"));
    assert(ReplaceFileDurably(target, "") == ReplaceStatus::Replaced);
    assert(std::filesystem::file_size(target) == 0);

    const auto blocked = root / L"blocked";
    std::filesystem::create_directories(blocked);
    assert(ReplaceFileDurably(blocked, "x") == ReplaceStatus::ReplaceFailed);
    assert(std::filesystem::is_directory(blocked));
    assert(!std::filesystem::exists(blocked.wstring() + L".tmp"));
    assert(ReplaceFileDurably(root / L"no-such-dir" / L"file", "x") ==
           ReplaceStatus::CreateFailed);
  }

  std::filesystem::remove_all(root, ec);
  assert(!ec);
  return 0;
}
