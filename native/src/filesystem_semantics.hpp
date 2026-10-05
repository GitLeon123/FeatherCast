#pragma once

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace feathercast::filesystem_semantics {

enum class Presence { Missing, Present, Error };

inline Presence ClassifyPresence(bool exists, const std::error_code& error) {
  if (error) return Presence::Error;
  return exists ? Presence::Present : Presence::Missing;
}

// Comparison key for a Windows path: lexically normalized, backslashes only,
// no trailing separator (except on a bare root such as "C:\"), and case-folded
// with the invariant locale so non-ASCII names compare case-insensitively too.
// "D:/Notes/./", "d:\notes" and "D:\NOTES\" share one key.
inline std::wstring PathKey(std::wstring_view path) {
  const std::filesystem::path normal =
      std::filesystem::path(path).lexically_normal().make_preferred();
  std::wstring key = normal.wstring();
  const std::size_t rootLength = normal.root_path().wstring().size();
  while (key.size() > rootLength && key.back() == L'\\') key.pop_back();
  if (key.empty() || key.size() > static_cast<std::size_t>(INT_MAX)) return key;
  const int length = static_cast<int>(key.size());
  std::wstring folded(key.size(), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, key.c_str(),
                    length, folded.data(), length, nullptr, nullptr,
                    0) != length) {
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
      return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a')
                                      : ch;
    });
    return key;
  }
  return folded;
}

inline bool SamePath(std::wstring_view left, std::wstring_view right) {
  return PathKey(left) == PathKey(right);
}

enum class ReplaceStatus { Replaced, CreateFailed, WriteFailed, ReplaceFailed };

// Replaces `target` with `bytes` through a sibling ".tmp" file. The data is
// flushed with FlushFileBuffers before the rename: MOVEFILE_WRITE_THROUGH
// only makes the rename itself durable, so without the flush a power loss can
// leave the new name pointing at an empty or zero-filled file.
inline ReplaceStatus ReplaceFileDurably(const std::filesystem::path& target,
                                        std::string_view bytes) {
  std::filesystem::path temporary = target;
  temporary += L".tmp";
  const HANDLE file =
      CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return ReplaceStatus::CreateFailed;
  bool written = true;
  std::size_t offset = 0;
  while (written && offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD count = 0;
    written = WriteFile(file, bytes.data() + offset, chunk, &count, nullptr) !=
                  FALSE &&
              count == chunk;
    offset += count;
  }
  written = written && FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  std::error_code ec;
  if (!written) {
    std::filesystem::remove(temporary, ec);
    return ReplaceStatus::WriteFailed;
  }
  if (!MoveFileExW(temporary.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary, ec);
    return ReplaceStatus::ReplaceFailed;
  }
  return ReplaceStatus::Replaced;
}

}  // namespace feathercast::filesystem_semantics
