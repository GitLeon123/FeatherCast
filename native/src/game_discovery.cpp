#include "game_discovery.hpp"

#include "core.hpp"
#include "discovery.hpp"
#include "extension_protocol.hpp"
#include "json.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::games {
namespace {

using app::AppEntry;
using app::LaunchType;
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxMetadataBytes = 8 * 1024 * 1024;

// Reads launcher metadata up to kMaxMetadataBytes. The size check skips
// oversized files cheaply; the bounded read also covers a file that grows
// after the check.
std::optional<std::string> ReadText(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > kMaxMetadataBytes) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::string text;
  text.reserve(static_cast<std::size_t>(size));
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (text.size() + count > kMaxMetadataBytes) return std::nullopt;
    text.append(buffer.data(), count);
  }
  if (input.bad()) return std::nullopt;
  return text;
}

bool IsDirectory(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_directory(path, ec);
}

bool IsFile(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec);
}

std::wstring TrimQuotes(std::wstring value) {
  value = core::Trim(value);
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::wstring ExecutableFromCommand(std::wstring value) {
  value = core::Trim(value);
  if (value.empty()) return {};
  if (value.front() == L'"') {
    const auto close = value.find(L'"', 1);
    return close == std::wstring::npos ? std::wstring{}
                                       : value.substr(1, close - 1);
  }
  const auto comma = value.find(L',');
  if (comma != std::wstring::npos) value.resize(comma);
  const auto exe = core::Lower(value).find(L".exe");
  if (exe != std::wstring::npos) value.resize(exe + 4);
  return core::Trim(value);
}

std::wstring ArgumentsFromCommand(std::wstring value) {
  value = core::Trim(value);
  if (value.empty()) return {};
  size_t end = std::wstring::npos;
  if (value.front() == L'"') {
    end = value.find(L'"', 1);
    if (end != std::wstring::npos) ++end;
  } else {
    const auto exe = core::Lower(value).find(L".exe");
    if (exe != std::wstring::npos) end = exe + 4;
  }
  return end == std::wstring::npos ? std::wstring{}
                                   : core::Trim(value.substr(end));
}

std::wstring ProviderKey(std::wstring_view provider) {
  std::wstring key = core::Lower(std::wstring(provider));
  key.erase(std::remove_if(key.begin(), key.end(), [](wchar_t ch) {
              return !std::iswalnum(ch);
            }),
            key.end());
  return key;
}

AppEntry Game(std::wstring provider, std::wstring id, std::wstring name,
              const fs::path& installDirectory) {
  AppEntry entry;
  entry.id = L"game:" + ProviderKey(provider) + L":" + id;
  entry.name = discovery::CleanName(name);
  entry.path = installDirectory.wstring();
  entry.source = L"game";
  entry.isGame = true;
  entry.gameProvider = std::move(provider);
  entry.keywords = discovery::UniqueKeywords(
      {entry.name, entry.gameProvider, L"game", L"games"});
  return entry;
}

wchar_t AsciiLowerChar(wchar_t ch) {
  return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const wchar_t a = left[i];
    const wchar_t b = right[i];
    if (a == b) continue;
    const wchar_t la = a < 128 ? AsciiLowerChar(a) : static_cast<wchar_t>(std::towlower(a));
    const wchar_t lb = b < 128 ? AsciiLowerChar(b) : static_cast<wchar_t>(std::towlower(b));
    if (la != lb) return false;
  }
  return true;
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view part) {
  if (part.empty()) return true;
  if (part.size() > value.size()) return false;
  const std::size_t limit = value.size() - part.size();
  for (std::size_t i = 0; i <= limit; ++i) {
    if (EqualsInsensitive(value.substr(i, part.size()), part)) return true;
  }
  return false;
}

