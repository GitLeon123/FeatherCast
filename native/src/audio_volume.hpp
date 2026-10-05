#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace feathercast::audio {

inline int ClampPercent(int percent) {
  return std::clamp(percent, 0, 100);
}

inline int AdjustPercent(int percent, int delta) {
  return ClampPercent(percent + delta);
}

inline int PercentFromTrack(float x, float left, float right) {
  if (right <= left) return 0;
  const float position = std::clamp((x - left) / (right - left), 0.0f, 1.0f);
  return ClampPercent(static_cast<int>(std::lround(position * 100.0f)));
}

struct DefaultOutputState {
  int percent = 0;
  bool muted = false;
  std::wstring deviceName;
};

std::optional<DefaultOutputState> ReadDefaultOutputState();
bool SetDefaultOutputVolumePercent(int percent);
bool StepDefaultOutputVolume(bool increase);
bool SetDefaultOutputMute(bool muted);
bool ToggleDefaultOutputMute();
// Keep the endpoint alive while the compact volume surface is open so repeated
// key repeats do not recreate the MMDevice enumerator for every tick.
void SetDefaultOutputEndpointCacheEnabled(bool enabled);

}  // namespace feathercast::audio
