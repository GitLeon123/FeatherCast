#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace feathercast::performance {

// Quality is deliberately ordered from cheapest to most expensive.  The
// governor never changes the user's persisted animation preference; it only
// supplies a temporary ceiling while the process is under pressure.
enum class QualityTier : std::uint8_t {
  Critical = 0,
  Reduced = 1,
  Full = 2,
};

struct HardwareProfile {
  unsigned logicalProcessors = 1;
  std::uint64_t memoryBytes = 0;
  bool warpRendering = false;
};

struct WorkPolicy {
  QualityTier tier = QualityTier::Full;
  std::size_t eventBatch = 32;
  std::uint32_t animationIntervalMs = 16;
  std::size_t searchWorkers = 2;
  std::size_t pluginWorkers = 2;
  std::size_t iconPromotionsPerTick = 2;
  bool allowBlurDuringMotion = true;
  bool allowRichPreview = true;
  bool allowMaintenance = true;
  bool interactive = false;
};

class PerformanceGovernor {
 public:
  PerformanceGovernor() = default;

  void Initialize(HardwareProfile profile) noexcept {
    profile_ = profile;
    lowEnd_.store(IsConstrained(profile), std::memory_order_release);
    tier_.store((profile.warpRendering || IsConstrained(profile))
                    ? QualityTier::Reduced
                    : QualityTier::Full,
                std::memory_order_release);
    goodSamples_.store(0, std::memory_order_release);
    pressureSamples_.store(0, std::memory_order_release);
  }

  WorkPolicy Policy() const noexcept {
    const QualityTier tier = tier_.load(std::memory_order_acquire);
    const bool interactive = interactive_.load(std::memory_order_acquire);
    WorkPolicy policy;
    policy.tier = tier;
    policy.interactive = interactive;
    policy.eventBatch = tier == QualityTier::Critical ? 12 :
                        (tier == QualityTier::Reduced ? 24 : 40);
    policy.animationIntervalMs = tier == QualityTier::Critical ? 33 : 16;
    policy.searchWorkers = tier == QualityTier::Critical ? 1 :
                           (tier == QualityTier::Reduced ? 1 : 2);
    policy.pluginWorkers = tier == QualityTier::Full ? 2 : 1;
    policy.iconPromotionsPerTick = tier == QualityTier::Critical ? 1 :
                                   (tier == QualityTier::Reduced ? 2 : 4);
    policy.allowBlurDuringMotion = tier == QualityTier::Full;
    // Markdown/image previews are useful but expendable. Keep them out of the
    // constrained path so Reduced mode is input-first as soon as pressure is
    // detected, not only after the governor reaches Critical.
    policy.allowRichPreview = tier == QualityTier::Full;
    policy.allowMaintenance = tier == QualityTier::Full && !interactive;
    return policy;
  }

  QualityTier Tier() const noexcept {
    return tier_.load(std::memory_order_acquire);
  }

  bool IsLowEnd() const noexcept {
    return lowEnd_.load(std::memory_order_acquire);
  }

  void SetWarpRendering(bool warp) noexcept {
    if (!warp) return;
    lowEnd_.store(true, std::memory_order_release);
    QualityTier current = tier_.load(std::memory_order_acquire);
    while (current == QualityTier::Full &&
           !tier_.compare_exchange_weak(current, QualityTier::Reduced,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
    }
  }

  void SetInteractive(bool interactive) noexcept {
    interactive_.store(interactive, std::memory_order_release);
  }

  // Called once after an event-pump/render turn. Durations are in microseconds
  // so callers can use either QPC or steady_clock without a platform-specific
  // dependency in this policy object.
  QualityTier ObserveUiTurn(std::uint64_t frameMicros,
                            std::uint64_t pumpMicros,
                            std::size_t pendingEvents,
                            bool presentBackpressured = false) noexcept {
    const bool criticalPressure = frameMicros >= 33'000 ||
                                  pumpMicros >= 8'000 ||
                                  pendingEvents >= 192;
    const bool reducedPressure = frameMicros >= 20'000 ||
                                 pumpMicros >= 2'000 ||
                                 pendingEvents >= 64 ||
                                 presentBackpressured;
    QualityTier current = tier_.load(std::memory_order_acquire);
    if (criticalPressure) {
      pressureSamples_.store(0, std::memory_order_release);
      goodSamples_.store(0, std::memory_order_release);
      current = QualityTier::Critical;
      tier_.store(current, std::memory_order_release);
      return current;
    }
    if (reducedPressure) {
      pressureSamples_.fetch_add(1, std::memory_order_acq_rel);
      goodSamples_.store(0, std::memory_order_release);
      if (current == QualityTier::Full ||
          pressureSamples_.load(std::memory_order_acquire) >= 2) {
        current = QualityTier::Reduced;
        tier_.store(current, std::memory_order_release);
      }
      return current;
    }

    pressureSamples_.store(0, std::memory_order_release);
    const unsigned good = goodSamples_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const unsigned recoverySamples = current == QualityTier::Critical ? 30 : 60;
    if (good >= recoverySamples) {
      if (current == QualityTier::Critical) {
        current = QualityTier::Reduced;
      } else if (current == QualityTier::Reduced) {
        current = QualityTier::Full;
      }
      goodSamples_.store(0, std::memory_order_release);
      tier_.store(current, std::memory_order_release);
    }
    return current;
  }

  static bool IsConstrained(const HardwareProfile& profile) noexcept {
    constexpr std::uint64_t kEightGiB = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    return profile.logicalProcessors <= 4 ||
           (profile.memoryBytes != 0 && profile.memoryBytes <= kEightGiB);
  }

 private:
  HardwareProfile profile_{};
  std::atomic<QualityTier> tier_{QualityTier::Full};
  std::atomic<bool> lowEnd_{false};
  std::atomic<bool> interactive_{false};
  std::atomic<unsigned> goodSamples_{0};
  std::atomic<unsigned> pressureSamples_{0};
};

}  // namespace feathercast::performance
