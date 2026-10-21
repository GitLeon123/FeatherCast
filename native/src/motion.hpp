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

inline double EaseOutQuint(double progress) {
  const double remaining = 1.0 - std::clamp(progress, 0.0, 1.0);
  return 1.0 - remaining * remaining * remaining * remaining * remaining;
}

inline double EaseInCubic(double progress) {
  const double t = std::clamp(progress, 0.0, 1.0);
  return t * t * t;
}

inline double EaseInOutCubic(double progress) {
  const double t = std::clamp(progress, 0.0, 1.0);
  if (t < 0.5) return 4.0 * t * t * t;
  const double remaining = -2.0 * t + 2.0;
  return 1.0 - remaining * remaining * remaining * 0.5;
}

// CSS / Fluent style cubic-bezier timing function with P0 = (0, 0) and
// P3 = (1, 1). Control x values are clamped to [0, 1] so x(t) is monotonic
// and every progress value maps to exactly one curve parameter.
class CubicBezier {
 public:
  constexpr CubicBezier(double x1, double y1, double x2, double y2)
      : x1_(x1 < 0.0 ? 0.0 : (x1 > 1.0 ? 1.0 : x1)),
        y1_(y1),
        x2_(x2 < 0.0 ? 0.0 : (x2 > 1.0 ? 1.0 : x2)),
        y2_(y2) {}

  double operator()(double progress) const {
    const double x = std::clamp(progress, 0.0, 1.0);
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return Sample(y1_, y2_, SolveCurveX(x));
  }

 private:
  static double Sample(double p1, double p2, double t) {
    // Bernstein form of a 1D cubic with endpoints 0 and 1.
    const double u = 1.0 - t;
    return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
  }

  static double SampleDerivative(double p1, double p2, double t) {
    const double u = 1.0 - t;
    return 3.0 * u * u * p1 + 6.0 * u * t * (p2 - p1) +
           3.0 * t * t * (1.0 - p2);
  }

  double SolveCurveX(double x) const {
    // Newton-Raphson converges in a few steps for typical UI curves; fall
    // back to bisection where the slope flattens (e.g. x1 = 0 near t = 0).
    double t = x;
    for (int iteration = 0; iteration < 8; ++iteration) {
      const double error = Sample(x1_, x2_, t) - x;
      if (std::abs(error) < 1e-7) return t;
      const double slope = SampleDerivative(x1_, x2_, t);
      if (std::abs(slope) < 1e-6) break;
      t = std::clamp(t - error / slope, 0.0, 1.0);
    }
    double low = 0.0;
    double high = 1.0;
    t = x;
    for (int iteration = 0; iteration < 48; ++iteration) {
      const double value = Sample(x1_, x2_, t);
      if (std::abs(value - x) < 1e-7) break;
      if (value < x) {
        low = t;
      } else {
        high = t;
      }
      t = (low + high) * 0.5;
    }
    return t;
  }

  double x1_;
  double y1_;
  double x2_;
  double y2_;
};

// Fluent "fast out, slow in" decelerate curve: windows and panels arrive
// quickly and settle softly. Visually close to OutQuint, but exact.
inline constexpr CubicBezier kDecelerateCurve{0.0, 0.0, 0.0, 1.0};
// Fluent point-to-point curve for elements moving between two resting places.
inline constexpr CubicBezier kPointToPointCurve{0.55, 0.55, 0.0, 1.0};

// Entrances decelerate into place (Out*, Decelerate), exits accelerate away
// (In*). Decelerate and PointToPoint are the motion-token curves.
enum class Easing {
  OutCubic,
  OutQuint,
  InCubic,
  InOutCubic,
  Decelerate,
  PointToPoint,
};