std::wstring UnescapeVdf(std::wstring_view raw) {
  std::wstring out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == L'\\' && i + 1 < raw.size() && raw[i + 1] == L'\\') {
      out.push_back(L'\\');
      ++i;
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

bool FindNextVdfPair(std::wstring_view text, std::wstring_view key,
                     std::size_t& cursor, std::wstring& valueOut) {
  while (cursor < text.size()) {
    const std::size_t openKey = text.find(L'"', cursor);
    if (openKey == std::wstring_view::npos) return false;
    const std::size_t keyStart = openKey + 1;
    if (keyStart + key.size() >= text.size()) return false;
    if (text[keyStart + key.size()] == L'"' &&
        EqualsInsensitive(text.substr(keyStart, key.size()), key)) {
      std::size_t pos = keyStart + key.size() + 1;
      while (pos < text.size() && std::iswspace(text[pos]) != 0) ++pos;
      if (pos < text.size() && text[pos] == L'"') {
        const std::size_t valStart = pos + 1;
        const std::size_t valEnd = text.find(L'"', valStart);
        if (valEnd != std::wstring_view::npos) {
          valueOut = UnescapeVdf(text.substr(valStart, valEnd - valStart));
          cursor = valEnd + 1;
          return true;
        }
      }
    }
    cursor = openKey + 1;
  }
  return false;
}

std::wstring VdfValue(std::wstring_view text, std::wstring_view key) {
  std::size_t cursor = 0;
  std::wstring value;
  return FindNextVdfPair(text, key, cursor, value) ? value : std::wstring{};
}

std::vector<fs::path> SteamLibraries(const fs::path& steamRoot) {
  std::vector<fs::path> roots;
  if (IsDirectory(steamRoot / L"steamapps")) roots.push_back(steamRoot);
  const auto text = ReadText(steamRoot / L"steamapps" / L"libraryfolders.vdf");
  if (!text) return roots;
  const std::wstring wide = extensions::Utf8ToWide(*text);
  std::size_t cursor = 0;
  std::wstring value;
  while (FindNextVdfPair(wide, L"path", cursor, value)) {
    if (value.empty()) continue;
    fs::path root(value);
    if (IsDirectory(root / L"steamapps") &&
        std::find(roots.begin(), roots.end(), root) == roots.end()) {
      roots.push_back(std::move(root));
    }
  }
  return roots;
}

bool IsSteamSupportPackage(std::wstring_view name,
                           std::wstring_view installName) {
  const std::wstring value =
      core::Lower(std::wstring(name) + L" " + std::wstring(installName));
  static constexpr std::array<std::wstring_view, 10> markers{
      L"steamworks common", L"steamworks shared", L"steam linux runtime",
      L"proton - ", L"proton easyanticheat", L"proton battleye",
      L"dedicated server", L"source sdk", L"redistributable",
      L"steamvr",
  };
  return std::any_of(markers.begin(), markers.end(), [&](std::wstring_view marker) {
    return value.find(marker) != std::wstring::npos;
  });
}

fs::path SteamIconPath(const fs::path& steamRoot, std::wstring_view appId) {
  const fs::path cacheRoot =
      steamRoot / L"appcache" / L"librarycache";
  const fs::path appCache = cacheRoot / appId;
  for (const auto& candidate : {
           cacheRoot / (std::wstring(appId) + L"_icon.jpg"),
           cacheRoot / (std::wstring(appId) + L"_icon.png"),
           appCache / L"icon.jpg",
           appCache / L"icon.png",
       }) {
    if (IsFile(candidate)) return candidate;
  }

  std::error_code ec;
  for (fs::directory_iterator it(appCache,
                                 fs::directory_options::skip_permission_denied,
                                 ec),
       end;
       it != end; it.increment(ec)) {
    if (ec || !it->is_regular_file(ec)) continue;
    const std::wstring extension =
        core::Lower(it->path().extension().wstring());
    const std::wstring stem = it->path().stem().wstring();
    const bool contentHash =
        stem.size() == 40 &&
        std::all_of(stem.begin(), stem.end(), [](wchar_t ch) {
          return std::iswxdigit(ch) != 0;
        });
    if (contentHash && (extension == L".jpg" || extension == L".jpeg" ||
                        extension == L".png")) {
      return it->path();
    }
  }
  return {};
}

void DiscoverSteam(const DiscoverySources& sources, std::vector<AppEntry>& out,
                   std::stop_token token) {
  const fs::path steamExe = sources.steamRoot / L"steam.exe";
  if (!IsFile(steamExe)) return;
  for (const auto& library : SteamLibraries(sources.steamRoot)) {
    std::error_code ec;
    for (fs::directory_iterator it(library / L"steamapps",
                                   fs::directory_options::skip_permission_denied,
                                   ec),
         end;
         it != end && !token.stop_requested(); it.increment(ec)) {
      if (ec || !it->is_regular_file(ec)) continue;
      const auto fileName = core::Lower(it->path().filename().wstring());
      if (!fileName.starts_with(L"appmanifest_") ||
          core::Lower(it->path().extension().wstring()) != L".acf") {
        continue;
      }
      const auto text = ReadText(it->path());
      if (!text) continue;
      const std::wstring wide = extensions::Utf8ToWide(*text);
      const std::wstring appId = VdfValue(wide, L"appid");
      const std::wstring name = VdfValue(wide, L"name");
      const std::wstring installName = VdfValue(wide, L"installdir");
      const std::wstring stateFlags = VdfValue(wide, L"StateFlags");
      unsigned long flags = 0;
      try {
        flags = std::stoul(stateFlags);
      } catch (...) {
        continue;
      }
      if (appId.empty() || name.empty() || installName.empty() ||
          (flags & 4u) == 0 || IsSteamSupportPackage(name, installName)) {
        continue;
      }
      const fs::path installDir = library / L"steamapps" / L"common" / installName;
      if (!IsDirectory(installDir)) continue;
      auto game = Game(L"Steam", appId, name, installDir);
      game.launchType = LaunchType::Exe;
      game.launchTarget = steamExe.wstring();
      game.args = L"-silent \"steam://rungameid/" + appId + L"\"";
      game.cwd = sources.steamRoot.wstring();
      game.iconKey = SteamIconPath(sources.steamRoot, appId).wstring();
      out.push_back(std::move(game));
    }
  }
}

const json::Value* JsonMember(const json::Value& object, const char* name,
                              json::Value::Type type) {
  const auto* value = object.Find(name);
  return value && value->type == type ? value : nullptr;
}

std::wstring JsonString(const json::Value& object, const char* name) {
  const auto* value = JsonMember(object, name, json::Value::Type::String);
  return value ? extensions::Utf8ToWide(value->str) : std::wstring{};
}

bool JsonBool(const json::Value& object, const char* name, bool fallback) {
  const auto* value = JsonMember(object, name, json::Value::Type::Bool);
  return value ? value->boolean : fallback;
}

bool JsonArrayContains(const json::Value& object, const char* name,
                       std::string_view expected) {
  const auto* value = JsonMember(object, name, json::Value::Type::Array);
  if (!value) return false;
  for (const auto& item : value->array) {
    if (item.type != json::Value::Type::String) continue;
    std::string lower = item.str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (lower == expected) return true;
  }
  return false;
}

void DiscoverEpic(const DiscoverySources& sources, std::vector<AppEntry>& out,
                  std::stop_token token) {
  if (!IsDirectory(sources.epicManifestDirectory)) return;
  std::set<std::wstring> seen;
  std::error_code ec;
  for (fs::directory_iterator it(sources.epicManifestDirectory,
                                 fs::directory_options::skip_permission_denied,
                                 ec),
       end;
       it != end && !token.stop_requested(); it.increment(ec)) {
    if (ec || !it->is_regular_file(ec) ||
        core::Lower(it->path().extension().wstring()) != L".item") {
      continue;
    }
    const auto text = ReadText(it->path());
    const auto root = text ? json::Parse(*text) : std::nullopt;
    if (!root || root->type != json::Value::Type::Object) continue;
    if (!JsonBool(*root, "bIsApplication", false) ||
        !JsonBool(*root, "bIsExecutable", false) ||
        JsonBool(*root, "bIsIncompleteInstall", true) ||
        !JsonArrayContains(*root, "AppCategories", "games")) {
      continue;
    }
    const std::wstring appName = JsonString(*root, "AppName");
    const std::wstring displayName = JsonString(*root, "DisplayName");
    const fs::path installDir(JsonString(*root, "InstallLocation"));
    const fs::path launchExe = installDir / JsonString(*root, "LaunchExecutable");
    if (appName.empty() || displayName.empty() || !IsDirectory(installDir) ||
        !IsFile(launchExe) || !seen.insert(core::Lower(appName)).second) {
      continue;
    }
    auto game = Game(L"Epic Games", appName, displayName, installDir);
    game.launchType = LaunchType::Shell;
    game.launchTarget = L"com.epicgames.launcher://apps/" + appName +
                        L"?action=launch&silent=true";
    game.targetPath = launchExe.wstring();
    game.iconKey = launchExe.wstring();
    out.push_back(std::move(game));
  }
}

std::optional<AppEntry> GogGame(const UninstallRecord& record,
                                const fs::path& galaxyExe) {
  if (!EqualsInsensitive(record.publisher, L"gog.com")) {
    return std::nullopt;
  }
  const std::wstring_view keyName = record.registryKeyName;
  if (keyName.size() < 5 ||
      !EqualsInsensitive(keyName.substr(keyName.size() - 4), L"_is1")) {
    return std::nullopt;
  }
  const std::wstring_view idView = keyName.substr(0, keyName.size() - 4);
  for (const wchar_t ch : idView) {
    if (ch < L'0' || ch > L'9') return std::nullopt;
  }
  const fs::path installDir(record.installLocation);
  if (!IsDirectory(installDir)) return std::nullopt;
  const std::wstring gameId(idView);
  auto game = Game(L"GOG", gameId, record.displayName, installDir);

  const auto manifestText = ReadText(installDir / (L"goggame-" + gameId + L".info"));
  const auto manifest = manifestText ? json::Parse(*manifestText) : std::nullopt;
  if (manifest && manifest->type == json::Value::Type::Object) {
    if (const auto* tasks = JsonMember(*manifest, "playTasks", json::Value::Type::Array)) {
      for (const auto& task : tasks->array) {
        if (task.type != json::Value::Type::Object ||
            !JsonBool(task, "isPrimary", false)) {
          continue;
        }
        const std::wstring pathText = JsonString(task, "path");
        fs::path executable(pathText);
        if (executable.is_relative()) executable = installDir / executable;
        if (!IsFile(executable)) continue;
        game.launchType = LaunchType::Exe;
        game.launchTarget = executable.wstring();
        game.targetPath = executable.wstring();
        game.args = JsonString(task, "arguments");
        fs::path cwd(JsonString(task, "workingDir"));
        if (cwd.empty()) cwd = installDir;
        if (cwd.is_relative()) cwd = installDir / cwd;
        game.cwd = cwd.wstring();
        game.iconKey = executable.wstring();
        game.adminSupported = true;
        return game;
      }
    }
  }

  // The local game manifest is also our proof that this uninstall entry is a
  // runnable base game rather than DLC, bonus content, or a stale registry key.
  if (!manifest || manifest->type != json::Value::Type::Object ||
      !IsFile(galaxyExe)) {
    return std::nullopt;
  }
  game.launchType = LaunchType::Exe;
  game.launchTarget = galaxyExe.wstring();
  game.args = L"/launchViaAutostart /gameId=" + gameId +
              L" /command=runGame /path=\"" + installDir.wstring() + L"\"";
  game.cwd = galaxyExe.parent_path().wstring();
  game.iconKey = record.displayIcon.empty() ? galaxyExe.wstring()
                                            : ExecutableFromCommand(record.displayIcon);
  return game;
}

void DiscoverGog(const DiscoverySources& sources, std::vector<AppEntry>& out,
                 std::stop_token token) {
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    if (auto game = GogGame(record, sources.gogGalaxyExecutable)) {
      out.push_back(std::move(*game));
    }
  }
}

