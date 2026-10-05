#pragma once

#include "extension_protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace feathercast::extensions {

struct PluginHealth {
  std::wstring id;
  std::wstring name;
  std::wstring version;
  std::filesystem::path directory;
  bool available = false;
  int failureStrikes = 0;
  std::wstring lastError;
};

class ExtensionManager {
 public:
  ExtensionManager() = default;
  ~ExtensionManager() { Shutdown(); }

  ExtensionManager(const ExtensionManager&) = delete;
  ExtensionManager& operator=(const ExtensionManager&) = delete;

  void Initialize(std::filesystem::path dataDir, std::filesystem::path exeDir, HWND notifyHwnd, UINT notifyMessage) {
    stop_.store(false);
    dataDir_ = std::move(dataDir);
    exeDir_ = std::move(exeDir);
    notifyHwnd_ = notifyHwnd;
    notifyMessage_ = notifyMessage;
    Reload();
    StartQueryWorker();
  }

  void SetInteractive(bool interactive) {
    interactive_.store(interactive, std::memory_order_release);
  }

  void SetConcurrencyLimit(std::size_t limit) {
    queryConcurrencyLimit_.store(std::max<std::size_t>(1, limit),
                                 std::memory_order_release);
  }

  void CancelQuery() {
    std::lock_guard lock(queryMutex_);
    pendingQuery_.reset();
    ++latestRequestedGeneration_;
  }

  void Shutdown() {
    {
      std::lock_guard lock(queryMutex_);
      stop_.store(true);
      pendingQuery_.reset();
    }
    queryCv_.notify_all();
    if (queryThread_.joinable()) {
      queryThread_.request_stop();
      queryThread_.join();
    }

    std::lock_guard pluginsLock(pluginsMutex_);
    for (const auto& plugin : plugins_) {
      plugin->available.store(false);
      std::lock_guard ioLock(plugin->ioMutex);
      StopProcess(*plugin);
    }
    plugins_.clear();
  }

  void Reload() {
    std::vector<std::shared_ptr<Plugin>> oldPlugins;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      oldPlugins.swap(plugins_);
    }
    for (const auto& plugin : oldPlugins) {
      plugin->available.store(false);
      std::lock_guard ioLock(plugin->ioMutex);
      StopProcess(*plugin);
    }

