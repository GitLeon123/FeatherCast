#pragma once

// Settings model plus JSON (de)serialization, extracted from main.cpp so the
// round-trip can be unit-tested. Clamping of overlayWidth/maxResults stays at
// the call sites in main.cpp next to the MIN/MAX constants.

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core.hpp"
#include "extension_protocol.hpp"  // Utf8ToWide / WideToUtf8
#include "json.hpp"

namespace feathercast::settings {

inline constexpr int kCurrentSettingsSchemaVersion = 3;
inline constexpr int kMinimumTextSizePercent = 90;
inline constexpr int kMaximumTextSizePercent = 200;
inline constexpr int kTextSizeStepPercent = 10;

inline constexpr int ClampTextSizePercent(int value) {
  return std::clamp(value, kMinimumTextSizePercent,
                    kMaximumTextSizePercent);
}

inline constexpr int StepTextSizePercent(int value, int direction) {
  return ClampTextSizePercent(
      value + direction * kTextSizeStepPercent);
}

inline std::wstring TextSizePercentLabel(int value) {
  return std::to_wstring(ClampTextSizePercent(value)) + L"%";
}

enum class AnimationLevel {
  Off,
  Reduced,
  Full,
};

enum class AnimationKind {
  Fade,
  Spatial,
  ControlFeedback,
};

inline constexpr std::string_view AnimationLevelKey(AnimationLevel level) {
  switch (level) {
    case AnimationLevel::Off: return "off";
    case AnimationLevel::Reduced: return "reduced";
    case AnimationLevel::Full: return "full";
  }
  return "full";
}

inline constexpr std::wstring_view AnimationLevelLabel(AnimationLevel level) {
  switch (level) {
    case AnimationLevel::Off: return L"Off";
    case AnimationLevel::Reduced: return L"Reduced";
    case AnimationLevel::Full: return L"Full";
  }
  return L"Full";
}

inline constexpr std::optional<AnimationLevel> ParseAnimationLevel(
    std::string_view value) {
  if (value == "off") return AnimationLevel::Off;
  if (value == "reduced") return AnimationLevel::Reduced;
  if (value == "full") return AnimationLevel::Full;
  return std::nullopt;
}

inline constexpr AnimationLevel StepAnimationLevel(AnimationLevel level,
                                                   int direction) {
  const int current = static_cast<int>(level);
  return static_cast<AnimationLevel>(
      std::clamp(current + direction, static_cast<int>(AnimationLevel::Off),
                 static_cast<int>(AnimationLevel::Full)));
}

inline constexpr bool AnimationAllowed(AnimationLevel level,
                                       AnimationKind kind,
                                       bool systemAnimationsEnabled = true,
                                       bool highContrast = false) {
  if (!systemAnimationsEnabled || highContrast || level == AnimationLevel::Off) {
    return false;
  }
  return level == AnimationLevel::Full || kind != AnimationKind::Spatial;
}

// A user-defined keyword that opens a URL, file, or folder directly.
struct Quicklink {
  std::wstring keyword;
  std::wstring name;
  std::wstring target;
};

inline std::map<std::wstring, std::wstring> DefaultSearchEngines() {
  return {
      {L"g", L"https://www.google.com/search?q=%s"},
      {L"ddg", L"https://duckduckgo.com/?q=%s"},
      {L"yt", L"https://www.youtube.com/results?search_query=%s"},
      {L"gh", L"https://github.com/search?q=%s"},
      {L"w", L"https://en.wikipedia.org/w/index.php?search=%s"},
  };
}

struct PrivacySettings {
  bool clipboardEnabled = false;
  bool fileIndexEnabled = false;
  bool fileContentIndexEnabled = false;
  std::vector<std::wstring> roots;
  int retention = 50;
  int retentionDays = 0;
  std::vector<std::wstring> clipboardExcludedApps;
  std::vector<std::wstring> fileIndexExcludePatterns;
};

struct Settings {
  std::wstring shortcut = L"Alt+Space";
  std::wstring screenshotFullscreenShortcut = L"none";
  std::wstring screenshotRegionShortcut = L"none";
  std::wstring recordFullscreenShortcut = L"none";
  std::wstring recordRegionShortcut = L"none";
  std::vector<std::wstring> recentApps;
  std::vector<std::wstring> pinnedApps;
  std::vector<std::wstring> hiddenApps;
  std::map<std::wstring, std::wstring> appAliases;
  // Stable command id -> one user-defined alias. Invocation collections use
  // the stable keys produced by DisplayItem::InvocationKey().
  std::map<std::wstring, std::wstring> commandAliases;
  std::vector<std::wstring> pinnedItems;
  std::vector<std::wstring> recentItems;
  struct UsageStat {
    int launches = 0;
    long long lastUsed = 0;
  };
  std::map<std::wstring, UsageStat> usageStats;
  bool searchLearningEnabled = false;
  // Optional local preferences for app/command queries only, capped at 256.
  std::map<std::wstring, std::wstring> learnedQueryActions;
  bool compactMode = false;
  bool autoFitResultHeight = true;
  AnimationLevel animationLevel = AnimationLevel::Full;
  bool syncAccentColor = true;
  std::wstring customAccentColor = L"#5b6cff";
  // DirectWrite sizes are deliberately explicit, so expose a supported
  // FeatherCast preference instead of relying on Windows text-size settings
  // that do not consistently reach a custom Direct2D surface.
  int textSizePercent = 100;
  bool startOnStartup = false;
  bool updateChecksEnabled = true;
  long long lastUpdateAttempt = 0;
  long long lastUpdateCheck = 0;
  std::wstring dismissedUpdateVersion;
  int overlayWidth = 720;  // WIN_WIDTH in main.cpp
  int maxResults = 200;    // MAX_RESULTS in main.cpp
  bool showOpenWindows = true;
  bool showStoreApps = true;
  // Personal-data features are opt-in. privacyConsentVersion records that the
  // user has seen the disclosure, regardless of which features they enabled.
  int privacyConsentVersion = 0;
  bool clipboardHistoryEnabled = false;
  int clipboardHistoryLimit = 50;
  // Zero keeps the existing count-only retention behavior.
  int clipboardRetentionDays = 0;
  std::vector<std::wstring> clipboardExcludedApps;
  bool fileIndexEnabled = false;
  bool fileContentIndexEnabled = false;
  int fileIndexMaxEntries = 5000;
  std::vector<std::wstring> fileIndexRoots;
  std::vector<std::wstring> fileIndexExcludePatterns;
  bool diagnosticsEnabled = false;
  // Phone link (LAN server for the FeatherCast Phone app) is opt-in.
  bool phoneLinkEnabled = false;
  bool phoneClipboardSync = true;
  bool phoneNotificationToasts = true;
  // Toast when the phone battery drops to this level; 0 turns it off.
  int phoneLowBatteryPercent = 20;
  // Web search prefixes: keyword -> URL template containing "%s" for the query.
  std::map<std::wstring, std::wstring> searchEngines =
      DefaultSearchEngines();
  std::vector<Quicklink> quicklinks;
  std::vector<Quicklink> scripts;
  std::vector<Quicklink> workspaces;
  std::map<std::wstring, std::wstring> commandShortcuts;