void DiscoverEa(const DiscoverySources& sources, std::vector<AppEntry>& out,
                std::stop_token token) {
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    if (!ContainsInsensitive(record.publisher, L"electronic arts") ||
        EqualsInsensitive(record.displayName, L"ea app")) {
      continue;
    }
    const fs::path installDir(record.installLocation);
    const fs::path executable(ExecutableFromCommand(record.displayIcon));
    if (!IsDirectory(installDir) || !IsFile(executable) ||
        !ContainsInsensitive(executable.extension().wstring(), L".exe") ||
        ContainsInsensitive(executable.filename().wstring(), L"uninstall") ||
        ContainsInsensitive(executable.filename().wstring(), L"cleanup")) {
      continue;
    }
    auto game = Game(L"EA app", record.registryKeyName, record.displayName,
                     installDir);
    game.launchType = LaunchType::Exe;
    game.launchTarget = executable.wstring();
    game.targetPath = executable.wstring();
    game.cwd = installDir.wstring();
    game.iconKey = executable.wstring();
    game.adminSupported = true;
    out.push_back(std::move(game));
  }
}

void DiscoverUbisoft(const DiscoverySources& sources, std::vector<AppEntry>& out,
                     std::stop_token token) {
  for (const auto& install : sources.ubisoftInstalls) {
    if (token.stop_requested()) return;
    const fs::path installDir(install.installDirectory);
    if (install.id.empty() || !IsDirectory(installDir)) continue;
    std::wstring name = installDir.filename().wstring();
    if (name.empty()) name = L"Ubisoft game " + install.id;
    auto game = Game(L"Ubisoft Connect", install.id, name, installDir);
    game.launchType = LaunchType::Shell;
    game.launchTarget = L"uplay://launch/" + install.id;
    out.push_back(std::move(game));
  }
}

