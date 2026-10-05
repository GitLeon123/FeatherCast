#pragma once

#include "app_types.hpp"

#include <algorithm>
#include <cstddef>
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

inline float TextScale(int textSizePercent) noexcept {
  return std::clamp(static_cast<float>(textSizePercent) / 100.0f, 0.9f, 2.0f);
}

inline float GrowForText(float base, float growthAt200Percent,
                         int textSizePercent) noexcept {
  return base + growthAt200Percent *
                    std::max(0.0f, TextScale(textSizePercent) - 1.0f);
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

inline float RecordingPanelWidth(int textSizePercent = 100) noexcept {
  return GrowForText(360.0f, 120.0f, textSizePercent);
}

inline float RecordingPanelHeight(int textSizePercent = 100) noexcept {
  return GrowForText(64.0f, 8.0f, textSizePercent);
}

inline RecordingLayout RecordingControls(float width = 360.0f,
                                         float height = 64.0f,
                                         int textSizePercent = 100) {
  width = std::max(1.0f, width);
  height = std::max(1.0f, height);
  constexpr float rightInset = 12.0f;
  constexpr float gap = 8.0f;
  const float pauseWidth = GrowForText(80.0f, 40.0f, textSizePercent);
  const float stopWidth = GrowForText(76.0f, 20.0f, textSizePercent);
  const float buttonHeight = GrowForText(44.0f, 4.0f, textSizePercent);
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
  float panelHeight = 210.0f;
  feathercast::app::RectF title;
  feathercast::app::RectF output;
  feathercast::app::RectF value;
  feathercast::app::RectF track;
  feathercast::app::RectF trackHit;
  feathercast::app::RectF mute;
  feathercast::app::RectF muteHit;
  feathercast::app::RectF footer;
};

inline VolumeLayout VolumeControl(float width, int textSizePercent = 100) {
  width = std::max(56.0f, width);
  const float scale = TextScale(textSizePercent);
  const float titleHeight = std::max(28.0f, 23.0f * scale);
  const float outputHeight = std::max(22.0f, 18.0f * scale);
  const feathercast::app::RectF title{24.0f, 18.0f, width - 150.0f,
                                      18.0f + titleHeight};
  const feathercast::app::RectF output{24.0f, title.bottom + 2.0f,
                                       width - 150.0f,
                                       title.bottom + 2.0f + outputHeight};
  const float valueHeight = std::max(42.0f, 32.0f * scale);
  const feathercast::app::RectF value{width - 145.0f, 10.0f,
                                      width - 24.0f, 10.0f + valueHeight};
  const float trackTop = output.bottom + 12.0f;
  const feathercast::app::RectF track{28.0f, trackTop,
                                      std::max(29.0f, width - 28.0f),
                                      trackTop + 12.0f};
  const float muteHeight = GrowForText(36.0f, 8.0f, textSizePercent);
  const feathercast::app::RectF mute{24.0f, track.bottom + 18.0f,
                                     128.0f, track.bottom + 18.0f + muteHeight};
  const float footerHeight = std::max(22.0f, 18.0f * scale);
  const feathercast::app::RectF footer{24.0f, mute.bottom + 22.0f,
                                       width - 24.0f,
                                       mute.bottom + 22.0f + footerHeight};
  return {footer.bottom + 18.0f,
          title,
          output,
          value,
          track,
          Inflate(track, 4.0f, 16.0f),
          mute,
          Inflate(mute, 4.0f, 4.0f),
          footer};
}

struct LauncherLayout {
  feathercast::app::RectF searchHit;
  feathercast::app::RectF query;
  feathercast::app::RectF settings;
};

struct TextLayoutMetrics {
  float scale = 1.0f;
  float launcherHeaderHeight = 60.0f;
  float sectionHeaderHeight = 26.0f;
  float resultRowHeight = 50.0f;
  float resultRowGap = 2.0f;
  float settingsRowHeight = 60.0f;

  float ResultRowStride() const noexcept {
    return resultRowHeight + resultRowGap;
  }
};

inline TextLayoutMetrics TextMetrics(int textSizePercent) noexcept {
  const float scale = TextScale(textSizePercent);
  const float growth = std::max(0.0f, scale - 1.0f);
  return {scale,
          60.0f + 24.0f * growth,
          26.0f + 14.0f * growth,
          50.0f + 34.0f * growth,
          2.0f,
          60.0f + 20.0f * growth};
}

// Thumbnail grid used by result browse views such as phone photos. Cells are
// square and laid out left to right, then top to bottom.
struct ResultGrid {
  int columns = 1;
  float cell = 0.0f;
  float gap = 8.0f;

  float Stride() const noexcept { return cell + gap; }
  int RowOf(int index) const noexcept {
    return columns > 0 ? std::max(0, index) / columns : 0;
  }
  int ColumnOf(int index) const noexcept {
    return columns > 0 ? std::max(0, index) % columns : 0;
  }
  int Rows(std::size_t count) const noexcept {
    return columns > 0
               ? static_cast<int>((count + static_cast<std::size_t>(columns) - 1) /
                                  static_cast<std::size_t>(columns))
               : 0;
  }
  // Height taken by count cells, including the gap below the last row.
  float Height(std::size_t count) const noexcept {
    return static_cast<float>(Rows(count)) * Stride();
  }
};

// Fits as many cells of at least minCell as possible into width and then
// stretches them so the row fills the available width exactly.
inline ResultGrid FitResultGrid(float width, float minCell = 112.0f,
                                float gap = 8.0f) noexcept {
  ResultGrid grid;
  grid.gap = gap;
  width = std::max(minCell, width);
  grid.columns = std::max(1, static_cast<int>((width + gap) / (minCell + gap)));
  grid.cell = (width - gap * static_cast<float>(grid.columns - 1)) /
              static_cast<float>(grid.columns);
  return grid;
}

// Arrow-key movement inside a grid. Returns the index unchanged when the move
// would leave the grid.
inline int GridMove(int index, int count, int columns, int dx,
                    int dy) noexcept {
  if (count <= 0 || columns <= 0) return index;
  index = std::clamp(index, 0, count - 1);
  if (dx != 0) {
    const int next = index + dx;
    return next < 0 || next >= count ? index : next;
  }
  if (dy != 0) {
    const int next = index + dy * columns;
    if (next < 0) return index;
    if (next >= count) {
      // Moving down from a row above a shorter last row lands on its last cell.
      const int lastRow = (count - 1) / columns;
      return index / columns < lastRow ? count - 1 : index;
    }
    return next;
  }
  return index;
}

inline float SettingsCategoryRowHeight(float availableHeight,
                                       std::size_t categoryCount) noexcept {
  if (categoryCount == 0) return 44.0f;
  return std::clamp(availableHeight / static_cast<float>(categoryCount),
                    24.0f, 44.0f);
}

inline LauncherLayout Launcher(float width, bool compactHintVisible,
                               int textSizePercent = 100) {
  width = std::max(1.0f, width);
  const auto metrics = TextMetrics(textSizePercent);
  const auto searchHit = feathercast::app::RectF{
      0.0f, 0.0f, std::max(1.0f, width - 60.0f),
      metrics.launcherHeaderHeight};
  const float hintLeft = width - 360.0f;
  const float inputRight = compactHintVisible ? hintLeft - 12.0f
                                               : width - 94.0f;
  const float settingsTop =
      (metrics.launcherHeaderHeight - 44.0f) * 0.5f;
  return {searchHit,
          {52.0f, 12.0f, std::max(53.0f, inputRight),
           metrics.launcherHeaderHeight - 10.0f},
          {width - 56.0f, settingsTop, width - 12.0f,
           settingsTop + 44.0f}};
}

inline float LauncherResultsTop(int textSizePercent = 100) noexcept {
  return TextMetrics(textSizePercent).launcherHeaderHeight;
}

inline feathercast::app::RectF SettingsFilter(float width) {
  width = std::max(1.0f, width);
  const float right = std::max(61.0f, width - 60.0f);
  const float left = std::clamp(width - 330.0f, 0.0f,
                                std::max(0.0f, right - 1.0f));
  return {left, 10.0f, right, 46.0f};
}

inline feathercast::app::RectF SettingsClose(float width) {
  width = std::max(1.0f, width);
  return {std::max(0.0f, width - 52.0f), 6.0f,
          std::max(1.0f, width - 8.0f), 50.0f};
}

}  // namespace feathercast::layout