  PrivacySettings Privacy() const {
    return {
      clipboardHistoryEnabled,
      fileIndexEnabled,
      fileContentIndexEnabled,
      fileIndexRoots,
      clipboardHistoryLimit,
      clipboardRetentionDays,
      clipboardExcludedApps,
      fileIndexExcludePatterns,
    };
  }
};

enum class ParseStatus {
  Missing,
  Valid,
  Invalid,
  UnsupportedVersion,
};

struct ParseResult {
  Settings value;
  ParseStatus status = ParseStatus::Missing;
  int documentVersion = 0;
};

inline std::string JsonEscape(const std::wstring& value) {
  const std::string in = feathercast::extensions::WideToUtf8(value);
  std::string out;
  for (const char ch : in) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      char buf[8]{};
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
      out += buf;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

namespace detail {

using feathercast::extensions::Utf8ToWide;
using feathercast::json::Value;

inline bool IsValidUtf8(std::string_view text) {
  const auto continuation = [](unsigned char ch) {
    return ch >= 0x80 && ch <= 0xBF;
  };

  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first <= 0x7F) {
      ++i;
      continue;
    }

    if (first >= 0xC2 && first <= 0xDF) {
      if (i + 1 >= text.size() ||
          !continuation(static_cast<unsigned char>(text[i + 1]))) {
        return false;
      }
      i += 2;
      continue;
    }

    if (first >= 0xE0 && first <= 0xEF) {
      if (i + 2 >= text.size()) return false;
      const auto second = static_cast<unsigned char>(text[i + 1]);
      const auto third = static_cast<unsigned char>(text[i + 2]);
      bool validSecond = continuation(second);
      if (first == 0xE0) validSecond = second >= 0xA0 && second <= 0xBF;
      else if (first == 0xED) validSecond = second >= 0x80 && second <= 0x9F;
      if (!validSecond || !continuation(third)) return false;
      i += 3;
      continue;
    }

    if (first >= 0xF0 && first <= 0xF4) {
      if (i + 3 >= text.size()) return false;
      const auto second = static_cast<unsigned char>(text[i + 1]);
      const auto third = static_cast<unsigned char>(text[i + 2]);
      const auto fourth = static_cast<unsigned char>(text[i + 3]);
      bool validSecond = continuation(second);
      if (first == 0xF0) validSecond = second >= 0x90 && second <= 0xBF;
      else if (first == 0xF4) validSecond = second >= 0x80 && second <= 0x8F;
      if (!validSecond || !continuation(third) || !continuation(fourth)) {
        return false;
      }
      i += 4;
      continue;
    }

    return false;
  }
  return true;
}

inline void ReadString(const Value& root, std::string_view key, std::wstring& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::String) {
    out = Utf8ToWide(value->str);
  }
}

