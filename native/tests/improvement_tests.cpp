#include "command_catalog.hpp"
#include "file_index_service.hpp"
#include "file_transfer.hpp"
#include "phone_messages.hpp"
#include "phone_store.hpp"
#include "search_pipeline.hpp"
#include "search_preferences.hpp"
#include "settings.hpp"
#include "automation.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

int main() {
  using namespace feathercast;
  auto snapshot = std::make_shared<app::SearchSnapshot>();
  app::DisplayItem appItem;
  appItem.app.id = L"app:vue";
  appItem.app.name = L"Vue Editor";
  appItem.app.source = L"start-menu";
  appItem.app.launchType = app::LaunchType::Exe;
  snapshot->pool.push_back(appItem);
  core::SearchItem searchApp;
  searchApp.id = appItem.app.id;
  searchApp.name = appItem.app.name;
  searchApp.kind = L"app";
  snapshot->searchItems.push_back(core::PrepareSearchItem(searchApp));
  const std::map<std::wstring, std::wstring> aliases{{L"volume-up", L"vu"}, {L"volume-down", L"2+2"}};
  for (const auto& command : commands::BuildCommandItems(aliases)) {
    snapshot->pool.push_back(command);
    snapshot->searchItems.push_back(core::PrepareSearchItem(commands::BuildSearchItem(command, aliases)));
  }
  app::QueryRequest request;
  request.snapshot = snapshot;
  request.limit = 100;
  request.query = L"vu";
  auto result = search_pipeline::ComputeResults(request);
  assert(!result.flatItems.empty());
  assert(result.flatItems.front().commandStableId == L"volume-up");

  request.query = L"2+2";
  result = search_pipeline::ComputeResults(request);
  assert(result.flatItems.front().commandStableId == L"volume-down");
  request.query = L"vol";
  result = search_pipeline::ComputeResults(request);
  assert(std::any_of(result.flatItems.begin(), result.flatItems.end(), [](const auto& item) {
    return item.commandStableId == L"volume-up";
  }));
  request.query = L"vu";
  request.preferredInvocationKey = appItem.InvocationKey();
  result = search_pipeline::ComputeResults(request);
  assert(result.flatItems.front().commandStableId == L"volume-up");

  app::Settings settings;
  search_preferences::Remember(settings, L"vue", appItem);
  assert(settings.learnedQueryActions.empty());
  settings.searchLearningEnabled = true;
  search_preferences::Remember(settings, L"Vue", appItem);
  assert(settings.learnedQueryActions.at(L"vue") == appItem.InvocationKey());
  search_preferences::Remember(settings, L"@files private", appItem);
  assert(settings.learnedQueryActions.size() == 1);
  const auto loaded = settings::ParseSettingsDocument(settings::SerializeSettings(settings));
  assert(loaded.value.searchLearningEnabled);
  assert(loaded.value.learnedQueryActions == settings.learnedQueryActions);
  settings.recentApps.push_back(appItem.app.id);
  settings.usageStats[appItem.app.id] = {10, 123};
  search_preferences::Reset(settings, appItem);
  assert(settings.learnedQueryActions.empty());
  assert(settings.recentApps.empty());
  assert(settings.usageStats.empty());

  const auto photoEvent = phone::ParseSessionMessage(
      *json::Parse(R"({"type":"photos.list","access":"denied","error":"Allow photo access","items":[]})"), {}, 42);
  assert(photoEvent);
  phone::PhoneStore phoneStore;
  phoneStore.Apply(*photoEvent, 42);
  assert(phoneStore.PhotosError() == "Allow photo access");
  assert(phoneStore.PhotosAccess() == "denied");

  const auto root = std::filesystem::temp_directory_path() /
      (L"FeatherCastCoverageTest-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(root);
  const auto script = root / L"example.ps1";
  std::ofstream(script) << "Write-Output 'example'";
  const settings::Quicklink scriptItem{L"example", L"Example script", script.wstring()};
  assert(!automation::Validate(automation::Kind::Script, scriptItem));
  assert(automation::Validate(automation::Kind::Script, {L"bad", L"", L"relative.ps1"}));
  const settings::Quicklink workspace{L"work", L"Work", L"https://example.com\n" + root.wstring()};
  assert(!automation::Validate(automation::Kind::Workspace, workspace));
  assert(automation::Validate(automation::Kind::Workspace, {L"bad", L"", L"javascript:alert(1)"}));
  assert(automation::Validate(automation::Kind::Workspace, {L"bad", L"", script.wstring()}));
  assert(!automation::ValidateShortcut(L"Ctrl+Alt+V"));
  assert(automation::ValidateShortcut(L"V"));
  assert(automation::ValidateShortcut(L"Win+V"));
  settings.scripts = {scriptItem};
  settings.workspaces = {workspace};
  settings.commandShortcuts = {{L"volume-up", L"Ctrl+Alt+V"}};
  const auto automationSettings = settings::ParseSettingsDocument(settings::SerializeSettings(settings));
  assert(automationSettings.value.scripts.front().target == script.wstring());
  assert(automationSettings.value.workspaces.front().target == workspace.target);
  assert(automationSettings.value.commandShortcuts == settings.commandShortcuts);
  std::filesystem::remove(script);
  {
    const auto transferRoot = root / L"transfers";
    phone::transfer::Receiver receiver(transferRoot.wstring());
    const auto begin = *json::Parse(R"({"type":"file.begin","id":"transfer-1","name":"movie.mp4","purpose":"file","size":3})");
    assert(receiver.Begin(begin, "movie.mp4").error.empty());
    auto chunk = *json::Parse(R"({"type":"file.chunk","id":"transfer-1","offset":0})");
    assert(receiver.Chunk(chunk, {1, 2, 3}).error.empty());
    const auto saved = receiver.Finish("transfer-1");
    assert(saved.complete && std::filesystem::file_size(saved.path) == 3);
    assert(receiver.Begin(begin, "movie.mp4").error.empty());
    chunk = *json::Parse(R"({"type":"file.chunk","id":"transfer-1","offset":1})");
    assert(!receiver.Chunk(chunk, {1}).error.empty());
    assert(!receiver.Finish("transfer-1").complete);
    assert(receiver.Begin(begin, "../escape").error.size() > 0);
    assert(receiver.Begin(begin, "movie.mp4").error.empty());
    receiver.Cancel();
    assert(!receiver.Finish("transfer-1").complete);
    const auto largeSize = 41LL * 1024 * 1024;
    const auto largeBegin = *json::Parse(phone::Json("file.begin").Str("id", "large-file")
        .Str("name", "large.bin").Str("purpose", "file").Int("size", largeSize).Build());
    assert(receiver.Begin(largeBegin, "large.bin").error.empty());
    const phone::Bytes block(phone::transfer::kChunkBytes, 0x5A);
    for (long long offset = 0; offset < largeSize; offset += static_cast<long long>(block.size())) {
      const auto message = *json::Parse(phone::Json("file.chunk").Str("id", "large-file").Int("offset", offset).Build());
      assert(receiver.Chunk(message, block).error.empty());
    }
    const auto largeSaved = receiver.Finish("large-file");
    assert(largeSaved.complete && std::filesystem::file_size(largeSaved.path) == largeSize);
    assert(receiver.Begin(begin, "movie.mp4").error.empty());
    receiver.RequestCancel();
    receiver.Maintain();
    assert(!receiver.Finish("transfer-1").complete);
    for (const auto& file : std::filesystem::directory_iterator(transferRoot)) {
      assert(file.path().extension() != L".part");
    }
    std::filesystem::remove_all(transferRoot);
  }
  for (int i = 0; i < 8; ++i) {
    std::ofstream(root / (std::to_wstring(i) + L".txt")) << "coverage";
  }
  std::promise<files::IndexStatus> completed;
  auto future = completed.get_future();
  std::atomic<bool> delivered = false;
  files::FileIndexService index([&](files::IndexStatus status) {
    if (!delivered.exchange(true)) completed.set_value(std::move(status));
  });
  index.Start();
  files::IndexRequest indexRequest;
  indexRequest.generation = 1;
  indexRequest.roots = {root.wstring()};
  indexRequest.limit = 3;
  assert(index.Reconfigure(indexRequest));
  assert(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  const auto coverage = future.get();
  index.Stop();
  assert(coverage.entries.size() == 3);
  assert(coverage.discoveredEntries == 8);
  assert(coverage.limitReached);
  assert(coverage.message.find(L"3 of 8") != std::wstring::npos);
  std::filesystem::remove_all(root);
}
