#pragma once

// Developer text helpers behind explicit keywords: "base64 hello",
// "base64d aGVsbG8=", "urlencode a b", "urldecode a%20b", "sha256 hello",
// "hash hello" and "json {...}". Everything runs locally on the query text;
// hashes come from the Windows CNG provider.

#include "json.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::text_utilities {

struct Result {
  std::wstring stableId;
  // Single-line preview shown as the result name.
  std::wstring title;
  // Full text copied on Enter. Empty for informational rows (invalid input).
  std::wstring value;
  std::wstring detail;
  std::vector<std::wstring> keywords;
};

// Larger pastes are refused instead of hashing or reformatting them on every
// keystroke.
inline constexpr std::size_t kMaxInputLength = 64 * 1024;
inline constexpr std::size_t kPreviewLength = 160;

enum class HashAlgorithm { Md5, Sha1, Sha256 };

// Strict conversions: lone surrogates or malformed UTF-8 fail instead of being
// replaced, so encoders never silently change the bytes they report on.
inline std::optional<std::string> ToUtf8(std::wstring_view text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return std::nullopt;
  std::string out(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr) != size) {
    return std::nullopt;
  }
  return out;
}

inline std::optional<std::wstring> FromUtf8(std::string_view bytes) {
  if (bytes.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                       static_cast<int>(bytes.size()), nullptr, 0);
  if (size <= 0) return std::nullopt;
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                          static_cast<int>(bytes.size()), out.data(), size) != size) {
    return std::nullopt;
  }
  return out;
}

