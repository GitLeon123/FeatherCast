#include "background_executor.hpp"
#include "settings.hpp"
#include "settings_io.hpp"
#include "test_framework.hpp"
#include "theme.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
  namespace settings = feathercast::settings;

  {
    const auto missing = settings::ParseSettingsDocument("");
    assert(missing.status == settings::ParseStatus::Missing);
    assert(missing.value.shortcut == L"Alt+Space");
    assert(missing.value.screenshotFullscreenShortcut == L"none");
    assert(missing.value.screenshotRegionShortcut == L"none");
    assert(missing.value.recordFullscreenShortcut == L"none");
    assert(missing.value.recordRegionShortcut == L"none");

    const auto valid =
        settings::ParseSettingsDocument(R"({"shortcut":"Ctrl+Space","maxResults":42})");
    assert(valid.status == settings::ParseStatus::Valid);
    assert(valid.value.shortcut == L"Ctrl+Space");
    assert(valid.value.maxResults == 42);
    assert(valid.documentVersion == 0);

    const auto versioned = settings::ParseSettingsDocument(
        R"({"schemaVersion":1,"shortcut":"Ctrl+Alt+Space"})");
    assert(versioned.status == settings::ParseStatus::Valid);
    assert(versioned.documentVersion == 1);
    assert(versioned.value.shortcut == L"Ctrl+Alt+Space");

    const auto current = settings::ParseSettingsDocument(
        R"({"schemaVersion":2,"fileContentIndexEnabled":true})");
    assert(current.status == settings::ParseStatus::Valid);
    assert(current.value.fileContentIndexEnabled);
    assert(current.value.screenshotFullscreenShortcut == L"none");

    const auto captureShortcuts = settings::ParseSettingsDocument(
        R"({"schemaVersion":2,"screenshotFullscreenShortcut":"Ctrl+Shift+1","screenshotRegionShortcut":"Ctrl+Shift+2","recordFullscreenShortcut":"Ctrl+Shift+3","recordRegionShortcut":"Ctrl+Shift+4"})");
    assert(captureShortcuts.status == settings::ParseStatus::Valid);
    assert(captureShortcuts.value.screenshotFullscreenShortcut ==
           L"Ctrl+Shift+1");
    assert(captureShortcuts.value.screenshotRegionShortcut == L"Ctrl+Shift+2");
    assert(captureShortcuts.value.recordFullscreenShortcut == L"Ctrl+Shift+3");
    assert(captureShortcuts.value.recordRegionShortcut == L"Ctrl+Shift+4");
    const auto captureRoundTrip = settings::ParseSettingsDocument(
        settings::SerializeSettings(captureShortcuts.value));
    assert(captureRoundTrip.value.screenshotFullscreenShortcut ==
           L"Ctrl+Shift+1");
    assert(captureRoundTrip.value.screenshotRegionShortcut == L"Ctrl+Shift+2");
    assert(captureRoundTrip.value.recordFullscreenShortcut == L"Ctrl+Shift+3");
    assert(captureRoundTrip.value.recordRegionShortcut == L"Ctrl+Shift+4");

    const auto future = settings::ParseSettingsDocument(
        R"({"schemaVersion":4,"shortcut":"DoNotLoad"})");
    assert(future.status == settings::ParseStatus::UnsupportedVersion);
    assert(future.documentVersion == 4);
    assert(future.value.shortcut == L"Alt+Space");

    assert(settings::ParseSettingsDocument(R"({"schemaVersion":1.5})").status ==
           settings::ParseStatus::Invalid);

    const auto malformed = settings::ParseSettingsDocument(R"({"shortcut":)");
    assert(malformed.status == settings::ParseStatus::Invalid);
    assert(malformed.value.shortcut == L"Alt+Space");

    const auto wrongRoot = settings::ParseSettingsDocument("[]");
    assert(wrongRoot.status == settings::ParseStatus::Invalid);

    std::string invalidUtf8 = R"({"shortcut":")";
    invalidUtf8.push_back(static_cast<char>(0xC3));
    invalidUtf8 += R"("})";
    assert(settings::ParseSettingsDocument(invalidUtf8).status ==
           settings::ParseStatus::Invalid);

    assert(settings::ParseSettings(R"({"shortcut":)").shortcut == L"Alt+Space");
    const auto serialized = settings::SerializeSettings(settings::Settings{});
    assert(serialized.find("\"schemaVersion\": 3") != std::string::npos);
    assert(serialized.find("\"screenshotFullscreenShortcut\": \"none\"") !=
           std::string::npos);
    assert(serialized.find("\"screenshotRegionShortcut\": \"none\"") !=
           std::string::npos);
    assert(serialized.find("\"recordFullscreenShortcut\": \"none\"") !=
           std::string::npos);
    assert(serialized.find("\"recordRegionShortcut\": \"none\"") !=
           std::string::npos);
  }

  {
    wchar_t tempPath[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, tempPath) > 0);
    const auto root =
        std::filesystem::path(tempPath) /
        (L"FeatherCastSettingsRecovery-" +
         std::to_wstring(GetCurrentProcessId()));
    const auto path = root / L"settings.json";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);

    const std::string original = R"({"shortcut":)";
    {
      std::ofstream file(path, std::ios::binary);
      file.write(original.data(), static_cast<std::streamsize>(original.size()));
    }

    const auto recovered = feathercast::settings_io::LoadSettingsFile(path);
    assert(recovered.status == settings::ParseStatus::Invalid);
    assert(recovered.persistenceAllowed);
    assert(!recovered.preservedPath.empty());
    assert(std::filesystem::exists(recovered.preservedPath));
    assert(!recovered.value.clipboardHistoryEnabled);
    assert(!recovered.value.fileIndexEnabled);

    {
      std::ifstream backup(recovered.preservedPath, std::ios::binary);
      std::ostringstream bytes;
      bytes << backup.rdbuf();
      assert(bytes.str() == original);
    }

    const auto emptyPath = root / L"empty-settings.json";
    {
      std::ofstream empty(emptyPath, std::ios::binary);
    }
    const auto emptyRecovered =
        feathercast::settings_io::LoadSettingsFile(emptyPath);
    assert(emptyRecovered.status == settings::ParseStatus::Invalid);
    assert(emptyRecovered.persistenceAllowed);
    assert(std::filesystem::exists(emptyRecovered.preservedPath));

    const auto futurePath = root / L"future-settings.json";
    {
      std::ofstream futureFile(futurePath, std::ios::binary);
      futureFile << R"({"schemaVersion":99,"shortcut":"Ctrl+Q"})";
    }
    const auto futureLoaded =
        feathercast::settings_io::LoadSettingsFile(futurePath);
    assert(futureLoaded.status == settings::ParseStatus::UnsupportedVersion);
    assert(!futureLoaded.persistenceAllowed);
    assert(futureLoaded.preservedPath.empty());
    assert(std::filesystem::exists(futurePath));

    const auto denied = std::make_error_code(std::errc::permission_denied);
    assert(feathercast::filesystem_semantics::ClassifyPresence(false, denied) ==
           feathercast::filesystem_semantics::Presence::Error);
    assert(feathercast::filesystem_semantics::ClassifyPresence(false, {}) ==
           feathercast::filesystem_semantics::Presence::Missing);
    assert(feathercast::filesystem_semantics::ClassifyPresence(true, {}) ==
           feathercast::filesystem_semantics::Presence::Present);

    std::filesystem::remove_all(root, ec);
    assert(!ec);
  }

  {
    feathercast::background::Executor executor;
    std::atomic<int> errorCount = 0;
    executor.Start(1, [&](std::exception_ptr failure) {
      ++errorCount;
      assert(failure);
      throw std::runtime_error("error handler failure");
    });

    std::promise<void> continued;
    auto continuedFuture = continued.get_future();
    assert(executor.Submit([](std::stop_token) {
      throw std::runtime_error("task failure");
    }));
    assert(executor.Submit([&](std::stop_token) {
      continued.set_value();
    }));

    assert(continuedFuture.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
    executor.Shutdown(true);
    assert(errorCount.load() == 1);
  }

  {
    // Shutdown is idempotent, rejects later submissions, and allows a restart.
    feathercast::background::Executor executor;
    executor.Shutdown();
    executor.Start(2);
    executor.Shutdown();
    executor.Shutdown(true);
    assert(!executor.Submit([](std::stop_token) {}));
    executor.Start(1);
    std::promise<void> ran;
    auto ranFuture = ran.get_future();
    assert(executor.Submit([&](std::stop_token) { ran.set_value(); }));
    assert(ranFuture.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
    executor.Shutdown();
  }

  {
    // A draining shutdown runs everything queued before it and rejects work
    // submitted while it is in progress.
    feathercast::background::Executor executor;
    executor.Start(1);
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::atomic<int> drained = 0;
    std::atomic<int> lateRan = 0;
    int lateAccepted = 0;
    assert(executor.Submit([releaseFuture](std::stop_token) {
      releaseFuture.wait_for(std::chrono::seconds(5));
    }));
    for (int i = 0; i < 3; ++i) {
      assert(executor.Submit([&](std::stop_token) { ++drained; }));
    }
    std::thread stopper([&] { executor.Shutdown(true); });
    bool rejected = false;
    for (int attempt = 0; attempt < 500 && !rejected; ++attempt) {
      rejected = !executor.Submit([&](std::stop_token) { ++lateRan; });
      if (!rejected) {
        ++lateAccepted;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    release.set_value();
    stopper.join();
    assert(rejected);
    assert(drained.load() == 3);
    // Tasks accepted before the shutdown began are drained too; the rejected
    // one never runs.
    assert(lateRan.load() == lateAccepted);
  }

  {
    // A non-draining shutdown escalates a draining one that is in progress:
    // queued work is dropped, the running task sees the stop request, and
    // both callers return.
    feathercast::background::Executor executor;
    executor.Start(1);
    std::atomic<bool> sawStop = false;
    std::atomic<bool> queuedRan = false;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    assert(executor.Submit([&](std::stop_token stopToken) {
      started.set_value();
      for (int i = 0; i < 500 && !stopToken.stop_requested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      sawStop = stopToken.stop_requested();
    }));
    assert(executor.Submit([&](std::stop_token) { queuedRan = true; }));
    assert(startedFuture.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
    std::thread drainer([&] { executor.Shutdown(true); });
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (!executor.Submit([](std::stop_token) {})) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    executor.Shutdown();
    drainer.join();
    assert(sawStop.load());
    assert(!queuedRan.load());
  }

  {
    // A worker may shut down its own executor without joining itself.
    feathercast::background::Executor executor;
    executor.Start(2);
    std::promise<bool> done;
    auto doneFuture = done.get_future();
    assert(executor.Submit([&](std::stop_token) {
      bool ok = true;
      try {
        executor.Shutdown();
      } catch (...) {
        ok = false;
      }
      done.set_value(ok);
    }));
    assert(doneFuture.wait_for(std::chrono::seconds(5)) ==
           std::future_status::ready);
    assert(doneFuture.get());
    assert(!executor.Submit([](std::stop_token) {}));
    executor.Shutdown();
  }

  {
    // A worker may even destroy the executor that owns it.
    auto executor = std::make_unique<feathercast::background::Executor>();
    executor->Start(1);
    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();
    assert(executor->Submit([&](std::stop_token) {
      executor.reset();
      destroyed.set_value();
    }));
    assert(destroyedFuture.wait_for(std::chrono::seconds(5)) ==
           std::future_status::ready);
    assert(!executor);
  }

  return 0;
}