inline void ReadBool(const Value& root, std::string_view key, bool& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Bool) {
    out = value->boolean;
  }
}

inline void ReadInt(const Value& root, std::string_view key, int& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Number) {
    if (const auto parsed = feathercast::json::ToInteger<int>(value->number)) {
      out = *parsed;
    }
  }
}

// Out-of-range values, including exactly 2^63, keep the default.
inline void ReadLongLong(const Value& root, std::string_view key, long long& out) {
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Number) {
    if (const auto parsed = feathercast::json::ToInteger<long long>(value->number)) {
      out = *parsed;
    }
  }
}

inline std::vector<std::wstring> ReadStringArray(const Value& root, std::string_view key) {
  std::vector<std::wstring> out;
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Array) {
    for (const auto& element : value->array) {
      if (element.type == Value::Type::String) out.push_back(Utf8ToWide(element.str));
    }
  }
  return out;
}

inline std::map<std::wstring, std::wstring> ReadStringObject(const Value& root, std::string_view key) {
  std::map<std::wstring, std::wstring> out;
  if (const Value* value = root.Find(key); value && value->type == Value::Type::Object) {
    for (const auto& member : value->object) {
      if (member.value.type == Value::Type::String) {
        out[Utf8ToWide(member.key)] = Utf8ToWide(member.value.str);
      }
    }
  }
  return out;
}

inline std::map<std::wstring, std::wstring> ReadCommandAliases(
    const Value& root, std::string_view key) {
  std::map<std::wstring, std::wstring> out;
  std::set<std::wstring> normalizedAliases;
  const Value* value = root.Find(key);
  if (!value || value->type != Value::Type::Object) return out;

  for (const auto& member : value->object) {
    const std::wstring targetId = feathercast::core::Trim(Utf8ToWide(member.key));
    if (targetId.empty() || member.value.type != Value::Type::String) continue;
    const auto validation =
        feathercast::core::ValidateAlias(Utf8ToWide(member.value.str));
    if (!validation.valid ||
        !normalizedAliases.insert(validation.normalized).second) {
      continue;
    }
    out[targetId] = validation.value;
  }
  return out;
}