inline double Ease(Easing easing, double progress) {
  switch (easing) {
    case Easing::OutQuint: return EaseOutQuint(progress);
    case Easing::InCubic: return EaseInCubic(progress);
    case Easing::InOutCubic: return EaseInOutCubic(progress);
    case Easing::Decelerate: return kDecelerateCurve(progress);
    case Easing::PointToPoint: return kPointToPointCurve(progress);
    case Easing::OutCubic: break;
  }
  return EaseOutCubic(progress);
}

// Motion tokens for the launcher and its panels. Keyboard-invoked surfaces
// keep fades near-instant; spatial settles stay short and nothing on a
// frequent path reaches the hard ceiling.
namespace tokens {
// Hard ceiling for any animation on a frequent path.
inline constexpr double kHardCeilingSeconds = 0.300;
// Opacity-only micro fade: hotkey show, newly arriving result rows.
inline constexpr double kMicroFadeSeconds = 0.083;
// Panel entrance: scale settles with the decelerate curve.
inline constexpr double kEntranceSeconds = 0.200;
inline constexpr double kEntranceScale = 0.96;
// Exits are opacity-led and always faster than the entrance. The launcher
// returns focus to the previous window only after its exit, so the exit is
// kept at the short end to never hold keystrokes back.
inline constexpr double kExitSeconds = 0.100;
inline constexpr double kExitScale = 0.98;
// Point-to-point movement such as rows sliding to their new position.
inline constexpr double kPointToPointSeconds = 0.140;
// Critically damped spring response for the selection pill. It reaches 95%
// of the distance in roughly 0.76 x response (~145 ms).
inline constexpr double kSelectionSpringResponseSeconds = 0.190;
inline constexpr double kSelectionPointerResponseSeconds = 0.120;
}  // namespace tokens

// Seconds between WM_KEYDOWN repeats for SPI_GETKEYBOARDSPEED (0 = ~2.5 Hz,
// 31 = ~30 Hz, linear in between as documented for the setting).
inline double KeyboardRepeatIntervalSeconds(unsigned speedSetting) {
  const double speed = static_cast<double>(std::min(speedSetting, 31u));
  const double hertz = 2.5 + speed * (27.5 / 31.0);
  return 1.0 / hertz;
}

// Detects a held navigation key from the cadence of its key-down messages.
// While the key repeats, the selection should snap to each row instead of
// gliding behind it; an isolated press still animates.
class KeyRepeatCadence {
 public:
  explicit KeyRepeatCadence(double thresholdSeconds = 0.090)
      : thresholdMs_(ThresholdMs(thresholdSeconds)) {}

  // Threshold that separates auto-repeat from deliberate taps: twice the
  // repeat interval, kept between 60 ms and 120 ms.
  static double ThresholdForRepeatInterval(double repeatIntervalSeconds) {
    return std::clamp(repeatIntervalSeconds * 2.0, 0.060, 0.120);
  }

  void SetThreshold(double thresholdSeconds) {
    thresholdMs_ = ThresholdMs(thresholdSeconds);
  }

  // `messageTimeMs` is a 32-bit millisecond tick such as GetMessageTime();
  // the difference is computed modulo 2^32 so tick wrap-around is harmless.
  bool Step(std::uint32_t messageTimeMs) {
    const bool repeating =
        hasPrevious_ &&
        static_cast<std::uint32_t>(messageTimeMs - previousMs_) <
            thresholdMs_;
    previousMs_ = messageTimeMs;
    hasPrevious_ = true;
    return repeating;
  }

  void Reset() { hasPrevious_ = false; }

 private:
  static std::uint32_t ThresholdMs(double seconds) {
    return static_cast<std::uint32_t>(
        std::lround(std::clamp(seconds, 0.0, 10.0) * 1000.0));
  }

  std::uint32_t thresholdMs_;
  std::uint32_t previousMs_ = 0;
  bool hasPrevious_ = false;
};

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

// Keeps one future deadline per display frame. Timer wake-up latency does not
// accumulate into every following frame, while genuinely missed frames are
// skipped instead of being replayed in a burst.
class DisplayFrameClock {
 public:
  void Start(std::int64_t now, std::int64_t period) {
    period_ = std::max<std::int64_t>(1, period);
    nextDeadline_ = now + period_;
  }

