#include "interaction_state.hpp"
#include "motion.hpp"
#include "test_framework.hpp"
#include "ui_state.hpp"

#include <string>

int main() {
  using feathercast::interaction::OverlayFocusPhase;
  using feathercast::interaction::OverlayFocusSession;
  using feathercast::interaction::OverlayRestoreIdentity;
  using feathercast::interaction::OverlayRestoreIdentityMatches;
  using feathercast::interaction::SearchPresentationState;
  using feathercast::interaction::ShouldAttemptOverlayRestore;
  using feathercast::interaction::StablePointerTargetMatches;

  {
    OverlayFocusSession focus;
    const auto first = focus.Begin(1000);
    assert(first == 1);
    assert(focus.Phase() == OverlayFocusPhase::Acquiring);
    assert(focus.GuardActive(1000));
    assert(focus.AcceptsExternalCandidate(1050));

    focus.RecordActivationAttempt(false);
    assert(focus.ActivationAttempts() == 1);
    assert(focus.Phase() == OverlayFocusPhase::Acquiring);
    focus.RecordActivationAttempt(true);
    assert(focus.ActivationAttempts() == 2);
    assert(focus.Phase() == OverlayFocusPhase::Stabilizing);
    assert(focus.AcceptsExternalCandidate(1150));
    assert(focus.AcceptsExternalCandidate(1250));
    assert(!focus.AcceptsExternalCandidate(1300));

    focus.Expire(1300);
    assert(focus.Phase() == OverlayFocusPhase::Idle);
    focus.BeginClosing();
    assert(focus.CanFinishClose(first));
    const auto second = focus.Begin(1400);
    assert(second == 2);
    assert(!focus.CanFinishClose(first));
    focus.BeginClosing();
    assert(focus.CanFinishClose(second));
    focus.End();
    assert(focus.Phase() == OverlayFocusPhase::Idle);
  }

  {
    const OverlayRestoreIdentity expected{0x1234, 10, 20, 7};
    assert(OverlayRestoreIdentityMatches(expected, 0x1234, 10, 20, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 11, 20, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 10, 21, 7));
    assert(!OverlayRestoreIdentityMatches(expected, 0x1234, 10, 20, 8));
    assert(!OverlayRestoreIdentityMatches(expected, 0, 10, 20, 7));

    assert(ShouldAttemptOverlayRestore(true, true, true));
    assert(!ShouldAttemptOverlayRestore(false, true, true));
    assert(!ShouldAttemptOverlayRestore(true, false, true));
    assert(!ShouldAttemptOverlayRestore(true, true, false));
  }

  {
    SearchPresentationState state;
    assert(!state.Pending());
    assert(state.CanActivate());

    const auto first = state.NextRequest();
    assert(first == 1);
    assert(state.Pending());
    assert(!state.CanActivate());

    state.MarkCompleted(first);
    assert(!state.CanActivate());
    assert(state.Publish(first));
    assert(!state.Pending());
    assert(state.CanActivate());

    const auto second = state.NextRequest();
    const auto third = state.NextRequest();
    assert(second == 2 && third == 3);
    state.MarkCompleted(second);
    assert(!state.Publish(second));
    assert(state.Pending());
    assert(!state.CanActivate());
    assert(state.Publish(third));
    assert(state.CanActivate());

    state.Invalidate();
    assert(state.Pending());
    assert(!state.CanActivate());
  }

  {
    feathercast::ui::OverlayState overlay;
    feathercast::ui::OverlayController::ArmSelectionRestoration(overlay);
    assert(feathercast::ui::OverlayController::CanRestoreSelection(overlay));
    feathercast::ui::OverlayController::ResetForShow(
        overlay, feathercast::app::View::Search);
    assert(!feathercast::ui::OverlayController::CanRestoreSelection(overlay));
    feathercast::ui::OverlayController::ArmSelectionRestoration(overlay);
    feathercast::ui::OverlayController::SetQuery(overlay, L"terminal");
    assert(!feathercast::ui::OverlayController::CanRestoreSelection(overlay));
  }

  {
    const std::wstring key = L"app:notepad";
    assert(StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, true,
                                      true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, false,
                                       true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 10, key, true,
                                       true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9,
                                       L"app:calculator", true, true));
    assert(!StablePointerTargetMatches(1, 4, 9, key, 1, 4, 9, key, true,
                                       false));
  }

  {
    feathercast::motion::ScalarAnimation animation;
    animation.Snap(10.0);
    animation.Retarget(20.0, 0.1);
    assert(animation.Active());
    animation.Update(0.05);
    assert(animation.Value() > 10.0 && animation.Value() < 20.0);

    const double interrupted = animation.Value();
    animation.Retarget(5.0, 0.1);
    assert(animation.Value() == interrupted);
    animation.Update(0.05);
    animation.Update(0.05);
    assert(!animation.Active());
    assert(std::abs(animation.Value() - 5.0) < 0.001);

    animation.Retarget(30.0, 1.0, false);
    assert(!animation.Active() && animation.Value() == 30.0);
  }

  {
    feathercast::motion::AnimationTimeline timeline;
    timeline.Start(0.21);
    assert(timeline.Active());
    timeline.Update(2.0);
    assert(timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds() -
                    feathercast::motion::kMaxFrameDeltaSeconds) < 0.001);

    timeline.Update(0.05);
    timeline.Start(0.21);
    assert(timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds()) < 0.001);

    timeline.Update(0.05);
    timeline.Update(0.05);
    timeline.Update(0.05);
    timeline.Update(0.05);
    assert(timeline.Active());
    timeline.Update(0.05);
    assert(!timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds() - 0.21) < 0.001);

    timeline.Reset();
    assert(!timeline.Active());
    assert(std::abs(timeline.ElapsedSeconds()) < 0.001);
  }

  {
    const double duration = 0.21;
    const double sharedProgress =
        feathercast::motion::NormalizedProgress(0.105, duration);
    // Every visible row uses this one reveal clock. There is deliberately no
    // row-index delay, so rows become available in the same frame.
    assert(std::abs(sharedProgress - 0.5) < 0.001);
    assert(std::abs(feathercast::motion::NormalizedProgress(0.35, duration) -
                    1.0) < 0.001);
  }

  {
    feathercast::motion::FrameRequestGate gate;
    assert(gate.TryQueue());
    assert(gate.Queued());
    assert(!gate.TryQueue());
    gate.Complete();
    assert(!gate.Queued());
    assert(gate.TryQueue());
    gate.Reset();
    assert(!gate.Queued());
  }

  {
    constexpr std::int64_t kQpcFrequency = 10'000'000;
    const auto period60 =
        feathercast::motion::DisplayFramePeriodQpc(kQpcFrequency, 60);
    const auto period120 =
        feathercast::motion::DisplayFramePeriodQpc(kQpcFrequency, 120);
    const auto period144 =
        feathercast::motion::DisplayFramePeriodQpc(kQpcFrequency, 144);
    assert(period60 == 166'667);
    assert(period120 == 83'333);
    assert(period144 == 69'444);
    assert(period60 > period120 && period120 > period144);

    feathercast::motion::DisplayFrameClock clock;
    clock.Start(100, period60);
    assert(clock.Active());
    assert(clock.Period() == period60);
    assert(clock.NextDeadline() == 100 + period60);

    clock.Advance(100 + period60);
    assert(clock.NextDeadline() == 100 + 2 * period60);
    clock.Advance(100 + 2 * period60 + 5);
    assert(clock.NextDeadline() == 100 + 3 * period60);
    clock.Advance(100 + 5 * period60 + 5);
    assert(clock.NextDeadline() == 100 + 6 * period60);

    // Small, normal timer lateness must not compound into a frame rate below
    // the display cadence.
    clock.Start(0, period120);
    for (int frame = 1; frame <= 120; ++frame) {
      clock.Advance(clock.NextDeadline() + 1'000);
    }
    assert(clock.NextDeadline() == 121 * period120);

    clock.Reset();
    assert(!clock.Active());
    assert(clock.Period() == 0);
    assert(clock.NextDeadline() == 0);
  }

  {
    feathercast::motion::AnimatedBounds bounds;
    bounds.Snap(100.0, 200.0, 400.0, 300.0);
    bounds.Retarget(90.0, 190.0, 420.0, 320.0, 0.16, true);
    bounds.Update(0.08);
    assert(bounds.left.Value() < 100.0 && bounds.left.Value() > 90.0);
    assert(bounds.width.Value() > 400.0 && bounds.width.Value() < 420.0);
    bounds.Update(0.05);
    bounds.Update(0.05);
    bounds.Update(0.05);
    assert(!bounds.Active());

    const feathercast::motion::Bounds target{100.0, 200.0, 400.0, 300.0};
    const auto centered =
        feathercast::motion::OpeningBounds(target, 0.98, false);
    assert(std::abs(centered.left - 104.0) < 0.001 &&
           std::abs(centered.top - 203.0) < 0.001);
    const auto topAnchored =
        feathercast::motion::OpeningBounds(target, 0.98, true);
    assert(std::abs(topAnchored.left - 104.0) < 0.001 &&
           std::abs(topAnchored.top - 200.0) < 0.001);
  }

  {
    using feathercast::motion::Easing;
    for (const Easing easing : {Easing::OutCubic, Easing::OutQuint,
                                Easing::InCubic, Easing::InOutCubic}) {
      assert(std::abs(feathercast::motion::Ease(easing, 0.0)) < 1e-9);
      assert(std::abs(feathercast::motion::Ease(easing, 1.0) - 1.0) < 1e-9);
      double previous = 0.0;
      for (int step = 1; step <= 20; ++step) {
        const double value =
            feathercast::motion::Ease(easing, static_cast<double>(step) / 20.0);
        assert(value >= previous);
        previous = value;
      }
    }
    // Exits accelerate: an ease-in covers less ground early than an ease-out.
    assert(feathercast::motion::Ease(Easing::InCubic, 0.25) <
           feathercast::motion::Ease(Easing::OutCubic, 0.25));

    feathercast::motion::ScalarAnimation closing;
    closing.Snap(1.0);
    closing.Retarget(0.0, 0.1, true, Easing::InCubic);
    closing.Update(0.025);
    assert(closing.Value() > 0.95);
    closing.Update(0.05);
    closing.Update(0.05);
    assert(!closing.Active() && closing.Value() == 0.0);
  }

  {
    constexpr std::int64_t kPeriod = 166'667;
    constexpr std::int64_t kVblank = 1'000'000;
    feathercast::motion::DisplayFrameClock clock;
    // Deadlines land mid-interval (half a period before each vblank),
    // whatever "now" is.
    clock.StartAligned(kVblank + 12'345, kPeriod, kVblank, kPeriod / 2);
    assert(clock.NextDeadline() > kVblank + 12'345);
    assert((clock.NextDeadline() - (kVblank - kPeriod / 2)) % kPeriod == 0);
    clock.StartAligned(kVblank - 3 * kPeriod + 7, kPeriod, kVblank,
                       kPeriod / 2);
    assert((clock.NextDeadline() - (kVblank - kPeriod / 2)) % kPeriod == 0);
    assert(clock.NextDeadline() - (kVblank - 3 * kPeriod + 7) <= kPeriod);

    // A missed frame skips ahead but keeps the vblank phase.
    const std::int64_t first = clock.NextDeadline();
    const std::int64_t periods = clock.Advance(first + 2 * kPeriod + 99);
    assert(periods == 3);
    assert(clock.NextDeadline() == first + 3 * kPeriod);
    assert(clock.Advance(clock.NextDeadline() + 10) == 1);
  }

  {
    feathercast::motion::SpringBounds bounds;
    bounds.Configure(0.2, 1.0);
    bounds.Snap(0.0, 0.0, 400.0, 300.0);
    bounds.Retarget(0.0, 0.0, 400.0, 500.0, true);
    for (int frame = 0; frame < 6; ++frame) bounds.Update(1.0 / 120.0);
    const double velocity = bounds.height.Velocity();
    assert(velocity > 0.0);
    // Retargeting mid-flight (another keystroke) keeps the momentum.
    bounds.Retarget(0.0, 0.0, 400.0, 520.0, true);
    assert(std::abs(bounds.height.Velocity() - velocity) < 1e-9);
    for (int frame = 0; frame < 240 && bounds.Active(); ++frame) {
      bounds.Update(1.0 / 120.0);
    }
    assert(!bounds.Active());
    assert(std::abs(bounds.height.Value() - 520.0) < 0.001);
    bounds.Retarget(10.0, 0.0, 400.0, 520.0, false);
    assert(!bounds.Active() && bounds.left.Value() == 10.0);
  }

  {
    feathercast::motion::PendingNavigation pending;
    pending.Move(1);
    pending.Move(8);
    assert(pending.Apply(0, 20) == 9);
    pending.Home();
    assert(pending.Apply(12, 20) == 0);
    pending.End();
    assert(pending.Apply(2, 20) == 19);
    pending.Clear();
    assert(pending.Empty());
    assert(feathercast::motion::WheelDeltaPixels(30) == 18.0);
  }

  {
    // A delta queued after an absolute jump applies relative to its target.
    feathercast::motion::PendingNavigation pending;
    pending.End();
    pending.Move(-1);
    assert(!pending.Empty());
    assert(pending.Apply(3, 20) == 18);
    pending.Clear();
    pending.Home();
    pending.Move(8);
    assert(pending.Apply(15, 20) == 8);
    pending.Move(100);
    assert(pending.Apply(0, 20) == 19);
    pending.Clear();
    pending.End();
    pending.Move(1);
    assert(pending.Apply(0, 5) == 4);
    // A later absolute jump discards earlier relative movement.
    pending.Move(-3);
    pending.Home();
    assert(pending.Apply(10, 20) == 0);
    pending.Move(-1);
    assert(pending.Apply(10, 20) == 0);
    assert(pending.Apply(10, 0) == 0);
  }

  {
    // Hover never beats the keyboard: a resting or barely nudged pointer is
    // ignored until it leaves the slop square around its anchor.
    feathercast::interaction::PointerHoverGate gate;
    assert(!gate.Armed());
    assert(gate.Admit(10, 10));
    gate.Arm(100, 200);
    assert(gate.Armed());
    assert(!gate.Admit(100, 200));
    assert(!gate.Admit(102, 198));
    assert(!gate.Admit(98, 202));
    assert(gate.Armed());
    assert(gate.Admit(103, 200));
    assert(!gate.Armed());
    // Once open, the gate stays open until it is armed again.
    assert(gate.Admit(100, 200));
    gate.Arm(50, 50);
    assert(!gate.Admit(50, 50, 8));
    assert(!gate.Admit(57, 43, 8));
    assert(gate.Admit(50, 58, 8));
    gate.Arm(0, 0);
    // A non-positive slop still requires an actual pixel of movement.
    assert(!gate.Admit(0, 0, 0));
    assert(gate.Admit(-1, 0, 0));
    gate.Arm(5, 5);
    gate.Disarm();
    assert(gate.Admit(5, 5));
  }

  {
    // "Searching..." only appears after the grace period, and a request that
    // continues pending work keeps the original start time.
    feathercast::interaction::DelayedBusyIndicator busy(500);
    assert(!busy.Visible(true, 0));
    assert(busy.RemainingMs(0) == 500);
    busy.NoteRequest(false, 1000);
    assert(!busy.Visible(true, 1000));
    assert(!busy.Visible(true, 1499));
    assert(busy.RemainingMs(1200) == 300);
    assert(busy.Visible(true, 1500));
    assert(busy.RemainingMs(1600) == 0);
    assert(!busy.Visible(false, 1600));
    busy.NoteRequest(true, 1700);
    assert(busy.Visible(true, 1700));
    // A fresh request after the previous one finished restarts the clock.
    busy.NoteRequest(false, 2000);
    assert(!busy.Visible(true, 2100));
    assert(busy.Visible(true, 2500));
    // A clock that appears to go backwards never shows the indicator early.
    assert(!busy.Visible(true, 1900));
    assert(busy.RemainingMs(1900) == 500);
    feathercast::interaction::DelayedBusyIndicator defaults;
    assert(defaults.DelayMs() ==
           feathercast::interaction::DelayedBusyIndicator::kDefaultDelayMs);
    defaults.NoteRequest(true, 10);
    assert(!defaults.Visible(true, 10));
  }

  {
    // An undo offer is only honored while its status message is on screen.
    feathercast::interaction::StatusUndoOffer<int> undo;
    const std::wstring hidden = L"Hidden from search. Press Ctrl+Z to undo.";
    const std::wstring other = L"Ranking reset for this item.";
    assert(!undo.Available(&hidden));
    assert(!undo.Take(&hidden));
    undo.Offer(hidden, 7);
    assert(undo.Available(&hidden));
    assert(!undo.Available(nullptr));
    assert(!undo.Available(&other));
    const auto taken = undo.Take(&hidden);
    assert(taken && *taken == 7);
    assert(!undo.Available(&hidden));
    assert(!undo.Take(&hidden));
    // A withdrawn offer cannot be redeemed when the same text returns.
    undo.Offer(hidden, 8);
    assert(!undo.Take(nullptr));
    assert(!undo.Take(&hidden));
    undo.Offer(hidden, 9);
    undo.Clear();
    assert(!undo.Available(&hidden));
  }

  {
    using feathercast::ui::EscapeContext;
    using feathercast::ui::EscapeStep;
    using feathercast::ui::NextEscapeStep;
    // Root search: clear typed text first, then hide.
    assert(NextEscapeStep({.queryEmpty = false}) == EscapeStep::ClearQuery);
    assert(NextEscapeStep({.queryEmpty = true}) == EscapeStep::Hide);
    // A scope prefix is peeled before the remaining terms.
    assert(NextEscapeStep({.scopedQuery = true, .queryEmpty = false}) ==
           EscapeStep::StripScope);
    // The preview panel closes before the query is touched.
    assert(NextEscapeStep({.previewOpen = true, .scopedQuery = true,
                           .queryEmpty = false}) == EscapeStep::ClosePreview);
    // A sub-view is the outermost layer: Esc goes back, keeping its text.
    assert(NextEscapeStep({.subView = true, .previewOpen = true,
                           .queryEmpty = false}) == EscapeStep::LeaveView);
    assert(NextEscapeStep({.subView = true}) == EscapeStep::LeaveView);
    // Outside the search view, typed text does not hold the launcher open.
    assert(NextEscapeStep({.queryEmpty = false, .searchView = false}) ==
           EscapeStep::Hide);

    feathercast::ui::OverlayState state;
    state.query = L"chr";
    state.caret = 3;
    state.selected = 4;
    // Esc clears the query as one undoable edit, so Ctrl+Z restores it.
    feathercast::ui::OverlayController::RecordUserEdit(state);
    const auto effects = feathercast::ui::OverlayController::SetQuery(state, L"");
    assert(feathercast::ui::HasEffect(effects,
                                      feathercast::ui::UiEffect::RequestSearch));
    assert(state.query.empty() && state.caret == 0 && state.selected == 0);
    feathercast::ui::OverlayController::UndoQueryEdit(state);
    assert(state.query == L"chr" && state.caret == 3);
  }

  return 0;
}