std::wstring BattleUid(std::wstring_view uninstallString) {
  constexpr std::wstring_view kPrefix = L"--uid=";
  if (uninstallString.size() <= kPrefix.size()) return {};
  for (std::size_t pos = 0; pos + kPrefix.size() < uninstallString.size(); ++pos) {
    if (EqualsInsensitive(uninstallString.substr(pos, kPrefix.size()), kPrefix)) {
      const std::size_t start = pos + kPrefix.size();
      std::size_t end = start;
      while (end < uninstallString.size() &&
             std::iswspace(uninstallString[end]) == 0 &&
             uninstallString[end] != L'"') {
        ++end;
      }
      if (end > start) return std::wstring(uninstallString.substr(start, end - start));
      pos = end;
    }
  }
  return {};
}

void AddBattleGame(std::vector<AppEntry>& out, std::set<std::wstring>& ids,
                   const fs::path& battleExe, std::wstring uid,
                   std::wstring name, const fs::path& installDir) {
  if (uid.empty() || !IsDirectory(installDir) || !IsFile(battleExe) ||
      !ids.insert(core::Lower(uid)).second) {
    return;
  }
  if (name.empty()) name = installDir.filename().wstring();
  auto game = Game(L"Battle.net", uid, name, installDir);
  game.launchType = LaunchType::Exe;
  game.launchTarget = battleExe.wstring();
  game.args = L"--exec=\"launch " + uid + L"\"";
  game.cwd = battleExe.parent_path().wstring();
  game.iconKey = battleExe.wstring();
  out.push_back(std::move(game));
}

