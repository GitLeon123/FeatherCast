#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace feathercast::motion {

inline constexpr double kMaxFrameDeltaSeconds = 0.05;

inline double ClampFrameDeltaSeconds(double deltaSeconds) {
  return std::clamp(deltaSeconds, 0.0, kMaxFrameDeltaSeconds);
}

inline double EaseOutCubic(double progress) {
  const double t = std::clamp(progress, 0.0, 1.0);
  const double remaining = 1.0 - t;
  return 1.0 - remaining * remaining * remaining;
}

inline double NormalizedProgress(double elapsedSeconds, double durationSeconds) {
  if (durationSeconds <= 0.0) return 1.0;
  return std::clamp(elapsedSeconds / durationSeconds, 0.0, 1.0);
}

inline std::int64_t DisplayFramePeriodQpc(std::int64_t qpcFrequency,
                                          std::uint32_t refreshRateHz) {
  if (qpcFrequency <= 0 || refreshRateHz == 0) return 1;
  const auto refresh = static_cast<std::int64_t>(refreshRateHz);
  return std::max<std::int64_t>(
      1, (qpcFrequency + refresh / 2) / refresh);
}

// Keeps one future deadline per display frame. A late frame is re-anchored to
// the next period instead of catching up with a burst of immediate wakeups.
class DisplayFrameClock {
 public:
  void Start(std::int64_t now, std::int64_t period) {
    period_ = std::max<std::int64_t>(1, period);
    nextDeadline_ = now + period_;
  }

  void Reset() {
    period_ = 0;
    nextDeadline_ = 0;
  }

  bool Active() const { return period_ > 0; }
  std::int64_t Period() const { return period_; }
  std::int64_t NextDeadline() const { return nextDeadline_; }

  void Advance(std::int64_t now) {
    if (!Active()) return;
    if (nextDeadline_ <= now) {
      nextDeadline_ = now + period_;
    } else {
      nextDeadline_ += period_;
    }
  }

 private:
  std::int64_t period_ = 0;
  std::int64_t nextDeadline_ = 0;
};

// Keeps the animation message queue coalesced. A queued frame represents the
// latest state, so older intermediate frames must never accumulate behind it.
class FrameRequestGate {
 public:
  bool TryQueue() {
    if (queued_) return false;
    queued_ = true;
    return true;
  }

  void Complete() { queued_ = false; }
  void Reset() { queued_ = false; }
  bool Queued() const { return queued_; }

 private:
  bool queued_ = false;
};

class ScalarAnimation {
 public:
  double Value() const { return value_; }
  double Target() const { return target_; }
  bool Active() const { return active_; }

  void Snap(double value) {
    value_ = value;
    start_ = value;
    target_ = value;
    elapsedSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    active_ = false;
  }

  void Retarget(double target, double durationSeconds, bool animate = true) {
    if (!animate || durationSeconds <= 0.0 ||
        std::abs(target - value_) <= 0.001) {
      Snap(target);
      return;
    }
    start_ = value_;
    target_ = target;
    elapsedSeconds_ = 0.0;
    durationSeconds_ = durationSeconds;
    active_ = true;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    elapsedSeconds_ += ClampFrameDeltaSeconds(deltaSeconds);
    const double progress = durationSeconds_ <= 0.0
                                ? 1.0
                                : elapsedSeconds_ / durationSeconds_;
    value_ = start_ + (target_ - start_) * EaseOutCubic(progress);
    if (progress >= 1.0 || std::abs(target_ - value_) <= 0.001) {
      Snap(target_);
    }
    return active_;
  }

 private:
  double value_ = 0.0;
  double start_ = 0.0;
  double target_ = 0.0;
  double elapsedSeconds_ = 0.0;
  double durationSeconds_ = 0.0;
  bool active_ = false;
};

// Velocity-carrying spring. Unlike ScalarAnimation, retargeting mid-flight keeps
// the current velocity, so an interrupted or reversed move continues smoothly
// instead of restarting from a standstill. Parameters follow Apple's
// designer-facing pair: response (seconds to reach the target) and damping ratio
// (1.0 = critically damped / no overshoot, <1.0 = bounce).
class Spring {
 public:
  double Value() const { return value_; }
  double Velocity() const { return velocity_; }
  double Target() const { return target_; }
  bool Active() const { return active_; }

  void Configure(double responseSeconds, double dampingRatio) {
    responseSeconds_ = std::max(responseSeconds, 0.001);
    dampingRatio_ = std::clamp(dampingRatio, 0.1, 2.0);
  }

  void Snap(double value) {
    value_ = value;
    target_ = value;
    velocity_ = 0.0;
    active_ = false;
  }

