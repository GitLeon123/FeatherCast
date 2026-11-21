#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace feathercast::timers {

struct Due { std::uint64_t generation = 0; };

class TimerService {
 public:
  using Sink = std::function<void(Due)>;
  explicit TimerService(Sink sink) : sink_(std::move(sink)) {}
  ~TimerService() { Stop(); }
  bool Start();
  bool Schedule(std::optional<long long> deadline, std::uint64_t generation);
  void Stop();
 private:
  static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER);
  Sink sink_;
  PTP_TIMER timer_ = nullptr;
  std::mutex mutex_;
  std::uint64_t generation_ = 0;
  unsigned long long dueFileTime_ = 0;
  bool armed_ = false;
  bool stopping_ = false;
};

}  // namespace feathercast::timers