bool ReadVarint(std::span<const unsigned char> data, size_t& pos,
                std::uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64 && pos < data.size(); shift += 7) {
    const unsigned char byte = data[pos++];
    value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

struct ProtoField {
  unsigned number = 0;
  std::span<const unsigned char> bytes;
};

std::vector<ProtoField> LengthFields(std::span<const unsigned char> data) {
  std::vector<ProtoField> fields;
  size_t pos = 0;
  while (pos < data.size()) {
    std::uint64_t tag = 0;
    if (!ReadVarint(data, pos, tag) || tag == 0) break;
    const unsigned wire = static_cast<unsigned>(tag & 7);
    const unsigned number = static_cast<unsigned>(tag >> 3);
    if (wire == 2) {
      std::uint64_t length = 0;
      if (!ReadVarint(data, pos, length) || length > data.size() - pos) break;
      fields.push_back({number, data.subspan(pos, static_cast<size_t>(length))});
      pos += static_cast<size_t>(length);
    } else if (wire == 0) {
      std::uint64_t ignored = 0;
      if (!ReadVarint(data, pos, ignored)) break;
    } else if (wire == 1) {
      if (data.size() - pos < 8) break;
      pos += 8;
    } else if (wire == 5) {
      if (data.size() - pos < 4) break;
      pos += 4;
    } else {
      break;
    }
  }
  return fields;
}

std::wstring ProtoString(std::span<const unsigned char> bytes) {
  return extensions::Utf8ToWide(std::string(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::optional<std::wstring> BattleProductName(std::wstring_view code) {
  static constexpr std::pair<std::wstring_view, std::wstring_view> kNames[] = {
      {L"auks", L"Call of Duty: Black Ops 4"},
      {L"d3", L"Diablo III"},
      {L"d4", L"Diablo IV"},
      {L"fen", L"Diablo II: Resurrected"},
      {L"hero", L"Heroes of the Storm"},
      {L"lazr", L"Call of Duty: Modern Warfare II"},
      {L"odin", L"Call of Duty: Modern Warfare"},
      {L"pro", L"Overwatch"},
      {L"s1", L"StarCraft"},
      {L"s2", L"StarCraft II"},
      {L"wow", L"World of Warcraft"},
      {L"wow_classic", L"World of Warcraft Classic"},
      {L"wow_classic_era", L"World of Warcraft Classic Era"},
      {L"wtcg", L"Hearthstone"},
      {L"zeus", L"Call of Duty: Black Ops Cold War"},
  };
  if (code.empty() || code.size() > 24) return std::nullopt;
  wchar_t lowerBuf[24];
  for (std::size_t i = 0; i < code.size(); ++i) {
    lowerBuf[i] = AsciiLowerChar(code[i]);
  }
  const std::wstring_view key(lowerBuf, code.size());
  const auto it = std::lower_bound(
      std::begin(kNames), std::end(kNames), key,
      [](const auto& entry, std::wstring_view target) {
        return entry.first < target;
      });
  if (it == std::end(kNames) || it->first != key) return std::nullopt;
  return std::wstring(it->second);
}

void DiscoverBattleNet(const DiscoverySources& sources,
                       std::vector<AppEntry>& out, std::stop_token token) {
  std::set<std::wstring> ids;
  for (const auto& record : sources.uninstallRecords) {
    if (token.stop_requested()) return;
    if (!ContainsInsensitive(record.uninstallString, L"battle.net")) {
      continue;
    }
    const std::wstring uid = BattleUid(record.uninstallString);
    if (uid.empty()) continue;
    AddBattleGame(out, ids, sources.battleNetExecutable, uid,
                  record.displayName, record.installLocation);
  }

  if (token.stop_requested()) return;
  const auto database = ReadText(sources.battleNetProductDb);
  if (!database) return;
  const std::span<const unsigned char> bytes(
      reinterpret_cast<const unsigned char*>(database->data()),
      database->size());
  for (const auto& wrapper : LengthFields(bytes)) {
    if (token.stop_requested()) return;
    std::wstring uid;
    std::wstring productCode;
    fs::path installDir;
    for (const auto& field : LengthFields(wrapper.bytes)) {
      if (field.number == 1) uid = ProtoString(field.bytes);
      if (field.number == 2) productCode = ProtoString(field.bytes);
      if (field.number == 3) {
        const fs::path directPath(ProtoString(field.bytes));
        if (IsDirectory(directPath)) installDir = directPath;
        for (const auto& nested : LengthFields(field.bytes)) {
          if (nested.number == 1) installDir = ProtoString(nested.bytes);
        }
      }
    }
    auto name = BattleProductName(productCode);
    std::wstring launchId = productCode;
    if (!name) {
      name = BattleProductName(uid);
      launchId = uid;
    }
    if (!name) continue;
    AddBattleGame(out, ids, sources.battleNetExecutable, launchId, *name,
                  installDir);
  }
}

bool IsWordChar(wchar_t ch) {
  return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
         (ch >= L'0' && ch <= L'9') || ch == L'_';
}

std::wstring XmlAttribute(std::wstring_view text, std::wstring_view element,
                          std::wstring_view attribute) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t lt = text.find(L'<', pos);
    if (lt == std::wstring_view::npos) return {};
    std::size_t cur = lt + 1;
    while (cur < text.size() && std::iswspace(text[cur]) != 0) ++cur;
    if (cur + element.size() <= text.size() &&
        EqualsInsensitive(text.substr(cur, element.size()), element) &&
        (cur + element.size() == text.size() ||
         !IsWordChar(text[cur + element.size()]))) {
      cur += element.size();
      const std::size_t gt = text.find(L'>', cur);
      const std::size_t tagEnd = gt == std::wstring_view::npos ? text.size() : gt;
      for (std::size_t attrPos = cur; attrPos + attribute.size() < tagEnd; ++attrPos) {
        if (attrPos > cur && IsWordChar(text[attrPos - 1])) continue;
        if (!EqualsInsensitive(text.substr(attrPos, attribute.size()), attribute)) {
          continue;
        }
        std::size_t after = attrPos + attribute.size();
        if (after < tagEnd && IsWordChar(text[after])) continue;
        while (after < tagEnd && std::iswspace(text[after]) != 0) ++after;
        if (after >= tagEnd || text[after] != L'=') continue;
        ++after;
        while (after < tagEnd && std::iswspace(text[after]) != 0) ++after;
        if (after >= tagEnd || text[after] != L'"') continue;
        const std::size_t valStart = after + 1;
        const std::size_t valEnd = text.find(L'"', valStart);
        if (valEnd != std::wstring_view::npos && valEnd < tagEnd && valEnd > valStart) {
          return std::wstring(text.substr(valStart, valEnd - valStart));
        }
      }
    }
    pos = lt + 1;
  }
  return {};
}

void DiscoverXbox(const DiscoverySources& sources,
                  const std::vector<AppEntry>& shellApps,
                  std::vector<AppEntry>& out, std::stop_token token) {
  std::set<std::wstring> matchedAumids;
  for (const auto& root : sources.xboxGameRoots) {
    if (!IsDirectory(root)) continue;
    std::error_code ec;
    for (fs::directory_iterator it(root,
                                   fs::directory_options::skip_permission_denied,
                                   ec),
         end;
         it != end && !token.stop_requested(); it.increment(ec)) {
      if (ec || !it->is_directory(ec)) continue;
      const fs::path content = it->path() / L"Content";
      const fs::path config = content / L"MicrosoftGame.config";
      const auto xmlBytes = ReadText(config);
      if (!xmlBytes) continue;
      const std::wstring xml = extensions::Utf8ToWide(*xmlBytes);
      const std::wstring identity = XmlAttribute(xml, L"Identity", L"Name");
      std::wstring name = XmlAttribute(xml, L"ShellVisuals", L"DefaultDisplayName");
      const std::wstring executable = XmlAttribute(xml, L"Executable", L"Name");
      const fs::path executablePath = content / executable;
      if (identity.empty() || executable.empty() || !IsFile(executablePath)) {
        continue;
      }
      if (name.empty() || name.starts_with(L"ms-resource:")) {
        name = it->path().filename().wstring();
      }
      const std::wstring lowerIdentity = core::Lower(identity);
      const std::wstring lowerName = core::Lower(name);
      const auto shell = std::find_if(shellApps.begin(), shellApps.end(),
                                      [&](const AppEntry& app) {
        if (app.appUserModelId.empty()) return false;
        return core::Lower(app.name) == lowerName ||
               core::Lower(app.appUserModelId).find(lowerIdentity) !=
                   std::wstring::npos;
      });
      if (shell == shellApps.end() ||
          !matchedAumids.insert(core::Lower(shell->appUserModelId)).second) {
        continue;
      }
      AppEntry game = *shell;
      game.id = L"game:xbox:" + identity;
      game.name = name;
      game.path = it->path().wstring();
      game.source = L"game";
      game.isGame = true;
      game.gameProvider = L"Xbox";
      game.targetPath = executablePath.wstring();
      game.keywords = discovery::UniqueKeywords(
          {name, identity, L"Xbox", L"Microsoft Store", L"game", L"games"});
      out.push_back(std::move(game));
    }
  }
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  wchar_t stackBuffer[256];
  DWORD bytes = sizeof(stackBuffer);
  std::wstring value;
  const LSTATUS firstStatus = RegQueryValueExW(
      key, name, nullptr, &type, reinterpret_cast<BYTE*>(stackBuffer), &bytes);
  if (firstStatus == ERROR_SUCCESS) {
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
      return {};
    }
    value.assign(stackBuffer, bytes / sizeof(wchar_t));
  } else if (firstStatus == ERROR_MORE_DATA) {
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
      return {};
    }
    value.resize((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
    DWORD bufferBytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(value.data()), &bufferBytes) !=
        ERROR_SUCCESS) {
      return {};
    }
    value.resize(bufferBytes / sizeof(wchar_t));
  } else {
    return {};
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  if (type == REG_EXPAND_SZ && !value.empty()) {
    const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed > 1) {
      std::wstring expanded(needed, L'\0');
      ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
      while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
      value = std::move(expanded);
    }
  }
  return value;
}

