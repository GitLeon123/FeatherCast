#include <windows.h>
#include <sddl.h>
#include "input_broker_protocol.hpp"
#include "input_broker_task.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr ULONG_PTR kOurInputTag = 0x4D59415050; // "MYAPP"
const std::wstring kPipeName = feathercast::input_broker::PipeName();
const std::wstring kBrokerMutexName = feathercast::input_broker::MutexName();
constexpr const wchar_t* kBrokerClassName = L"FeatherCastBrokerMsgClass";
constexpr UINT WM_APP_TRIGGER_ACTIVATION = WM_APP + 1;

using BrokerRegistration = feathercast::input_broker::Registration;
using BrokerCommand = feathercast::input_broker::Command;

struct WinState {
  bool leftDown = false;
  bool rightDown = false;
  bool hadOtherKey = false;
};

std::wstring GetProcessIntegrityLevel(HANDLE process);

// Diagnostic log. The keyboard hook runs on the message-loop thread and must
// return well inside the system hook timeout, so no caller does file I/O:
// lines go into a bounded ring under a very short lock and a dedicated thread
// writes them out. Overflow drops lines (and says so) instead of blocking.
class BrokerLog {
 public:
  void Start() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
  }

  // Writes everything still queued, then ends the writer thread.
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

  void Post(const wchar_t* text) {
    if (!text || !text[0]) return;
    {
      std::lock_guard lock(mutex_);
      Enqueue(text);
    }
    wake_.notify_one();
  }

  // For the keyboard hook: never waits, drops the line if the queue is busy.
  void TryPost(const wchar_t* text) {
    if (!text || !text[0]) return;
    {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      Enqueue(text);
    }
    wake_.notify_one();
  }

 private:
  static constexpr std::size_t kCapacity = 256;
  static constexpr std::size_t kLineChars = 192;
  struct Line {
    wchar_t text[kLineChars];
  };

  // Caller holds mutex_.
  void Enqueue(const wchar_t* text) {
    if (count_ == kCapacity) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    Line& line = ring_[(head_ + count_) % kCapacity];
    wcsncpy_s(line.text, text, _TRUNCATE);
    ++count_;
  }

  void Run() {
    std::vector<Line> batch;
    batch.reserve(kCapacity);
    for (;;) {
      bool stopping = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stop_ || count_ > 0; });
        batch.clear();
        while (count_ > 0) {
          batch.push_back(ring_[head_]);
          head_ = (head_ + 1) % kCapacity;
          --count_;
        }
        stopping = stop_;
      }
      try {
        const unsigned dropped = dropped_.exchange(0, std::memory_order_relaxed);
        if (dropped != 0) {
          Line note{};
          swprintf_s(note.text, L"[Broker] %u log lines dropped\n", dropped);
          batch.push_back(note);
        }
        Write(batch);
      } catch (...) {
      }
      if (stopping) return;
    }
  }

  static void Write(const std::vector<Line>& lines) {
    if (lines.empty()) return;
    // An elevated broker must not open log files in a user-writable directory.
    static const auto integrity = GetProcessIntegrityLevel(GetCurrentProcess());
    FILE* file = nullptr;
    if (integrity != L"HIGH" && integrity != L"SYSTEM") {
      wchar_t local[MAX_PATH]{};
      const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
      if (length != 0 && length < MAX_PATH) {
        const std::wstring path = std::wstring(local) + L"\\FeatherCast\\broker.log";
        if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0) file = nullptr;
      }
    }
    for (const Line& line : lines) {
      if (file) fputws(line.text, file);
      OutputDebugStringW(line.text);
    }
    if (file) fclose(file);
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::array<Line, kCapacity> ring_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  bool stop_ = false;
  std::atomic<unsigned> dropped_{0};
};

BrokerLog g_brokerLog;

// Ends the log thread (after flushing) on every exit path of wWinMain.
struct BrokerLogScope {
  BrokerLogScope() { g_brokerLog.Start(); }
  ~BrokerLogScope() { g_brokerLog.Stop(); }
  BrokerLogScope(const BrokerLogScope&) = delete;
  BrokerLogScope& operator=(const BrokerLogScope&) = delete;
};

void AppendBrokerLog(const wchar_t* text) { g_brokerLog.Post(text); }
void AppendBrokerLogFromHook(const wchar_t* text) { g_brokerLog.TryPost(text); }

