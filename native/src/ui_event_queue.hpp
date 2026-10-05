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

  // Installing a notifier while events are already queued wakes the consumer
  // once, so events pushed before the notifier existed are not stranded.
  void SetNotifier(Notifier notifier) {
    Notifier wake;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return;
      notifier_ = std::move(notifier);
      if (!notifier_) {
        notificationPending_ = false;
        return;
      }
      if (notificationPending_ || events_.empty()) return;
      notificationPending_ = true;
      wake = notifier_;
    }
    wake();
  }

  bool Push(Event event) {
    Notifier notifier;
    {
      std::lock_guard lock(mutex_);
      if (closed_) return false;
      events_.push_back(std::move(event));
      // Only a notification that is actually delivered may suppress later
      // ones; without a notifier the flag must stay clear.
      if (notificationPending_ || !notifier_) return true;
      notificationPending_ = true;
      notifier = notifier_;
    }
    notifier();
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
      notificationPending_ = result.more && static_cast<bool>(notifier_);
      if (notificationPending_) notifier = notifier_;
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
