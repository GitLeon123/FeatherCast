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

// The broker answers a registration it accepts with this value. A client
// that gets anything else (or a closed pipe) is not served and must keep
// handling the Win key itself.
constexpr DWORD kRegistrationAccepted = 0x4B434146;  // "FACK"

constexpr bool ShouldHandleWinKey(bool connected, bool enabled) {
  return connected && enabled;
}

}  // namespace feathercast::input_broker
