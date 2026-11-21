#pragma once

// Pure app-discovery helpers (name cleanup, skip filters, keyword derivation),
// extracted from main.cpp so they can be unit-tested.

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
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
  const std::size_t slash = path.find_last_of(L"\\/");
  const std::wstring_view name =
      slash == std::wstring::npos ? std::wstring_view(path)
                                  : std::wstring_view(path).substr(slash + 1);
  if (name.size() < 7 || name.size() > 14) return false;
  wchar_t lower[16];
  for (std::size_t i = 0; i < name.size(); ++i) {
    const wchar_t ch = name[i];
    if (ch >= L'A' && ch <= L'Z') {
      lower[i] = static_cast<wchar_t>(ch - L'A' + L'a');
    } else if (ch < 128) {
      lower[i] = ch;
    } else {
      return false;
    }
  }
  const std::wstring_view fn(lower, name.size());
  static constexpr std::wstring_view kHosts[] = {
      L"bash.exe", L"cmd.exe", L"cscript.exe", L"explorer.exe",
      L"java.exe", L"javaw.exe", L"msiexec.exe", L"onenote.exe",
      L"powershell.exe", L"pwsh.exe", L"python.exe", L"pythonw.exe",
      L"rundll32.exe", L"wscript.exe", L"wsl.exe",
  };
  return std::binary_search(std::begin(kHosts), std::end(kHosts), fn);
}

inline bool SupportsAdminLaunchTarget(const std::wstring& target) {
  if (target.empty()) return false;

  // URLs and shell: targets are opened by their registered handler and are
  // not executable files that can be passed to the runas verb.
  const size_t colon = target.find(L':');
  const bool drivePath = colon == 1 && std::iswalpha(target.front());
  const bool extendedDrivePath = target.rfind(L"\\\\?\\", 0) == 0 &&
                                 colon == 5 && std::iswalpha(target[4]);
  if (colon != std::wstring::npos && !drivePath && !extendedDrivePath) {
    return false;
  }

  const std::wstring extension = Lower(
      std::filesystem::path(target).extension().wstring());
  return extension == L".exe" || extension == L".com" ||
         extension == L".bat" || extension == L".cmd" ||
         extension == L".lnk";
}

inline bool SupportsAdminExecutableTarget(const std::wstring& target) {
  return SupportsAdminLaunchTarget(target) &&
         Lower(std::filesystem::path(target).extension().wstring()) != L".lnk";
}

enum class AdminLaunchRoute { Unsupported, Direct, AppsFolder };

inline AdminLaunchRoute AdminRouteFor(const app::AppEntry& app) {
  switch (app.launchType) {
    case app::LaunchType::Exe:
      return SupportsAdminLaunchTarget(app.launchTarget)
                 ? AdminLaunchRoute::Direct : AdminLaunchRoute::Unsupported;
    case app::LaunchType::Shortcut:
      if (SupportsAdminExecutableTarget(app.targetPath))
        return AdminLaunchRoute::Direct;
      return app.appUserModelId.empty() ? AdminLaunchRoute::Unsupported
                                        : AdminLaunchRoute::AppsFolder;
    case app::LaunchType::AppsFolder:
      return app.launchTarget.empty() ? AdminLaunchRoute::Unsupported
                                      : AdminLaunchRoute::AppsFolder;
    case app::LaunchType::Shell:
      return AdminLaunchRoute::Unsupported;
  }
  return AdminLaunchRoute::Unsupported;
}

inline std::wstring DisambiguateShortcutName(const std::wstring& stemName, const std::filesystem::path& shortcutPath) {
  const std::wstring parent = shortcutPath.parent_path().filename().wstring();
  if (parent.empty()) return stemName;

  const std::wstring lowerParent = Lower(parent);
  static constexpr std::wstring_view kIgnoreParents[] = {
      L"accessories", L"commonprograms", L"desktop", L"programs",
      L"public", L"start menu", L"system tools", L"windows tools",
  };
  if (std::binary_search(std::begin(kIgnoreParents), std::end(kIgnoreParents),
                         std::wstring_view(lowerParent))) {
    return stemName;
  }

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
  static constexpr std::wstring_view kNames[] = {
      L"calculator", L"command prompt", L"powershell 7 (x64)",
      L"settings", L"terminal", L"windows powershell", L"windows terminal",
  };
  const std::wstring key = NameKey(name);
  return std::binary_search(std::begin(kNames), std::end(kNames),
                            std::wstring_view(key));
}

inline std::wstring CanonicalPathKey(const std::wstring& rawPath) {
  if (rawPath.empty()) return {};
  std::wstring expandedPath;
  const std::wstring* pathRef = &rawPath;
  if (rawPath.find(L'%') != std::wstring::npos) {
    wchar_t stackBuf[512];
    const DWORD needed = ExpandEnvironmentStringsW(
        rawPath.c_str(), stackBuf, static_cast<DWORD>(std::size(stackBuf)));
    if (needed > 0 && needed <= std::size(stackBuf)) {
      expandedPath.assign(stackBuf, needed - 1);
      pathRef = &expandedPath;
    } else if (needed > std::size(stackBuf)) {
      expandedPath.resize(needed);
      const DWORD written = ExpandEnvironmentStringsW(
          rawPath.c_str(), expandedPath.data(), needed);
      if (written > 0 && written <= needed) {
        expandedPath.resize(written - 1);
        pathRef = &expandedPath;
      }
    }
  }
  const std::wstring& resolved = *pathRef;
  const bool isFsPath = (resolved.size() >= 2 && resolved[1] == L':') ||
                        (resolved.size() >= 2 && (resolved[0] == L'\\' || resolved[0] == L'/'));
  if (!isFsPath) {
    return Lower(resolved);
  }
  std::filesystem::path path(resolved);
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  std::wstring value = Lower((ec ? path.lexically_normal() : canonical).wstring());
  while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return value;
}

