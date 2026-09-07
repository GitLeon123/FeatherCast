#include "timers.hpp"
#include "timer_service.hpp"
#include "timer_items.hpp"
#include "snapshot_memory.hpp"
#include "storage.hpp"
#include "search_pipeline.hpp"
#include "settings_catalog.hpp"
#include "command_catalog.hpp"
#include "file_index_service.hpp"
#include "background_services.hpp"
#include "shortcut.hpp"
#include "test_framework.hpp"

#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>

using namespace feathercast;

void Sql(const std::filesystem::path& path, const char* sql) {
  sqlite3* database = nullptr;
  assert(sqlite3_open16(path.c_str(), &database) == SQLITE_OK);
  assert(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
}

int main() {
  auto parsed = timers::Parse(L"timer 1h 30m 2s Tea");
  assert(parsed && parsed->action == timers::Action::Create && parsed->duration == 5402000 && parsed->name == L"Tea");
  assert(timers::Parse(L"timer 2m30s")->duration == 150000);
  assert(!timers::Parse(L"timers"));
  for (const auto* invalid : {L"timer", L"timer 0s", L"timer -1m", L"timer 1.5h", L"timer 9999999999999999999999999h", L"timer 30", L"timer 5monkeys"}) {
    assert(timers::Parse(invalid)->action == timers::Action::Invalid);
  }
  timers::State state;
  const auto request = *timers::Parse(L"timer 2s Tea");
  assert(timers::Apply(state, request, 1000, 10));
  const auto id = state.timers.front().id;
  assert(timers::Apply(state, {timers::Action::Pause, id}, 2000, 20));
  assert(state.timers.front().remaining == 1000);
  assert(!timers::NextDeadline(state));
  assert(timers::Expire(state, 9000).empty());
  assert(timers::Apply(state, {timers::Action::Resume, id}, 9000, 30));
  assert(*timers::NextDeadline(state) == 10000);
  assert(timers::Apply(state, request, 8000, 30));
  assert(timers::Expire(state, 10001).size() == 2);
  assert(timers::Expire(state, 10002).empty());
  assert(timers::Apply(state, {timers::Action::Restart, id}, 10002, 30));
  assert(state.timers.front().deadline == 12002);
  assert(timers::Apply(state, {timers::Action::StopwatchStart}, 0, 100));
  assert(state.stopwatch.Elapsed(5100) == 5000);
  assert(timers::Apply(state, {timers::Action::StopwatchPause}, 0, 5100));
  assert(state.stopwatch.Elapsed(8000) == 5000);
  assert(timers::Apply(state, {timers::Action::StopwatchStart}, 0, 9000));
  assert(state.stopwatch.Elapsed(10000) == 6000);

  const auto root = std::filesystem::temp_directory_path() / (L"FeatherCastBehaviorTests-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(root);
  const auto database = root / L"state.db";
  storage::Storage store;
  assert(store.Open(database));
  assert(store.SaveTimers(state));
  auto loaded = store.LoadTimers();
  assert(loaded.timers.size() == 2 && loaded.timers.front().deadline == 12002);
  assert(!loaded.stopwatch.running && loaded.stopwatch.elapsed == 5000);
  assert(timers::Expire(loaded, 50000).size() == 1);
  assert(store.SaveTimers(loaded));
  assert(timers::Expire(loaded = store.LoadTimers(), 60000).empty());
  Sql(database, "CREATE TRIGGER reject_timer_write BEFORE INSERT ON timers BEGIN SELECT RAISE(ABORT,'test failure'); END;");
  assert(!store.SaveTimers(state));
  assert(store.LoadTimers().timers.front().phase == timers::Phase::Finished);
  Sql(database, "DROP TRIGGER reject_timer_write;");

  auto first = store.AddClipboardEntry(L"favorite", L"favorite", 1, 2);
  assert(first && store.PinClipboard(first->id, true, 2));
  for (int i = 0; i < 4; ++i) assert(store.AddClipboardEntry(std::to_wstring(i), L"sample", i + 2, 2));
  auto history = store.LoadClipboardHistory(2);
  assert(history.size() == 3 && history.front().id == first->id && history.front().pinned);
  auto duplicate = store.AddClipboardEntry(L"favorite", L"favorite", 20, 2);
  assert(duplicate && duplicate->id == first->id && duplicate->pinned);
  assert(store.PruneClipboardHistory(1));
  assert(store.LoadClipboardHistory(1).size() == 2);
  assert(store.PinClipboard(first->id, false, 1));
  assert(store.LoadClipboardHistory(1).size() == 1);
  assert(store.ClearClipboardHistory());
  for (int i = 0; i < 100; ++i) {
    auto entry = store.AddClipboardEntry(std::to_wstring(i), L"favorite", i, 1);
    assert(entry && store.PinClipboard(entry->id, true, 1));
  }
  auto extra = store.AddClipboardEntry(L"overflow favorite", L"overflow", 101, 1);
  assert(extra && !store.PinClipboard(extra->id, true, 1));
  assert(store.LoadClipboardHistory(1).size() == 101);
  assert(store.ClearClipboardHistory() && store.LoadClipboardHistory(1).empty());
  const auto legacyEntry = store.AddClipboardEntry(L"preserved migration text", L"preserved", 123, 5);
  assert(legacyEntry);
  store.Close();
  Sql(database, "ALTER TABLE clipboard_history DROP COLUMN pinned; DROP TABLE timers; DROP TABLE stopwatch; PRAGMA user_version=3;");
  assert(store.Open(database));
  assert(std::filesystem::exists(database.wstring() + L".pre-v4.bak"));
  assert(store.LoadTimers().timers.empty());
  const auto migratedHistory = store.LoadClipboardHistory(5);
  assert(migratedHistory.size() == 1 && migratedHistory.front().id == legacyEntry->id);
  assert(migratedHistory.front().text == L"preserved migration text" && !migratedHistory.front().pinned);
  assert(store.AddClipboardEntry(L"after migration", L"after migration", 1, 5));
  store.Close();

  app::QueryRequest query;
  query.limit = 50;
  query.query = L"animations";
  const auto settings = search_pipeline::ComputeResults(query);
  bool animation = false;
  for (const auto& item : settings.flatItems) {
    for (const auto& descriptor : settings_catalog::Catalog()) {
      if (item.settingId == descriptor.stableId && descriptor.hit == app::HitType::AnimationLevel) animation = true;
    }
  }
  assert(animation);
  query.query = L"timer 5m Test";
  const auto timerSearch = search_pipeline::ComputeResults(query);
  assert(std::any_of(timerSearch.flatItems.begin(), timerSearch.flatItems.end(), [](const auto& item) { return item.timerRequest && item.timerRequest->action == timers::Action::Create; }));
  assert(timers::Items(state, 10000, 10000).size() == state.timers.size() + 2);
  assert(timers::Actions(state, id).size() == 3);
  app::DisplayItem clip;
  clip.isClipboard = true;
  clip.clipboard = {L"1", L"text", L"preview", 1, true};
  const auto actions = commands::BuildActions(clip, {});
  assert(actions.back().action == app::ActionKind::UnpinClipboard);
  assert(std::get<app::ClipboardEntry>(actions.back().actionTarget).id == L"1");

  app::SearchSnapshot snapshot;
  snapshot.pool.push_back(clip);
  const auto small = search::SnapshotBytes(snapshot);
  snapshot.pool.front().clipboard.text.resize(9 * 1024 * 1024, L'x');
  assert(search::SnapshotBytes(snapshot) > 16 * 1024 * 1024);
  assert(small < 16 * 1024 * 1024);

  std::mutex mutex;
  std::condition_variable cv;
  std::uint64_t dueGeneration = 0;
  timers::TimerService service([&](timers::Due due) {
    { std::lock_guard lock(mutex); dueGeneration = due.generation; }
    cv.notify_all();
  });
  assert(service.Start());
  assert(service.Schedule(timers::Now() + 200, 1));
  assert(service.Schedule(std::nullopt, 2));
  {
    std::unique_lock lock(mutex);
    assert(!cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return dueGeneration != 0; }));
  }
  assert(service.Schedule(timers::Now() + 20, 3));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return dueGeneration == 3; }));
  }
  service.Stop();

  std::uint64_t indexGeneration = 0;
  files::FileIndexService index([&](files::IndexStatus result) {
    { std::lock_guard lock(mutex); indexGeneration = result.generation; }
    cv.notify_all();
  });
  index.Start();
  assert(index.Reconfigure({1, {root.wstring()}, 5, false}));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return indexGeneration == 1; }));
  }
  index.Pause();
  assert(!index.IsCurrent(1));
  std::ofstream(root / L"changed.txt") << "new";
  assert(index.Reconfigure({2, {root.wstring()}, 5, false}));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return indexGeneration == 2; }));
  }
  index.Pause();
  index.Stop();
  std::filesystem::remove_all(root);
  return 0;
}
