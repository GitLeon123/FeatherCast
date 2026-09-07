#pragma once
#include "app_types.hpp"

namespace feathercast::timers {

inline app::DisplayItem Item(Request request, std::wstring title, std::wstring detail) {
  app::DisplayItem item;
  item.timerRequest = std::move(request);
  item.commandName = std::move(title);
  item.commandDetail = std::move(detail);
  item.commandKeywords = {item.commandName, item.commandDetail};
  return item;
}

inline std::vector<app::DisplayItem> Items(const State& state, long long now, unsigned long long ticks) {
  std::vector<app::DisplayItem> items;
  for (const auto& timer : state.timers) {
    const std::wstring status = timer.phase == Phase::Finished ? L"Finished" :
        timer.phase == Phase::Paused ? L"Paused" : L"Running";
    items.push_back(Item({Action::Open, timer.id}, timer.name + L"  " + Format(Remaining(timer, now)), status + L" - Enter for controls"));
  }
  items.push_back(Item({Action::Open, 0}, L"Stopwatch  " + Format(state.stopwatch.Elapsed(ticks)),
                       state.stopwatch.running ? L"Running - Enter for controls" : L"Paused - Enter for controls"));
  items.push_back(Item({Action::Open, -1}, L"New Timer", L"Try: timer 10m Tea"));
  return items;
}

inline std::vector<app::DisplayItem> Actions(const State& state, long long id) {
  if (id == 0) {
    return {Item({state.stopwatch.running ? Action::StopwatchPause : Action::StopwatchStart},
                 state.stopwatch.running ? L"Pause Stopwatch" : L"Start / Resume Stopwatch", L"Keep elapsed time"),
            Item({Action::StopwatchReset}, L"Reset Stopwatch", L"Stop and reset elapsed time to zero")};
  }
  const auto found = std::find_if(state.timers.begin(), state.timers.end(), [&](const auto& timer) { return timer.id == id; });
  if (found == state.timers.end()) return {};
  std::vector<app::DisplayItem> items;
  if (found->phase != Phase::Finished) {
    items.push_back(Item({found->phase == Phase::Running ? Action::Pause : Action::Resume, id},
                         found->phase == Phase::Running ? L"Pause Timer" : L"Resume Timer", found->name));
  }
  items.push_back(Item({Action::Restart, id}, L"Restart Timer", found->name + L" - " + Format(found->duration)));
  items.push_back(Item({Action::Delete, id}, L"Delete Timer", found->name));
  return items;
}

}  // namespace feathercast::timers
