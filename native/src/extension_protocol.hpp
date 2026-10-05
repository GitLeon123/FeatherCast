#pragma once

#include "json.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::extensions {

inline constexpr uint32_t kApiVersion = 2;
inline constexpr uint32_t kMinSupportedApiVersion = 1;
inline constexpr size_t kMaxResponseBytes = 1024 * 1024;
inline constexpr int kDefaultQueryLimit = 20;

inline std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return L"";
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (needed <= 0) return L"";
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
  return out;
}

inline std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return "";
  const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return "";
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

inline std::string JsonEscapeUtf8(std::string_view value) {
  std::string out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          static constexpr char kHex[] = "0123456789ABCDEF";
          out += "\\u00";
          out.push_back(kHex[ch >> 4]);
          out.push_back(kHex[ch & 0x0F]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

inline std::string QuoteUtf8(std::string_view value) {
  return "\"" + JsonEscapeUtf8(value) + "\"";
}

inline std::string QuoteWide(const std::wstring& value) {
  return QuoteUtf8(WideToUtf8(value));
}

inline std::string UnescapeJsonString(std::string_view value) {
  std::string out;
  bool escaped = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (!escaped) {
      if (ch == '\\') escaped = true;
      else out.push_back(ch);
      continue;
    }

    escaped = false;
    switch (ch) {
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '\\':
      case '"':
      case '/':
        out.push_back(ch);
        break;
      case 'u':
        // The protocol emits UTF-8 directly and does not require \u parsing for
        // v1. Preserve unsupported escapes literally rather than guessing.
        out += "\\u";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  if (escaped) out.push_back('\\');
  return out;
}

inline std::optional<size_t> FindJsonValue(const std::string& json, const std::string& key) {
  for (size_t pos = 0; pos < json.size();) {
    pos = json.find('"', pos);
    if (pos == std::string::npos) return std::nullopt;

    const size_t stringStart = ++pos;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
      const char ch = json[pos];
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        break;
      }
    }
    if (pos >= json.size()) return std::nullopt;

    const size_t stringEnd = pos;
    size_t colon = json.find_first_not_of(" \t\r\n", pos + 1);
    if (colon != std::string::npos && json[colon] == ':' &&
        json.compare(stringStart, stringEnd - stringStart, key) == 0) {
      const size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
      if (value == std::string::npos) return std::nullopt;
      return value;
    }
    ++pos;
  }
  return std::nullopt;
}

inline std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
  const auto start = FindJsonValue(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') return std::nullopt;
  std::string raw;
  bool escaped = false;
  for (size_t i = *start + 1; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaped) {
      raw.push_back('\\');
      raw.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return UnescapeJsonString(raw);
    raw.push_back(ch);
  }
  return std::nullopt;
}

inline bool JsonBool(const std::string& json, const std::string& key, bool fallback) {
  const auto start = FindJsonValue(json, key);
  if (!start) return fallback;
  if (json.compare(*start, 4, "true") == 0) return true;
  if (json.compare(*start, 5, "false") == 0) return false;
  return fallback;
}

inline std::optional<double> JsonNumber(const std::string& json, const std::string& key) {
  const auto start = FindJsonValue(json, key);
  if (!start) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(json.c_str() + *start, &end);
  if (end == json.c_str() + *start) return std::nullopt;
  return value;
}

inline std::optional<std::string> JsonBalancedSlice(const std::string& json, const std::string& key,
                                                   char open, char close) {
  const auto start = FindJsonValue(json, key);
  if (!start || *start >= json.size() || json[*start] != open) return std::nullopt;

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = *start; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == open) ++depth;
    else if (ch == close) {
      --depth;
      if (depth == 0) return json.substr(*start, i - *start + 1);
    }
  }
  return std::nullopt;
}

inline std::optional<std::string> JsonArraySlice(const std::string& json, const std::string& key) {
  return JsonBalancedSlice(json, key, '[', ']');
}

inline std::vector<std::string> JsonObjectArray(const std::string& json, const std::string& key) {
  std::vector<std::string> out;
  const auto array = JsonArraySlice(json, key);
  if (!array) return out;

  for (size_t i = 1; i + 1 < array->size();) {
    if ((*array)[i] != '{') {
      ++i;
      continue;
    }

    const size_t start = i;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; i < array->size(); ++i) {
      const char ch = (*array)[i];
      if (inString) {
        if (escaped) escaped = false;
        else if (ch == '\\') escaped = true;
        else if (ch == '"') inString = false;
        continue;
      }
      if (ch == '"') inString = true;
      else if (ch == '{') ++depth;
      else if (ch == '}') {
        --depth;
        if (depth == 0) {
          out.push_back(array->substr(start, i - start + 1));
          ++i;
          break;
        }
      }
    }
  }
  return out;
}

