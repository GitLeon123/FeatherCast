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
  assert(policy.allowRichPreview);
  assert(policy.markdownPrewarmLimit == 4);
  assert(policy.richPreviewDelayMs == 320);

  constrained.ObserveUiTurn(34'000, 0, 0);
  assert(constrained.Tier() == QualityTier::Critical);
  policy = constrained.Policy();
  assert(policy.searchWorkers == 1);
  assert(policy.eventBatch == 12);
  assert(!policy.allowBlurDuringMotion);
  assert(!policy.allowRichPreview);
  assert(policy.markdownPrewarmLimit == 0);
  assert(policy.richPreviewDelayMs == 800);
  for (int sample = 0; sample < 29; ++sample) {
    constrained.ObserveUiTurn(1'000, 500, 0);
    assert(constrained.Tier() == QualityTier::Critical);
  }
  constrained.ObserveUiTurn(1'000, 500, 0);
  assert(constrained.Tier() == QualityTier::Reduced);

  PerformanceGovernor warp;
  warp.Initialize({8, 16ULL * 1024ULL * 1024ULL * 1024ULL, false});
  warp.SetWarpRendering(true);
  assert(warp.Tier() == QualityTier::Reduced);
  assert(warp.Policy().searchWorkers == 1);
  assert(!warp.Policy().allowBlurDuringMotion);
  assert(warp.Policy().allowRichPreview);

  PerformanceGovernor desktop;
  desktop.Initialize({8, 16ULL * 1024ULL * 1024ULL * 1024ULL, false});
  assert(desktop.Tier() == QualityTier::Full);
  desktop.ObserveUiTurn(1'000, 1'000, 0, true);
  assert(desktop.Tier() == QualityTier::Reduced);
  policy = desktop.Policy();
  assert(policy.searchWorkers == 1);
  assert(policy.eventBatch == 24);
  assert(!policy.allowBlurDuringMotion);
  assert(policy.allowRichPreview);
  assert(policy.markdownPrewarmLimit == 4);
  assert(policy.richPreviewDelayMs == 320);
  for (int sample = 0; sample < 59; ++sample) {
    desktop.ObserveUiTurn(1'000, 500, 0);
    assert(desktop.Tier() == QualityTier::Reduced);
  }
  desktop.ObserveUiTurn(1'000, 500, 0);
  assert(desktop.Tier() == QualityTier::Full);
  policy = desktop.Policy();
  assert(policy.searchWorkers == 2);
  assert(policy.allowBlurDuringMotion);
  assert(policy.allowRichPreview);
  assert(policy.markdownPrewarmLimit == 16);
  assert(policy.richPreviewDelayMs == 120);

  desktop.SetInteractive(true);
  assert(!desktop.Policy().allowMaintenance);
  desktop.SetInteractive(false);
  assert(desktop.Policy().allowMaintenance);
  return 0;
}