    auto discovery = DiscoverManifests(dataDir_, exeDir_);
    for (const auto& error : discovery.errors) Log(error);
    std::vector<std::shared_ptr<Plugin>> loaded;
    loaded.reserve(discovery.manifests.size());
    for (auto& manifest : discovery.manifests) {
      auto plugin = std::make_shared<Plugin>();
      plugin->manifest = std::move(manifest);
      loaded.push_back(std::move(plugin));
    }
    const size_t loadedCount = loaded.size();
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      plugins_ = std::move(loaded);
    }
    Log(L"Loaded " + std::to_wstring(loadedCount) + L" extension(s)");

    {
      std::lock_guard cacheLock(cacheMutex_);
      cache_.clear();
    }
  }

  void RequestQuery(std::wstring query, unsigned long long generation) {
    if (TrimWide(query).empty()) return;
    {
      std::lock_guard cacheLock(cacheMutex_);
      if (cache_.contains(query)) return;
    }

    {
      std::lock_guard queryLock(queryMutex_);
      if (runningQuery_ == query) {
        runningGeneration_ = generation;
        latestRequestedGeneration_ = generation;
        return;
      }
      if (pendingQuery_ && pendingQuery_->query == query) {
        pendingQuery_->generation = generation;
        latestRequestedGeneration_ = generation;
        return;
      }
      pendingQuery_ = PendingQuery{std::move(query), generation};
      latestRequestedGeneration_ = generation;
      lastQueryRequestTick_.store(GetTickCount64(), std::memory_order_release);
    }
    queryCv_.notify_one();
  }

  std::vector<QueryResultItem> CachedResultsFor(const std::wstring& query) const {
    std::lock_guard cacheLock(cacheMutex_);
    if (const auto found = cache_.find(query); found != cache_.end()) return found->second;
    return {};
  }

  std::vector<PluginHealth> Health() const {
    std::vector<PluginHealth> out;
    std::lock_guard pluginsLock(pluginsMutex_);
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) {
      std::lock_guard healthLock(plugin->healthMutex);
      out.push_back({
          plugin->manifest.id,
          plugin->manifest.name,
          plugin->manifest.version,
          plugin->manifest.directory,
          plugin->available.load(),
          plugin->failureStrikes.load(),
          plugin->lastError,
      });
    }
    return out;
  }

  std::optional<ActivationResponse> Activate(const QueryResultItem& item,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    std::shared_ptr<Plugin> plugin;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      for (const auto& candidate : plugins_) {
        if (candidate->manifest.id == item.pluginId) {
          plugin = candidate;
          break;
        }
      }
    }
    if (!plugin || !plugin->available.load()) return std::nullopt;

    const auto response =
        Exchange(*plugin, BuildActivateRequestJson(plugin->manifest, dataDir_, item), timeout);
    if (!response) return std::nullopt;
    return ParseActivationResponse(*response);
  }

 private:
  struct Plugin {
    Manifest manifest;
    std::atomic<bool> available = true;
    std::atomic<int> failureStrikes = 0;
    mutable std::mutex healthMutex;
    std::wstring lastError;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    std::mutex ioMutex;
  };

  struct PendingQuery {
    std::wstring query;
    unsigned long long generation = 0;
  };

  static std::wstring TrimWide(std::wstring value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
    if (first >= last) return L"";
    return std::wstring(first, last);
  }

  static void CloseHandleIfSet(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = nullptr;
    }
  }

  static std::wstring QuoteCommandArg(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring out = L"\"";
    for (const wchar_t ch : value) {
      if (ch == L'"') out += L"\\\"";
      else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
  }

  static bool ProcessRunning(HANDLE process) {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  }

  void StartQueryWorker() {
    if (queryThread_.joinable()) return;
    queryThread_ = std::jthread([this](std::stop_token stopToken) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      for (;;) {
        PendingQuery pending;
        {
          std::unique_lock lock(queryMutex_);
          queryCv_.wait(lock, [&] {
            return pendingQuery_.has_value() || stop_.load() || stopToken.stop_requested();
          });
          if (stop_.load() || stopToken.stop_requested()) return;
          pending = std::move(*pendingQuery_);
          pendingQuery_.reset();
          runningQuery_ = pending.query;
          runningGeneration_ = pending.generation;
        }

        std::vector<QueryResultItem> results;
        // Plugin hosts are deliberately idle while the user is typing.  The
        // normal search path remains responsive; extension results arrive
        // after a short quiet period and are discarded if they are stale.
        while (interactive_.load(std::memory_order_acquire) &&
               !stop_.load(std::memory_order_acquire) &&
               !stopToken.stop_requested()) {
          const ULONGLONG elapsed =
              GetTickCount64() -
              lastQueryRequestTick_.load(std::memory_order_acquire);
          if (elapsed >= kInteractiveQueryIdleMs) break;
          Sleep(static_cast<DWORD>(kInteractiveQueryIdleMs - elapsed));
        }
        try {
          results = QueryPlugins(pending.query, stopToken);
        } catch (...) {
          Log(L"plugin query coordinator failed; the launcher kept running");
        }
        {
          std::lock_guard cacheLock(cacheMutex_);
          if (cache_.size() > 32) cache_.clear();
          cache_[pending.query] = std::move(results);
        }

        bool newest = false;
        {
          std::lock_guard queryLock(queryMutex_);
          newest = runningQuery_ == pending.query && runningGeneration_ == latestRequestedGeneration_;
          if (runningQuery_ == pending.query) runningQuery_.clear();
        }
        if (newest && notifyHwnd_) PostMessageW(notifyHwnd_, notifyMessage_, 0, 0);
      }
    });
  }

  std::vector<QueryResultItem> QueryPlugins(const std::wstring& query, std::stop_token stopToken) {
    std::vector<QueryResultItem> results;
    std::vector<std::shared_ptr<Plugin>> plugins;
    {
      std::lock_guard pluginsLock(pluginsMutex_);
      plugins = plugins_;
    }

    std::mutex resultsMutex;
    std::atomic<size_t> nextPlugin = 0;
    std::vector<std::jthread> workers;
    const size_t workerCount = std::min(
        queryConcurrencyLimit_.load(std::memory_order_acquire),
        plugins.size());
    workers.reserve(workerCount);
    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
      workers.emplace_back([&](std::stop_token workerStop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try {
          for (;;) {
            const size_t pluginIndex =
                nextPlugin.fetch_add(1, std::memory_order_relaxed);
            if (pluginIndex >= plugins.size()) return;
            const auto& plugin = plugins[pluginIndex];
            if (workerStop.stop_requested() || stopToken.stop_requested() ||
                stop_.load() || !plugin->available.load()) {
              continue;
            }

            const auto response = Exchange(
                *plugin,
                BuildQueryRequestJson(plugin->manifest, dataDir_, query,
                                      kDefaultQueryLimit),
                std::chrono::milliseconds(250));
            if (!response) continue;

            auto parsed = ParseQueryResponse(*response, kDefaultQueryLimit);
            if (!parsed) {
              Log(plugin->manifest.id + L": invalid query response");
              continue;
            }
            std::lock_guard resultsLock(resultsMutex);
            for (auto& item : parsed->items) {
              item.pluginId = plugin->manifest.id;
              item.pluginName = plugin->manifest.name;
              results.push_back(std::move(item));
            }
          }
        } catch (...) {
          Log(L"plugin query worker failed; remaining plugins were skipped");
        }
      });
    }
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }

    std::sort(results.begin(), results.end(), [](const QueryResultItem& a, const QueryResultItem& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.title < b.title;
    });
    return results;
  }

  bool EnsureProcess(Plugin& plugin) {
    if (!plugin.available.load()) return false;
    if (ProcessRunning(plugin.process)) return true;

    StopProcess(plugin);

    const auto hostPath = exeDir_ / L"FeatherCastPluginHost.exe";
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStderr = nullptr;

    if (!CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0) ||
        !CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0)) {
      CloseHandleIfSet(childStdinRead);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      CloseHandleIfSet(childStdoutWrite);
      MarkUnavailable(plugin, L"failed to create plugin host pipes");
      return false;
    }

    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);
    // CreateFileW reports failure as INVALID_HANDLE_VALUE, not null. Without
    // NUL the host simply gets no stderr; it must never share the protocol
    // pipe, where diagnostics would corrupt the response stream.
    childStderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (childStderr == INVALID_HANDLE_VALUE) childStderr = nullptr;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childStdinRead;
    startup.StartupInfo.hStdOutput = childStdoutWrite;
    startup.StartupInfo.hStdError = childStderr;

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<BYTE> attributesBuffer(attributesSize);
    startup.lpAttributeList =
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    HANDLE inheritedHandles[] = {
      childStdinRead,
      childStdoutWrite,
      childStderr,
    };
    const size_t inheritedCount = childStderr ? 3 : 2;
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributesSize) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritedHandles, inheritedCount * sizeof(HANDLE), nullptr,
                                   nullptr)) {
      if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
      CloseHandleIfSet(childStdinRead);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      CloseHandleIfSet(childStdoutWrite);
      CloseHandleIfSet(childStderr);
      MarkUnavailable(plugin, L"failed to restrict plugin host handle inheritance");
      return false;
    }

    PROCESS_INFORMATION process{};
    std::wstring command = QuoteCommandArg(hostPath) + L" " + QuoteCommandArg(plugin.manifest.dllPath);
    const BOOL created = CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                            EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr,
                                        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);

    CloseHandleIfSet(childStdinRead);
    CloseHandleIfSet(childStdoutWrite);
    CloseHandleIfSet(childStderr);

    if (!created) {
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to start FeatherCastPluginHost.exe");
      return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 256ull * 1024ull * 1024ull;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      if (job) CloseHandle(job);
      TerminateProcess(process.hProcess, 0);
      CloseHandleIfSet(process.hThread);
      CloseHandleIfSet(process.hProcess);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to apply plugin host resource limits");
      return false;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      CloseHandle(job);
      TerminateProcess(process.hProcess, 0);
      CloseHandleIfSet(process.hThread);
      CloseHandleIfSet(process.hProcess);
      CloseHandleIfSet(parentStdinWrite);
      CloseHandleIfSet(parentStdoutRead);
      MarkUnavailable(plugin, L"failed to start constrained plugin host");
      return false;
    }
    CloseHandleIfSet(process.hThread);
    plugin.job = job;
    plugin.process = process.hProcess;
    plugin.stdinWrite = parentStdinWrite;
    plugin.stdoutRead = parentStdoutRead;
    return WaitForReady(plugin);
  }

  enum class ReadResult { Line, Exited, Failed, TimedOut, TooLarge, Unframed };

  static const wchar_t* ReadFailureReason(ReadResult result) {
    switch (result) {
      case ReadResult::Exited: return L"plugin host exited unexpectedly";
      case ReadResult::TimedOut: return L"plugin host timed out";
      case ReadResult::TooLarge: return L"plugin host response exceeded 1 MiB";
      case ReadResult::Unframed: return L"plugin host wrote unexpected output";
      case ReadResult::Line:
      case ReadResult::Failed: break;
    }
    return L"plugin host read failed";
  }

  // Reads exactly one protocol line. The host answers each request with one
  // line, so bytes after the line terminator mean the stream is out of sync.
  static ReadResult ReadLine(Plugin& plugin, std::chrono::steady_clock::time_point deadline,
                             std::string& line) {
    std::string buffer;
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(plugin.stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? ReadResult::Exited : ReadResult::Failed;
      }
      if (available > 0) {
        char chunk[4096];
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(chunk)));
        DWORD read = 0;
        if (!ReadFile(plugin.stdoutRead, chunk, toRead, &read, nullptr)) return ReadResult::Failed;
        buffer.append(chunk, chunk + read);
        if (const size_t newline = buffer.find('\n'); newline != std::string::npos) {
          if (newline + 1 != buffer.size()) return ReadResult::Unframed;
          buffer.resize(newline);
          if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();
          line = std::move(buffer);
          return ReadResult::Line;
        }
        if (buffer.size() > kMaxResponseBytes) return ReadResult::TooLarge;
        continue;
      }
      if (!ProcessRunning(plugin.process)) {
        // Output written right before the exit is still in the pipe.
        DWORD late = 0;
        if (PeekNamedPipe(plugin.stdoutRead, nullptr, 0, nullptr, &late, nullptr) && late > 0) continue;
        return ReadResult::Exited;
      }
      if (std::chrono::steady_clock::now() >= deadline) return ReadResult::TimedOut;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // Waits for the host's ready line. Startup failures are reported from the
  // host's exit code: a DLL without the extension API or with an unsupported
  // version can never work, so it is disabled right away instead of being
  // retried until the strike limit.
  bool WaitForReady(Plugin& plugin) {
    std::string line;
    const ReadResult result =
        ReadLine(plugin, std::chrono::steady_clock::now() + kHostStartupTimeout, line);
    if (result == ReadResult::Line) {
      if (IsHostReadyJson(line)) return true;
      RecordRequestFailure(plugin, L"plugin host sent an invalid ready line");
      return false;
    }
    if (result == ReadResult::TimedOut) {
      RecordRequestFailure(plugin, L"plugin host did not become ready");
      return false;
    }
    if (result != ReadResult::Exited) {
      RecordRequestFailure(plugin, ReadFailureReason(result));
      return false;
    }

    DWORD exitCode = STILL_ACTIVE;
    WaitForSingleObject(plugin.process, 1000);
    if (!GetExitCodeProcess(plugin.process, &exitCode)) exitCode = STILL_ACTIVE;
    switch (exitCode) {
      case kHostExitMissingExports:
        MarkUnavailable(plugin, L"plugin DLL does not export the FeatherCast extension API");
        return false;
      case kHostExitUnsupportedApi:
        MarkUnavailable(plugin, L"plugin uses an unsupported extension API version");
        return false;
      case kHostExitLoadFailed:
        RecordRequestFailure(plugin, L"plugin DLL could not be loaded");
        return false;
      default:
        RecordRequestFailure(plugin, L"plugin host exited during startup (code " +
                                         std::to_wstring(exitCode) + L")");
        return false;
    }
  }

  // Sends one request and returns the parsed response document. A line that is
  // not valid JSON means the stream can no longer be trusted, so the host is
  // restarted and the failure counts as a strike; later responses are never
  // read from a desynchronized pipe.
  std::optional<json::Value> Exchange(Plugin& plugin, const std::string& request,
                                      std::chrono::milliseconds timeout) {
    std::lock_guard ioLock(plugin.ioMutex);
    if (!EnsureProcess(plugin)) return std::nullopt;

    DWORD stale = 0;
    if (PeekNamedPipe(plugin.stdoutRead, nullptr, 0, nullptr, &stale, nullptr) && stale > 0) {
      RecordRequestFailure(plugin, ReadFailureReason(ReadResult::Unframed));
      return std::nullopt;
    }

    const std::string line = request + "\n";
    DWORD written = 0;
    if (!WriteFile(plugin.stdinWrite, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size()) {
      RecordRequestFailure(plugin, L"plugin host write failed");
      return std::nullopt;
    }

    std::string response;
    const ReadResult result =
        ReadLine(plugin, std::chrono::steady_clock::now() + timeout, response);
    if (result != ReadResult::Line) {
      RecordRequestFailure(plugin, ReadFailureReason(result));
      return std::nullopt;
    }
    auto document = json::Parse(response);
    if (!document) {
      RecordRequestFailure(plugin, L"plugin host returned invalid JSON");
      return std::nullopt;
    }
    RecordRequestSuccess(plugin);
    return document;
  }

  void RecordRequestSuccess(Plugin& plugin) {
    plugin.failureStrikes.store(0);
    std::lock_guard healthLock(plugin.healthMutex);
    plugin.lastError.clear();
  }

  void RecordRequestFailure(Plugin& plugin, const std::wstring& reason) {
    const int strikes = plugin.failureStrikes.fetch_add(1) + 1;
    {
      std::lock_guard healthLock(plugin.healthMutex);
      plugin.lastError = reason;
    }
    Log(plugin.manifest.id + L": " + reason + L" (strike " +
        std::to_wstring(strikes) + L"/3)");
    StopProcess(plugin);
    if (strikes >= 3) {
      plugin.available.store(false);
      Log(plugin.manifest.id + L": disabled after repeated plugin host failures");
    }
  }

  void MarkUnavailable(Plugin& plugin, const std::wstring& reason) {
    Log(plugin.manifest.id + L": " + reason);
    plugin.available.store(false);
    plugin.failureStrikes.store(3);
    {
      std::lock_guard healthLock(plugin.healthMutex);
      plugin.lastError = reason;
    }
    StopProcess(plugin);
  }

  void StopProcess(Plugin& plugin) {
    CloseHandleIfSet(plugin.stdinWrite);
    CloseHandleIfSet(plugin.stdoutRead);
    if (plugin.process) {
      if (WaitForSingleObject(plugin.process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(plugin.process, 0);
      }
      CloseHandleIfSet(plugin.process);
    }
    CloseHandleIfSet(plugin.job);
  }

  void Log(const std::wstring& message) const {
    if (dataDir_.empty()) return;
    std::lock_guard lock(logMutex_);
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    std::ofstream file(dataDir_ / L"extension-log.txt", std::ios::binary | std::ios::app);
    if (!file) return;
    const auto now = static_cast<long long>(std::time(nullptr));
    file << now << " " << WideToUtf8(message) << "\n";
  }

  std::filesystem::path dataDir_;
  std::filesystem::path exeDir_;
  HWND notifyHwnd_ = nullptr;
  UINT notifyMessage_ = 0;

  mutable std::mutex logMutex_;
  mutable std::mutex cacheMutex_;
  std::map<std::wstring, std::vector<QueryResultItem>> cache_;

  mutable std::mutex pluginsMutex_;
  std::vector<std::shared_ptr<Plugin>> plugins_;

  std::jthread queryThread_;
  std::mutex queryMutex_;
  std::condition_variable queryCv_;
  std::optional<PendingQuery> pendingQuery_;
  std::wstring runningQuery_;
  unsigned long long runningGeneration_ = 0;
  unsigned long long latestRequestedGeneration_ = 0;
  std::atomic<bool> stop_ = false;
  std::atomic<bool> interactive_ = false;
  std::atomic<std::size_t> queryConcurrencyLimit_ = 2;
  std::atomic<ULONGLONG> lastQueryRequestTick_ = 0;
  static constexpr ULONGLONG kInteractiveQueryIdleMs = 90;
  // Budget for starting the host and loading the plugin DLL. Request timeouts
  // start only after the ready line, so a cold start does not eat into them.
  static constexpr std::chrono::milliseconds kHostStartupTimeout{2000};
};

}  // namespace feathercast::extensions
