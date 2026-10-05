#pragma once

#include "phone_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace feathercast::phone {

inline constexpr std::size_t kMaxScreenPacketBytes = 2 * 1024 * 1024;
inline constexpr long long kScreenRequestLifetimeMs = 60'000;
inline constexpr int kScreenCoordinateScale = 1'000'000;
inline constexpr std::size_t kMaxScreenTextBytes = 16 * 1024;
// Earlier phone app builds read sealed screen input frames of at most 32 KB.
inline constexpr std::size_t kMaxScreenInputFrameBytes = 32 * 1024;

enum class ScreenPacketKind { State, VideoConfig, Video, AudioConfig, Audio };

struct ScreenPacket {
  ScreenPacketKind kind = ScreenPacketKind::State;
  std::string sessionId;
  std::string state;
  std::string detail;
  int generation = 0;
  int width = 0;
  int height = 0;
  long long ptsUs = 0;
  bool keyframe = false;
  bool control = false;
  bool keyboard = false;
  bool audio = false;
  Bytes data;
};

// Coordinates are millionths of the displayed content, never of its letterbox.
struct ScreenInput {
  std::string sessionId;
  int generation = 0;
  std::string action;
  int x = 0;
  int y = 0;
  int value = 0;
  std::string text;
};

inline std::string ScreenInputJson(const ScreenInput& input) {
  return Json("screen.input").Str("session", input.sessionId)
      .Int("generation", input.generation).Str("action", input.action)
      .Int("x", input.x).Int("y", input.y).Int("value", input.value)
      .Str("text", input.text).Build();
}

inline bool ValidScreenInput(const ScreenInput& input) {
  if (input.sessionId.empty() || input.sessionId.size() > 64 ||
      input.generation <= 0 || input.text.size() > kMaxScreenTextBytes) return false;
  // Quotes, backslashes and control characters grow when escaped; the sealed
  // frame ([u32 length][JSON] + 16-byte tag) must still fit the phone's limit.
  if (!input.text.empty() &&
      ScreenInputJson(input).size() + 4 + 16 > kMaxScreenInputFrameBytes) return false;
  if (input.action == "down" || input.action == "move" || input.action == "up" ||
      input.action == "scroll") {
    return input.x >= 0 && input.x <= kScreenCoordinateScale &&
           input.y >= 0 && input.y <= kScreenCoordinateScale &&
           (input.action != "scroll" || (input.value >= -10 && input.value <= 10));
  }
  if (input.action == "key") {
    // Android keycodes: Enter, Delete, forward Delete and cursor navigation.
    return input.value == 66 || input.value == 67 || input.value == 112 ||
           (input.value >= 19 && input.value <= 22) || input.value == 122 ||
           input.value == 123 || input.value == 61 || input.value == 0;
  }
  return input.action == "text" || input.action == "back" || input.action == "home" ||
         input.action == "recents" || input.action == "cancel" || input.action == "keyframe";
}

inline std::optional<ScreenPacket> ParseScreenPacket(const Payload& payload) {
  if (payload.json.size() > 8192 || payload.binary.size() > kMaxScreenPacketBytes) return {};
  const auto root = json::Parse(payload.json);
  if (!root || root->type != json::Value::Type::Object) return {};
  const auto integer = [&](std::string_view key, long long maximum) -> std::optional<long long> {
    const auto* value = root->Find(key);
    if (!value) return 0;
    if (value->type != json::Value::Type::Number || !std::isfinite(value->number) ||
        value->number < 0 || value->number > static_cast<double>(maximum) ||
        std::floor(value->number) != value->number) return {};
    return static_cast<long long>(value->number);
  };
  ScreenPacket packet;
  packet.sessionId = JsonString(*root, "session");
  if (packet.sessionId.empty() || packet.sessionId.size() > 64) return {};
  const auto type = JsonString(*root, "type");
  if (type == "screen.state") packet.kind = ScreenPacketKind::State;
  else if (type == "screen.video.config") packet.kind = ScreenPacketKind::VideoConfig;
  else if (type == "screen.video") packet.kind = ScreenPacketKind::Video;
  else if (type == "screen.audio.config") packet.kind = ScreenPacketKind::AudioConfig;
  else if (type == "screen.audio") packet.kind = ScreenPacketKind::Audio;
  else return {};
  const auto generation = integer("generation", 1'000'000);
  const auto pts = integer("pts", 900'000'000'000'000'000LL);
  if (!generation || !pts) return {};
  packet.generation = static_cast<int>(*generation);
  packet.ptsUs = *pts;
  packet.state = JsonString(*root, "state");
  packet.detail = JsonString(*root, "detail");
  packet.control = JsonBool(*root, "control");
  packet.keyboard = JsonBool(*root, "keyboard");
  packet.audio = JsonBool(*root, "audio");
  packet.keyframe = JsonBool(*root, "keyframe");
  if (packet.kind == ScreenPacketKind::VideoConfig) {
    const auto w = integer("width", 4096), h = integer("height", 4096);
    if (!w || !h || *w < 2 || *h < 2 || (*w % 2) || (*h % 2) ||
        payload.binary.empty() || payload.binary.size() > 65536) return {};
    packet.width = static_cast<int>(*w);
    packet.height = static_cast<int>(*h);
  }
  if (packet.kind != ScreenPacketKind::State &&
      (packet.generation <= 0 || payload.binary.empty())) return {};
  if (packet.kind == ScreenPacketKind::AudioConfig && payload.binary.size() > 64) return {};
  if (packet.kind == ScreenPacketKind::Audio && payload.binary.size() > 65536) return {};
  packet.data = payload.binary;
  return packet;
}

struct ScreenRect { float left = 0, top = 0, right = 0, bottom = 0; };

inline ScreenRect FitScreenRect(ScreenRect viewport, int width, int height) {
  if (width <= 0 || height <= 0 || viewport.right <= viewport.left ||
      viewport.bottom <= viewport.top) return {};
  const float scale = std::min((viewport.right - viewport.left) / width,
                               (viewport.bottom - viewport.top) / height);
  const float w = width * scale, h = height * scale;
  const float left = (viewport.left + viewport.right - w) * 0.5f;
  const float top = (viewport.top + viewport.bottom - h) * 0.5f;
  return {left, top, left + w, top + h};
}

inline std::optional<std::pair<int, int>> ScreenCoordinates(ScreenRect rect, float x, float y) {
  if (rect.right <= rect.left || rect.bottom <= rect.top || x < rect.left ||
      x >= rect.right || y < rect.top || y >= rect.bottom) return {};
  return std::pair{static_cast<int>((x - rect.left) * kScreenCoordinateScale /
                                   (rect.right - rect.left)),
                   static_cast<int>((y - rect.top) * kScreenCoordinateScale /
                                   (rect.bottom - rect.top))};
}

}  // namespace feathercast::phone
