#pragma once

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::timers {

inline constexpr long long kMaxDeadline = 900000000000000LL;

inline long long Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

enum class Phase { Running, Paused, Finished };
enum class Action { Open, Create, Pause, Resume, Restart, Delete,
                    StopwatchStart, StopwatchPause, StopwatchReset, Invalid };

struct Timer {
  long long id = 0;
  std::wstring name;
  long long duration = 0;
  long long deadline = 0;
  long long remaining = 0;
  Phase phase = Phase::Paused;
};

struct Stopwatch {
  long long elapsed = 0;
  bool running = false;
  // A runtime monotonic clock that includes sleep; never written to disk.
  unsigned long long started = 0;
  long long Elapsed(unsigned long long now) const {
    return elapsed + (running && now >= started ? static_cast<long long>(now - started) : 0);
  }
  void Pause(unsigned long long now) { elapsed = Elapsed(now); running = false; }
};

struct State {
  std::vector<Timer> timers;
  Stopwatch stopwatch;
};

struct Request {
  Action action = Action::Open;
  long long id = 0;
  long long duration = 0;
  std::wstring name;
};

inline long long Remaining(const Timer& timer, long long now) {
  return timer.phase == Phase::Running ? std::max(0LL, timer.deadline - now)
                                      : timer.remaining;
}

inline std::wstring Format(long long milliseconds) {
  const auto seconds = std::max(0LL, milliseconds + 999) / 1000;
  auto two = [](long long value) { return (value < 10 ? L"0" : L"") + std::to_wstring(value); };
  return std::to_wstring(seconds / 3600) + L":" + two(seconds / 60 % 60) + L":" + two(seconds % 60);
}

// No side effects while typing: only Enter activates the returned request.
inline std::optional<Request> Parse(const std::wstring& query) {
  std::size_t pos = 0;
  while (pos < query.size() && std::iswspace(query[pos])) ++pos;
  const auto start = pos;
  while (pos < query.size() && !std::iswspace(query[pos])) ++pos;
  std::wstring prefix = query.substr(start, pos - start);
  std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  if (prefix != L"timer") return std::nullopt;
  Request request{Action::Invalid};
  long long total = 0;
  while (pos < query.size()) {
    while (pos < query.size() && std::iswspace(query[pos])) ++pos;
    if (pos == query.size()) break;
    if (query[pos] < L'0' || query[pos] > L'9') {
      if (total == 0) return request;
      request.name = query.substr(pos);
      while (!request.name.empty() && std::iswspace(request.name.back())) request.name.pop_back();
      break;
    }
    long long value = 0;
    while (pos < query.size() && query[pos] >= L'0' && query[pos] <= L'9') {
      const int digit = query[pos++] - L'0';
      if (value > (std::numeric_limits<long long>::max() - digit) / 10) return request;
      value = value * 10 + digit;
    }
    if (pos == query.size()) return request;
    const auto unit = std::towlower(query[pos++]);
    const long long factor = unit == L'h' ? 3600000 : unit == L'm' ? 60000 : unit == L's' ? 1000 : 0;
    if (!factor || value > (std::numeric_limits<long long>::max() / 10000 - total) / factor) return request;
    total += value * factor;
    if (pos < query.size() && !std::iswspace(query[pos]) && (query[pos] < L'0' || query[pos] > L'9')) return request;
  }
  if (total <= 0) return request;
  request.action = Action::Create;
  request.duration = total;
  if (request.name.empty()) request.name = L"Timer";
  return request;
}

inline std::vector<std::wstring> Expire(State& state, long long now) {
  std::vector<std::wstring> names;
  for (auto& timer : state.timers) {
    if (timer.phase == Phase::Running && timer.deadline <= now) {
      timer.phase = Phase::Finished;
      timer.remaining = 0;
      names.push_back(timer.name);
    }
  }
  return names;
}

inline std::optional<long long> NextDeadline(const State& state) {
  std::optional<long long> result;
  for (const auto& timer : state.timers) {
    if (timer.phase == Phase::Running && (!result || timer.deadline < *result)) result = timer.deadline;
  }
  return result;
}

inline bool Apply(State& state, const Request& request, long long now,
                  unsigned long long monotonicNow) {
  if (request.action == Action::Create) {
    if (now < 0 || now > kMaxDeadline || request.duration <= 0 || request.duration > kMaxDeadline - now) return false;
    long long id = std::max(1LL, now);
    for (const auto& timer : state.timers) {
      if (timer.id == std::numeric_limits<long long>::max()) return false;
      id = std::max(id, timer.id + 1);
    }
    state.timers.push_back({id, request.name, request.duration, now + request.duration, request.duration, Phase::Running});
    return true;
  }
  auto& watch = state.stopwatch;
  switch (request.action) {
    case Action::StopwatchStart:
      if (watch.running) return false;
      watch.started = monotonicNow; watch.running = true; return true;
    case Action::StopwatchPause:
      if (!watch.running) return false;
      watch.Pause(monotonicNow); return true;
    case Action::StopwatchReset: watch = {}; return true;
    default: break;
  }
  const auto found = std::find_if(state.timers.begin(), state.timers.end(), [&](const Timer& timer) { return timer.id == request.id; });
  if (found == state.timers.end()) return false;
  switch (request.action) {
    case Action::Pause:
      if (found->phase != Phase::Running) return false;
      found->remaining = Remaining(*found, now); found->phase = Phase::Paused; return true;
    case Action::Resume:
      if (found->phase != Phase::Paused || found->remaining <= 0) return false;
      if (now < 0 || now > kMaxDeadline - found->remaining) return false;
      found->deadline = now + found->remaining; found->phase = Phase::Running; return true;
    case Action::Restart:
      if (now < 0 || now > kMaxDeadline - found->duration) return false;
      found->deadline = now + found->duration; found->remaining = found->duration; found->phase = Phase::Running; return true;
    case Action::Delete: state.timers.erase(found); return true;
    default: return false;
  }
}

}  // namespace feathercast::timers
