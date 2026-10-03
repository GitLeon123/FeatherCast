#pragma once

#include <windows.h>
#include <cstdint>

namespace feathercast::input_broker {

#pragma pack(push, 1)
struct Registration {
  DWORD pid = 0;
  std::uint64_t hwnd = 0;
  DWORD threadId = 0;
  DWORD winShortcutEnabled = 0;
};
#pragma pack(pop)

enum class Command : DWORD { DisableWinShortcut, EnableWinShortcut, Shutdown };

constexpr bool ShouldHandleWinKey(bool connected, bool enabled) {
  return connected && enabled;
}

}  // namespace feathercast::input_broker
