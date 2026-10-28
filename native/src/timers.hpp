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

// Milliseconds per unit word ("s", "sec", "minutes", "hr"), or 0.
inline long long UnitFactor(const std::wstring& unit) {
  for (const auto* name : {L"s", L"sec", L"secs", L"second", L"seconds"}) {
    if (unit == name) return 1000;
  }
  for (const auto* name : {L"m", L"min", L"mins", L"minute", L"minutes"}) {
    if (unit == name) return 60000;
  }
  for (const auto* name : {L"h", L"hr", L"hrs", L"hour", L"hours"}) {
    if (unit == name) return 3600000;
  }
  return 0;
}

// Durations are <number><unit> pairs, glued ("1h30m") or spaced ("10 min").
// A lone number without any unit means minutes ("timer 5"). The first token
// that is not part of the duration starts the name, so "timer 10m 2 eggs" is
// a ten-minute timer named "2 eggs".
// No side effects while typing: only Enter activates the returned request.
inline std::optional<Request> Parse(const std::wstring& query) {
  auto lower = [](std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
  };
  struct Token {
    std::size_t start = 0;
    std::wstring text;
  };
  std::vector<Token> tokens;
  for (std::size_t pos = 0; pos < query.size();) {
    while (pos < query.size() && std::iswspace(query[pos])) ++pos;
    const auto start = pos;
    while (pos < query.size() && !std::iswspace(query[pos])) ++pos;
    if (pos > start) tokens.push_back({start, lower(query.substr(start, pos - start))});
  }
  if (tokens.empty() || tokens.front().text != L"timer") return std::nullopt;

  Request request{Action::Invalid};
  constexpr long long kLimit = std::numeric_limits<long long>::max() / 10000;
  long long total = 0;
  auto add = [&](long long value, long long factor) {
    if (value > (kLimit - total) / factor) return false;
    total += value * factor;
    return true;
  };
  bool sawDuration = false;
  std::size_t index = 1;
  for (; index < tokens.size(); ++index) {
    const std::wstring& text = tokens[index].text;
    // Split the token into <digits><letters> pairs ("1h30m"). A token of bare
    // digits ("10") takes its unit from the next token, if there is one.
    long long tokenTotal = 0;
    long long bareValue = -1;
    bool valid = true;
    for (std::size_t pos = 0; valid && pos < text.size();) {
      if (text[pos] < L'0' || text[pos] > L'9') {
        valid = false;
        break;
      }
      const auto numberStart = pos;
      long long value = 0;
      while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
        const int digit = text[pos++] - L'0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) return request;
        value = value * 10 + digit;
      }
      const auto unitStart = pos;
      while (pos < text.size() && std::iswalpha(text[pos])) ++pos;
      if (unitStart == pos) {
        // Digits without letters are only valid as the whole token.
        valid = numberStart == 0 && pos == text.size();
        if (valid) bareValue = value;
        break;
      }
      const long long factor = UnitFactor(text.substr(unitStart, pos - unitStart));
      if (!factor || value > (kLimit - tokenTotal) / factor) {
        if (factor) return request;
        valid = false;
        break;
      }
      tokenTotal += value * factor;
    }
    if (!valid) break;
    if (bareValue < 0) {
      if (!add(tokenTotal, 1)) return request;
      sawDuration = true;
      continue;
    }
    if (index + 1 < tokens.size()) {
      if (const long long factor = UnitFactor(tokens[index + 1].text)) {
        if (!add(bareValue, factor)) return request;
        sawDuration = true;
        ++index;
        continue;
      }
    }
    // A number without a unit is minutes only as the whole duration;
    // after a duration it starts the name ("timer 10m 2 eggs").
    if (!sawDuration) {
      if (!add(bareValue, 60000)) return request;
      sawDuration = true;
      ++index;
    }
    break;
  }
  if (!sawDuration || total <= 0) return request;
  if (index < tokens.size()) {
    request.name = query.substr(tokens[index].start);
    while (!request.name.empty() && std::iswspace(request.name.back())) request.name.pop_back();
  }
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
