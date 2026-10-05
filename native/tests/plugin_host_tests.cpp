#include "extension_manager.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

void CloseHandleIfSet(HANDLE& handle) {
  if (handle && handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle);
    handle = nullptr;
  }
}

std::wstring Quote(const std::wstring& value) {
  std::wstring out = L"\"";
  for (const wchar_t ch : value) {
    if (ch == L'"') out += L"\\\"";
    else out.push_back(ch);
  }
  out.push_back(L'"');
  return out;
}

class HostSession {
 public:
  HostSession(const std::wstring& hostPath, const std::wstring& dllPath) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childStdinRead = nullptr;
    HANDLE parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    const BOOL inputPipeCreated = CreatePipe(&childStdinRead, &parentStdinWrite, &inheritable, 0);
    assert(inputPipeCreated);
    const BOOL outputPipeCreated = CreatePipe(&parentStdoutRead, &childStdoutWrite, &inheritable, 0);
    assert(outputPipeCreated);
    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStdoutWrite;

    std::wstring command = Quote(hostPath) + L" " + Quote(dllPath);
    const BOOL processCreated =
        CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_);
    assert(processCreated);

    CloseHandleIfSet(childStdinRead);
    CloseHandleIfSet(childStdoutWrite);
    CloseHandleIfSet(process_.hThread);
    stdinWrite_ = parentStdinWrite;
    stdoutRead_ = parentStdoutRead;
    // The host announces itself once the plugin DLL is loaded; requests are
    // only answered after that line.
    ready_ = ReadLine(std::chrono::seconds(5));
  }

  [[nodiscard]] const std::string& ReadyLine() const { return ready_; }

  ~HostSession() {
    CloseHandleIfSet(stdinWrite_);
    CloseHandleIfSet(stdoutRead_);
    if (process_.hProcess) {
      if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 1000);
      }
      CloseHandle(process_.hProcess);
    }
  }

  std::string Send(const std::string& request, std::chrono::milliseconds timeout) {
    const std::string line = request + "\n";
    DWORD written = 0;
    const BOOL writeSucceeded =
        WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    assert(writeSucceeded);
    assert(written == line.size());
    return ReadLine(timeout);
  }

  // Bytes the host wrote beyond the lines read so far.
  [[nodiscard]] DWORD PendingBytes() const {
    DWORD available = 0;
    const BOOL peekSucceeded = PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr);
    assert(peekSucceeded);
    return available + static_cast<DWORD>(buffer_.size());
  }

 private:
  std::string ReadLine(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      if (const size_t newline = buffer_.find('\n'); newline != std::string::npos) {
        std::string response = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!response.empty() && response.back() == '\r') response.pop_back();
        return response;
      }
      if (std::chrono::steady_clock::now() >= deadline) return "";
      DWORD available = 0;
      const BOOL peekSucceeded = PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr);
      assert(peekSucceeded);
      if (available > 0) {
        std::string chunk(std::min<DWORD>(available, 64 * 1024), '\0');
        DWORD read = 0;
        const BOOL readSucceeded =
            ReadFile(stdoutRead_, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
        assert(readSucceeded);
        buffer_.append(chunk.data(), read);
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  PROCESS_INFORMATION process_{};
  HANDLE stdinWrite_ = nullptr;
  HANDLE stdoutRead_ = nullptr;
  std::string buffer_;
  std::string ready_;
};

DWORD RunAndWait(const std::wstring& hostPath, const std::wstring& dllPath) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring command = Quote(hostPath) + L" " + Quote(dllPath);
  const BOOL processCreated =
      CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  assert(processCreated);
  CloseHandleIfSet(process.hThread);
  WaitForSingleObject(process.hProcess, 5000);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandleIfSet(process.hProcess);
  return exitCode;
}

