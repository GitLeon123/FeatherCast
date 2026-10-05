#pragma once

#include "settings.hpp"
#include "shortcut.hpp"

#include <filesystem>
#include <sstream>

namespace feathercast::automation {

enum class Kind { Script, Workspace };

inline std::vector<std::wstring> WorkspaceTargets(const std::wstring& value) {
  std::vector<std::wstring> targets;
  std::wistringstream lines(value);
  std::wstring line;
  while (std::getline(lines, line)) {
    line = shortcut::Trim(std::move(line));
    if (!line.empty()) targets.push_back(std::move(line));
  }
  return targets;
}

inline bool ValidWorkspaceTarget(const std::wstring& target) {
  if (target.empty() || target.size() > 2048 || target.find(L'"') != std::wstring::npos) return false;
  const auto lower = core::Normalize(target);
  if (lower.starts_with(L"https://") || lower.starts_with(L"http://")) return target.find(L' ') == std::wstring::npos;
  const std::filesystem::path path(target);
  if (!path.is_absolute()) return false;
  // Workspaces open apps, documents and folders, rather than interpret commands.
  const auto extension = core::Normalize(path.extension().wstring());
  return extension != L".ps1" && extension != L".bat" && extension != L".cmd" && extension != L".vbs";
}

inline std::optional<std::wstring> Validate(Kind kind, const settings::Quicklink& item) {
  const auto keyword = core::ValidateAlias(item.keyword);
  if (!keyword.valid) return L"Use a keyword of 1–64 characters without a reserved prefix or line breaks.";
  if (item.name.size() > 128 || item.name.find_first_of(L"\r\n") != std::wstring::npos) return L"Use a name of up to 128 characters on one line.";
  if (kind == Kind::Script) {
    const std::filesystem::path path(item.target);
    if (!path.is_absolute() || core::Normalize(path.extension().wstring()) != L".ps1" ||
        item.target.find_first_of(L"\"\r\n") != std::wstring::npos) return L"Choose an absolute path to a PowerShell (.ps1) file.";
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return L"The PowerShell file could not be found.";
  } else {
    const auto targets = WorkspaceTargets(item.target);
    if (targets.empty() || targets.size() > 16) return L"Enter 1–16 apps, files, folders or web addresses, one per line.";
    for (const auto& target : targets) if (!ValidWorkspaceTarget(target)) return L"Each workspace entry needs an absolute path or an http/https address.";
  }
  return std::nullopt;
}

inline std::optional<std::wstring> ValidateShortcut(const std::wstring& value) {
  const auto parsed = shortcut::ParseShortcut(value);
  if (!parsed.valid || !shortcut::ToHotKeySpec(parsed).supported || (!parsed.ctrl && !parsed.alt)) {
    return L"Use Ctrl or Alt with a key, for example Ctrl+Alt+V. Windows-key shortcuts are unavailable here.";
  }
  return std::nullopt;
}

}  // namespace feathercast::automation