// Missing or malformed fields keep their defaults (same lenient behavior as
// the previous scanner, but keys inside string values can no longer match).
inline Settings ParseSettingsRoot(const std::optional<Value>& root) {
  Settings settings;
  if (!root || root->type != Value::Type::Object) return settings;

  ReadString(*root, "shortcut", settings.shortcut);
  ReadString(*root, "screenshotFullscreenShortcut",
             settings.screenshotFullscreenShortcut);
  ReadString(*root, "screenshotRegionShortcut",
             settings.screenshotRegionShortcut);
  ReadString(*root, "recordFullscreenShortcut",
             settings.recordFullscreenShortcut);
  ReadString(*root, "recordRegionShortcut", settings.recordRegionShortcut);
  settings.recentApps = ReadStringArray(*root, "recentApps");
  settings.pinnedApps = ReadStringArray(*root, "pinnedApps");
  settings.hiddenApps = ReadStringArray(*root, "hiddenApps");
  settings.appAliases = ReadStringObject(*root, "appAliases");
  settings.commandAliases = ReadCommandAliases(*root, "commandAliases");
  settings.pinnedItems = ReadStringArray(*root, "pinnedItems");
  settings.recentItems = ReadStringArray(*root, "recentItems");
  ReadBool(*root, "searchLearningEnabled", settings.searchLearningEnabled);
  settings.learnedQueryActions = ReadStringObject(*root, "learnedQueryActions");
  std::erase_if(settings.learnedQueryActions, [](const auto& entry) {
    return entry.first.empty() || entry.first.size() > 64 || entry.first.find_first_of(L"\r\n") != std::wstring::npos ||
        entry.first.front() == L'@' || entry.first.front() == L'>' || entry.first.front() == L':' || entry.second.size() > 2048;
  });
  if (!settings.searchLearningEnabled) settings.learnedQueryActions.clear();
  while (settings.learnedQueryActions.size() > 256) {
    settings.learnedQueryActions.erase(settings.learnedQueryActions.begin());
  }
  if (const Value* stats = root->Find("usageStats"); stats && stats->type == Value::Type::Object) {
    for (const auto& member : stats->object) {
      if (member.value.type != Value::Type::Object) continue;
      Settings::UsageStat stat;
      ReadInt(member.value, "launches", stat.launches);
      ReadLongLong(member.value, "lastUsed", stat.lastUsed);
      settings.usageStats[Utf8ToWide(member.key)] = stat;
    }
  }
  ReadBool(*root, "compactMode", settings.compactMode);
  ReadBool(*root, "autoFitResultHeight", settings.autoFitResultHeight);
  bool legacyAnimationsEnabled = true;
  ReadBool(*root, "animationsEnabled", legacyAnimationsEnabled);
  settings.animationLevel = legacyAnimationsEnabled ? AnimationLevel::Full
                                                    : AnimationLevel::Off;
  if (const Value* animationLevel = root->Find("animationLevel");
      animationLevel && animationLevel->type == Value::Type::String) {
    if (const auto parsed = ParseAnimationLevel(animationLevel->str)) {
      settings.animationLevel = *parsed;
    }
  }
  ReadBool(*root, "syncAccentColor", settings.syncAccentColor);
  ReadString(*root, "customAccentColor", settings.customAccentColor);
  ReadInt(*root, "textSizePercent", settings.textSizePercent);
  settings.textSizePercent = ClampTextSizePercent(settings.textSizePercent);
  ReadBool(*root, "startOnStartup", settings.startOnStartup);
  ReadBool(*root, "updateChecksEnabled", settings.updateChecksEnabled);
  ReadLongLong(*root, "lastUpdateAttempt", settings.lastUpdateAttempt);
  ReadLongLong(*root, "lastUpdateCheck", settings.lastUpdateCheck);
  ReadString(*root, "dismissedUpdateVersion", settings.dismissedUpdateVersion);
  ReadInt(*root, "overlayWidth", settings.overlayWidth);
  ReadInt(*root, "maxResults", settings.maxResults);
  ReadBool(*root, "showOpenWindows", settings.showOpenWindows);
  ReadBool(*root, "showStoreApps", settings.showStoreApps);
  ReadInt(*root, "privacyConsentVersion", settings.privacyConsentVersion);
  ReadBool(*root, "clipboardHistoryEnabled", settings.clipboardHistoryEnabled);
  ReadBool(*root, "phoneLinkEnabled", settings.phoneLinkEnabled);
  ReadBool(*root, "phoneClipboardSync", settings.phoneClipboardSync);
  ReadBool(*root, "phoneNotificationToasts", settings.phoneNotificationToasts);
  ReadInt(*root, "phoneLowBatteryPercent", settings.phoneLowBatteryPercent);
  settings.phoneLowBatteryPercent = std::clamp(settings.phoneLowBatteryPercent, 0, 100);
  ReadInt(*root, "clipboardHistoryLimit", settings.clipboardHistoryLimit);
  ReadInt(*root, "clipboardRetentionDays", settings.clipboardRetentionDays);
  settings.clipboardExcludedApps =
      ReadStringArray(*root, "clipboardExcludedApps");
  ReadBool(*root, "fileIndexEnabled", settings.fileIndexEnabled);
  ReadBool(*root, "fileContentIndexEnabled", settings.fileContentIndexEnabled);
  ReadInt(*root, "fileIndexMaxEntries", settings.fileIndexMaxEntries);
  settings.fileIndexRoots = ReadStringArray(*root, "fileIndexRoots");
  settings.fileIndexExcludePatterns =
      ReadStringArray(*root, "fileIndexExcludePatterns");
  ReadBool(*root, "diagnosticsEnabled", settings.diagnosticsEnabled);
  if (const Value* engines = root->Find("searchEngines");
      engines && engines->type == Value::Type::Object) {
    settings.searchEngines = ReadStringObject(*root, "searchEngines");
  }
  if (const Value* links = root->Find("quicklinks"); links && links->type == Value::Type::Array) {
    for (const auto& element : links->array) {
      if (element.type != Value::Type::Object) continue;
      Quicklink link;
      ReadString(element, "keyword", link.keyword);
      ReadString(element, "name", link.name);
      ReadString(element, "target", link.target);
      if (!link.keyword.empty() && !link.target.empty()) settings.quicklinks.push_back(std::move(link));
    }
  }
  for (const auto& [key, destination] :
       std::array<std::pair<const char*, std::vector<Quicklink>*>, 2>{{
           {"scripts", &settings.scripts}, {"workspaces", &settings.workspaces}}}) {
    if (const Value* items = root->Find(key); items && items->type == Value::Type::Array) {
      for (const auto& element : items->array) {
        if (element.type != Value::Type::Object || destination->size() >= 256) continue;
        Quicklink item;
        ReadString(element, "keyword", item.keyword);
        ReadString(element, "name", item.name);
        ReadString(element, "target", item.target);
        if (!item.keyword.empty() && !item.target.empty()) destination->push_back(std::move(item));
      }
    }
  }
  settings.commandShortcuts = ReadStringObject(*root, "commandShortcuts");
  return settings;
}

}  // namespace detail