inline std::optional<std::string> ReadTextFileUtf8(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

inline std::wstring LowerPathText(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    if (ch == L'/') return L'\\';
    return static_cast<wchar_t>(std::towlower(ch));
  });
  while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
  return value;
}

inline std::filesystem::path CanonicalForContainment(const std::filesystem::path& path) {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) return canonical;
  return std::filesystem::absolute(path, ec).lexically_normal();
}

inline bool PathInsideDirectory(const std::filesystem::path& candidate, const std::filesystem::path& directory) {
  std::wstring base = LowerPathText(CanonicalForContainment(directory).wstring());
  std::wstring path = LowerPathText(CanonicalForContainment(candidate).wstring());
  if (base.empty() || path.empty() || path == base) return false;
  base.push_back(L'\\');
  return path.rfind(base, 0) == 0;
}

struct Manifest {
  std::wstring id;
  std::wstring name;
  std::wstring version;
  std::wstring description;
  bool enabled = true;
  std::filesystem::path manifestPath;
  std::filesystem::path directory;
  std::filesystem::path dllPath;
};

struct ManifestLoadResult {
  std::optional<Manifest> manifest;
  std::wstring error;
};

inline bool IsValidPluginId(const std::wstring& id) {
  if (id.empty()) return false;
  for (const wchar_t ch : id) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      return false;
    }
  }
  return true;
}

inline ManifestLoadResult ParseManifestJson(const std::string& json, const std::filesystem::path& manifestPath) {
  Manifest manifest;
  manifest.manifestPath = manifestPath;
  manifest.directory = manifestPath.parent_path();
  manifest.enabled = JsonBool(json, "enabled", true);

  const auto id = JsonString(json, "id");
  const auto name = JsonString(json, "name");
  const auto version = JsonString(json, "version");
  const auto dll = JsonString(json, "dll");
  if (!id || !name || !version || !dll || id->empty() || name->empty() || version->empty() || dll->empty()) {
    return {std::nullopt, L"plugin.json is missing required id, name, version, or dll"};
  }

  manifest.id = Utf8ToWide(*id);
  manifest.name = Utf8ToWide(*name);
  manifest.version = Utf8ToWide(*version);
  if (auto desc = JsonString(json, "description")) manifest.description = Utf8ToWide(*desc);

  if (!IsValidPluginId(manifest.id)) {
    return {std::nullopt, L"plugin id contains invalid path characters"};
  }

  std::filesystem::path dllPath = Utf8ToWide(*dll);
  if (dllPath.is_relative()) dllPath = manifest.directory / dllPath;
  dllPath = CanonicalForContainment(dllPath);
  if (!PathInsideDirectory(dllPath, manifest.directory)) {
    return {std::nullopt, L"plugin dll must stay inside the plugin directory"};
  }
  manifest.dllPath = std::move(dllPath);
  return {std::move(manifest), L""};
}

inline ManifestLoadResult LoadManifest(const std::filesystem::path& manifestPath) {
  const auto json = ReadTextFileUtf8(manifestPath);
  if (!json) return {std::nullopt, L"unable to read plugin.json"};
  return ParseManifestJson(*json, manifestPath);
}

struct ManifestDiscovery {
  std::vector<Manifest> manifests;
  std::vector<std::wstring> errors;
};

