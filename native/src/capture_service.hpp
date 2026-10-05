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

// Converts high-resolution counter readings (QueryPerformanceCounter ticks)
// into media sample times in 100 ns units, relative to the start of the
// recording and excluding paused intervals. Sample times are strictly
// increasing so the encoder never receives duplicate or reordered frames.
class RecordingClock {
 public:
  explicit RecordingClock(std::int64_t frequency = 1) noexcept
      : frequency_(frequency > 0 ? frequency : 1) {}

  void Start(std::int64_t now) noexcept {
    start_ = now;
    pausedTicks_ = 0;
    pauseStart_ = now;
    lastSample_ = -1;
    started_ = true;
    paused_ = false;
  }

  void Pause(std::int64_t now) noexcept {
    if (!started_ || paused_) return;
    paused_ = true;
    pauseStart_ = now;
  }

  void Resume(std::int64_t now) noexcept {
    if (!started_ || !paused_) return;
    paused_ = false;
    if (now > pauseStart_) pausedTicks_ += now - pauseStart_;
  }

  [[nodiscard]] bool Paused() const noexcept { return paused_; }

  // Media time of a frame captured at `now`.
  [[nodiscard]] std::int64_t SampleTime(std::int64_t now) noexcept {
    std::int64_t time = ToHundredNanoseconds(ActiveTicks(now));
    if (lastSample_ >= 0 && time <= lastSample_) time = lastSample_ + 1;
    lastSample_ = time;
    return time;
  }

  // Recorded (unpaused) time up to `now`.
  [[nodiscard]] std::chrono::nanoseconds Elapsed(
      std::int64_t now) const noexcept {
    return std::chrono::nanoseconds(ToHundredNanoseconds(ActiveTicks(now)) *
                                    100);
  }

 private:
  [[nodiscard]] std::int64_t ActiveTicks(std::int64_t now) const noexcept {
    if (!started_) return 0;
    const std::int64_t end = paused_ ? pauseStart_ : now;
    const std::int64_t active = end - start_ - pausedTicks_;
    return active > 0 ? active : 0;
  }

  [[nodiscard]] std::int64_t ToHundredNanoseconds(
      std::int64_t ticks) const noexcept {
    constexpr std::int64_t kUnitsPerSecond = 10'000'000;
    return ticks / frequency_ * kUnitsPerSecond +
           ticks % frequency_ * kUnitsPerSecond / frequency_;
  }

  std::int64_t frequency_ = 1;
  std::int64_t start_ = 0;
  std::int64_t pausedTicks_ = 0;
  std::int64_t pauseStart_ = 0;
  std::int64_t lastSample_ = -1;
  bool started_ = false;
  bool paused_ = false;
};
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
  // Stops the current operation. A screenshot that is waiting in the editor
  // has no worker, so it is canceled immediately (Stopping + Completed).
  bool Stop();
  void Shutdown();
  [[nodiscard]] CaptureState State() const noexcept;

 private:
  class Impl;
  Impl* impl_;
};

}  // namespace feathercast::capture
