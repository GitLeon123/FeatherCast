#include "app_types.hpp"
#include "background_services.hpp"
#include "core.hpp"
#include "extension_manager.hpp"
#include "file_index_service.hpp"
#include "file_search_service.hpp"
#include "persistence_service.hpp"
#include "preview_service.hpp"
#include "search_coordinator.hpp"
#include "search_pipeline.hpp"
#include "search_scope.hpp"
#include "test_framework.hpp"
#include "ui_event_queue.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr UINT kRuntimeReady = WM_APP + 80;
constexpr UINT kPluginReady = WM_APP + 81;
constexpr std::size_t kInputMessages = 512;
constexpr std::size_t kPluginQueries = 16;

enum class FloodKind : std::uint8_t {
  Search,
  Icon,
  Preview,
  Persistence,
  FileIndex,
  FileSearch,
  FileProjection,
};

struct FloodEvent {
  FloodKind kind = FloodKind::Search;
  std::uint64_t sequence = 0;
};

struct ProbeState {
  std::atomic<std::size_t> inputMessages = 0;
  std::atomic<std::size_t> animationMessages = 0;
  std::atomic<std::uint64_t> inputPostedMicros = 0;
  std::atomic<std::uint64_t> firstInputLatencyMicros =
      std::numeric_limits<std::uint64_t>::max();
  std::atomic<std::size_t> pluginNotifications = 0;
};

std::uint64_t NowMicros() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const auto seconds = now.QuadPart / frequency.QuadPart;
  const auto remainder = now.QuadPart % frequency.QuadPart;
  return static_cast<std::uint64_t>(seconds) * 1'000'000ULL +
         static_cast<std::uint64_t>(remainder) * 1'000'000ULL /
             static_cast<std::uint64_t>(frequency.QuadPart);
}

LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
  auto* state = reinterpret_cast<ProbeState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state) {
    if (message == WM_KEYDOWN && (wParam == VK_DOWN || wParam == VK_UP)) {
      state->inputMessages.fetch_add(1, std::memory_order_relaxed);
      const auto posted = state->inputPostedMicros.load(
          std::memory_order_acquire);
      if (posted != 0) {
        const auto latency = NowMicros() - posted;
        auto expected = std::numeric_limits<std::uint64_t>::max();
        state->firstInputLatencyMicros.compare_exchange_strong(
            expected, latency, std::memory_order_acq_rel,
            std::memory_order_acquire);
      }
    } else if (message == WM_TIMER) {
      state->animationMessages.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

bool DispatchOneInput(HWND window) {
  MSG message{};
  constexpr std::array<std::pair<UINT, UINT>, 3> kInputRanges = {
      std::pair{WM_KEYFIRST, WM_KEYLAST},
      std::pair{WM_MOUSEFIRST, WM_MOUSELAST},
      std::pair{WM_HOTKEY, WM_HOTKEY},
  };
  for (const auto [first, last] : kInputRanges) {
    if (!PeekMessageW(&message, window, first, last, PM_REMOVE)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
    return true;
  }
  return false;
}

void WriteUtf8(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output.good());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  assert(output.good());
}

feathercast::app::AppEntry FileApp(const std::filesystem::path& path,
                                   std::size_t index) {
  feathercast::app::AppEntry item;
  item.id = L"file:" + path.wstring();
  item.name = L"stress-report-" + std::to_wstring(index) + L".txt";
  item.path = path.wstring();
  item.source = L"file";
  item.iconKey = L"file:" + item.path;
  return item;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc >= 3);
  const std::filesystem::path pluginHostPath =
      std::filesystem::path(argv[1]);
  const std::filesystem::path pluginSourcePath =
      std::filesystem::path(argv[2]);
  assert(std::filesystem::exists(pluginHostPath));
  assert(std::filesystem::exists(pluginSourcePath));

  const auto root = std::filesystem::temp_directory_path() /
                    (L"FeatherCastPerformanceSelfTest-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  assert(!error);

  ProbeState probe;
  const std::wstring className =
      L"FeatherCastPerformanceProbe-" +
      std::to_wstring(GetCurrentProcessId());
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = ProbeWindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = className.c_str();
  assert(RegisterClassW(&windowClass) != 0);
  HWND window = CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, windowClass.hInstance,
                                nullptr);
  assert(window != nullptr);
  SetWindowLongPtrW(window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(&probe));

  std::atomic<std::uint64_t> nextSequence = 0;
  std::array<std::atomic<std::size_t>, 7> received{};
  feathercast::runtime::UiEventQueue<FloodEvent> events([&] {
    PostMessageW(window, kRuntimeReady, 0, 0);
  });
  const auto pushEvent = [&](FloodKind kind) {
    events.Push({kind, nextSequence.fetch_add(1, std::memory_order_relaxed)});
  };

  const auto indexedRoot = root / L"indexed";
  std::filesystem::create_directories(indexedRoot, error);
  assert(!error);
  for (std::size_t index = 0; index < 192; ++index) {
    const auto path = indexedRoot / (L"stress-report-" +
                                    std::to_wstring(index) + L".txt");
    WriteUtf8(path, "searchable report content for no-stutter testing\n");
  }
  const auto previewPath = root / L"preview.txt";
  WriteUtf8(previewPath,
            std::string(64 * 1024, 'x') +
                "\npreview content remains on the worker thread\n");

  const auto pluginData = root / L"plugin-data";
  const auto pluginDirectory = pluginData / L"plugins" / L"stress";
  std::filesystem::create_directories(pluginDirectory, error);
  assert(!error);
  const auto pluginPath = pluginDirectory / L"plugin.dll";
  assert(CopyFileW(pluginSourcePath.c_str(), pluginPath.c_str(), FALSE));
  WriteUtf8(pluginDirectory / L"plugin.json",
            R"({"id":"stress","name":"Performance Stress","version":"1.0","dll":"plugin.dll"})");

  feathercast::persistence::PersistenceService persistence(
      root / L"settings.json", root / L"feathercast.db",
      [&](feathercast::persistence::Event) {
        pushEvent(FloodKind::Persistence);
      });
  persistence.Start();

  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  snapshot->pool.reserve(5000);
  snapshot->searchItems.reserve(5000);
  for (std::size_t index = 0; index < 5000; ++index) {
    feathercast::app::DisplayItem display;
    display.app.id = L"app:stress:" + std::to_wstring(index);
    display.app.name = L"Stress Search Entry " + std::to_wstring(index);
    display.app.source = L"stress";
    snapshot->pool.push_back(display);

    feathercast::core::SearchItem searchItem;
    searchItem.id = display.app.id;
    searchItem.name = display.app.name;
    searchItem.kind = L"app";
    searchItem.source = L"stress";
    snapshot->searchItems.push_back(
        feathercast::core::PrepareSearchItem(searchItem));
  }
  snapshot->appItems = snapshot->pool;

  feathercast::search::SearchCoordinator search(
      [&](feathercast::app::ResultsCollection) {
        pushEvent(FloodKind::Search);
      });
  search.Start([](const feathercast::app::QueryRequest& request) {
    return feathercast::search_pipeline::ComputeResults(request);
  });

  feathercast::runtime::IconResolver icons(
      [&](feathercast::runtime::DecodedIcon) {
        pushEvent(FloodKind::Icon);
      });
  icons.Start(1, [](const std::wstring& key, std::stop_token) {
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });

  feathercast::preview::PreviewService preview(
      [&](feathercast::preview::Result) {
        pushEvent(FloodKind::Preview);
      });
  preview.Start();

  feathercast::files::FileIndexService fileIndex(
      [&](feathercast::files::IndexStatus) {
        pushEvent(FloodKind::FileIndex);
      });
  fileIndex.SetInteractive(true);
  fileIndex.Start();

  feathercast::files::FileSearchService fileSearch(
      root / L"unused-content.db",
      [&](feathercast::app::ResultsCollection) {
        pushEvent(FloodKind::FileSearch);
      },
      {},
      [&](feathercast::files::PreparedFileIndex) {
        pushEvent(FloodKind::FileProjection);
      });
  fileSearch.Start();

  feathercast::extensions::ExtensionManager extensions;
  extensions.Initialize(pluginData, pluginHostPath.parent_path(), window,
                        kPluginReady);
  extensions.SetConcurrencyLimit(1);
  extensions.SetInteractive(false);
  assert(extensions.Health().size() == 1);

  feathercast::settings::Settings settings;
  for (std::size_t index = 0; index < 64; ++index) {
    assert(persistence.SaveSettings(settings));
    assert(persistence.LoadTimers());
  }

  for (std::size_t index = 0; index < 256; ++index) {
    assert(icons.Queue(L"stress-icon-" + std::to_wstring(index)));
  }
  for (std::size_t generation = 1; generation <= 128; ++generation) {
    assert(preview.Load({generation, previewPath, L"preview"}));
  }

  std::vector<feathercast::app::AppEntry> fileApps;
  fileApps.reserve(2000);
  for (std::size_t index = 0; index < 2000; ++index) {
    fileApps.push_back(FileApp(
        indexedRoot / (L"stress-report-" + std::to_wstring(index) + L".txt"),
        index));
  }
  assert(fileSearch.UpdateFilesAsync(std::move(fileApps), 200));
  std::vector<feathercast::storage::FileIndexEntry> storageFiles;
  storageFiles.reserve(192);
  for (std::size_t index = 0; index < 192; ++index) {
    feathercast::storage::FileIndexEntry entry;
    entry.path = (indexedRoot / (L"stress-report-" +
                                 std::to_wstring(index) + L".txt"))
                     .wstring();
    entry.name = L"stress-report-" + std::to_wstring(index) + L".txt";
    entry.root = indexedRoot.wstring();
    entry.iconKey = L"file:" + entry.path;
    storageFiles.push_back(std::move(entry));
  }
  assert(fileSearch.UpdateStorageFilesAsync(std::move(storageFiles), 201));
  for (std::size_t generation = 1; generation <= 128; ++generation) {
    assert(fileSearch.Query({generation, L"stress report", 20, false, 1}));
  }

  for (std::size_t generation = 1; generation <= 256; ++generation) {
    feathercast::app::QueryRequest request;
    request.generation = generation;
    request.query = L"stress entry";
    request.scope = feathercast::search_scope::Scope::Apps;
    request.limit = 20;
    request.maxWorkers = 1;
    request.snapshot = snapshot;
    assert(search.Query(std::move(request)));
  }
  for (std::size_t generation = 1; generation <= 16; ++generation) {
    assert(fileIndex.Reconfigure(
        {generation, {indexedRoot.wstring()}, 192, false}));
  }

  probe.inputPostedMicros.store(NowMicros(), std::memory_order_release);
  for (std::size_t index = 0; index < kInputMessages; ++index) {
    assert(PostMessageW(window, WM_KEYDOWN,
                        index % 2 == 0 ? VK_DOWN : VK_UP, 0));
    assert(PostMessageW(window, WM_TIMER, 1, 0));
  }

  std::size_t pluginQueriesSent = 0;
  extensions.RequestQuery(L"stress-0", 1);
  pluginQueriesSent = 1;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(15);
  std::uint64_t maxPumpMicros = 0;
  std::size_t maxQueueDepth = 0;
  std::size_t maxBatch = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto turnStart = NowMicros();
    bool didWork = DispatchOneInput(window);

    maxQueueDepth = std::max(maxQueueDepth, events.Size());
    auto batch = events.Drain(24);
    maxBatch = std::max(maxBatch, batch.events.size());
    for (const auto& event : batch.events) {
      ++received[static_cast<std::size_t>(event.kind)];
      didWork = true;
    }

    MSG message{};
    if (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
      didWork = true;
      if (message.message == kPluginReady) {
        probe.pluginNotifications.fetch_add(1, std::memory_order_relaxed);
        if (pluginQueriesSent < kPluginQueries) {
          extensions.RequestQuery(
              L"stress-" + std::to_wstring(pluginQueriesSent),
              pluginQueriesSent + 1);
          ++pluginQueriesSent;
        }
      } else {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }

    maxPumpMicros = std::max(maxPumpMicros, NowMicros() - turnStart);
    if (!didWork) Sleep(0);

    const bool allRuntimeKindsReceived = std::all_of(
        received.begin(), received.end(),
        [](const auto& count) { return count.load() > 0; });
    if (probe.inputMessages.load() >= kInputMessages &&
        probe.animationMessages.load() >= kInputMessages &&
        probe.pluginNotifications.load() >= kPluginQueries &&
        allRuntimeKindsReceived && events.Empty()) {
      break;
    }
  }

  const auto inputLatency = probe.firstInputLatencyMicros.load();
  assert(probe.inputMessages.load() == kInputMessages);
  assert(probe.animationMessages.load() == kInputMessages);
  assert(probe.pluginNotifications.load() >= kPluginQueries);
  assert(inputLatency != std::numeric_limits<std::uint64_t>::max());
  assert(inputLatency <= 100'000);
  assert(maxBatch <= 24);
  assert(maxPumpMicros <= 33'000);
  for (const auto& count : received) assert(count.load() > 0);

  std::printf(
      "performance_self_test input_latency_us=%llu max_pump_us=%llu "
      "peak_queue=%zu max_batch=%zu events=%llu\n",
      static_cast<unsigned long long>(inputLatency),
      static_cast<unsigned long long>(maxPumpMicros), maxQueueDepth, maxBatch,
      static_cast<unsigned long long>(nextSequence.load()));

  extensions.Shutdown();
  icons.Stop();
  preview.Stop();
  fileSearch.Stop();
  fileIndex.Stop();
  search.Stop();
  persistence.Stop(true);
  events.Close();
  DestroyWindow(window);
  UnregisterClassW(className.c_str(), windowClass.hInstance);
  std::filesystem::remove_all(root, error);
  assert(!error);
  return 0;
}
