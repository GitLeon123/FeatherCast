#pragma once

#include "phone_protocol.hpp"
#include "extension_protocol.hpp"

#include <windows.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>

namespace feathercast::phone::transfer {

inline constexpr long long kMaximumBytes = 8LL * 1024 * 1024 * 1024;
inline constexpr std::size_t kChunkBytes = 256 * 1024;
inline constexpr std::size_t kMaximumConcurrent = 4;

inline bool ValidId(std::string_view id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

struct Progress {
  std::string id;
  std::string name;
  std::string purpose;
  std::string reference;
  long long bytes = 0;
  long long total = 0;
};

struct Result {
  Progress progress;
  std::wstring path;
  std::string error;
  bool complete = false;
  bool reportProgress = true;
};

// The receiving session owns this bounded set of temporary files. Failed,
// cancelled, and disconnected transfers never publish a partially written file.
class Receiver {
 public:
  explicit Receiver(std::wstring directory) : directory_(std::move(directory)) {}

  Result Begin(const json::Value& message, std::string safeName) {
    std::lock_guard lock(mutex_);
    CollectCancelled();
    Progress progress{JsonString(message, "id"), std::move(safeName),
                      JsonString(message, "purpose"), JsonString(message, "ref"),
                      0, JsonInt(message, "size", -1)};
    Result result;
    result.progress = progress;
    if (!ValidId(progress.id) || progress.name.empty() ||
        progress.name.find_first_of("/\\:") != std::string::npos ||
        progress.name == "." || progress.name == ".." || progress.total < 0 ||
        progress.total > kMaximumBytes ||
        (progress.purpose != "file" && progress.purpose != "storage" && progress.purpose != "photo") ||
        files_.contains(progress.id) || files_.size() >= kMaximumConcurrent) {
      result.error = "Invalid transfer or too many transfers in progress.";
      return result;
    }
    auto file = std::make_unique<Incoming>();
    file->progress = progress;
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (!error) {
      file->temporary = std::filesystem::path(directory_) /
          (L".feathercast-" + extensions::Utf8ToWide(progress.id) + L"-" +
           std::to_wstring(GetCurrentProcessId()) + L".part");
      file->handle = CreateFileW(file->temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    }
    if (error || file->handle == INVALID_HANDLE_VALUE) {
      file->temporary.clear();
      result.error = "Could not create a temporary file in Downloads.";
      return result;
    }
    files_.emplace(progress.id, std::move(file));
    return result;
  }

  Result Chunk(const json::Value& message, const Bytes& bytes) {
    std::lock_guard lock(mutex_);
    CollectCancelled();
    const auto id = JsonString(message, "id");
    Result result;
    result.progress.id = id;
    const auto found = files_.find(id);
    if (found == files_.end()) {
      result.error = "The transfer is no longer active.";
      return result;
    }
    auto& file = *found->second;
    result.progress = file.progress;
    DWORD written = 0;
    if (bytes.empty() || bytes.size() > kChunkBytes ||
        JsonInt(message, "offset", -1) != file.progress.bytes ||
        static_cast<long long>(bytes.size()) > file.progress.total - file.progress.bytes ||
        !WriteFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) {
      result.error = "Invalid file chunk or the file could not be written.";
      files_.erase(found);
      return result;
    }
    file.progress.bytes += written;
    file.lastActivity = std::chrono::steady_clock::now();
    result.progress = file.progress;
    const int percent = file.progress.total == 0 ? 100 :
        static_cast<int>(file.progress.bytes * 100 / file.progress.total);
    result.reportProgress = percent != file.lastPercent;
    file.lastPercent = percent;
    return result;
  }

  Result Finish(std::string_view id) {
    std::lock_guard lock(mutex_);
    CollectCancelled();
    Result result;
    result.progress.id = std::string(id);
    const auto found = files_.find(std::string(id));
    if (found == files_.end()) {
      result.error = "The transfer is no longer active.";
      return result;
    }
    auto& file = *found->second;
    result.progress = file.progress;
    if (file.progress.bytes != file.progress.total || !FlushFileBuffers(file.handle) || cancelRequested_.load()) {
      result.error = "The file transfer was incomplete or could not be saved.";
    } else {
      CloseHandle(file.handle);
      file.handle = INVALID_HANDLE_VALUE;
      const auto original = std::filesystem::path(extensions::Utf8ToWide(file.progress.name));
      for (int suffix = 0; suffix < 1000; ++suffix) {
        const auto name = suffix == 0 ? original : std::filesystem::path(
            original.stem().wstring() + L" (" + std::to_wstring(suffix + 1) + L")" + original.extension().wstring());
        const auto target = std::filesystem::path(directory_) / name;
        if (MoveFileW(file.temporary.c_str(), target.c_str())) {
          SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
          file.temporary.clear();
          result.path = target.wstring();
          result.complete = true;
          break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
      }
      if (!result.complete) result.error = "Could not publish the received file in Downloads.";
    }
    files_.erase(found);
    return result;
  }

  std::vector<Progress> Cancel(std::string_view id = {}) {
    std::lock_guard lock(mutex_);
    std::vector<Progress> cancelled;
    if (id.empty()) {
      for (const auto& [unused, file] : files_) cancelled.push_back(file->progress);
      files_.clear();
    } else if (const auto found = files_.find(std::string(id)); found != files_.end()) {
      cancelled.push_back(found->second->progress);
      files_.erase(found);
    }
    return cancelled;
  }
  void RequestCancel() { cancelRequested_.store(true); }
  void Maintain() {
    std::lock_guard lock(mutex_);
    CollectCancelled();
  }

 private:
  void CollectCancelled() {
    if (cancelRequested_.exchange(false)) files_.clear();
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    std::erase_if(files_, [&](const auto& entry) { return entry.second->lastActivity < cutoff; });
  }
  struct Incoming {
    Progress progress;
    std::filesystem::path temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    int lastPercent = -1;
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
    ~Incoming() {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      if (!temporary.empty()) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
      }
    }
  };
  std::wstring directory_;
  std::map<std::string, std::unique_ptr<Incoming>> files_;
  std::mutex mutex_;
  std::atomic<bool> cancelRequested_ = false;
};

}  // namespace feathercast::phone::transfer