std::wstring ReadRegistryPath(HKEY root, const wchar_t* subkey,
                              const wchar_t* value, REGSAM view = 0) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return {};
  }
  const std::wstring result = ReadRegistryString(raw, value);
  RegCloseKey(raw);
  return result;
}

void ReadUninstallView(HKEY root, REGSAM view,
                       std::vector<UninstallRecord>& out) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return;
  }
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 512> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (RegEnumKeyExW(raw, index, name.data(), &length, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS) {
      break;
    }
    HKEY child = nullptr;
    if (RegOpenKeyExW(raw, name.data(), 0, KEY_READ | view, &child) != ERROR_SUCCESS) {
      continue;
    }
    std::wstring displayName = ReadRegistryString(child, L"DisplayName");
    if (displayName.empty()) {
      RegCloseKey(child);
      continue;
    }
    UninstallRecord record;
    record.registryKeyName.assign(name.data(), length);
    record.displayName = std::move(displayName);
    record.publisher = ReadRegistryString(child, L"Publisher");
    record.installLocation = ReadRegistryString(child, L"InstallLocation");
    record.displayIcon = ReadRegistryString(child, L"DisplayIcon");
    record.uninstallString = ReadRegistryString(child, L"UninstallString");
    RegCloseKey(child);
    out.push_back(std::move(record));
  }
  RegCloseKey(raw);
}

