#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr ULONG_PTR kOurInputTag = 0x4D59415050; // "MYAPP"
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\FeatherCastInputBroker";
constexpr const wchar_t* kBrokerMutexName = L"FeatherCastInputBrokerMutex";
constexpr const wchar_t* kBrokerClassName = L"FeatherCastBrokerMsgClass";
constexpr UINT WM_APP_TRIGGER_ACTIVATION = WM_APP + 1;

#pragma pack(push, 1)
struct BrokerRegistration {
  DWORD pid = 0;
  std::uint64_t hwnd = 0;
  DWORD threadId = 0;
};
#pragma pack(pop)

struct WinState {
  bool leftDown = false;
  bool rightDown = false;
  bool hadOtherKey = false;
};

std::mutex g_logMutex;
void AppendBrokerLog(const wchar_t* text) {
  if (!text || !text[0]) return;
  try {
    std::lock_guard lock(g_logMutex);
    wchar_t local[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) return;
    std::wstring path = std::wstring(local) + L"\\FeatherCast\\broker.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
      fputws(text, f);
      fclose(f);
    }
  } catch (...) {
  }
  OutputDebugStringW(text);
}

std::wstring ProcessPath(DWORD pid) {
  if (pid == 0) return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  wchar_t buffer[MAX_PATH]{};
  DWORD size = MAX_PATH;
  if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
    CloseHandle(process);
    return std::wstring(buffer, size);
  }
  CloseHandle(process);
  return {};
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

std::wstring GetVkName(UINT vk, DWORD scanCode, DWORD flags) {
  if (vk == VK_LWIN) return L"LWIN";
  if (vk == VK_RWIN) return L"RWIN";
  if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return L"SHIFT";
  if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return L"CTRL";
  if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return L"ALT";
  if (vk == VK_TAB) return L"TAB";
  if (vk == VK_RETURN) return L"ENTER";
  if (vk == VK_ESCAPE) return L"ESC";
  if (vk == VK_SPACE) return L"SPACE";
  if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
  LONG lParam = static_cast<LONG>(scanCode << 16);
  if (flags & LLKHF_EXTENDED) lParam |= (1 << 24);
  wchar_t name[64]{};
  if (GetKeyNameTextW(lParam, name, 64) > 0) {
    return name;
  }
  wchar_t fallback[32]{};
  swprintf_s(fallback, L"0x%02X", vk);
  return fallback;
}

std::mutex g_appMutex;
BrokerRegistration g_appReg{};
std::atomic<bool> g_appConnected{false};
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
    if (!g_appConnected) {
      AppendBrokerLog(L"[Broker] Trigger skipped: no app connected\n");
      return;
    }
    reg = g_appReg;
  }

  HWND appHwnd = reinterpret_cast<HWND>(reg.hwnd);
  if (!appHwnd || !IsWindow(appHwnd)) return;

  // 1. Post trigger message directly to FeatherCast window
  PostMessageW(appHwnd, WM_APP + 25, 0, 0); // WM_APP_WINKEY_TRIGGER

  // 2. Perform elevated foreground activation
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

  const bool isUp = (k->flags & LLKHF_UP) != 0;
  const UINT vk = k->vkCode;

  if (vk == VK_LWIN || vk == VK_RWIN) {
    if (!isUp) {
      const bool wasAlreadyDown = g_win.leftDown || g_win.rightDown;
      if (vk == VK_LWIN) g_win.leftDown = true;
      if (vk == VK_RWIN) g_win.rightDown = true;
      if (!wasAlreadyDown) g_win.hadOtherKey = false;

      AppendBrokerLog((vk == VK_RWIN) ? L"[Broker] RWIN DOWN PASS\n" : L"[Broker] LWIN DOWN PASS\n");
      return CallNextHookEx(nullptr, nCode, wParam, lParam);
    } else {
      if (vk == VK_LWIN) g_win.leftDown = false;
      if (vk == VK_RWIN) g_win.rightDown = false;

      if (g_win.hadOtherKey) {
        if (!g_win.leftDown && !g_win.rightDown) {
          g_win.hadOtherKey = false;
        }
        AppendBrokerLog((vk == VK_RWIN) ? L"[Broker] RWIN UP COMBO\n" : L"[Broker] LWIN UP COMBO\n");
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
      } else {
        // Solo Win tap!
        AppendBrokerLog((vk == VK_RWIN) ? L"[Broker] RWIN UP SOLO -> MASK & TRIGGER\n" : L"[Broker] LWIN UP SOLO -> MASK & TRIGGER\n");

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
        g_win.hadOtherKey = true;
        const std::wstring keyName = GetVkName(vk, k->scanCode, k->flags);
        wchar_t buf[256]{};
        swprintf_s(buf, L"[Broker] other key: YES (%ls)\n", keyName.c_str());
        AppendBrokerLog(buf);
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
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:(A;;GRGW;;;WD)S:(ML;;NW;;;LW)",
      SDDL_REVISION_1,
      &sd,
      nullptr);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;

  while (!g_stopPipeServer.load()) {
    HANDLE pipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        1024, 1024, 0,
        &sa);

    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(500);
      continue;
    }

    AppendBrokerLog(L"[Broker] Named pipe listening for FeatherCast...\n");

    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    BrokerRegistration reg{};
    DWORD read = 0;
    if (ReadFile(pipe, &reg, sizeof(reg), &read, nullptr) && read == sizeof(reg)) {
      {
        std::lock_guard lock(g_appMutex);
        g_appReg = reg;
        g_appConnected = true;
        g_activePipe.store(pipe);
      }

      wchar_t regBuf[256]{};
      swprintf_s(regBuf, L"[Broker] App registered: pid=%lu hwnd=0x%llX tid=%lu\n",
                 reg.pid, reg.hwnd, reg.threadId);
      AppendBrokerLog(regBuf);

      // Non-blocking connection monitor: check if pipe is alive without blocking ReadFile
      while (!g_stopPipeServer.load()) {
        DWORD bytesAvail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
          break; // Client disconnected
        }
        if (bytesAvail > 0) {
          char buf[128]{};
          DWORD bytesRead = 0;
          if (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            if (strstr(buf, "SHUTDOWN") != nullptr) {
              AppendBrokerLog(L"[Broker] Received SHUTDOWN from app\n");
              break;
            }
          }
        }
        Sleep(200);
      }

      {
        std::lock_guard lock(g_appMutex);
        g_appConnected = false;
        g_activePipe.store(INVALID_HANDLE_VALUE);
        g_appReg = {};
      }
      AppendBrokerLog(L"[Broker] App disconnected. Exiting broker.\n");
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);

      // When the connected app exits or disconnects, the broker exits cleanly too
      if (msgHwnd) {
        PostMessageW(msgHwnd, WM_QUIT, 0, 0);
      }
      break;
    }

    CloseHandle(pipe);
  }

  if (sd) LocalFree(sd);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
  if ((pCmdLine && wcsstr(pCmdLine, L"--self-test")) ||
      (GetCommandLineW() && wcsstr(GetCommandLineW(), L"--self-test"))) {
    return 0;
  }

  // Ensure single broker instance
  HANDLE mutex = CreateMutexW(nullptr, TRUE, kBrokerMutexName);
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    AppendBrokerLog(L"[Broker] Another instance already running. Exiting.\n");
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  AppendBrokerLog(L"[Broker] Starting InputBroker (elevated)...\n");

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