inline ParseResult ParseSettingsDocument(const std::string& text) {
  if (text.empty()) return {};
  if (!detail::IsValidUtf8(text)) {
    return {Settings{}, ParseStatus::Invalid, 0};
  }

  const auto root = feathercast::json::Parse(text);
  if (!root || root->type != feathercast::json::Value::Type::Object) {
    return {Settings{}, ParseStatus::Invalid, 0};
  }

  int documentVersion = 0;
  if (const auto* version = root->Find("schemaVersion")) {
    if (version->type != feathercast::json::Value::Type::Number ||
        !std::isfinite(version->number) ||
        std::floor(version->number) != version->number ||
        version->number < 0 ||
        version->number >
            static_cast<double>(std::numeric_limits<int>::max())) {
      return {Settings{}, ParseStatus::Invalid, 0};
    }
    documentVersion = static_cast<int>(version->number);
  }

  if (documentVersion > kCurrentSettingsSchemaVersion) {
    return {Settings{}, ParseStatus::UnsupportedVersion, documentVersion};
  }
  return {detail::ParseSettingsRoot(root), ParseStatus::Valid,
          documentVersion};
}

// Compatibility wrapper for callers that intentionally want defaults for a
// missing or invalid document.
inline Settings ParseSettings(const std::string& text) {
  return ParseSettingsDocument(text).value;
}