void ReadUbisoftView(REGSAM view, std::vector<UbisoftInstall>& out) {
  HKEY raw = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Ubisoft\\Launcher\\Installs", 0,
                    KEY_READ | view, &raw) != ERROR_SUCCESS) {
    return;
  }
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, 256> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (RegEnumKeyExW(raw, index, name.data(), &length, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS) {
      break;
    }
    HKEY child = nullptr;
    if (RegOpenKeyExW(raw, name.data(), 0, KEY_READ | view, &child) != ERROR_SUCCESS) {
      continue;
    }
    UbisoftInstall install;
    install.id.assign(name.data(), length);
    install.installDirectory = ReadRegistryString(child, L"InstallDir");
    RegCloseKey(child);
    if (!install.installDirectory.empty()) out.push_back(std::move(install));
  }
  RegCloseKey(raw);
}

std::vector<fs::path> XboxRoots() {
  std::vector<fs::path> roots;
  const DWORD drives = GetLogicalDrives();
  for (int index = 0; index < 26; ++index) {
    if ((drives & (1u << index)) == 0) continue;
    const wchar_t driveRoot[4] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
    const UINT driveType = GetDriveTypeW(driveRoot);
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) continue;
    const fs::path drive(driveRoot);
    const fs::path conventional = drive / L"XboxGames";
    if (IsDirectory(conventional)) roots.push_back(conventional);

    const auto gamingRoot = ReadText(drive / L".GamingRoot");
    if (!gamingRoot) continue;
    const std::span<const unsigned char> bytes(
        reinterpret_cast<const unsigned char*>(gamingRoot->data()),
        gamingRoot->size());
    for (size_t offset : {size_t{6}, size_t{8}}) {
      if (offset >= bytes.size()) continue;
      std::wstring folder;
      for (size_t pos = offset; pos + 1 < bytes.size(); pos += 2) {
        const wchar_t ch = static_cast<wchar_t>(bytes[pos] | (bytes[pos + 1] << 8));
        if (ch == L'\0') break;
        if (ch < 0x20) {
          folder.clear();
          break;
        }
        folder.push_back(ch);
      }
      const fs::path candidate = drive / folder;
      if (!folder.empty() && IsDirectory(candidate) &&
          std::find(roots.begin(), roots.end(), candidate) == roots.end()) {
        roots.push_back(candidate);
      }
    }
  }
  return roots;
}

}  // namespace

