#include "file_content.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <string_view>

namespace feathercast::file_content {
namespace {

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return extension;
}

std::optional<std::wstring> DecodeCodePage(const unsigned char* data,
                                           std::size_t size, UINT codePage,
                                           DWORD flags) {
  if (size == 0) return std::wstring{};
  const int needed = MultiByteToWideChar(codePage, flags,
                                         reinterpret_cast<const char*>(data),
                                         static_cast<int>(size), nullptr, 0);
  if (needed <= 0) return std::nullopt;
  std::wstring text(static_cast<std::size_t>(needed), L'\0');
  if (MultiByteToWideChar(codePage, flags,
                          reinterpret_cast<const char*>(data),
                          static_cast<int>(size), text.data(), needed) <= 0) {
    return std::nullopt;
  }
  return text;
}

bool LooksBinary(const std::vector<unsigned char>& bytes) {
  std::size_t controls = 0;
  for (const auto byte : bytes) {
    if (byte == 0) return true;
    if (byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t' &&
        byte != '\f' && byte != '\b') {
      ++controls;
    }
  }
  return !bytes.empty() && controls * 20 > bytes.size();
}

bool LooksBinaryText(const std::wstring& text) {
  std::size_t controls = 0;
  for (const wchar_t ch : text) {
    if (ch == L'\0') return true;
    if (ch < 0x20 && ch != L'\r' && ch != L'\n' && ch != L'\t' &&
        ch != L'\f' && ch != L'\b') {
      ++controls;
    }
  }
  return !text.empty() && controls * 20 > text.size();
}

}  // namespace

bool Supports(const std::filesystem::path& path) {
  static constexpr std::wstring_view kExtensions[] = {
      L".bat",  L".c",    L".cc",   L".cfg",  L".cmd",  L".cpp",  L".cs",
      L".css",  L".csv",  L".cxx",  L".h",    L".hh",   L".hpp",  L".htm",
      L".html", L".hxx",  L".ini",  L".java", L".js",   L".json", L".jsx",
      L".kt",   L".log",  L".md",   L".ps1",  L".py",   L".scss", L".sh",
      L".sql",  L".toml", L".ts",   L".tsv",  L".tsx",  L".txt",  L".xml",
      L".yaml", L".yml",
  };
  const std::wstring ext = LowerExtension(path);
  return std::binary_search(std::begin(kExtensions), std::end(kExtensions),
                            std::wstring_view(ext));
}

bool IsImage(const std::filesystem::path& path) {
  static constexpr std::wstring_view kExtensions[] = {
      L".bmp", L".gif", L".ico", L".jpeg", L".jpg", L".png", L".tif", L".tiff",
  };
  const std::wstring ext = LowerExtension(path);
  return std::binary_search(std::begin(kExtensions), std::end(kExtensions),
                            std::wstring_view(ext));
}

Extraction Extract(const std::filesystem::path& path, std::size_t maxBytes,
                   std::stop_token token) {
  Extraction result;
  if (!Supports(path)) {
    result.state = State::Unsupported;
    return result;
  }

  WIN32_FILE_ATTRIBUTE_DATA attrData{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrData) ||
      (attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    result.state = State::Unavailable;
    return result;
  }
  const DWORD attributes = attrData.dwFileAttributes;
  bool cloudOnly = (attributes & FILE_ATTRIBUTE_OFFLINE) != 0;
#ifdef FILE_ATTRIBUTE_RECALL_ON_OPEN
  cloudOnly = cloudOnly || (attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0;
#endif
#ifdef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
  cloudOnly =
      cloudOnly || (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
#endif
  if (cloudOnly) {
    result.state = State::CloudOnly;
    return result;
  }

  const std::uint64_t size =
      (static_cast<std::uint64_t>(attrData.nFileSizeHigh) << 32) |
      attrData.nFileSizeLow;
  result.sourceBytes = static_cast<std::size_t>(size);
  if (size > maxBytes) {
    result.state = State::TooLarge;
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.state = State::Unavailable;
    return result;
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (token.stop_requested()) {
      result.state = State::Unavailable;
      return result;
    }
    const auto chunk = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset),
               static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      result.state = State::Unavailable;
      return result;
    }
    offset += chunk;
  }

  if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
    const std::size_t chars = (bytes.size() - 2) / 2;
    result.text.resize(chars);
    for (std::size_t i = 0; i < chars; ++i) {
      result.text[i] = static_cast<wchar_t>(
          bytes[2 + i * 2] | (static_cast<unsigned>(bytes[3 + i * 2]) << 8));
    }
  } else if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
    const std::size_t chars = (bytes.size() - 2) / 2;
    result.text.resize(chars);
    for (std::size_t i = 0; i < chars; ++i) {
      result.text[i] = static_cast<wchar_t>(
          (static_cast<unsigned>(bytes[2 + i * 2]) << 8) | bytes[3 + i * 2]);
    }
  } else {
    const std::size_t bomOffset = bytes.size() >= 3 && bytes[0] == 0xef &&
                                          bytes[1] == 0xbb && bytes[2] == 0xbf
                                      ? 3
                                      : 0;
    if (LooksBinary(bytes)) {
      result.state = State::Binary;
      return result;
    }
    auto decoded = DecodeCodePage(bytes.data() + bomOffset,
                                  bytes.size() - bomOffset, CP_UTF8,
                                  MB_ERR_INVALID_CHARS);
    if (!decoded) {
      decoded = DecodeCodePage(bytes.data() + bomOffset,
                               bytes.size() - bomOffset, CP_ACP, 0);
    }
    if (!decoded) {
      result.state = State::Binary;
      return result;
    }
    result.text = std::move(*decoded);
  }

  if (LooksBinaryText(result.text)) {
    result.text.clear();
    result.state = State::Binary;
    return result;
  }
  result.state = State::Indexed;
  return result;
}

}  // namespace feathercast::file_content
