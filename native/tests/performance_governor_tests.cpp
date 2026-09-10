#include "performance_governor.hpp"
#include "test_framework.hpp"

int main() {
  using feathercast::performance::PerformanceGovernor;
  using feathercast::performance::QualityTier;

  constexpr std::uint64_t kEightGiB = 8ULL * 1024ULL * 1024ULL * 1024ULL;
  PerformanceGovernor constrained;
  constrained.Initialize({4, kEightGiB, false});
  assert(constrained.IsLowEnd());
  assert(constrained.Tier() == QualityTier::Reduced);
  auto policy = constrained.Policy();
  assert(policy.searchWorkers == 1);
  assert(policy.eventBatch == 24);
  assert(!policy.allowBlurDuringMotion);
  assert(!policy.allowRichPreview);

  constrained.ObserveUiTurn(34'000, 0, 0);
  assert(constrained.Tier() == QualityTier::Critical);
  for (int sample = 0; sample < 29; ++sample) {
    constrained.ObserveUiTurn(1'000, 500, 0);
    assert(constrained.Tier() == QualityTier::Critical);
  }
  constrained.ObserveUiTurn(1'000, 500, 0);
  assert(constrained.Tier() == QualityTier::Reduced);

  PerformanceGovernor desktop;
  desktop.Initialize({8, 16ULL * 1024ULL * 1024ULL * 1024ULL, false});
  assert(desktop.Tier() == QualityTier::Full);
  desktop.ObserveUiTurn(1'000, 1'000, 0, true);
  assert(desktop.Tier() == QualityTier::Reduced);
  for (int sample = 0; sample < 59; ++sample) {
    desktop.ObserveUiTurn(1'000, 500, 0);
    assert(desktop.Tier() == QualityTier::Reduced);
  }
  desktop.ObserveUiTurn(1'000, 500, 0);
  assert(desktop.Tier() == QualityTier::Full);
  policy = desktop.Policy();
  assert(policy.searchWorkers == 2);
  assert(policy.allowBlurDuringMotion);

  desktop.SetInteractive(true);
  assert(!desktop.Policy().allowMaintenance);
  desktop.SetInteractive(false);
  assert(desktop.Policy().allowMaintenance);
  return 0;
}
