#include "discovery_service.hpp"
#include "app_cache.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

int main() {
  // A new process can restore complete launch metadata without shell discovery.
  feathercast::app::AppEntry cachedApp;
  cachedApp.id = L"test-app";
  cachedApp.name = L"Editor \u00e4\u65e5";
  cachedApp.path = L"C:\\Apps\\Editor.lnk";
  cachedApp.source = L"shortcut";
  cachedApp.launchType = feathercast::app::LaunchType::Exe;
  cachedApp.launchTarget = L"C:\\Apps\\Editor.exe";
  cachedApp.targetPath = cachedApp.launchTarget;
  cachedApp.args = L"--profile \"Work\"";
  cachedApp.cwd = L"C:\\Apps";
  cachedApp.appUserModelId = L"Editor.Desktop";
  cachedApp.iconKey = cachedApp.path;
  cachedApp.isGame = true;
  cachedApp.gameProvider = L"steam";
  cachedApp.adminSupported = true;
  cachedApp.systemEssential = true;
  cachedApp.keywords = {L"edit", L"\u00e4\u65e5"};
  constexpr long long savedAt = 100000;
  const auto encoded = feathercast::discovery_cache::Encode({cachedApp}, savedAt);
  const auto decoded = feathercast::discovery_cache::Decode(encoded, savedAt + 60);
  assert(decoded && decoded->size() == 1);
  // Re-encoding compares every persisted field, including Unicode and arguments.
  assert(feathercast::discovery_cache::Encode(*decoded, savedAt) == encoded);
  // Startup can use the last good list after a long shutdown or clock reset.
  assert(feathercast::discovery_cache::Decode(encoded, savedAt - 1));
  assert(feathercast::discovery_cache::Decode(encoded, savedAt + 7 * 24 * 60 * 60));
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    assert(!feathercast::discovery_cache::Decode(
        std::string_view(encoded).substr(0, length), savedAt));
  }
  assert(!feathercast::discovery_cache::Decode(encoded + "garbage", savedAt));
  auto wrongVersion = encoded;
  wrongVersion[7] = '2';
  assert(!feathercast::discovery_cache::Decode(wrongVersion, savedAt));
  const auto cachePath = std::filesystem::temp_directory_path() /
      (L"FeatherCast-app-cache-test-" + std::to_wstring(GetCurrentProcessId()));
  feathercast::discovery_cache::Save(cachePath, {cachedApp}, savedAt);
  const auto restored = feathercast::discovery_cache::Load(cachePath, savedAt + 60);
  assert(restored && restored->size() == 1);
  assert(feathercast::discovery_cache::Encode(*restored, savedAt) == encoded);
  feathercast::discovery_cache::Save(cachePath, {}, savedAt + 100);
  const auto afterEmptyScan = feathercast::discovery_cache::Load(cachePath, savedAt + 100);
  assert(afterEmptyScan &&
         feathercast::discovery_cache::Encode(*afterEmptyScan, savedAt) == encoded);
  std::filesystem::remove(cachePath);
  assert(!feathercast::discovery_cache::Load(cachePath, savedAt));

  std::mutex mutex;
  std::condition_variable cv;
  std::uint64_t published = 0;

  feathercast::discovery_runtime::DiscoveryService service(
      [&](feathercast::app::DiscoveryResult result) {
        {
          std::lock_guard lock(mutex);
          published = result.generation;
        }
        cv.notify_all();
      });
  service.Start([](const feathercast::app::DiscoveryRequest& request,
                   std::stop_token) {
    if (request.generation == 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    return std::optional{std::move(result)};
  });

  feathercast::app::DiscoveryRequest first;
  first.generation = 1;
  assert(service.Refresh(first));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  feathercast::app::DiscoveryRequest second;
  second.generation = 2;
  assert(service.Refresh(second));

  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return published == 2; }));
  }
  assert(service.IsCurrent(2));
  assert(service.Cancel() == 3);
  assert(!service.IsCurrent(2));
  service.Stop();
  assert(!service.Refresh(second));
  assert(service.IsCurrent(4));

  // Fast retries keep the test short; production waits 1 s, 2 s, 4 s, ...
  const feathercast::discovery_runtime::RetryPolicy fastRetry{
      std::chrono::milliseconds(5), std::chrono::milliseconds(20), 3};
  std::atomic<int> errors = 0;
  std::atomic<int> failedRuns = 0;
  std::uint64_t recovered = 0;
  feathercast::discovery_runtime::DiscoveryService resilient(
      [&](feathercast::app::DiscoveryResult result) {
        {
          std::lock_guard lock(mutex);
          recovered = result.generation;
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++errors;
        cv.notify_all();
      },
      fastRetry);
  resilient.Start([&](const feathercast::app::DiscoveryRequest& request,
                      std::stop_token) {
    if (request.generation == 10) {
      ++failedRuns;
      throw std::runtime_error("discovery failure");
    }
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    return std::optional{std::move(result)};
  });
  feathercast::app::DiscoveryRequest failing;
  failing.generation = 10;
  assert(resilient.Refresh(failing));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return errors == 1; }));
  }
  // A persistent failure is reported once, after every attempt, and then the
  // worker stays idle instead of retrying forever.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(failedRuns == 3 && errors == 1);
  feathercast::app::DiscoveryRequest recovery;
  recovery.generation = 11;
  assert(resilient.Refresh(recovery));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return recovered == 11; }));
  }
  resilient.Stop();

  // A cold login failure recovers by itself, without a manual second refresh.
  std::atomic<int> attempts = 0;
  std::atomic<int> retryErrors = 0;
  std::uint64_t automaticRecovery = 0;
  feathercast::discovery_runtime::DiscoveryService retrying(
      [&](feathercast::app::DiscoveryResult result) {
        {
          std::lock_guard lock(mutex);
          automaticRecovery = result.generation;
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) { ++retryErrors; }, fastRetry);
  retrying.Start([&](const feathercast::app::DiscoveryRequest& request,
                     std::stop_token) {
    if (++attempts <= 2) throw std::runtime_error("shell starting");
    assert(!request.allowCache);
    feathercast::app::DiscoveryResult result;
    result.generation = request.generation;
    result.apps.push_back(cachedApp);
    return std::optional{std::move(result)};
  });
  feathercast::app::DiscoveryRequest coldLogin;
  coldLogin.generation = 20;
  coldLogin.allowCache = true;
  assert(retrying.Refresh(coldLogin));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(6),
                       [&] { return automaticRecovery == 20; }));
  }
  // Transient login failures recover quietly, without a "task failed" alert.
  assert(attempts == 3 && retryErrors == 0);
  retrying.Stop();

  // Stop interrupts a production-length retry wait promptly.
  std::atomic<int> shutdownErrors = 0;
  std::atomic<int> shutdownAttempts = 0;
  feathercast::discovery_runtime::DiscoveryService stopping(
      {}, [&](std::exception_ptr) { ++shutdownErrors; });
  stopping.Start([&](const feathercast::app::DiscoveryRequest&,
                     std::stop_token) -> std::optional<feathercast::app::DiscoveryResult> {
    {
      std::lock_guard lock(mutex);
      ++shutdownAttempts;
    }
    cv.notify_all();
    throw std::runtime_error("shell unavailable");
  });
  coldLogin.generation = 30;
  assert(stopping.Refresh(coldLogin));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return shutdownAttempts == 1; }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto stopStarted = std::chrono::steady_clock::now();
  stopping.Stop();
  assert(std::chrono::steady_clock::now() - stopStarted < std::chrono::milliseconds(500));
  assert(shutdownAttempts == 1 && shutdownErrors == 0);
  return 0;
}
