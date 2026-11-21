#include "timer_service.hpp"

namespace feathercast::timers {

void CALLBACK TimerService::OnTimer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
  auto* self = static_cast<TimerService*>(context);
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(self->mutex_);
    if (self->stopping_ || !self->timer_ || !self->armed_) return;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    const unsigned long long now =
        (static_cast<unsigned long long>(nowFt.dwHighDateTime) << 32) |
        static_cast<unsigned long long>(nowFt.dwLowDateTime);
    if (now < self->dueFileTime_) {
      // Either a stale callback from an earlier deadline raced with a later
      // Schedule(), or the OS woke slightly before dueFileTime_. Re-arming for
      // dueFileTime_ handles both safely without losing the timer.
      FILETIME due{};
      due.dwLowDateTime = static_cast<DWORD>(self->dueFileTime_ & 0xFFFFFFFFULL);
      due.dwHighDateTime = static_cast<DWORD>(self->dueFileTime_ >> 32);
      SetThreadpoolTimer(self->timer_, &due, 0, 0);
      return;
    }
    self->armed_ = false;
    generation = self->generation_;
  }
  if (self->sink_) self->sink_(Due{generation});
}

bool TimerService::Start() {
  std::lock_guard lock(mutex_);
  if (timer_) return true;
  stopping_ = false;
  armed_ = false;
  dueFileTime_ = 0;
  timer_ = CreateThreadpoolTimer(&TimerService::OnTimer, this, nullptr);
  return timer_ != nullptr;
}

bool TimerService::Schedule(std::optional<long long> deadline, std::uint64_t generation) {
  std::lock_guard lock(mutex_);
  if (!timer_ || stopping_) return false;
  generation_ = generation;
  if (!deadline) {
    armed_ = false;
    dueFileTime_ = 0;
    SetThreadpoolTimer(timer_, nullptr, 0, 0);
    return true;
  }
  // Positive absolute UTC FILETIME includes sleep and never wakes the PC.
  const long long ticks = (*deadline + 11644473600000LL) * 10000LL;
  const unsigned long long u = ticks <= 0 ? 1ULL : static_cast<unsigned long long>(ticks);
  dueFileTime_ = u;
  armed_ = true;
  FILETIME due{};
  due.dwLowDateTime = static_cast<DWORD>(u & 0xFFFFFFFFULL);
  due.dwHighDateTime = static_cast<DWORD>(u >> 32);
  SetThreadpoolTimer(timer_, &due, 0, 0);
  return true;
}

void TimerService::Stop() {
  PTP_TIMER timer = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (!timer_) return;
    stopping_ = true;
    armed_ = false;
    dueFileTime_ = 0;
    timer = timer_;
    timer_ = nullptr;
    SetThreadpoolTimer(timer, nullptr, 0, 0);
  }
  // Wait outside mutex_ so any in-flight OnTimer callback cannot deadlock.
  WaitForThreadpoolTimerCallbacks(timer, TRUE);
  CloseThreadpoolTimer(timer);
  std::lock_guard lock(mutex_);
  stopping_ = false;
}

}  // namespace feathercast::timers
