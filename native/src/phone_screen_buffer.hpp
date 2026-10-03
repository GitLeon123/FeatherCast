#pragma once

#include "phone_screen_protocol.hpp"

#include <deque>

namespace feathercast::phone {

// Caller supplies synchronization. Keep complete codec access units; when video
// falls behind, recover at an IDR instead of decoding orphaned delta frames.
class ScreenPacketBuffer {
 public:
  bool Push(ScreenPacket packet) {
    if (packet.data.size() > kMaxScreenPacketBytes) return false;
    if (packet.kind == ScreenPacketKind::VideoConfig) {
      std::erase_if(packets_, [](const auto& p) {
        return p.kind == ScreenPacketKind::Video || p.kind == ScreenPacketKind::VideoConfig;
      });
      waitingForKeyframe_ = true;
    } else if (packet.kind == ScreenPacketKind::AudioConfig) {
      std::erase_if(packets_, [](const auto& p) {
        return p.kind == ScreenPacketKind::Audio || p.kind == ScreenPacketKind::AudioConfig;
      });
    } else if (packet.kind == ScreenPacketKind::Video) {
      std::size_t count = 0, bytes = 0;
      long long oldest = packet.ptsUs;
      for (const auto& p : packets_) {
        bytes += p.data.size();
        if (p.kind == ScreenPacketKind::Video) { if (!count) oldest = p.ptsUs; ++count; }
      }
      if (count >= 6 || packet.ptsUs - oldest > 250'000 ||
          bytes + packet.data.size() > 4 * 1024 * 1024) {
        std::erase_if(packets_, [](const auto& p) { return p.kind == ScreenPacketKind::Video; });
        waitingForKeyframe_ = true;
      }
      if (waitingForKeyframe_ && !packet.keyframe) return false;
      waitingForKeyframe_ = false;
    } else if (packet.kind == ScreenPacketKind::Audio) {
      std::size_t count = 0;
      for (const auto& p : packets_) if (p.kind == ScreenPacketKind::Audio) ++count;
      std::erase_if(packets_, [&](const auto& p) {
        if (p.kind != ScreenPacketKind::Audio) return false;
        if (count >= 6 || packet.ptsUs - p.ptsUs > 150'000) { --count; return true; }
        return false;
      });
    }
    packets_.push_back(std::move(packet));
    return true;
  }
  std::optional<ScreenPacket> Pop() {
    if (packets_.empty()) return {};
    auto packet = std::move(packets_.front());
    packets_.pop_front();
    return packet;
  }
  void Clear() { packets_.clear(); waitingForKeyframe_ = true; }
  std::size_t Size() const { return packets_.size(); }

 private:
  std::deque<ScreenPacket> packets_;
  bool waitingForKeyframe_ = true;
};

}  // namespace feathercast::phone
