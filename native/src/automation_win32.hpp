#pragma once

#include "automation.hpp"
#include <windows.h>
#include <shellapi.h>

namespace feathercast::automation {

inline bool Launch(Kind kind, const std::wstring& name, const std::wstring& target) {
  if (Validate(kind, {L"launch", name, target})) return false;
  if (kind == Kind::Script) {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return false;
    const auto executable = std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    const auto arguments = L"-NoLogo -NoProfile -File \"" + target + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.c_str(), arguments.c_str(),
        std::filesystem::path(target).parent_path().c_str(), SW_SHOWNORMAL)) > 32;
  }
  bool succeeded = true;
  for (const auto& entry : WorkspaceTargets(target)) {
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", entry.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) succeeded = false;
  }
  return succeeded;
}

}  // namespace feathercast::automation
