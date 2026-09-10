#pragma once

#include "screenshot_editor.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::capture {

enum class CaptureOperation { Screenshot, Recording };
enum class CaptureScope { Fullscreen, Region };
enum class CaptureState {
  Idle,
  PreparingScreenshot,
  ScreenshotEditing,
  StartingScreenshot,
  StartingRecording,
  Recording,
  Paused,
  Stopping,
};
enum class CaptureEventKind {
  Started,
  ScreenshotReady,
  Paused,
  Resumed,
  Stopping,
  Completed,
  ScreenshotOutputFailed,
  Failed,
};

struct PixelRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  [[nodiscard]] int Width() const noexcept { return right - left; }
  [[nodiscard]] int Height() const noexcept { return bottom - top; }
  [[nodiscard]] bool Empty() const noexcept {
    return right <= left || bottom <= top;
  }
};

struct MonitorSlice {
  HMONITOR monitor = nullptr;
  PixelRect monitorBounds;
  PixelRect intersection;
};

struct CaptureEvent {
  CaptureEventKind kind = CaptureEventKind::Failed;
  CaptureOperation operation = CaptureOperation::Screenshot;
  CaptureScope scope = CaptureScope::Region;
  PixelRect bounds;
  std::filesystem::path path;
  std::wstring message;
  std::chrono::milliseconds elapsed{};
  bool clipboardSucceeded = false;
  std::shared_ptr<const screenshot::Draft> screenshotDraft;
};

[[nodiscard]] PixelRect NormalizeRect(PixelRect rect) noexcept;
[[nodiscard]] std::optional<PixelRect> IntersectRects(
    PixelRect first, PixelRect second) noexcept;
[[nodiscard]] std::vector<MonitorSlice> GetMonitorSlices(PixelRect bounds);
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> EvenRecordingSize(
    PixelRect bounds) noexcept;
[[nodiscard]] std::chrono::nanoseconds RecordingTimestamp(
    std::uint64_t encodedFrameCount) noexcept;
[[nodiscard]] std::filesystem::path CaptureOutputPath(
    const std::filesystem::path& folder, CaptureOperation operation,
    const SYSTEMTIME& localTime, unsigned collision = 1,
    bool partial = false);
[[nodiscard]] std::optional<std::wstring> RecordingSupportError();
bool ExcludeWindowFromCapture(HWND window) noexcept;

class CaptureService {
 public:
  using Callback = std::function<void(CaptureEvent)>;

  explicit CaptureService(Callback callback = {});
  ~CaptureService();

  CaptureService(const CaptureService&) = delete;
  CaptureService& operator=(const CaptureService&) = delete;

  void SetCallback(Callback callback);
  bool PrepareScreenshot(
      PixelRect sourceBounds, CaptureScope scope = CaptureScope::Region);
  bool FinalizeScreenshot(
      std::shared_ptr<const screenshot::Draft> draft,
      screenshot::Rect crop,
      std::vector<screenshot::Annotation> annotations,
      screenshot::Destination destination);
  bool CancelScreenshot();
  bool StartRecording(
      PixelRect bounds, CaptureScope scope = CaptureScope::Region);
  bool Pause();
  bool Resume();
  bool Stop();
  void Shutdown();
  [[nodiscard]] CaptureState State() const noexcept;

 private:
  class Impl;
  Impl* impl_;
};

}  // namespace feathercast::capture
