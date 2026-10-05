#pragma once

#include "extension_protocol.hpp"
#include "json.hpp"

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::snippets {

struct Snippet {
  std::wstring keyword;
  std::wstring name;
  std::wstring text;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

enum class ParseStatus { Valid, InvalidDocument, InvalidEntry };

struct ParseResult {
  std::vector<Snippet> snippets;
  ParseStatus status = ParseStatus::InvalidDocument;
};

// The one snippets.json parser (snippets_io::Load reads files through it).
// The document must be an object with a "snippets" array. A single entry
// without a non-blank keyword, name, and text rejects the whole file, so a
// later save can never silently drop it. Keyword and name are trimmed; the
// text is kept verbatim.
inline ParseResult ParseSnippetsDocument(std::string_view document) {
  ParseResult result;
  const auto root = json::Parse(document);
  const auto* items = root && root->type == json::Value::Type::Object
                          ? root->Find("snippets")
                          : nullptr;
  if (!items || items->type != json::Value::Type::Array) return result;

  auto read = [](const json::Value& object, const char* key,
                 std::wstring& output) {
    const auto* value = object.Find(key);
    if (!value || value->type != json::Value::Type::String) return false;
    output = extensions::Utf8ToWide(value->str);
    return true;
  };
  for (const auto& value : items->array) {
    Snippet snippet;
    if (value.type != json::Value::Type::Object ||
        !read(value, "keyword", snippet.keyword) ||
        !read(value, "name", snippet.name) ||
        !read(value, "text", snippet.text) ||
        Trim(snippet.keyword).empty() || Trim(snippet.name).empty() ||
        Trim(snippet.text).empty()) {
      return {{}, ParseStatus::InvalidEntry};
    }
    snippet.keyword = Trim(std::move(snippet.keyword));
    snippet.name = Trim(std::move(snippet.name));
    result.snippets.push_back(std::move(snippet));
  }
  result.status = ParseStatus::Valid;
  return result;
}

}  // namespace feathercast::snippets