void WriteUtf8(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

bool WaitForPluginResult(feathercast::extensions::ExtensionManager& manager,
                         const std::wstring& query,
                         const std::wstring& title,
                         std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& item : manager.CachedResultsFor(query)) {
      if (item.title == title) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

template <typename Predicate>
bool WaitForHealth(feathercast::extensions::ExtensionManager& manager, Predicate predicate,
                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto health = manager.Health();
    if (health.size() == 1 && predicate(health[0])) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  assert(argc == 5);
  const std::wstring hostPath = argv[1];
  const std::wstring pluginPath = argv[2];
  const std::wstring v1PluginPath = argv[3];
  const std::wstring badApiPath = argv[4];

  {
    using feathercast::extensions::BuildHostReadyJson;
    using feathercast::extensions::FrameResponseLine;
    using feathercast::extensions::IsHostReadyJson;
    using feathercast::extensions::ParseActivationResponse;
    using feathercast::extensions::ParseQueryResponse;

    assert(IsHostReadyJson(BuildHostReadyJson(2)));
    assert(!IsHostReadyJson("{\"ready\":false}"));
    assert(!IsHostReadyJson("{\"items\":[]}"));
    assert(!IsHostReadyJson("ready"));

    // Line breaks between JSON tokens fold into spaces; inside a string they
    // are invalid JSON and must not split the response into several lines.
    assert(FrameResponseLine("{\"a\":\n1}\r\n") == std::optional<std::string>("{\"a\": 1}  "));
    assert(FrameResponseLine("{\"a\":\"x\\\\\"}\n") ==
           std::optional<std::string>("{\"a\":\"x\\\\\"} "));
    assert(!FrameResponseLine("{\"a\":\"x\ny\"}"));
    assert(!FrameResponseLine("{\"a\":\"x\\\ny\"}"));

    // Keys only match on their own object, never inside nested objects or
    // string contents, and payloads round-trip as compact JSON.
    const auto nested = ParseQueryResponse(
        "{\"items\":[{\"detail\":{\"title\":\"Inner\"},\"id\":\"x\",\"title\":\"Outer\","
        "\"payload\":{\"n\":1.5,\"s\":\"a\\nb\\u00e9\",\"list\":[true,null,-2]}}]}");
    assert(nested && nested->items.size() == 1);
    assert(nested->items[0].title == L"Outer");
    assert(nested->items[0].detailTitle == L"Inner");
    assert(nested->items[0].payloadJson ==
           "{\"n\":1.5,\"s\":\"a\\nb\xC3\xA9\",\"list\":[true,null,-2]}");
    const auto spoofed = ParseQueryResponse(
        "{\"items\":[{\"id\":\"x\",\"subtitle\":\"\\\"title\\\":\\\"Fake\\\"\"}]}");
    assert(spoofed && spoofed->items.empty());
    assert(!ParseQueryResponse("{\"items\":["));
    assert(!ParseQueryResponse("{\"other\":{\"items\":[]}}"));
    assert(!ParseActivationResponse("not json"));
    assert(!ParseActivationResponse("[]"));
    const auto nestedAction = ParseActivationResponse(
        "{\"handled\":true,\"meta\":{\"closeOverlay\":false},\"action\":{\"type\":\"openUrl\","
        "\"value\":\"https://example.com\"}}");
    assert(nestedAction && nestedAction->handled && nestedAction->closeOverlay);
    assert(nestedAction->action == feathercast::extensions::HostActionType::OpenUrl);
  }

  {
    HostSession session(hostPath, pluginPath);
    assert(session.ReadyLine() == "{\"ready\":true,\"apiVersion\":2}");
    const auto query = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                    std::chrono::seconds(2));
    assert(query.find("\"Demo Result\"") != std::string::npos);

    const auto activation = session.Send("{\"apiVersion\":1,\"type\":\"activate\",\"itemId\":\"demo\",\"payload\":{}}",
                                         std::chrono::seconds(2));
    assert(activation.find("\"copyText\"") != std::string::npos);

    const auto thrown = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"throw\",\"limit\":20}",
                                     std::chrono::seconds(2));
    assert(thrown.find("plugin-call-failed") != std::string::npos);

    const auto stillAlive = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                         std::chrono::seconds(2));
    assert(stillAlive.find("\"Demo Result\"") != std::string::npos);

    const auto huge = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"huge\",\"limit\":20}",
                                   std::chrono::seconds(2));
    assert(huge.find("plugin-call-failed") != std::string::npos);

    const auto malformed = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"malformed\",\"limit\":20}",
                                        std::chrono::seconds(2));
    assert(malformed == "{\"items\":[");

    const auto v2 = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"version\",\"limit\":20}",
                                 std::chrono::seconds(2));
    assert(v2.find("\"API v2\"") != std::string::npos);

    const auto detail = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"detail\",\"limit\":20}",
                                     std::chrono::seconds(2));
    assert(detail.find("\"detail\"") != std::string::npos);
    assert(detail.find("\"markdown\"") != std::string::npos);

    const auto setQuery = session.Send("{\"apiVersion\":2,\"type\":\"activate\",\"itemId\":\"set-query\",\"payload\":{}}",
                                       std::chrono::seconds(2));
    assert(setQuery.find("\"setQuery\"") != std::string::npos);

    // Console output and input of the plugin are isolated from the protocol.
    const auto isolated = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"isolated\",\"limit\":20}",
                                       std::chrono::seconds(2));
    assert(isolated.find("\"Isolated\"") != std::string::npos);
    const auto afterIsolated = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                            std::chrono::seconds(2));
    assert(afterIsolated.find("\"Demo Result\"") != std::string::npos);

    // A response above the old 4 KiB first buffer still runs the plugin once.
    const auto large = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"large\",\"limit\":20}",
                                    std::chrono::seconds(2));
    assert(large.size() > 64 * 1024 && large.find("\"Large\"") != std::string::npos);
    const auto largeCalls = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"large-calls\",\"limit\":20}",
                                         std::chrono::seconds(2));
    assert(largeCalls.find("calls=1") != std::string::npos);

    // Pretty-printed JSON stays one protocol line; a raw line break inside a
    // string is rejected instead of desynchronizing later responses.
    const auto multiline = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"multiline\",\"limit\":20}",
                                        std::chrono::seconds(2));
    assert(multiline.find("\"Multiline\"") != std::string::npos);
    assert(feathercast::json::Parse(multiline));
    const auto brokenLine = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"broken-line\",\"limit\":20}",
                                         std::chrono::seconds(2));
    assert(brokenLine == "{\"error\":\"invalid-response\"}");
    const auto afterFraming = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"demo\",\"limit\":20}",
                                           std::chrono::seconds(2));
    assert(afterFraming.find("\"Demo Result\"") != std::string::npos);
    assert(session.PendingBytes() == 0);
  }

  {
    HostSession session(hostPath, v1PluginPath);
    assert(session.ReadyLine() == "{\"ready\":true,\"apiVersion\":1}");
    const auto v1 = session.Send("{\"apiVersion\":2,\"type\":\"query\",\"query\":\"version\",\"limit\":20}",
                                 std::chrono::seconds(2));
    assert(v1.find("\"API v1\"") != std::string::npos);
  }

  {
    HostSession session(hostPath, pluginPath);
    const auto slow = session.Send("{\"apiVersion\":1,\"type\":\"query\",\"query\":\"slow\",\"limit\":20}",
                                   std::chrono::milliseconds(100));
    assert(slow.empty());
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastParallelExtensionTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    const auto dataDir = tempRoot / L"data";
    for (const auto& id : {L"fast", L"slow-one", L"slow-two", L"slow-three"}) {
      const auto pluginDir = dataDir / L"plugins" / id;
      std::filesystem::create_directories(pluginDir, ec);
      const auto dll = pluginDir / L"plugin.dll";
      const BOOL copied = CopyFileW(pluginPath.c_str(), dll.c_str(), FALSE);
      assert(copied);
      WriteUtf8(pluginDir / L"plugin.json",
                "{\"id\":\"" + feathercast::extensions::WideToUtf8(id) + "\",\"name\":\"Parallel Test\","
                "\"version\":\"1.0\",\"dll\":\"plugin.dll\"}");
    }

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);
    // Start every host first. The 250 ms request budget deliberately excludes
    // host startup (see the ready handshake), so timing the cold start here
    // would measure process creation instead of query isolation.
    manager.RequestQuery(L"warmup", 1);
    assert(WaitForPluginResult(manager, L"warmup", L"Demo Result", std::chrono::seconds(10)));
    // Three of the four plugins sleep for 1 s on this query. With two query
    // workers, enforced 250 ms timeouts finish in about half a second, while
    // a timeout that is not enforced would take two full sleeps (2 s).
    const auto start = std::chrono::steady_clock::now();
    manager.RequestQuery(L"parallel", 2);
    assert(WaitForPluginResult(manager, L"parallel", L"Demo Result", std::chrono::seconds(5)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(1500));
    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastExtensionManagerTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);

    const auto dataDir = tempRoot / L"data";
    const auto pluginDir = dataDir / L"plugins" / L"manager";
    std::filesystem::create_directories(pluginDir, ec);
    const auto managerDll = pluginDir / L"manager.dll";
    const BOOL pluginCopied = CopyFileW(pluginPath.c_str(), managerDll.c_str(), FALSE);
    assert(pluginCopied);
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"manager\",\"name\":\"Manager Test\",\"version\":\"1.0\",\"dll\":\"manager.dll\"}");

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);

    // Host startup is not part of the 250 ms request budget, so wait for the
    // strike instead of assuming a fixed delay.
    manager.RequestQuery(L"slow-one", 1);
    assert(WaitForHealth(manager, [](const auto& health) { return health.failureStrikes == 1; },
                         std::chrono::seconds(5)));
    {
      const auto health = manager.Health();
      assert(health.size() == 1);
      assert(health[0].available);
      assert(health[0].failureStrikes == 1);
      assert(health[0].lastError == L"plugin host timed out");
    }
    manager.RequestQuery(L"demo-after-timeout", 2);
    assert(WaitForPluginResult(manager, L"demo-after-timeout", L"Demo Result", std::chrono::seconds(10)));
    {
      const auto health = manager.Health();
      assert(health.size() == 1);
      assert(health[0].available);
      assert(health[0].failureStrikes == 0);
      assert(health[0].lastError.empty());
    }

    manager.RequestQuery(L"slow-disable-one", 3);
    assert(WaitForHealth(manager, [](const auto& health) { return health.failureStrikes == 1; },
                         std::chrono::seconds(5)));
    manager.RequestQuery(L"slow-disable-two", 4);
    assert(WaitForHealth(manager, [](const auto& health) { return health.failureStrikes == 2; },
                         std::chrono::seconds(5)));
    manager.RequestQuery(L"slow-disable-three", 5);
    assert(WaitForHealth(manager, [](const auto& health) { return !health.available; },
                         std::chrono::seconds(5)));
    manager.RequestQuery(L"demo-after-disable", 6);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(manager.CachedResultsFor(L"demo-after-disable").empty());
    {
      const auto health = manager.Health();
      assert(health.size() == 1);
      assert(!health[0].available);
      assert(health[0].failureStrikes >= 3);
      assert(!health[0].lastError.empty());
    }

    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    // An invalid JSON line restarts the host and counts as a strike; the next
    // request runs against a fresh host. Activation reuses the round-tripped
    // payload.
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastExtensionProtocolTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    const auto dataDir = tempRoot / L"data";
    const auto pluginDir = dataDir / L"plugins" / L"protocol";
    std::filesystem::create_directories(pluginDir, ec);
    const BOOL pluginCopied = CopyFileW(pluginPath.c_str(), (pluginDir / L"protocol.dll").c_str(), FALSE);
    assert(pluginCopied);
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"protocol\",\"name\":\"Protocol Test\",\"version\":\"1.0\",\"dll\":\"protocol.dll\"}");

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);
    manager.RequestQuery(L"malformed", 1);
    assert(WaitForHealth(manager, [](const auto& health) { return health.failureStrikes == 1; },
                         std::chrono::seconds(10)));
    {
      const auto health = manager.Health();
      assert(health[0].available);
      assert(health[0].lastError == L"plugin host returned invalid JSON");
    }
    manager.RequestQuery(L"isolated", 2);
    assert(WaitForPluginResult(manager, L"isolated", L"Isolated", std::chrono::seconds(10)));
    assert(manager.CachedResultsFor(L"isolated").size() == 1);
    assert(manager.Health()[0].failureStrikes == 0);

    manager.RequestQuery(L"demo", 3);
    assert(WaitForPluginResult(manager, L"demo", L"Demo Result", std::chrono::seconds(10)));
    const auto demo = manager.CachedResultsFor(L"demo");
    assert(demo.size() == 1 && demo[0].payloadJson == "{\"token\":\"abc\"}");
    const auto activation = manager.Activate(demo[0]);
    assert(activation && activation->handled);
    assert(activation->action == feathercast::extensions::HostActionType::CopyText);
    assert(activation->value == L"activated");
    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  {
    // A DLL with an unsupported API version fails the ready handshake and is
    // disabled after the first attempt instead of timing out three times.
    const auto tempRoot = std::filesystem::temp_directory_path() / L"FeatherCastExtensionReadyTests";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    const auto dataDir = tempRoot / L"data";
    const auto pluginDir = dataDir / L"plugins" / L"bad-api";
    std::filesystem::create_directories(pluginDir, ec);
    const BOOL pluginCopied = CopyFileW(badApiPath.c_str(), (pluginDir / L"bad.dll").c_str(), FALSE);
    assert(pluginCopied);
    WriteUtf8(pluginDir / L"plugin.json",
              "{\"id\":\"bad-api\",\"name\":\"Bad API\",\"version\":\"1.0\",\"dll\":\"bad.dll\"}");

    feathercast::extensions::ExtensionManager manager;
    manager.Initialize(dataDir, std::filesystem::path(hostPath).parent_path(), nullptr, 0);
    manager.RequestQuery(L"demo", 1);
    assert(WaitForHealth(manager, [](const auto& health) { return !health.available; },
                         std::chrono::seconds(10)));
    assert(manager.Health()[0].lastError == L"plugin uses an unsupported extension API version");
    manager.Shutdown();
    std::filesystem::remove_all(tempRoot, ec);
  }

  assert(RunAndWait(hostPath, badApiPath) == 5);
  return 0;
}
