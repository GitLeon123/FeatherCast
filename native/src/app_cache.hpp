#pragma once

#include "app_types.hpp"
#include "filesystem_semantics.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::discovery_cache {

constexpr std::size_t kMaxBytes = 32 * 1024 * 1024;

// Versioned, Windows-local cache. Never serialize AppEntry's object layout.
inline std::string Encode(const std::vector<app::AppEntry>& apps, long long now) {
  std::string bytes = "FCAP0001";
  auto number = [&bytes](auto value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  auto string = [&](const std::wstring& value) {
    number(static_cast<std::uint32_t>(value.size()));
    bytes.append(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
  };
  number(now);
  number(static_cast<std::uint32_t>(apps.size()));
  for (const auto& entry : apps) {
    for (const auto* value : {&entry.id, &entry.name, &entry.path, &entry.source,
                             &entry.launchTarget, &entry.targetPath, &entry.args,
                             &entry.cwd, &entry.appUserModelId, &entry.iconKey,
                             &entry.gameProvider}) string(*value);
    number(static_cast<std::uint32_t>(entry.launchType));
    number(static_cast<std::uint32_t>((entry.isGame ? 1 : 0) |
        (entry.adminSupported ? 2 : 0) | (entry.systemEssential ? 4 : 0)));
    number(static_cast<std::uint32_t>(entry.keywords.size()));
    for (const auto& keyword : entry.keywords) string(keyword);
  }
  return bytes;
}

inline std::optional<std::vector<app::AppEntry>> Decode(std::string_view bytes,
                                                       long long /*now*/) {
  if (bytes.size() > kMaxBytes || !bytes.starts_with("FCAP0001")) return std::nullopt;
  bytes.remove_prefix(8);
  auto number = [&bytes](auto& value) {
    if (bytes.size() < sizeof(value)) return false;
    std::memcpy(&value, bytes.data(), sizeof(value));
    bytes.remove_prefix(sizeof(value));
    return true;
  };
  auto string = [&](std::wstring& value) {
    std::uint32_t length = 0;
    if (!number(length) || length > bytes.size() / sizeof(wchar_t)) return false;
    value.resize(length);
    std::memcpy(value.data(), bytes.data(), length * sizeof(wchar_t));
    bytes.remove_prefix(length * sizeof(wchar_t));
    return true;
  };
  long long savedAt = 0;
  std::uint32_t count = 0;
  // Keep the last successful catalog across reboots and clock changes. It is
  // only a startup preview; every process refreshes it from the shell.
  if (!number(savedAt) || savedAt < 0 || !number(count) || count > 50000)
    return std::nullopt;
  std::vector<app::AppEntry> apps;
  for (std::uint32_t i = 0; i < count; ++i) {
    app::AppEntry entry;
    for (auto* value : {&entry.id, &entry.name, &entry.path, &entry.source,
                        &entry.launchTarget, &entry.targetPath, &entry.args,
                        &entry.cwd, &entry.appUserModelId, &entry.iconKey,
                        &entry.gameProvider}) {
      if (!string(*value)) return std::nullopt;
    }
    std::uint32_t type = 0, flags = 0, keywords = 0;
    if (entry.id.empty() || entry.name.empty() || !number(type) ||
        type > static_cast<std::uint32_t>(app::LaunchType::Shell) ||
        !number(flags) || flags > 7 || !number(keywords) || keywords > 4096)
      return std::nullopt;
    entry.launchType = static_cast<app::LaunchType>(type);
    entry.isGame = (flags & 1) != 0;
    entry.adminSupported = (flags & 2) != 0;
    entry.systemEssential = (flags & 4) != 0;
    for (std::uint32_t k = 0; k < keywords; ++k) {
      std::wstring keyword;
      if (!string(keyword)) return std::nullopt;
      entry.keywords.push_back(std::move(keyword));
    }
    apps.push_back(std::move(entry));
  }
  if (!bytes.empty()) return std::nullopt;
  return apps;
}

inline std::optional<std::vector<app::AppEntry>> Load(
    const std::filesystem::path& path, long long now) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return std::nullopt;
  const auto size = file.tellg();
  if (size < 0 || size > static_cast<std::streamoff>(kMaxBytes)) return std::nullopt;
  std::string bytes(static_cast<std::size_t>(size), '\0');
  file.seekg(0);
  if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
  return Decode(bytes, now);
}

inline void Save(const std::filesystem::path& path,
                 const std::vector<app::AppEntry>& apps, long long now) {
  // A shell that is still starting must not erase a working catalog.
  if (apps.empty()) return;
  const auto bytes = Encode(apps, now);
  if (bytes.size() <= kMaxBytes)
    (void)filesystem_semantics::ReplaceFileDurably(path, bytes);
}

}  // namespace feathercast::discovery_cache