DiscoverySources ReadLocalSources() {
  DiscoverySources sources;
  std::wstring steam = ReadRegistryPath(HKEY_CURRENT_USER,
                                        L"Software\\Valve\\Steam", L"SteamPath");
  if (steam.empty()) {
    steam = ReadRegistryPath(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam",
                             L"InstallPath", KEY_WOW64_32KEY);
  }
  std::replace(steam.begin(), steam.end(), L'/', L'\\');
  sources.steamRoot = steam;

  wchar_t programDataBuf[512];
  DWORD programDataLen = GetEnvironmentVariableW(
      L"ProgramData", programDataBuf, static_cast<DWORD>(std::size(programDataBuf)));
  std::wstring programData;
  if (programDataLen > 0 && programDataLen < std::size(programDataBuf)) {
    programData.assign(programDataBuf, programDataLen);
  } else if (programDataLen >= std::size(programDataBuf)) {
    programData.resize(programDataLen);
    const DWORD written = GetEnvironmentVariableW(
        L"ProgramData", programData.data(), programDataLen);
    if (written > 0 && written < programDataLen) {
      programData.resize(written);
    } else {
      programData.clear();
    }
  }
  if (!programData.empty()) {
    sources.epicManifestDirectory =
        fs::path(programData) / L"Epic" / L"EpicGamesLauncher" /
        L"Data" / L"Manifests";
    sources.battleNetProductDb = fs::path(programData) / L"Battle.net" /
                                 L"Agent" / L"product.db";
  }

  ReadUninstallView(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_CURRENT_USER, KEY_WOW64_64KEY,
                    sources.uninstallRecords);
  ReadUninstallView(HKEY_CURRENT_USER, KEY_WOW64_32KEY,
                    sources.uninstallRecords);
  ReadUbisoftView(KEY_WOW64_32KEY, sources.ubisoftInstalls);
  ReadUbisoftView(KEY_WOW64_64KEY, sources.ubisoftInstalls);

  for (const auto& record : sources.uninstallRecords) {
    const std::wstring lowerName = core::Lower(record.displayName);
    if (lowerName == L"battle.net") {
      fs::path executable(ExecutableFromCommand(record.displayIcon));
      if (!IsFile(executable)) executable = fs::path(record.installLocation) / L"Battle.net.exe";
      if (IsFile(executable)) sources.battleNetExecutable = executable;
    }
    if (lowerName == L"gog galaxy") {
      fs::path executable = fs::path(record.installLocation) / L"GalaxyClient.exe";
      if (IsFile(executable)) sources.gogGalaxyExecutable = executable;
    }
  }
  sources.xboxGameRoots = XboxRoots();
  return sources;
}

std::vector<AppEntry> DiscoverFromSources(
    const DiscoverySources& sources, const std::vector<AppEntry>& shellApps,
    std::stop_token stopToken) {
  std::vector<AppEntry> games;
  DiscoverSteam(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverEpic(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverGog(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverEa(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverUbisoft(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverBattleNet(sources, games, stopToken);
  if (!stopToken.stop_requested()) DiscoverXbox(sources, shellApps, games, stopToken);
  return games;
}

std::vector<AppEntry> DiscoverInstalledGames(
    const std::vector<AppEntry>& shellApps, std::stop_token stopToken) {
  return DiscoverFromSources(ReadLocalSources(), shellApps, stopToken);
}

}  // namespace feathercast::games
