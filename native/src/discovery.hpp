#pragma once

// Pure app-discovery helpers (name cleanup, skip filters, keyword derivation),
// extracted from main.cpp so they can be unit-tested.

#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core.hpp"
#include "app_types.hpp"

namespace feathercast::discovery {

using feathercast::core::Lower;
using feathercast::core::Trim;

inline bool StartsWith(const std::wstring& value, const std::wstring& prefix) {
  return value.rfind(prefix, 0) == 0;
}

inline std::wstring BaseNameNoExt(const std::wstring& path) {
  std::filesystem::path p(path);
  return p.stem().wstring();
}

inline std::wstring StripCopySuffix(std::wstring name) {
  name = Trim(name);
  while (true) {
    bool changed = false;
    if (name.size() >= 4 && name.back() == L')') {
      const size_t openParen = name.rfind(L'(');
      if (openParen != std::wstring::npos && openParen > 0 && name[openParen - 1] == L' ') {
        bool allDigits = true;
        for (size_t i = openParen + 1; i < name.size() - 1; ++i) {
          if (!std::iswdigit(name[i])) {
            allDigits = false;
            break;
          }
        }
        if (allDigits && openParen + 1 < name.size() - 1) {
          name = Trim(name.substr(0, openParen - 1));
          changed = true;
          continue;
        }
      }
    }

    const std::wstring lower = Lower(name);
    static const wchar_t* copySuffixes[] = {
      L" - copy", L" - kopie", L" - copie"
    };
    for (const auto* suffix : copySuffixes) {
      const size_t len = std::wcslen(suffix);
      if (lower.size() > len && lower.ends_with(suffix)) {
        name = Trim(name.substr(0, name.size() - len));
        changed = true;
        break;
      }
    }

    if (!changed) break;
  }
  return name;
}

inline std::wstring CleanName(const std::wstring& value) {
  std::wstring name = Trim(value);
  if (Lower(name).ends_with(L".lnk")) name.resize(name.size() - 4);
  return StripCopySuffix(name);
}

inline bool IsHostExecutable(const std::wstring& path) {
  if (path.empty()) return false;
  std::filesystem::path p(path);
  const std::wstring fn = Lower(p.filename().wstring());
  static const std::set<std::wstring> hosts = {
    L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"msiexec.exe",
    L"cscript.exe", L"wscript.exe", L"python.exe", L"pythonw.exe",
    L"javaw.exe", L"java.exe", L"rundll32.exe", L"explorer.exe",
    L"bash.exe", L"wsl.exe", L"onenote.exe"
  };
  return hosts.contains(fn);
}

inline std::wstring DisambiguateShortcutName(const std::wstring& stemName, const std::filesystem::path& shortcutPath) {
  const std::wstring parent = shortcutPath.parent_path().filename().wstring();
  if (parent.empty()) return stemName;

  const std::wstring lowerParent = Lower(parent);
  static const std::set<std::wstring> ignoreParents = {
    L"programs", L"start menu", L"desktop", L"commonprograms",
    L"public", L"accessories", L"system tools", L"windows tools"
  };
  if (ignoreParents.contains(lowerParent)) return stemName;

  const std::wstring lowerStem = Lower(stemName);
  if (lowerParent.rfind(lowerStem + L" ", 0) == 0) {
    return parent;
  }

  bool parentHasVersion = false;
  for (size_t i = 0; i + 2 < parent.size(); ++i) {
    if (std::iswdigit(parent[i]) && (parent[i + 1] == L'.' || parent[i + 2] == L'.')) {
      parentHasVersion = true;
      break;
    }
  }

  bool stemHasVersion = false;
  for (size_t i = 0; i + 2 < stemName.size(); ++i) {
    if (std::iswdigit(stemName[i]) && (stemName[i + 1] == L'.' || stemName[i + 2] == L'.')) {
      stemHasVersion = true;
      break;
    }
  }

  if (parentHasVersion && !stemHasVersion) {
    return stemName + L" (" + parent + L")";
  }

  return stemName;
}

inline std::wstring NameKey(const std::wstring& value) {
  return Lower(CleanName(value));
}

inline bool ShouldSkipName(const std::wstring& value) {
  const std::wstring name = NameKey(value);
  static const wchar_t* prefixes[] = {
    L"uninstall", L"deinstall", L"readme", L"hilfe", L"help", L"website", L"homepage",
  };
  for (const auto* prefix : prefixes) {
    if (StartsWith(name, prefix)) return true;
  }
  return false;
}

inline std::vector<std::wstring> SplitWords(const std::wstring& value) {
  std::vector<std::wstring> out;
  std::wstring current;
  for (const wchar_t ch : Lower(value)) {
    if (std::iswalnum(ch)) {
      current.push_back(ch);
    } else if (!current.empty()) {
      out.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

inline std::vector<std::wstring> UniqueKeywords(const std::vector<std::wstring>& values) {
  std::set<std::wstring> seen;
  std::vector<std::wstring> out;
  for (const auto& value : values) {
    for (const auto& word : SplitWords(value)) {
      if (word.size() < 2 || seen.contains(word)) continue;
      seen.insert(word);
      out.push_back(word);
    }
  }
  return out;
}

inline std::vector<std::wstring> KeywordsFor(const std::wstring& name, const std::wstring& target, const std::wstring& appId) {
  std::vector<std::wstring> groups = {name, BaseNameNoExt(target), appId};
  const std::wstring lower = Lower(name + L" " + target + L" " + appId);
  if (lower.find(L"terminal") != std::wstring::npos || lower.find(L"wt.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"wt", L"shell", L"console", L"cmd", L"powershell"});
  }
  if (lower.find(L"command prompt") != std::wstring::npos || lower.find(L"cmd.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"cmd", L"console", L"terminal"});
  }
  if (lower.find(L"powershell") != std::wstring::npos || lower.find(L"pwsh.exe") != std::wstring::npos) {
    groups.insert(groups.end(), {L"pwsh", L"shell", L"terminal"});
  }
  if (lower.find(L"settings") != std::wstring::npos || lower.find(L"immersivecontrolpanel") != std::wstring::npos) {
    groups.insert(groups.end(), {L"preferences", L"control panel", L"system"});
  }
  if (lower.find(L"calculator") != std::wstring::npos || lower.find(L"calc.exe") != std::wstring::npos) {
    groups.push_back(L"calc");
  }
  return UniqueKeywords(groups);
}

inline bool IsSystemEssentialName(const std::wstring& name) {
  static const std::set<std::wstring> names = {
    L"terminal", L"windows terminal", L"command prompt", L"windows powershell",
    L"powershell 7 (x64)", L"settings", L"calculator",
  };
  return names.contains(NameKey(name));
}

inline std::wstring CanonicalPathKey(const std::wstring& rawPath) {
  if (rawPath.empty()) return {};
  std::wstring expandedPath = rawPath;
  if (rawPath.find(L'%') != std::wstring::npos) {
    wchar_t buf[32768]{};
    if (ExpandEnvironmentStringsW(rawPath.c_str(), buf, static_cast<DWORD>(std::size(buf))) > 0) {
      expandedPath = buf;
    }
  }
  const bool isFsPath = (expandedPath.size() >= 2 && expandedPath[1] == L':') ||
                        (expandedPath.size() >= 2 && (expandedPath[0] == L'\\' || expandedPath[0] == L'/'));
  if (!isFsPath) {
    return Lower(expandedPath);
  }
  std::filesystem::path path(expandedPath);
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  std::wstring value = Lower((ec ? path.lexically_normal() : canonical).wstring());
  while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return value;
}

inline bool ShouldMergeApps(const app::AppEntry& existing, const app::AppEntry& incoming) {
  if (existing.id.empty() || incoming.id.empty()) return false;

  // 1. Direct ID match
  if (CanonicalPathKey(existing.id) == CanonicalPathKey(incoming.id)) return true;

  // 2. AUMID match
  if (!existing.appUserModelId.empty() && !incoming.appUserModelId.empty()) {
    if (Lower(existing.appUserModelId) == Lower(incoming.appUserModelId)) return true;
  }

  // 3. Launch target match
  if (!existing.launchTarget.empty() && !incoming.launchTarget.empty()) {
    if (CanonicalPathKey(existing.launchTarget) == CanonicalPathKey(incoming.launchTarget)) return true;
  }

  // 4. Game install path match
  if (incoming.isGame && !incoming.path.empty() && existing.isGame && !existing.path.empty()) {
    if (CanonicalPathKey(existing.path) == CanonicalPathKey(incoming.path)) return true;
  }

  // 5. Target executable match (for non-host executables)
  if (!existing.targetPath.empty() && !incoming.targetPath.empty()) {
    if (CanonicalPathKey(existing.targetPath) == CanonicalPathKey(incoming.targetPath)) {
      if (!IsHostExecutable(existing.targetPath)) {
        if (NameKey(existing.name) == NameKey(incoming.name)) return true;
        if (existing.args.empty() && incoming.args.empty()) return true;
      }
    }
  }

  // 6. Name match
  if (!existing.name.empty() && !incoming.name.empty() &&
      NameKey(existing.name) == NameKey(incoming.name)) {
    // If both specify different concrete target executables, they are distinct apps (unless host)
    if (!existing.targetPath.empty() && !incoming.targetPath.empty() &&
        !IsHostExecutable(existing.targetPath) && !IsHostExecutable(incoming.targetPath)) {
      return CanonicalPathKey(existing.targetPath) == CanonicalPathKey(incoming.targetPath);
    }
    // If one is a game and other is not, game correlation handles it if paths match
    if (existing.isGame != incoming.isGame) {
      return true;
    }
    // At least one lacks an explicit targetPath (e.g. shell:AppsFolder entry vs shortcut)
    return true;
  }

  return false;
}

inline void MergeAppEntries(app::AppEntry& existing, app::AppEntry incoming) {
  if (incoming.isGame && !existing.isGame) {
    const std::wstring visibleName = existing.name;
    const std::wstring localIcon = existing.iconKey;
    const std::wstring localTarget = existing.targetPath;
    const bool adminSupported = existing.adminSupported || incoming.adminSupported;
    const bool systemEssential = existing.systemEssential || incoming.systemEssential;
    incoming.name = visibleName;
    if (!localIcon.empty()) incoming.iconKey = localIcon;
    if (incoming.targetPath.empty()) incoming.targetPath = localTarget;
    incoming.adminSupported = adminSupported;
    incoming.systemEssential = systemEssential;
    incoming.keywords = UniqueKeywords({
        core::JoinKeywords(existing.keywords),
        core::JoinKeywords(incoming.keywords),
    });
    existing = std::move(incoming);
    return;
  }

  // If existing was an AppsFolder entry and incoming is a shortcut, upgrade launch metadata to direct shortcut
  if (existing.launchType == app::LaunchType::AppsFolder &&
      incoming.launchType == app::LaunchType::Shortcut) {
    existing.launchType = incoming.launchType;
    existing.launchTarget = std::move(incoming.launchTarget);
    if (!incoming.targetPath.empty()) existing.targetPath = std::move(incoming.targetPath);
    if (!incoming.args.empty()) existing.args = std::move(incoming.args);
    if (!incoming.cwd.empty()) existing.cwd = std::move(incoming.cwd);
    if (!incoming.source.empty()) existing.source = std::move(incoming.source);
    if (!incoming.name.empty()) existing.name = std::move(incoming.name);
    existing.adminSupported = incoming.adminSupported;
  } else {
    existing.adminSupported = existing.adminSupported || incoming.adminSupported;
  }

  existing.systemEssential = existing.systemEssential || incoming.systemEssential;
  existing.isGame = existing.isGame || incoming.isGame;
  if (!incoming.gameProvider.empty() &&
      existing.gameProvider.find(incoming.gameProvider) == std::wstring::npos) {
    if (!existing.gameProvider.empty()) existing.gameProvider += L" + ";
    existing.gameProvider += incoming.gameProvider;
  }
  if (incoming.isGame && !incoming.path.empty()) existing.path = incoming.path;
  if (existing.iconKey.empty()) existing.iconKey = std::move(incoming.iconKey);
  if (existing.targetPath.empty()) existing.targetPath = std::move(incoming.targetPath);
  if (existing.appUserModelId.empty()) existing.appUserModelId = std::move(incoming.appUserModelId);
  existing.keywords = UniqueKeywords({
      core::JoinKeywords(existing.keywords),
      core::JoinKeywords(incoming.keywords),
  });
}

}  // namespace feathercast::discovery