inline bool IsFileSystemPath(const std::wstring& value) {
  return (value.size() >= 3 && value[1] == L':' &&
          (value[2] == L'\\' || value[2] == L'/')) ||
         value.starts_with(L"\\\\") || value.starts_with(L"//");
}

// Whether `other` starts an executable inside the install folder of `game`.
inline bool StartsInsideInstall(const app::AppEntry& game, const app::AppEntry& other) {
  if (!game.isGame || game.path.empty() || other.isGame ||
      !IsFileSystemPath(other.targetPath)) {
    return false;
  }
  const std::wstring install = CanonicalPathKey(game.path);
  const std::wstring target = CanonicalPathKey(other.targetPath);
  if (install.empty() || target.size() <= install.size() ||
      !target.starts_with(install)) {
    return false;
  }
  return install.back() == L'\\' || install.back() == L'/' ||
         target[install.size()] == L'\\' || target[install.size()] == L'/';
}

// A file target that is the app itself rather than a script host or a
// launcher invoked with arguments.
inline bool StartsOwnExecutable(const app::AppEntry& entry) {
  return IsFileSystemPath(entry.targetPath) &&
         !IsHostExecutable(entry.targetPath) && entry.args.empty();
}

inline bool EqualsInsensitiveSimple(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const wchar_t a = left[i];
    const wchar_t b = right[i];
    if (a == b) continue;
    if (std::towlower(a) != std::towlower(b)) return false;
  }
  return true;
}

inline bool IsPathLikeKey(std::wstring_view value) {
  return value.find(L'%') != std::wstring_view::npos ||
         (value.size() >= 2 && (value[1] == L':' || value[0] == L'\\' || value[0] == L'/'));
}

inline bool SameCanonicalKey(const std::wstring& left, const std::wstring& right) {
  if (left == right) return true;
  if (!IsPathLikeKey(left) && !IsPathLikeKey(right)) {
    return EqualsInsensitiveSimple(left, right);
  }
  return CanonicalPathKey(left) == CanonicalPathKey(right);
}

inline bool ShouldMergeApps(const app::AppEntry& existing, const app::AppEntry& incoming) {
  if (existing.id.empty() || incoming.id.empty()) return false;

  // 1. Direct ID match
  if (SameCanonicalKey(existing.id, incoming.id)) return true;

  // 2. AUMID match
  if (!existing.appUserModelId.empty() && !incoming.appUserModelId.empty()) {
    if (EqualsInsensitiveSimple(existing.appUserModelId, incoming.appUserModelId)) return true;
  }

  // 3. Launch target match. Launchers start many games through one
  // executable (steam.exe, Battle.net.exe), so the arguments must match too.
  if (!existing.launchTarget.empty() && !incoming.launchTarget.empty() &&
      existing.args == incoming.args) {
    if (SameCanonicalKey(existing.launchTarget, incoming.launchTarget)) return true;
  }

  // 4. Game install path match
  if (incoming.isGame && !incoming.path.empty() && existing.isGame && !existing.path.empty()) {
    if (SameCanonicalKey(existing.path, incoming.path)) return true;
  }

  // Cache target-path checks across steps 5 and 6 to avoid repeated
  // weakly_canonical() filesystem calls on the same pair.
  bool computedTargetMatch = false;
  bool sameTarget = false;
  bool computedExistingHost = false;
  bool existingHost = false;
  const auto getExistingHost = [&] {
    if (!computedExistingHost) {
      existingHost = IsHostExecutable(existing.targetPath);
      computedExistingHost = true;
    }
    return existingHost;
  };
  const auto getSameTarget = [&] {
    if (!computedTargetMatch) {
      sameTarget = SameCanonicalKey(existing.targetPath, incoming.targetPath);
      computedTargetMatch = true;
    }
    return sameTarget;
  };

  bool computedNames = false;
  bool sameName = false;
  const auto getSameName = [&] {
    if (!computedNames) {
      sameName = !existing.name.empty() && !incoming.name.empty() &&
                 NameKey(existing.name) == NameKey(incoming.name);
      computedNames = true;
    }
    return sameName;
  };

  // 5. Target executable match (for non-host executables)
  if (!existing.targetPath.empty() && !incoming.targetPath.empty()) {
    if (getSameTarget() && !getExistingHost()) {
      if (getSameName()) return true;
      if (existing.args.empty() && incoming.args.empty()) return true;
    }
  }

  // 6. Name match
  if (getSameName()) {
    // A provider knows the install folder while a shortcut knows the
    // executable, which may be a launcher inside that folder.
    if (StartsInsideInstall(existing, incoming) ||
        StartsInsideInstall(incoming, existing)) {
      return true;
    }
    // Two games installed in different folders are separate copies; equal
    // folders already matched above.
    if (existing.isGame && incoming.isGame && !existing.path.empty() &&
        !incoming.path.empty()) {
      return false;
    }
    // If both specify different concrete target executables, they are distinct apps (unless host)
    if (!existing.targetPath.empty() && !incoming.targetPath.empty() &&
        !getExistingHost() && !IsHostExecutable(incoming.targetPath)) {
      return getSameTarget();
    }
    // An ordinary app that starts its own executable from outside a game's
    // install folder only shares the game's name. Shortcuts that pass
    // arguments to a launcher, or carry no file target, still merge.
    if (existing.isGame != incoming.isGame) {
      const auto& game = existing.isGame ? existing : incoming;
      const auto& other = existing.isGame ? incoming : existing;
      if (!game.path.empty() && StartsOwnExecutable(other)) return false;
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

