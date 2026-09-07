#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

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
  Sink sink_;
  HANDLE timer_ = nullptr;
  HANDLE changed_ = nullptr;
  std::jthread worker_;
  std::mutex mutex_;
  std::uint64_t generation_ = 0;
};

}  // namespace feathercast::timers
