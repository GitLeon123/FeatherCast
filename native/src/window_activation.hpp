#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <functional>

namespace feathercast::window_activation {

enum class ExistingInstanceResult { Activated, NotFound, StaleWindow, TimedOut };

struct ExistingInstanceAdapter {
  std::function<HWND()> findWindow;
  std::function<bool(HWND)> validWindow;
  std::function<bool(HWND, unsigned)> sendShow;
};

inline ExistingInstanceResult ActivateExisting(
    const ExistingInstanceAdapter& adapter, unsigned timeoutMilliseconds = 1000) {
  const HWND window = adapter.findWindow ? adapter.findWindow() : nullptr;
  if (!window) return ExistingInstanceResult::NotFound;
  if (!adapter.validWindow || !adapter.validWindow(window)) {
    return ExistingInstanceResult::StaleWindow;
  }
  const unsigned boundedTimeout = std::clamp(timeoutMilliseconds, 1u, 1000u);
  return adapter.sendShow && adapter.sendShow(window, boundedTimeout)
             ? ExistingInstanceResult::Activated
             : ExistingInstanceResult::TimedOut;
}

struct FocusAdapter {
  std::function<bool(HWND)> validWindow;
  std::function<bool(HWND)> iconic;
  std::function<void(HWND)> restore;
  std::function<HWND()> foreground;
  std::function<DWORD(HWND)> threadId;
  std::function<DWORD()> currentThreadId;
  std::function<bool(DWORD, DWORD, bool)> attachInput;
  std::function<void(HWND)> bringToTop;
  std::function<bool(HWND)> setForeground;
};

inline bool FocusVerified(HWND target, const FocusAdapter& adapter) {
  if (!target || !adapter.validWindow || !adapter.validWindow(target)) {
    return false;
  }
  if (adapter.iconic && adapter.iconic(target) && adapter.restore) {
    adapter.restore(target);
  }
  if (adapter.setForeground) adapter.setForeground(target);
  if (adapter.foreground && adapter.foreground() == target) return true;

  const HWND foreground = adapter.foreground ? adapter.foreground() : nullptr;
  const DWORD targetThread = adapter.threadId ? adapter.threadId(target) : 0;
  const DWORD foregroundThread =
      foreground && adapter.threadId ? adapter.threadId(foreground) : 0;
  const DWORD currentThread =
      adapter.currentThreadId ? adapter.currentThreadId() : 0;
  const DWORD attachThread = foregroundThread ? foregroundThread : targetThread;
  const bool canAttach = attachThread != 0 && currentThread != 0 &&
                         attachThread != currentThread && adapter.attachInput;
  const bool attached =
      canAttach && adapter.attachInput(attachThread, currentThread, true);
  if (attached) {
    if (adapter.bringToTop) adapter.bringToTop(target);
    if (adapter.setForeground) adapter.setForeground(target);
    adapter.attachInput(attachThread, currentThread, false);
  }
  return adapter.foreground && adapter.foreground() == target;
}

inline FocusAdapter Win32FocusAdapter() {
  return {
      [](HWND window) { return IsWindow(window) != FALSE; },
      [](HWND window) { return IsIconic(window) != FALSE; },
      [](HWND window) { ShowWindowAsync(window, SW_RESTORE); },
      [] { return GetForegroundWindow(); },
      [](HWND window) { return GetWindowThreadProcessId(window, nullptr); },
      [] { return GetCurrentThreadId(); },
      [](DWORD from, DWORD to, bool attach) {
        return AttachThreadInput(from, to, attach ? TRUE : FALSE) != FALSE;
      },
      [](HWND window) { BringWindowToTop(window); },
      [](HWND window) { return SetForegroundWindow(window) != FALSE; },
  };
}

inline bool FocusVerified(HWND target) {
  return FocusVerified(target, Win32FocusAdapter());
}

}  // namespace feathercast::window_activation