  void Retarget(double target, bool animate = true) {
    if (!animate) {
      Snap(target);
      return;
    }
    target_ = target;
    active_ = std::abs(target_ - value_) > kEpsilon ||
              std::abs(velocity_) > kEpsilon;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    double remaining = ClampFrameDeltaSeconds(deltaSeconds);
    // Fixed sub-steps keep the integration stable when a frame runs long.
    constexpr double kStep = 1.0 / 240.0;
    const double omega = 6.283185307179586 / responseSeconds_;
    while (remaining > 0.0) {
      const double dt = std::min(kStep, remaining);
      remaining -= dt;
      const double acceleration = -2.0 * dampingRatio_ * omega * velocity_ -
                                  omega * omega * (value_ - target_);
      velocity_ += acceleration * dt;
      value_ += velocity_ * dt;
    }
    if (std::abs(target_ - value_) <= kEpsilon &&
        std::abs(velocity_) <= kEpsilon * 20.0) {
      Snap(target_);
    }
    return active_;
  }

 private:
  static constexpr double kEpsilon = 0.15;

  double value_ = 0.0;
  double target_ = 0.0;
  double velocity_ = 0.0;
  double responseSeconds_ = 0.3;
  double dampingRatio_ = 1.0;
  bool active_ = false;
};

class AnimationTimeline {
 public:
  double ElapsedSeconds() const { return elapsedSeconds_; }
  double DurationSeconds() const { return durationSeconds_; }
  bool Active() const { return active_; }

  void Start(double durationSeconds) {
    elapsedSeconds_ = 0.0;
    durationSeconds_ = std::max(0.0, durationSeconds);
    active_ = durationSeconds_ > 0.0;
  }

  void Reset() {
    elapsedSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    active_ = false;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    elapsedSeconds_ += ClampFrameDeltaSeconds(deltaSeconds);
    if (elapsedSeconds_ >= durationSeconds_) {
      elapsedSeconds_ = durationSeconds_;
      active_ = false;
    }
    return active_;
  }

 private:
  double elapsedSeconds_ = 0.0;
  double durationSeconds_ = 0.0;
  bool active_ = false;
};

struct AnimatedBounds {
  ScalarAnimation left;
  ScalarAnimation top;
  ScalarAnimation width;
  ScalarAnimation height;

  bool Active() const {
    return left.Active() || top.Active() || width.Active() || height.Active();
  }

  bool Update(double deltaSeconds) {
    left.Update(deltaSeconds);
    top.Update(deltaSeconds);
    width.Update(deltaSeconds);
    height.Update(deltaSeconds);
    return Active();
  }

  void Snap(double x, double y, double w, double h) {
    left.Snap(x);
    top.Snap(y);
    width.Snap(w);
    height.Snap(h);
  }

  void Retarget(double x, double y, double w, double h,
                double durationSeconds, bool animate) {
    left.Retarget(x, durationSeconds, animate);
    top.Retarget(y, durationSeconds, animate);
    width.Retarget(w, durationSeconds, animate);
    height.Retarget(h, durationSeconds, animate);
  }
};

struct Bounds {
  double left = 0.0;
  double top = 0.0;
  double width = 0.0;
  double height = 0.0;
};

inline Bounds OpeningBounds(const Bounds& target, double scale,
                            bool topAnchored) {
  const double clampedScale = std::clamp(scale, 0.01, 1.0);
  const double width = target.width * clampedScale;
  const double height = target.height * clampedScale;
  return {
      target.left + (target.width - width) * 0.5,
      topAnchored ? target.top
                  : target.top + (target.height - height) * 0.5,
      width,
      height,
  };
}

enum class AbsoluteNavigation { None, Home, End };

class PendingNavigation {
 public:
  bool Empty() const {
    return relative_ == 0 && absolute_ == AbsoluteNavigation::None;
  }

  void Clear() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::None;
  }

  void Move(int delta) {
    if (absolute_ != AbsoluteNavigation::None) absolute_ = AbsoluteNavigation::None;
    relative_ = std::clamp(relative_ + delta, -1000, 1000);
  }

  void Home() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::Home;
  }

  void End() {
    relative_ = 0;
    absolute_ = AbsoluteNavigation::End;
  }

  int Apply(int current, int itemCount) const {
    if (itemCount <= 0) return 0;
    if (absolute_ == AbsoluteNavigation::Home) return 0;
    if (absolute_ == AbsoluteNavigation::End) return itemCount - 1;
    return std::clamp(current + relative_, 0, itemCount - 1);
  }

 private:
  int relative_ = 0;
  AbsoluteNavigation absolute_ = AbsoluteNavigation::None;
};

inline double WheelDeltaPixels(int delta, double pixelsPerNotch = 72.0) {
  return static_cast<double>(delta) * pixelsPerNotch / 120.0;
}

}  // namespace feathercast::motion