  // Phase-locks the deadlines to the compositor's vblank. Each tick then lands
  // `lead` before a vblank, so exactly one new frame is ready per refresh
  // instead of the timer phase drifting against the display and occasionally
  // producing two frames in one refresh and none in the next.
  void StartAligned(std::int64_t now, std::int64_t period,
                    std::int64_t vblank, std::int64_t lead) {
    period_ = std::max<std::int64_t>(1, period);
    lead = std::clamp<std::int64_t>(lead, 0, period_ / 2);
    const std::int64_t anchor = vblank - lead;
    std::int64_t offset = (now - anchor) % period_;
    if (offset < 0) offset += period_;
    nextDeadline_ = now - offset + period_;
  }

  void Reset() {
    period_ = 0;
    nextDeadline_ = 0;
  }

  bool Active() const { return period_ > 0; }
  std::int64_t Period() const { return period_; }
  std::int64_t NextDeadline() const { return nextDeadline_; }

  // Returns how many display periods passed, so callers can advance
  // animation time in whole refreshes rather than by jittery wall time.
  std::int64_t Advance(std::int64_t now) {
    if (!Active()) return 0;
    const std::int64_t elapsed = std::max<std::int64_t>(
        0, now - nextDeadline_);
    const std::int64_t periods = elapsed / period_ + 1;
    nextDeadline_ += periods * period_;
    return periods;
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

  void Retarget(double target, double durationSeconds, bool animate = true,
                Easing easing = Easing::OutCubic) {
    if (!animate || durationSeconds <= 0.0 ||
        std::abs(target - value_) <= 0.001) {
      Snap(target);
      return;
    }
    start_ = value_;
    target_ = target;
    elapsedSeconds_ = 0.0;
    durationSeconds_ = durationSeconds;
    easing_ = easing;
    active_ = true;
  }

  bool Update(double deltaSeconds) {
    if (!active_) return false;
    elapsedSeconds_ += ClampFrameDeltaSeconds(deltaSeconds);
    const double progress = durationSeconds_ <= 0.0
                                ? 1.0
                                : elapsedSeconds_ / durationSeconds_;
    value_ = start_ + (target_ - start_) * Ease(easing_, progress);
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
  Easing easing_ = Easing::OutCubic;
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

// Window geometry driven by springs: when the target changes mid-flight (for
// example the result count changing on every keystroke) the resize keeps its
// velocity instead of restarting from a standstill.
struct SpringBounds {
  Spring left;
  Spring top;
  Spring width;
  Spring height;

  void Configure(double responseSeconds, double dampingRatio) {
    left.Configure(responseSeconds, dampingRatio);
    top.Configure(responseSeconds, dampingRatio);
    width.Configure(responseSeconds, dampingRatio);
    height.Configure(responseSeconds, dampingRatio);
  }

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

  void Retarget(double x, double y, double w, double h, bool animate) {
    left.Retarget(x, animate);
    top.Retarget(y, animate);
    width.Retarget(w, animate);
    height.Retarget(h, animate);
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

  // A delta queued after Home/End is applied relative to that absolute
  // target, so "End, Up" lands on the second-to-last item.
  void Move(int delta) {
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
    int base = current;
    if (absolute_ == AbsoluteNavigation::Home) base = 0;
    if (absolute_ == AbsoluteNavigation::End) base = itemCount - 1;
    return std::clamp(base + relative_, 0, itemCount - 1);
  }

 private:
  int relative_ = 0;
  AbsoluteNavigation absolute_ = AbsoluteNavigation::None;
};

inline double WheelDeltaPixels(int delta, double pixelsPerNotch = 72.0) {
  return static_cast<double>(delta) * pixelsPerNotch / 120.0;
}

}  // namespace feathercast::motion