namespace detail {

inline void WriteStringArray(std::ostringstream& out, const std::vector<std::wstring>& values) {
  out << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << JsonEscape(values[i]) << "\"";
  }
  out << "]";
}

inline void WriteStringObject(std::ostringstream& out, const std::map<std::wstring, std::wstring>& values) {
  out << "{";
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) out << ", ";
    first = false;
    out << "\"" << JsonEscape(key) << "\": \"" << JsonEscape(value) << "\"";
  }
  out << "}";
}

}  // namespace detail

inline std::string SerializeSettings(const Settings& settings) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schemaVersion\": " << kCurrentSettingsSchemaVersion << ",\n";
  out << "  \"shortcut\": \"" << JsonEscape(settings.shortcut) << "\",\n";
  out << "  \"screenshotFullscreenShortcut\": \""
      << JsonEscape(settings.screenshotFullscreenShortcut) << "\",\n";
  out << "  \"screenshotRegionShortcut\": \""
      << JsonEscape(settings.screenshotRegionShortcut) << "\",\n";
  out << "  \"recordFullscreenShortcut\": \""
      << JsonEscape(settings.recordFullscreenShortcut) << "\",\n";
  out << "  \"recordRegionShortcut\": \""
      << JsonEscape(settings.recordRegionShortcut) << "\",\n";
  out << "  \"recentApps\": ";
  detail::WriteStringArray(out, settings.recentApps);
  out << ",\n";
  out << "  \"pinnedApps\": ";
  detail::WriteStringArray(out, settings.pinnedApps);
  out << ",\n";
  out << "  \"hiddenApps\": ";
  detail::WriteStringArray(out, settings.hiddenApps);
  out << ",\n";
  out << "  \"appAliases\": ";
  detail::WriteStringObject(out, settings.appAliases);
  out << ",\n";
  out << "  \"commandAliases\": ";
  detail::WriteStringObject(out, settings.commandAliases);
  out << ",\n";
  out << "  \"pinnedItems\": ";
  detail::WriteStringArray(out, settings.pinnedItems);
  out << ",\n";
  out << "  \"recentItems\": ";
  detail::WriteStringArray(out, settings.recentItems);
  out << ",\n";
  out << "  \"searchLearningEnabled\": "
      << (settings.searchLearningEnabled ? "true" : "false") << ",\n";
  out << "  \"learnedQueryActions\": ";
  detail::WriteStringObject(out, settings.learnedQueryActions);
  out << ",\n";
  out << "  \"usageStats\": {";
  {
    bool first = true;
    for (const auto& [key, stat] : settings.usageStats) {
      if (!first) out << ", ";
      first = false;
      out << "\"" << JsonEscape(key) << "\": {\"launches\": " << stat.launches
          << ", \"lastUsed\": " << stat.lastUsed << "}";
    }
  }
  out << "},\n";
  out << "  \"compactMode\": " << (settings.compactMode ? "true" : "false") << ",\n";
  out << "  \"autoFitResultHeight\": "
      << (settings.autoFitResultHeight ? "true" : "false") << ",\n";
  out << "  \"animationLevel\": \"" << AnimationLevelKey(settings.animationLevel)
      << "\",\n";
  out << "  \"animationsEnabled\": "
      << (settings.animationLevel != AnimationLevel::Off ? "true" : "false")
      << ",\n";
  out << "  \"syncAccentColor\": " << (settings.syncAccentColor ? "true" : "false") << ",\n";
  out << "  \"customAccentColor\": \"" << JsonEscape(settings.customAccentColor) << "\",\n";
  out << "  \"textSizePercent\": " << ClampTextSizePercent(settings.textSizePercent)
      << ",\n";
  out << "  \"startOnStartup\": " << (settings.startOnStartup ? "true" : "false") << ",\n";
  out << "  \"updateChecksEnabled\": " << (settings.updateChecksEnabled ? "true" : "false") << ",\n";
  out << "  \"lastUpdateAttempt\": " << settings.lastUpdateAttempt << ",\n";
  out << "  \"lastUpdateCheck\": " << settings.lastUpdateCheck << ",\n";
  out << "  \"dismissedUpdateVersion\": \"" << JsonEscape(settings.dismissedUpdateVersion) << "\",\n";
  out << "  \"overlayWidth\": " << settings.overlayWidth << ",\n";
  out << "  \"maxResults\": " << settings.maxResults << ",\n";
  out << "  \"showOpenWindows\": " << (settings.showOpenWindows ? "true" : "false") << ",\n";
  out << "  \"showStoreApps\": " << (settings.showStoreApps ? "true" : "false") << ",\n";
  out << "  \"privacyConsentVersion\": " << settings.privacyConsentVersion << ",\n";
  out << "  \"clipboardHistoryEnabled\": " << (settings.clipboardHistoryEnabled ? "true" : "false") << ",\n";
  out << "  \"phoneLinkEnabled\": " << (settings.phoneLinkEnabled ? "true" : "false") << ",\n";
  out << "  \"phoneClipboardSync\": " << (settings.phoneClipboardSync ? "true" : "false") << ",\n";
  out << "  \"phoneNotificationToasts\": " << (settings.phoneNotificationToasts ? "true" : "false") << ",\n";
  out << "  \"phoneLowBatteryPercent\": " << settings.phoneLowBatteryPercent << ",\n";
  out << "  \"clipboardHistoryLimit\": " << settings.clipboardHistoryLimit << ",\n";
  out << "  \"clipboardRetentionDays\": " << settings.clipboardRetentionDays
      << ",\n";
  out << "  \"clipboardExcludedApps\": ";
  detail::WriteStringArray(out, settings.clipboardExcludedApps);
  out << ",\n";
  out << "  \"fileIndexEnabled\": " << (settings.fileIndexEnabled ? "true" : "false") << ",\n";
  out << "  \"fileContentIndexEnabled\": "
      << (settings.fileContentIndexEnabled ? "true" : "false") << ",\n";
  out << "  \"fileIndexMaxEntries\": " << settings.fileIndexMaxEntries << ",\n";
  out << "  \"fileIndexRoots\": ";
  detail::WriteStringArray(out, settings.fileIndexRoots);
  out << ",\n";
  out << "  \"fileIndexExcludePatterns\": ";
  detail::WriteStringArray(out, settings.fileIndexExcludePatterns);
  out << ",\n";
  out << "  \"diagnosticsEnabled\": " << (settings.diagnosticsEnabled ? "true" : "false") << ",\n";
  out << "  \"searchEngines\": ";
  detail::WriteStringObject(out, settings.searchEngines);
  out << ",\n";
  out << "  \"quicklinks\": [";
  {
    bool first = true;
    for (const auto& link : settings.quicklinks) {
      if (!first) out << ", ";
      first = false;
      out << "{\"keyword\": \"" << JsonEscape(link.keyword) << "\", \"name\": \""
          << JsonEscape(link.name) << "\", \"target\": \"" << JsonEscape(link.target) << "\"}";
    }
  }
  out << "],\n";
  for (const auto& [key, items] :
       std::array<std::pair<const char*, const std::vector<Quicklink>*>, 2>{{
           {"scripts", &settings.scripts}, {"workspaces", &settings.workspaces}}}) {
    out << "  \"" << key << "\": [";
    bool first = true;
    for (const auto& item : *items) {
      if (!first) out << ", ";
      first = false;
      out << "{\"keyword\": \"" << JsonEscape(item.keyword) << "\", \"name\": \""
          << JsonEscape(item.name) << "\", \"target\": \"" << JsonEscape(item.target) << "\"}";
    }
    out << "],\n";
  }
  out << "  \"commandShortcuts\": ";
  detail::WriteStringObject(out, settings.commandShortcuts);
  out << "\n";
  out << "}\n";
  return out.str();
}

}  // namespace feathercast::settings
