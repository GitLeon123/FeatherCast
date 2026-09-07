#include "timer_service.hpp"

namespace feathercast::timers {

bool TimerService::Start() {
  if (worker_.joinable()) return true;
  changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (!changed_ || !timer_) { Stop(); return false; }
  worker_ = std::jthread([this](std::stop_token stop) {
    const HANDLE events[] = {changed_, timer_};
    while (!stop.stop_requested()) {
      const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
      if (stop.stop_requested() || result == WAIT_FAILED) break;
      if (result == WAIT_OBJECT_0 + 1) {
        std::uint64_t generation;
        {
          std::lock_guard lock(mutex_);
          generation = generation_;
        }
        sink_(Due{generation});
      }
    }
  });
  return true;
}

bool TimerService::Schedule(std::optional<long long> deadline, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (!timer_) return false;
  generation_ = generation;
  if (!deadline) { CancelWaitableTimer(timer_); return true; }
  LARGE_INTEGER due{};
  // Absolute UTC deadline includes sleep. fResume=FALSE never wakes the PC.
  due.QuadPart = (*deadline + 11644473600000LL) * 10000LL;
  return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

void TimerService::Stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    SetEvent(changed_);
    worker_.join();
  }
  if (timer_) CloseHandle(timer_);
  if (changed_) CloseHandle(changed_);
  timer_ = changed_ = nullptr;
}

}  // namespace feathercast::timers
