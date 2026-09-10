#include "runtime_events.hpp"
#include "test_framework.hpp"
#include "ui_event_queue.hpp"

#include <atomic>
#include <string>
#include <variant>

int main() {
  using feathercast::runtime::BackgroundSubsystem;
  using feathercast::runtime::BackgroundTaskFailed;
  using feathercast::runtime::IconResolved;
  using feathercast::runtime::UiEvent;
  using feathercast::runtime::UiEventQueue;

  std::atomic<int> notifications = 0;
  UiEventQueue<UiEvent> queue([&] { ++notifications; });

  assert(queue.Push(IconResolved{L"one"}));
  assert(queue.Push(IconResolved{L"two"}));
  assert(notifications.load() == 1);

  auto partial = queue.Drain(1);
  assert(partial.events.size() == 1);
  assert(partial.more);
  assert(std::get<IconResolved>(partial.events.front()).key == L"one");
  // A partial drain must re-arm exactly one notification for the remainder.
  assert(notifications.load() == 2);

  auto remainder = queue.Drain(1);
  assert(remainder.events.size() == 1);
  assert(!remainder.more);
  assert(std::get<IconResolved>(remainder.events.front()).key == L"two");
  assert(queue.Empty());

  assert(queue.Push(BackgroundTaskFailed{
      BackgroundSubsystem::Persistence, L"save failed"}));
  assert(notifications.load() == 3);
  auto events = queue.Drain();
  assert(events.size() == 1);
  assert(std::get<BackgroundTaskFailed>(events.front()).message ==
         L"save failed");

  queue.Close();
  assert(queue.Closed());
  assert(!queue.Push(IconResolved{L"ignored"}));
  assert(queue.Drain().empty());
  return 0;
}