std::wstring ProcessPath(DWORD pid) {
  if (pid == 0) return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  // Long paths do not fit MAX_PATH; grow the buffer instead of rejecting them.
  std::wstring buffer(MAX_PATH, L'\0');
  std::wstring result;
  for (;;) {
    DWORD size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
      buffer.resize(size);
      result = std::move(buffer);
      break;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) break;
    buffer.resize(buffer.size() * 2);
  }
  CloseHandle(process);
  return result;
}

// Windows paths are case-insensitive: compare ordinally, ignoring case, so a
// differently-cased but identical path is still recognised (and nothing
// locale-dependent can make two different paths equal).
bool PathsEqualIgnoreCase(const std::wstring& left, const std::wstring& right) {
  if (left.empty() || right.empty() || left.size() > INT_MAX || right.size() > INT_MAX) {
    return false;
  }
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                              right.c_str(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

std::wstring ProcessName(DWORD pid) {
  if (pid == 0) return L"unknown";
  const std::wstring full = ProcessPath(pid);
  if (!full.empty()) {
    return std::filesystem::path(full).filename().wstring();
  }
  return L"unknown";
}

std::wstring GetProcessIntegrityLevel(HANDLE process) {
  if (!process) return L"UNKNOWN";
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    return L"UNKNOWN";
  }
  DWORD len = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
  if (len == 0) {
    CloseHandle(token);
    return L"UNKNOWN";
  }
  std::vector<BYTE> buf(len);
  if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(), len, &len)) {
    CloseHandle(token);
    return L"UNKNOWN";
  }
  CloseHandle(token);
  auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
  if (!til || !til->Label.Sid) return L"UNKNOWN";
  UCHAR* countPtr = GetSidSubAuthorityCount(til->Label.Sid);
  if (!countPtr || *countPtr == 0) return L"UNKNOWN";
  DWORD rid = *GetSidSubAuthority(til->Label.Sid, static_cast<DWORD>(*countPtr - 1));
  if (rid < SECURITY_MANDATORY_LOW_RID) return L"UNTRUSTED";
  if (rid < SECURITY_MANDATORY_MEDIUM_RID) return L"LOW";
  if (rid < SECURITY_MANDATORY_HIGH_RID) return L"MEDIUM";
  if (rid < SECURITY_MANDATORY_SYSTEM_RID) return L"HIGH";
  return L"SYSTEM";
}

std::mutex g_appMutex;
BrokerRegistration g_appReg{};
std::atomic<bool> g_appConnected{false};
std::atomic<bool> g_winShortcutEnabled{false};
std::atomic<unsigned> g_shortcutGeneration{0};
std::atomic<HANDLE> g_activePipe{INVALID_HANDLE_VALUE};

WinState g_win;
HHOOK g_hook = nullptr;
DWORD g_mainThreadId = 0;
HWND g_msgHwnd = nullptr;
std::atomic<bool> g_stopPipeServer{false};

