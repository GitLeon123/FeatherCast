#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <cmath>

namespace feathercast::layout {

// All interactive native surfaces use DIP geometry.  Keep the conversion and
// the hit-test edge convention here so painting, pointer input, and MSAA do
// not grow subtly different copies of the same rectangles.
inline int DipToPixelsRounded(float dip, float scale) noexcept {
  return static_cast<int>(std::lround(std::max(0.0f, dip) *
                                      std::max(0.01f, scale)));
}

inline feathercast::app::RectF Inflate(feathercast::app::RectF rect,
                                       float horizontal, float vertical) {
  rect.left -= horizontal;
  rect.right += horizontal;
  rect.top -= vertical;
  rect.bottom += vertical;
  return rect;
}

inline bool Contains(const feathercast::app::RectF& rect, float x,
                     float y) noexcept {
  return x >= rect.left && x < rect.right && y >= rect.top &&
         y < rect.bottom;
}

inline bool ContainsRounded(const feathercast::app::RectF& rect, float x,
                            float y, float radius) noexcept {
  if (!Contains(rect, x, y)) return false;
  const float width = rect.right - rect.left;
  const float height = rect.bottom - rect.top;
  const float r = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
  if (r <= 0.0f || (x >= rect.left + r && x < rect.right - r) ||
      (y >= rect.top + r && y < rect.bottom - r)) {
    return true;
  }

  const float centerX = x < rect.left + r ? rect.left + r : rect.right - r;
  const float centerY = y < rect.top + r ? rect.top + r : rect.bottom - r;
  const float dx = x - centerX;
  const float dy = y - centerY;
  return dx * dx + dy * dy <= r * r;
}

struct RecordingLayout {
  feathercast::app::RectF panel;
  feathercast::app::RectF pause;
  feathercast::app::RectF stop;
};

inline RecordingLayout RecordingControls(float width = 360.0f,
                                         float height = 64.0f) {
  width = std::max(1.0f, width);
  height = std::max(1.0f, height);
  constexpr float rightInset = 12.0f;
  constexpr float gap = 8.0f;
  constexpr float pauseWidth = 80.0f;
  constexpr float stopWidth = 76.0f;
  constexpr float buttonHeight = 40.0f;
  const float right = std::max(1.0f, width - rightInset);
  const float stopLeft = std::max(0.0f, right - stopWidth);
  const float pauseRight = std::max(0.0f, stopLeft - gap);
  const float pauseLeft = std::max(0.0f, pauseRight - pauseWidth);
  const float top = std::clamp((height - buttonHeight) * 0.5f, 0.0f,
                               std::max(0.0f, height - 1.0f));
  const float bottom = std::min(height, top + buttonHeight);
  return {{0.0f, 0.0f, width, height},
          {pauseLeft, top, pauseRight, bottom},
          {stopLeft, top, right, bottom}};
}

struct VolumeLayout {
  feathercast::app::RectF track;
  feathercast::app::RectF trackHit;
  feathercast::app::RectF mute;
  feathercast::app::RectF muteHit;
};

inline VolumeLayout VolumeControl(float width) {
  width = std::max(56.0f, width);
  const feathercast::app::RectF track{28.0f, 82.0f,
                                      std::max(29.0f, width - 28.0f), 94.0f};
  const feathercast::app::RectF mute{24.0f, 112.0f, 128.0f, 148.0f};
  return {track, Inflate(track, 4.0f, 16.0f), mute, Inflate(mute, 4.0f, 4.0f)};
}

inline feathercast::app::RectF LauncherSearch(float width) {
  width = std::max(1.0f, width);
  return {0.0f, 0.0f, std::max(1.0f, width - 60.0f), 60.0f};
}

struct LauncherLayout {
  feathercast::app::RectF searchHit;
  feathercast::app::RectF query;
  feathercast::app::RectF settings;
};

inline LauncherLayout Launcher(float width, bool compactHintVisible) {
  width = std::max(1.0f, width);
  const auto searchHit = LauncherSearch(width);
  const float hintLeft = width - 360.0f;
  const float inputRight = compactHintVisible ? hintLeft - 12.0f
                                               : width - 94.0f;
  return {searchHit,
          {52.0f, 15.0f, std::max(53.0f, inputRight), 48.0f},
          {width - 52.0f, 14.0f, width - 16.0f, 50.0f}};
}

inline constexpr float LauncherResultsTop() noexcept { return 60.0f; }

inline feathercast::app::RectF SettingsFilter(float width) {
  width = std::max(1.0f, width);
  const float right = std::max(61.0f, width - 60.0f);
  const float left = std::clamp(width - 330.0f, 0.0f,
                                std::max(0.0f, right - 1.0f));
  return {left, 10.0f, right, 46.0f};
}

inline feathercast::app::RectF SettingsClose(float width) {
  width = std::max(1.0f, width);
  return {std::max(0.0f, width - 46.0f), 12.0f,
          std::max(1.0f, width - 14.0f), 44.0f};
}

}  // namespace feathercast::layout