inline void DiscoverManifestRoot(const std::filesystem::path& root, std::set<std::wstring>& seen,
                                 ManifestDiscovery& out) {
  const auto pluginsDir = root / L"plugins";
  std::error_code ec;
  if (!std::filesystem::is_directory(pluginsDir, ec)) return;

  for (std::filesystem::directory_iterator it(pluginsDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const auto manifestPath = it->path() / L"plugin.json";
    if (!std::filesystem::is_regular_file(manifestPath, ec)) continue;
    auto result = LoadManifest(manifestPath);
    if (!result.manifest) {
      out.errors.push_back(manifestPath.wstring() + L": " + result.error);
      continue;
    }
    const std::wstring key = LowerPathText(result.manifest->id);
    if (seen.contains(key)) continue;
    seen.insert(key);
    if (result.manifest->enabled) out.manifests.push_back(std::move(*result.manifest));
  }
}

inline ManifestDiscovery DiscoverManifests(const std::filesystem::path& dataDir,
                                           const std::filesystem::path& exeDir) {
  ManifestDiscovery out;
  std::set<std::wstring> seen;
  DiscoverManifestRoot(dataDir, seen, out);
  DiscoverManifestRoot(exeDir, seen, out);
  return out;
}

struct QueryResultItem {
  std::wstring pluginId;
  std::wstring pluginName;
  std::wstring id;
  std::wstring title;
  std::wstring subtitle;
  std::vector<std::wstring> keywords;
  double score = 0.0;
  std::wstring iconPath;
  std::wstring detailType;
  std::wstring detailTitle;
  std::wstring detailBody;
  std::string payloadJson = "{}";
};

struct QueryResponse {
  std::vector<QueryResultItem> items;
};

// Compact JSON serialization of a parsed value. The output never contains a
// raw line break, so it is safe inside the line-framed host protocol.
inline void AppendJson(std::string& out, const json::Value& value) {
  switch (value.type) {
    case json::Value::Type::Null:
      out += "null";
      break;
    case json::Value::Type::Bool:
      out += value.boolean ? "true" : "false";
      break;
    case json::Value::Type::Number: {
      char buffer[32]{};
      const auto [end, error] =
          std::to_chars(buffer, buffer + sizeof(buffer), value.number);
      if (error == std::errc{} && std::isfinite(value.number)) {
        out.append(buffer, end);
      } else {
        out += "null";
      }
      break;
    }
    case json::Value::Type::String:
      out += QuoteUtf8(value.str);
      break;
    case json::Value::Type::Array:
      out.push_back('[');
      for (size_t index = 0; index < value.array.size(); ++index) {
        if (index > 0) out.push_back(',');
        AppendJson(out, value.array[index]);
      }
      out.push_back(']');
      break;
    case json::Value::Type::Object:
      out.push_back('{');
      for (size_t index = 0; index < value.object.size(); ++index) {
        if (index > 0) out.push_back(',');
        out += QuoteUtf8(value.object[index].key);
        out.push_back(':');
        AppendJson(out, value.object[index].value);
      }
      out.push_back('}');
      break;
  }
}

inline std::string SerializeJson(const json::Value& value) {
  std::string out;
  AppendJson(out, value);
  return out;
}

// Member of `object` when it exists and has the requested type.
inline const json::Value* JsonMember(const json::Value& object, std::string_view key,
                                     json::Value::Type type) {
  const json::Value* member = object.Find(key);
  return member && member->type == type ? member : nullptr;
}

inline std::optional<std::wstring> JsonMemberWide(const json::Value& object, std::string_view key) {
  const auto* member = JsonMember(object, key, json::Value::Type::String);
  if (!member) return std::nullopt;
  return Utf8ToWide(member->str);
}

// Responses are parsed as complete JSON documents: keys are only matched on
// the object they belong to, never inside nested values or string contents.
inline std::optional<QueryResponse> ParseQueryResponse(const json::Value& root,
                                                       size_t maxItems = kDefaultQueryLimit) {
  const auto* items = JsonMember(root, "items", json::Value::Type::Array);
  if (!items) return std::nullopt;
  QueryResponse response;
  for (const auto& object : items->array) {
    if (response.items.size() >= maxItems) break;
    if (object.type != json::Value::Type::Object) continue;
    auto id = JsonMemberWide(object, "id");
    auto title = JsonMemberWide(object, "title");
    if (!id || !title || id->empty() || title->empty()) continue;

    QueryResultItem item;
    item.id = std::move(*id);
    item.title = std::move(*title);
    if (auto subtitle = JsonMemberWide(object, "subtitle")) item.subtitle = std::move(*subtitle);
    if (const auto* keywords = JsonMember(object, "keywords", json::Value::Type::Array)) {
      for (const auto& keyword : keywords->array) {
        if (keyword.type == json::Value::Type::String) item.keywords.push_back(Utf8ToWide(keyword.str));
      }
    }
    if (const auto* score = JsonMember(object, "score", json::Value::Type::Number)) item.score = score->number;
    if (auto iconPath = JsonMemberWide(object, "iconPath")) item.iconPath = std::move(*iconPath);
    if (const auto* detail = JsonMember(object, "detail", json::Value::Type::Object)) {
      if (auto type = JsonMemberWide(*detail, "type")) item.detailType = std::move(*type);
      if (auto detailTitle = JsonMemberWide(*detail, "title")) item.detailTitle = std::move(*detailTitle);
      if (auto body = JsonMemberWide(*detail, "body")) item.detailBody = std::move(*body);
    }
    if (const auto* payload = JsonMember(object, "payload", json::Value::Type::Object)) {
      item.payloadJson = SerializeJson(*payload);
    }
    response.items.push_back(std::move(item));
  }
  return response;
}

inline std::optional<QueryResponse> ParseQueryResponse(const std::string& text,
                                                       size_t maxItems = kDefaultQueryLimit) {
  const auto root = json::Parse(text);
  if (!root) return std::nullopt;
  return ParseQueryResponse(*root, maxItems);
}

enum class HostActionType {
  None,
  OpenUrl,
  OpenPath,
  CopyText,
  SetQuery,
};

struct ActivationResponse {
  bool handled = false;
  bool closeOverlay = true;
  HostActionType action = HostActionType::None;
  std::wstring value;
};

inline HostActionType HostActionTypeFromString(const std::string& value) {
  if (value == "openUrl") return HostActionType::OpenUrl;
  if (value == "openPath") return HostActionType::OpenPath;
  if (value == "copyText") return HostActionType::CopyText;
  if (value == "setQuery") return HostActionType::SetQuery;
  return HostActionType::None;
}

inline std::optional<ActivationResponse> ParseActivationResponse(const json::Value& root) {
  if (root.type != json::Value::Type::Object) return std::nullopt;
  ActivationResponse response;
  if (const auto* handled = JsonMember(root, "handled", json::Value::Type::Bool)) {
    response.handled = handled->boolean;
  }
  if (const auto* closeOverlay = JsonMember(root, "closeOverlay", json::Value::Type::Bool)) {
    response.closeOverlay = closeOverlay->boolean;
  }
  if (const auto* action = JsonMember(root, "action", json::Value::Type::Object)) {
    if (const auto* type = JsonMember(*action, "type", json::Value::Type::String)) {
      response.action = HostActionTypeFromString(type->str);
    }
    if (auto value = JsonMemberWide(*action, "value")) response.value = std::move(*value);
  }
  return response;
}

inline std::optional<ActivationResponse> ParseActivationResponse(const std::string& text) {
  const auto root = json::Parse(text);
  if (!root) return std::nullopt;
  return ParseActivationResponse(*root);
}

inline std::string BuildQueryRequestJson(const Manifest& manifest, const std::filesystem::path& dataDir,
                                         const std::wstring& query, int limit) {
  std::ostringstream json;
  json << "{\"apiVersion\":" << kApiVersion
       << ",\"type\":\"query\""
       << ",\"query\":" << QuoteWide(query)
       << ",\"limit\":" << std::max(1, limit)
       << ",\"capabilities\":{\"detailMarkdown\":true,\"setQuery\":true}"
       << ",\"context\":{\"pluginId\":" << QuoteWide(manifest.id)
       << ",\"pluginDir\":" << QuoteWide(manifest.directory.wstring())
       << ",\"dataDir\":" << QuoteWide(dataDir.wstring())
       << "}}";
  return json.str();
}

inline std::string BuildActivateRequestJson(const Manifest& manifest, const std::filesystem::path& dataDir,
                                            const QueryResultItem& item) {
  const std::string payload = item.payloadJson.empty() ? "{}" : item.payloadJson;
  std::ostringstream json;
  json << "{\"apiVersion\":" << kApiVersion
       << ",\"type\":\"activate\""
       << ",\"itemId\":" << QuoteWide(item.id)
       << ",\"payload\":" << payload
       << ",\"context\":{\"pluginId\":" << QuoteWide(manifest.id)
       << ",\"pluginDir\":" << QuoteWide(manifest.directory.wstring())
       << ",\"dataDir\":" << QuoteWide(dataDir.wstring())
       << "}}";
  return json.str();
}

inline bool ResponseSizeAllowed(size_t requiredBytes) {
  return requiredBytes > 0 && requiredBytes <= kMaxResponseBytes;
}

// FeatherCastPluginHost exit codes for failures before the ready line.
inline constexpr unsigned long kHostExitMissingArgument = 2;
inline constexpr unsigned long kHostExitLoadFailed = 3;
inline constexpr unsigned long kHostExitMissingExports = 4;
inline constexpr unsigned long kHostExitUnsupportedApi = 5;
inline constexpr unsigned long kHostExitChannelFailed = 6;

// First line the plugin host writes, once the plugin DLL is loaded and its API
// version is supported. The manager sends requests only after it arrives, so
// startup failures surface as exit codes instead of request timeouts.
inline std::string BuildHostReadyJson(uint32_t pluginApiVersion) {
  return "{\"ready\":true,\"apiVersion\":" + std::to_string(pluginApiVersion) + "}";
}

inline bool IsHostReadyJson(const std::string& line) {
  const auto root = json::Parse(line);
  if (!root) return false;
  const auto* ready = JsonMember(*root, "ready", json::Value::Type::Bool);
  return ready && ready->boolean;
}

// The host protocol frames every response as exactly one line. JSON allows
// line breaks only as whitespace between tokens, so those become spaces. A raw
// line break inside a string is invalid JSON; such a response is rejected
// instead of being split into several protocol lines.
inline std::optional<std::string> FrameResponseLine(std::string response) {
  bool inString = false;
  bool escaped = false;
  for (char& ch : response) {
    if (ch == '\n' || ch == '\r') {
      if (inString) return std::nullopt;
      ch = ' ';
      continue;
    }
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
    } else if (ch == '"') {
      inString = true;
    }
  }
  return response;
}

}  // namespace feathercast::extensions