void TriggerAndActivateApp() {
  BrokerRegistration reg{};
  {
    std::lock_guard lock(g_appMutex);
    if (!feathercast::input_broker::ShouldHandleWinKey(g_appConnected.load(), g_winShortcutEnabled.load())) {
      AppendBrokerLog(L"[Broker] Trigger skipped: no app connected\n");
      return;
    }
    reg = g_appReg;
  }

  HWND appHwnd = reinterpret_cast<HWND>(reg.hwnd);
  if (!appHwnd || !IsWindow(appHwnd)) return;

  // 1. Post trigger message directly to FeatherCast window
  PostMessageW(appHwnd, WM_APP + 25, 0, 0); // WM_APP_WINKEY_TRIGGER

  // 2. Perform foreground activation
  HWND fg = GetForegroundWindow();
  DWORD fgThread = (fg && IsWindow(fg)) ? GetWindowThreadProcessId(fg, nullptr) : 0;
  DWORD appThread = reg.threadId ? reg.threadId : GetWindowThreadProcessId(appHwnd, nullptr);
  DWORD brokerThread = GetCurrentThreadId();

  bool attachedFg = false;
  bool attachedApp = false;

  if (fgThread != 0 && fgThread != brokerThread) {
    attachedFg = (AttachThreadInput(brokerThread, fgThread, TRUE) != FALSE);
  }
  if (appThread != 0 && appThread != brokerThread) {
    attachedApp = (AttachThreadInput(brokerThread, appThread, TRUE) != FALSE);
  }

  ShowWindowAsync(appHwnd, SW_RESTORE);
  SetWindowPos(appHwnd, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  const BOOL sfwResult = SetForegroundWindow(appHwnd);
  SetFocus(appHwnd);

  if (attachedApp) {
    AttachThreadInput(brokerThread, appThread, FALSE);
  }
  if (attachedFg) {
    AttachThreadInput(brokerThread, fgThread, FALSE);
  }

  HWND fgAfter = GetForegroundWindow();
  DWORD pidAfter = 0;
  if (fgAfter && IsWindow(fgAfter)) GetWindowThreadProcessId(fgAfter, &pidAfter);
  std::wstring procAfter = ProcessName(pidAfter);

  wchar_t actBuf[512]{};
  swprintf_s(actBuf,
             L"[Broker] TriggerAndActivateApp: SetForegroundWindow=%ls, foreground-after=%ls\n",
             sfwResult ? L"TRUE" : L"FALSE", procAfter.c_str());
  AppendBrokerLog(actBuf);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode < 0) return CallNextHookEx(nullptr, nCode, wParam, lParam);
  const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
  if (!k) return CallNextHookEx(nullptr, nCode, wParam, lParam);

  // Ignore our own injected dummy input
  if (k->dwExtraInfo == kOurInputTag) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  static unsigned lastGeneration = 0;
  const auto generation = g_shortcutGeneration.load();
  if (generation != lastGeneration) {
    g_win = {};
    lastGeneration = generation;
  }
  if (!feathercast::input_broker::ShouldHandleWinKey(
          g_appConnected.load(), g_winShortcutEnabled.load())) {
    g_win = {};
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  const bool isUp = (k->flags & LLKHF_UP) != 0;
  const UINT vk = k->vkCode;

  if (vk == VK_LWIN || vk == VK_RWIN) {
    if (!isUp) {
      const bool wasAlreadyDown = g_win.leftDown || g_win.rightDown;
      if (vk == VK_LWIN) g_win.leftDown = true;
      if (vk == VK_RWIN) g_win.rightDown = true;
      if (!wasAlreadyDown) g_win.hadOtherKey = false;

      AppendBrokerLogFromHook((vk == VK_RWIN) ? L"[Broker] RWIN DOWN PASS\n" : L"[Broker] LWIN DOWN PASS\n");
      return CallNextHookEx(nullptr, nCode, wParam, lParam);
    } else {
      const bool wasLeftDown = g_win.leftDown;
      const bool wasRightDown = g_win.rightDown;
      if (vk == VK_LWIN) g_win.leftDown = false;
      if (vk == VK_RWIN) g_win.rightDown = false;

      if (g_win.hadOtherKey) {
        if (!g_win.leftDown && !g_win.rightDown) {
          g_win.hadOtherKey = false;
        }
        AppendBrokerLogFromHook((vk == VK_RWIN) ? L"[Broker] RWIN UP COMBO\n" : L"[Broker] LWIN UP COMBO\n");
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
      } else {
        // Ignore a release whose press occurred while the shortcut was disabled.
        if (!g_win.leftDown && !g_win.rightDown &&
            !(vk == VK_LWIN ? wasLeftDown : wasRightDown)) {
          return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        // Solo Win tap!
        AppendBrokerLogFromHook((vk == VK_RWIN) ? L"[Broker] RWIN UP SOLO -> MASK & TRIGGER\n" : L"[Broker] LWIN UP SOLO -> MASK & TRIGGER\n");

        // 1. Send mask key (0xE8 down and up) to suppress Windows Start menu.
        // 0xE8 (VK_OEM_RESET) is unassigned in Windows keyboard layouts and is the standard
        // mask key used by AutoHotkey and PowerToys.
        INPUT maskKeys[2]{};
        maskKeys[0].type = INPUT_KEYBOARD;
        maskKeys[0].ki.wVk = 0xE8;
        maskKeys[0].ki.dwExtraInfo = kOurInputTag;

        maskKeys[1].type = INPUT_KEYBOARD;
        maskKeys[1].ki.wVk = 0xE8;
        maskKeys[1].ki.dwFlags = KEYEVENTF_KEYUP;
        maskKeys[1].ki.dwExtraInfo = kOurInputTag;

        SendInput(2, maskKeys, sizeof(INPUT));

        // 2. Post activation message to broker message loop (asynchronous, 0 ms)
        if (g_msgHwnd) {
          PostMessageW(g_msgHwnd, WM_APP_TRIGGER_ACTIVATION, 0, 0);
        }

        // 3. Return immediately so Windows never times out on the hook
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
      }
    }
  }

  if (g_win.leftDown || g_win.rightDown) {
    if (vk != VK_LWIN && vk != VK_RWIN) {
      if (!isUp) {
        // Only the fact is logged, never which key: the hook sees everything
        // typed while Win is held, and a key log does not belong in a file.
        if (!g_win.hadOtherKey) {
          AppendBrokerLogFromHook(L"[Broker] other key: YES\n");
        }
        g_win.hadOtherKey = true;
      }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK BrokerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_APP_TRIGGER_ACTIVATION) {
    TriggerAndActivateApp();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RunPipeServer(HWND msgHwnd) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  const auto userSid = feathercast::input_broker::CurrentUserSid();
  if (userSid.empty()) {
    PostMessageW(msgHwnd, WM_QUIT, 0, 0);
    return;
  }
  const auto security = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + userSid + L")S:(ML;;NW;;;ME)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      security.c_str(),
      SDDL_REVISION_1,
      &sd,
      nullptr)) {
    PostMessageW(msgHwnd, WM_QUIT, 0, 0);
    return;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;

  // The only client is the FeatherCast.exe installed next to this broker.
  const std::wstring selfPath = ProcessPath(GetCurrentProcessId());
  if (selfPath.empty()) {
    AppendBrokerLog(L"[Broker] Cannot resolve the broker path. Exiting.\n");
    LocalFree(sd);
    PostMessageW(msgHwnd, WM_QUIT, 0, 0);
    return;
  }
  const std::wstring expectedClientPath =
      (std::filesystem::path(selfPath).parent_path() / L"FeatherCast.exe").wstring();

  // The instance is created once, as the first one for this name, and kept for
  // the life of the broker. Another process can therefore neither create the
  // name before the broker nor take it over between two connections; if the
  // name is already taken, this broker must not serve anyone.
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 10 && !g_stopPipeServer.load(); ++attempt) {
    pipe = CreateNamedPipeW(
        kPipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        1024, 1024, 0,
        &sa);
    if (pipe != INVALID_HANDLE_VALUE) break;
    Sleep(200);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    AppendBrokerLog(L"[Broker] Could not create the first pipe instance. Exiting.\n");
    LocalFree(sd);
    PostMessageW(msgHwnd, WM_QUIT, 0, 0);
    return;
  }

  while (!g_stopPipeServer.load()) {
    AppendBrokerLog(L"[Broker] Named pipe listening for FeatherCast...\n");

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      DisconnectNamedPipe(pipe);
      Sleep(50);
      continue;
    }

    BrokerRegistration reg{};
    DWORD read = 0;
    ULONG clientPid = 0;
    DWORD windowPid = 0;
    if (ReadFile(pipe, &reg, sizeof(reg), &read, nullptr) && read == sizeof(reg) &&
        GetNamedPipeClientProcessId(pipe, &clientPid) && clientPid == reg.pid &&
        GetWindowThreadProcessId(reinterpret_cast<HWND>(reg.hwnd), &windowPid) == reg.threadId &&
        windowPid == reg.pid &&
        PathsEqualIgnoreCase(ProcessPath(reg.pid), expectedClientPath)) {
      {
        std::lock_guard lock(g_appMutex);
        g_appReg = reg;
        g_winShortcutEnabled.store(reg.winShortcutEnabled != 0);
        g_shortcutGeneration.fetch_add(1);
        g_appConnected = true;
        g_activePipe.store(pipe);
      }

      wchar_t regBuf[256]{};
      swprintf_s(regBuf, L"[Broker] App registered: pid=%lu hwnd=0x%llX tid=%lu\n",
                 reg.pid, reg.hwnd, reg.threadId);
      AppendBrokerLog(regBuf);

      // Commands are message framed; blocking here applies shortcut changes promptly.
      while (!g_stopPipeServer.load()) {
        BrokerCommand command{};
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, &command, sizeof(command), &bytesRead, nullptr) ||
            bytesRead != sizeof(command)) break;
        if (command == BrokerCommand::Shutdown) {
          AppendBrokerLog(L"[Broker] Received SHUTDOWN from app\n");
          break;
        }
        if (command == BrokerCommand::EnableWinShortcut ||
            command == BrokerCommand::DisableWinShortcut) {
          g_winShortcutEnabled.store(command == BrokerCommand::EnableWinShortcut);
          g_shortcutGeneration.fetch_add(1);
        }
      }

      {
        std::lock_guard lock(g_appMutex);
        g_appConnected = false;
        g_winShortcutEnabled = false;
        g_activePipe.store(INVALID_HANDLE_VALUE);
        g_appReg = {};
      }
      AppendBrokerLog(L"[Broker] App disconnected. Exiting broker.\n");
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;

      // When the connected app exits or disconnects, the broker exits cleanly too
      if (msgHwnd) {
        PostMessageW(msgHwnd, WM_QUIT, 0, 0);
      }
      break;
    }

    // Not the FeatherCast next to this broker: drop the client, keep the pipe.
    DisconnectNamedPipe(pipe);
  }

  if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
  if (sd) LocalFree(sd);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
  if ((pCmdLine && wcsstr(pCmdLine, L"--self-test")) ||
      (GetCommandLineW() && wcsstr(GetCommandLineW(), L"--self-test"))) {
    return 0;
  }

  const BrokerLogScope logScope;

  // Ensure single broker instance
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kBrokerMutexName.c_str());
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    AppendBrokerLog(L"[Broker] Another instance already running. Exiting.\n");
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  AppendBrokerLog(L"[Broker] Starting InputBroker...\n");

  // Bind to interactive window station and desktop
  HWINSTA hwinsta = OpenWindowStationW(L"WinSta0", FALSE, MAXIMUM_ALLOWED);
  if (hwinsta) {
    SetProcessWindowStation(hwinsta);
  }
  HDESK hdesk = OpenDesktopW(L"Default", 0, FALSE, MAXIMUM_ALLOWED);
  if (hdesk) {
    SetThreadDesktop(hdesk);
  }

  g_mainThreadId = GetCurrentThreadId();

  // Register window class and create hidden message window
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BrokerWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = kBrokerClassName;
  RegisterClassExW(&wc);

  g_msgHwnd = CreateWindowExW(
      0,
      kBrokerClassName,
      L"FeatherCastBrokerMsgWindow",
      0, 0, 0, 0, 0,
      HWND_MESSAGE,
      nullptr,
      hInstance,
      nullptr);

  // Install WH_KEYBOARD_LL hook directly on the main thread
  g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
  const DWORD hookErr = GetLastError();

  wchar_t logBuf[256]{};
  swprintf_s(logBuf, L"[Broker] Hook installed: hook=%p err=%lu tid=%lu msgHwnd=%p\n",
             g_hook, hookErr, g_mainThreadId, g_msgHwnd);
  AppendBrokerLog(logBuf);

  if (!g_hook) {
    if (g_msgHwnd) DestroyWindow(g_msgHwnd);
    UnregisterClassW(kBrokerClassName, hInstance);
    if (hdesk) CloseDesktop(hdesk);
    if (hwinsta) CloseWindowStation(hwinsta);
    CloseHandle(mutex);
    return 1;
  }

  // Start named pipe server on a background thread
  g_stopPipeServer.store(false);
  std::thread pipeThread([msgHwnd = g_msgHwnd]() {
    RunPipeServer(msgHwnd);
  });

  // Run Win32 message loop on the main thread
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Cleanup
  g_stopPipeServer.store(true);
  if (g_hook) {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }

  HANDLE curPipe = g_activePipe.load();
  if (curPipe != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(curPipe);
  }

  if (pipeThread.joinable()) {
    pipeThread.join();
  }

  if (g_msgHwnd) DestroyWindow(g_msgHwnd);
  UnregisterClassW(kBrokerClassName, hInstance);
  if (hdesk) CloseDesktop(hdesk);
  if (hwinsta) CloseWindowStation(hwinsta);
  CloseHandle(mutex);
  AppendBrokerLog(L"[Broker] InputBroker exited.\n");
  return 0;
}
