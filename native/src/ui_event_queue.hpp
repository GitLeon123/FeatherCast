#pragma once

#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace feathercast::runtime {

template <typename Event>
class UiEventQueue {
 public:
  using Notifier = std::function<void()>;

  struct DrainResult {
    std::vector<Event> events;
    bool more = false;
  };

  explicit UiEventQueue(Notifier notifier = {})
      : notifier_(std::move(notifier)) {}

  UiEventQueue(const UiEventQueue&) = delete;
  UiEventQueue& operator=(const UiEventQueue&) = delete;

  void SetNotifier(Notifier notifier) {
    std::lock_guard lock(mutex_);
    notifier_ = std::move(notifier);
  }

  bool Push(Event event) {
    Notifier notifier;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return false;
      events_.push_back(std::move(event));
      if (notificationPending_) return true;
      notificationPending_ = true;
      notifier = notifier_;
    }
    if (notifier) notifier();
    return true;
  }

  DrainResult Drain(std::size_t maxCount) {
    DrainResult result;
    Notifier notifier;
    {
      std::lock_guard lock(mutex_);
      const std::size_t count = std::min(maxCount, events_.size());
      result.events.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        result.events.push_back(std::move(events_.front()));
        events_.pop_front();
      }
      result.more = !events_.empty();
      notificationPending_ = result.more;
      if (result.more) notifier = notifier_;
    }
    // The message that caused this drain has already been consumed. Re-arm a
    // single notification for the remainder without invoking user code while
    // the queue mutex is held.
    if (notifier) notifier();
    return result;
  }

  std::vector<Event> Drain() {
    return Drain(std::numeric_limits<std::size_t>::max()).events;
  }

  void Close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    events_.clear();
    notificationPending_ = false;
    notifier_ = {};
  }

  bool Closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  bool Empty() const {
    std::lock_guard lock(mutex_);
    return events_.empty();
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return events_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Event> events_;
  Notifier notifier_;
  bool notificationPending_ = false;
  bool closed_ = false;
};

}  // namespace feathercast::runtime
