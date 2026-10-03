#pragma once

#include "phone_screen_protocol.hpp"

#include <functional>
#include <memory>

namespace feathercast::phone {

struct ScreenFrame {
  std::string sessionId;
  int width = 0;
  int height = 0;
  int generation = 0;
  Bytes bgra;
};

struct ScreenPlaybackCallbacks {
  std::function<void(std::shared_ptr<const ScreenFrame>)> frame;
  std::function<void(ScreenPacket)> status;
  std::function<void(ScreenInput)> input;
};

// All codecs and WASAPI objects live on one MTA worker, allocated only for an
// actual screen session. Callbacks run there, not on the window thread.
class ScreenPlayback {
 public:
  explicit ScreenPlayback(ScreenPlaybackCallbacks callbacks);
  ~ScreenPlayback();
  ScreenPlayback(const ScreenPlayback&) = delete;
  ScreenPlayback& operator=(const ScreenPlayback&) = delete;
  void Begin(std::string sessionId);
  void End();
  void OnPacket(ScreenPacket packet);
  void SetAudio(bool enabled, float volume);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace feathercast::phone