inline std::wstring WidenAscii(std::string_view text) {
  std::wstring out;
  out.reserve(text.size());
  for (const char ch : text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
  return out;
}

inline std::string Base64Encode(std::string_view bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t index = 0;
  const auto byteAt = [&](std::size_t position) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[position]));
  };
  for (; index + 2 < bytes.size(); index += 3) {
    const std::uint32_t group = (byteAt(index) << 16) | (byteAt(index + 1) << 8) |
                                byteAt(index + 2);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back(kAlphabet[group & 0x3F]);
  }
  const std::size_t remaining = bytes.size() - index;
  if (remaining == 1) {
    const std::uint32_t group = byteAt(index) << 16;
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out += "==";
  } else if (remaining == 2) {
    const std::uint32_t group = (byteAt(index) << 16) | (byteAt(index + 1) << 8);
    out.push_back(kAlphabet[(group >> 18) & 0x3F]);
    out.push_back(kAlphabet[(group >> 12) & 0x3F]);
    out.push_back(kAlphabet[(group >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

// Accepts the standard and URL-safe alphabets, optional "=" padding and
// whitespace between symbols (line-wrapped input).
inline std::optional<std::string> Base64Decode(std::wstring_view text) {
  std::string out;
  out.reserve(text.size() / 4 * 3 + 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  std::size_t symbols = 0;
  std::size_t padding = 0;
  for (const wchar_t ch : text) {
    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') continue;
    if (ch == L'=') {
      ++padding;
      continue;
    }
    if (padding > 0) return std::nullopt;  // data after padding
    std::uint32_t value = 0;
    if (ch >= L'A' && ch <= L'Z') value = static_cast<std::uint32_t>(ch - L'A');
    else if (ch >= L'a' && ch <= L'z') value = static_cast<std::uint32_t>(ch - L'a') + 26;
    else if (ch >= L'0' && ch <= L'9') value = static_cast<std::uint32_t>(ch - L'0') + 52;
    else if (ch == L'+' || ch == L'-') value = 62;
    else if (ch == L'/' || ch == L'_') value = 63;
    else return std::nullopt;
    buffer = ((buffer << 6) | value) & 0xFFFFFFu;
    bits += 6;
    ++symbols;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  // A lone trailing symbol carries fewer than eight bits.
  if (symbols == 0 || symbols % 4 == 1 || padding > 2) return std::nullopt;
  if (padding > 0 && (symbols + padding) % 4 != 0) return std::nullopt;
  return out;
}

// RFC 3986 percent-encoding: only unreserved characters stay literal and a
// space becomes %20, so the output is valid in paths and queries alike.
inline std::string PercentEncode(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 3);
  for (const char raw : bytes) {
    const auto ch = static_cast<unsigned char>(raw);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('%');
      out.push_back(kHex[ch >> 4]);
      out.push_back(kHex[ch & 0x0F]);
    }
  }
  return out;
}

// Decodes %XX escapes and form-style "+" spaces. A "%" that does not start a
// valid escape is kept literally, as browsers do.
inline std::string PercentDecode(std::string_view text) {
  const auto hexValue = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '+') {
      out.push_back(' ');
    } else if (ch == '%' && index + 2 < text.size() &&
               hexValue(text[index + 1]) >= 0 && hexValue(text[index + 2]) >= 0) {
      out.push_back(static_cast<char>(hexValue(text[index + 1]) * 16 +
                                      hexValue(text[index + 2])));
      index += 2;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

inline std::wstring HexString(std::string_view bytes, bool spaced = false) {
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring out;
  out.reserve(bytes.size() * (spaced ? 3 : 2));
  for (const char raw : bytes) {
    const auto ch = static_cast<unsigned char>(raw);
    if (spaced && !out.empty()) out.push_back(L' ');
    out.push_back(kHex[ch >> 4]);
    out.push_back(kHex[ch & 0x0F]);
  }
  return out;
}

// Lowercase hex digest, or nullopt when the CNG provider is unavailable.
inline std::optional<std::wstring> HashHex(HashAlgorithm algorithm,
                                           std::string_view bytes) {
  const wchar_t* id = algorithm == HashAlgorithm::Md5    ? BCRYPT_MD5_ALGORITHM
                      : algorithm == HashAlgorithm::Sha1 ? BCRYPT_SHA1_ALGORITHM
                                                         : BCRYPT_SHA256_ALGORITHM;
  BCRYPT_ALG_HANDLE handle = nullptr;
  if (BCryptOpenAlgorithmProvider(&handle, id, nullptr, 0) < 0) return std::nullopt;
  DWORD length = 0;
  ULONG written = 0;
  NTSTATUS status = BCryptGetProperty(handle, BCRYPT_HASH_LENGTH,
                                      reinterpret_cast<PUCHAR>(&length),
                                      sizeof(length), &written, 0);
  std::string digest;
  if (status >= 0 && length > 0 && length <= 64) {
    digest.resize(length);
    static unsigned char empty = 0;
    auto* input = bytes.empty()
        ? &empty
        : reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data()));
    status = BCryptHash(handle, nullptr, 0, input, static_cast<ULONG>(bytes.size()),
                        reinterpret_cast<PUCHAR>(digest.data()), length);
  }
  BCryptCloseAlgorithmProvider(handle, 0);
  if (status < 0 || digest.empty()) return std::nullopt;
  return HexString(digest);
}

struct JsonCheck {
  bool valid = false;
  // One-based character position near the syntax error.
  std::size_t errorPosition = 0;
};

inline JsonCheck CheckJson(std::string_view utf8) {
  json::detail::Parser parser{utf8};
  json::Value value;
  bool valid = parser.ParseValue(value);
  if (valid) {
    parser.SkipWs();
    valid = parser.pos == utf8.size();
  }
  if (valid) return {true, 0};
  std::size_t position = 1;
  for (std::size_t index = 0; index < parser.pos && index < utf8.size(); ++index) {
    // Count code points, not UTF-8 continuation bytes.
    if ((static_cast<unsigned char>(utf8[index]) & 0xC0) != 0x80) ++position;
  }
  return {false, position};
}

// Re-indents (two spaces) or minifies a document CheckJson accepted. Works on
// tokens rather than a parsed tree, so key order, number spelling and string
// escapes are kept exactly as typed.
inline std::string ReformatJson(std::string_view utf8, bool pretty) {
  const auto whitespace = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  std::string out;
  out.reserve(utf8.size() + (pretty ? utf8.size() / 2 : 0));
  std::size_t depth = 0;
  const auto newline = [&] {
    out.push_back('\n');
    out.append(depth * 2, ' ');
  };
  for (std::size_t index = 0; index < utf8.size(); ++index) {
    const char ch = utf8[index];
    if (whitespace(ch)) continue;
    if (ch == '"') {
      const std::size_t start = index++;
      while (index < utf8.size() && utf8[index] != '"') {
        if (utf8[index] == '\\') ++index;
        ++index;
      }
      out.append(utf8.substr(start, index - start + 1));
      continue;
    }
    if (ch == '{' || ch == '[') {
      const char close = ch == '{' ? '}' : ']';
      std::size_t next = index + 1;
      while (next < utf8.size() && whitespace(utf8[next])) ++next;
      out.push_back(ch);
      if (next < utf8.size() && utf8[next] == close) {
        out.push_back(close);  // keep empty containers compact
        index = next;
        continue;
      }
      ++depth;
      if (pretty) newline();
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (depth > 0) --depth;
      if (pretty) newline();
      out.push_back(ch);
      continue;
    }
    out.push_back(ch);
    if (ch == ',' && pretty) newline();
    if (ch == ':' && pretty) out.push_back(' ');
  }
  return out;
}

// One line of at most kPreviewLength characters: whitespace runs collapse to a
// single space and longer text ends in an ellipsis.
inline std::wstring Preview(std::wstring_view text) {
  std::wstring out;
  bool pendingSpace = false;
  bool truncated = false;
  for (const wchar_t ch : text) {
    if (std::iswspace(ch) != 0 || ch < 0x20 || ch == 0x7F) {
      pendingSpace = !out.empty();
      continue;
    }
    if (out.size() + (pendingSpace ? 2 : 1) > kPreviewLength) {
      truncated = true;
      break;
    }
    if (pendingSpace) out.push_back(L' ');
    pendingSpace = false;
    out.push_back(ch);
  }
  if (truncated) {
    // Never end on half of a surrogate pair.
    if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();
    out += L"…";
  }
  if (out.empty()) return text.empty() ? L"(empty)" : L"(whitespace)";
  return out;
}

namespace detail {

inline std::wstring Bytes(std::size_t count) {
  return std::to_wstring(count) + (count == 1 ? L" byte" : L" bytes");
}

inline Result Notice(std::wstring stableId, std::wstring title, std::wstring detail,
                     std::vector<std::wstring> keywords) {
  return {std::move(stableId), std::move(title), L"", std::move(detail),
          std::move(keywords)};
}

// Decoded bytes become text when they are clean UTF-8, otherwise spaced hex.
inline Result Decoded(std::wstring stableId, std::wstring_view label,
                      const std::string& bytes, std::vector<std::wstring> keywords) {
  if (auto text = FromUtf8(bytes)) {
    bool binary = false;
    for (const wchar_t ch : *text) {
      if ((ch < 0x20 && ch != L'\t' && ch != L'\r' && ch != L'\n') || ch == 0x7F) {
        binary = true;
        break;
      }
    }
    if (!binary) {
      std::wstring title = Preview(*text);
      return {std::move(stableId), std::move(title), std::move(*text),
              std::wstring(label) + L" · " + Bytes(bytes.size()),
              std::move(keywords)};
    }
  }
  std::wstring hex = HexString(bytes, true);
  std::wstring title = Preview(hex);
  return {std::move(stableId), std::move(title), std::move(hex),
          std::wstring(label) + L" · " + Bytes(bytes.size()) +
              L" of binary data, shown as hex",
          std::move(keywords)};
}

inline bool IsSpace(wchar_t ch) {
  return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

}  // namespace detail

// Results for "<keyword> <text>", or nothing when the query does not start
// with a text utility keyword. Cheap to call for every query: anything but a
// short known keyword followed by text returns before allocating.
inline std::vector<Result> Evaluate(std::wstring_view query) {
  std::vector<Result> results;
  std::size_t start = 0;
  while (start < query.size() && detail::IsSpace(query[start])) ++start;
  std::size_t space = start;
  while (space < query.size() && !detail::IsSpace(query[space])) ++space;
  const std::size_t keywordLength = space - start;
  if (keywordLength < 3 || keywordLength > 9 || space >= query.size()) return results;
  std::wstring keyword(query.substr(start, keywordLength));
  for (auto& ch : keyword) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  enum class Kind { None, Base64, Base64Decode, UrlEncode, UrlDecode, Md5, Sha1, Sha256, Hash, Json };
  Kind kind = Kind::None;
  if (keyword == L"base64" || keyword == L"b64") kind = Kind::Base64;
  else if (keyword == L"base64d" || keyword == L"b64d") kind = Kind::Base64Decode;
  else if (keyword == L"urlencode") kind = Kind::UrlEncode;
  else if (keyword == L"urldecode") kind = Kind::UrlDecode;
  else if (keyword == L"md5") kind = Kind::Md5;
  else if (keyword == L"sha1") kind = Kind::Sha1;
  else if (keyword == L"sha256") kind = Kind::Sha256;
  else if (keyword == L"hash") kind = Kind::Hash;
  else if (keyword == L"json") kind = Kind::Json;
  if (kind == Kind::None) return results;

  std::size_t textBegin = space;
  while (textBegin < query.size() && detail::IsSpace(query[textBegin])) ++textBegin;
  std::size_t textEnd = query.size();
  while (textEnd > textBegin && detail::IsSpace(query[textEnd - 1])) --textEnd;
  if (textBegin >= textEnd) return results;
  const std::wstring_view text = query.substr(textBegin, textEnd - textBegin);
  if (kind == Kind::Json && text.front() != L'{' && text.front() != L'[' &&
      text.front() != L'"') {
    return results;
  }

  const std::vector<std::wstring> keywords = {keyword, L"text", L"developer"};
  if (text.size() > kMaxInputLength) {
    results.push_back(detail::Notice(
        L"text-too-long", L"Text is too long for text utilities",
        L"Limit is " + std::to_wstring(kMaxInputLength) + L" characters", keywords));
    return results;
  }
  const auto utf8 = ToUtf8(text);
  if (!utf8) {
    results.push_back(detail::Notice(
        L"text-invalid", L"Text contains invalid characters",
        L"Unpaired surrogate characters cannot be encoded", keywords));
    return results;
  }

  switch (kind) {
    case Kind::Base64: {
      std::wstring encoded = WidenAscii(Base64Encode(*utf8));
      std::wstring title = Preview(encoded);
      results.push_back({L"text-base64", std::move(title), std::move(encoded),
                         L"Base64 encoded · " + detail::Bytes(utf8->size()),
                         keywords});
      break;
    }
    case Kind::Base64Decode:
      if (const auto decoded = Base64Decode(text)) {
        results.push_back(detail::Decoded(L"text-base64-decode", L"Base64 decoded",
                                          *decoded, keywords));
      } else {
        results.push_back(detail::Notice(
            L"text-base64-invalid", L"Invalid Base64",
            L"Use A–Z, a–z, 0–9, + and / (or - and _) with optional = padding",
            keywords));
      }
      break;
    case Kind::UrlEncode: {
      std::wstring encoded = WidenAscii(PercentEncode(*utf8));
      std::wstring title = Preview(encoded);
      results.push_back({L"text-url-encode", std::move(title), std::move(encoded),
                         L"URL encoded (RFC 3986)", keywords});
      break;
    }
    case Kind::UrlDecode:
      results.push_back(detail::Decoded(L"text-url-decode", L"URL decoded",
                                        PercentDecode(*utf8), keywords));
      break;
    case Kind::Md5:
    case Kind::Sha1:
    case Kind::Sha256:
    case Kind::Hash: {
      struct Row {
        Kind kind;
        HashAlgorithm algorithm;
        const wchar_t* stableId;
        const wchar_t* label;
      };
      static constexpr Row kRows[] = {
          {Kind::Sha256, HashAlgorithm::Sha256, L"text-sha256", L"SHA-256"},
          {Kind::Sha1, HashAlgorithm::Sha1, L"text-sha1", L"SHA-1"},
          {Kind::Md5, HashAlgorithm::Md5, L"text-md5", L"MD5"},
      };
      for (const auto& row : kRows) {
        if (kind != Kind::Hash && kind != row.kind) continue;
        auto digest = HashHex(row.algorithm, *utf8);
        if (!digest) continue;
        std::wstring title = *digest;
        results.push_back({row.stableId, std::move(title), std::move(*digest),
                           std::wstring(row.label) + L" of " +
                               detail::Bytes(utf8->size()) + L" (UTF-8)",
                           keywords});
      }
      break;
    }
    case Kind::Json: {
      const auto check = CheckJson(*utf8);
      if (!check.valid) {
        results.push_back(detail::Notice(
            L"text-json-invalid", L"Invalid JSON",
            L"Syntax error near character " + std::to_wstring(check.errorPosition),
            keywords));
        break;
      }
      const std::string pretty = ReformatJson(*utf8, true);
      const std::string minified = ReformatJson(*utf8, false);
      std::size_t lines = 1;
      for (const char ch : pretty) lines += ch == '\n' ? 1 : 0;
      if (auto formatted = FromUtf8(pretty)) {
        std::wstring title = Preview(*formatted);
        results.push_back({L"text-json-format", std::move(title), std::move(*formatted),
                           L"Formatted JSON · " + std::to_wstring(lines) +
                               (lines == 1 ? L" line" : L" lines"),
                           keywords});
      }
      if (auto compact = FromUtf8(minified)) {
        std::wstring title = Preview(*compact);
        const std::size_t length = compact->size();
        results.push_back({L"text-json-minify", std::move(title), std::move(*compact),
                           L"Minified JSON · " + std::to_wstring(length) +
                               L" characters",
                           keywords});
      }
      break;
    }
    case Kind::None:
      break;
  }
  return results;
}

}  // namespace feathercast::text_utilities
