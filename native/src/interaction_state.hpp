#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace feathercast::interaction {

enum class OverlayFocusPhase {
  Idle,
  Acquiring,
  Stabilizing,
  Closing,
};

class OverlayFocusSession {
 public:
  static constexpr std::uint64_t kGuardDurationMs = 300;

  std::uint64_t Begin(std::uint64_t now) {
    ++generation_;
    phase_ = OverlayFocusPhase::Acquiring;
    deadline_ = now + kGuardDurationMs;
    activationAttempts_ = 0;
    return generation_;
  }

  bool GuardActive(std::uint64_t now) const {
    return (phase_ == OverlayFocusPhase::Acquiring ||
            phase_ == OverlayFocusPhase::Stabilizing) &&
           now < deadline_;
  }

  bool AcceptsExternalCandidate(std::uint64_t now) const {
    return GuardActive(now);
  }

  void RecordActivationAttempt(bool activated) {
    ++activationAttempts_;
    if (activated && phase_ == OverlayFocusPhase::Acquiring) {
      phase_ = OverlayFocusPhase::Stabilizing;
    }
  }

  void Expire(std::uint64_t now) {
    if (!GuardActive(now) && phase_ != OverlayFocusPhase::Closing) {
      phase_ = OverlayFocusPhase::Idle;
    }
  }

  void BeginClosing() { phase_ = OverlayFocusPhase::Closing; }

  bool CanFinishClose(std::uint64_t generation) const {
    return phase_ == OverlayFocusPhase::Closing && generation == generation_;
  }

  void End() { phase_ = OverlayFocusPhase::Idle; }

  OverlayFocusPhase Phase() const { return phase_; }
  std::uint64_t Generation() const { return generation_; }
  std::uint64_t Deadline() const { return deadline_; }
  int ActivationAttempts() const { return activationAttempts_; }

 private:
  OverlayFocusPhase phase_ = OverlayFocusPhase::Idle;
  std::uint64_t generation_ = 0;
  std::uint64_t deadline_ = 0;
  int activationAttempts_ = 0;
};

struct OverlayRestoreIdentity {
  std::uintptr_t window = 0;
  std::uint32_t processId = 0;
  std::uint32_t threadId = 0;
  std::uint64_t generation = 0;
};

inline bool OverlayRestoreIdentityMatches(
    const OverlayRestoreIdentity& expected, std::uintptr_t currentWindow,
    std::uint32_t currentProcessId, std::uint32_t currentThreadId,
    std::uint64_t currentGeneration) {
  return expected.window != 0 && expected.window == currentWindow &&
         expected.processId == currentProcessId &&
         expected.threadId == currentThreadId &&
         expected.generation == currentGeneration;
}

inline bool ShouldAttemptOverlayRestore(bool explicitDismiss,
                                        bool overlayStillForeground,
                                        bool candidateValid) {
  return explicitDismiss && overlayStillForeground && candidateValid;
}

class SearchPresentationState {
 public:
  std::uint64_t NextRequest() { return ++requested_; }

  void Invalidate() { ++requested_; }

  void MarkCompleted(std::uint64_t generation) {
    completed_ = std::max(completed_, generation);
  }

  bool Publish(std::uint64_t generation) {
    MarkCompleted(generation);
    if (generation != requested_) return false;
    displayed_ = generation;
    return true;
  }

  bool Pending() const { return displayed_ != requested_; }

  bool CanActivate() const {
    return !Pending() && displayed_ == completed_;
  }

  std::uint64_t Requested() const { return requested_; }
  std::uint64_t Displayed() const { return displayed_; }
  std::uint64_t Completed() const { return completed_; }

 private:
  std::uint64_t requested_ = 0;
  std::uint64_t displayed_ = 0;
  std::uint64_t completed_ = 0;
};

// Hover may only change the selection after the pointer genuinely moves.
// Windows synthesizes WM_MOUSEMOVE when a window appears, resizes, or reflows
// under a resting cursor, and a nudge of a pixel or two is not intent either.
// The gate is armed at the cursor position on show, on keyboard input, and on
// result updates; it opens once the pointer leaves the slop square around that
// anchor and stays open until it is armed again.
class PointerHoverGate {
 public:
  static constexpr int kDefaultSlopPixels = 3;

  void Arm(int x, int y) {
    armed_ = true;
    anchorX_ = x;
    anchorY_ = y;
  }

  void Disarm() { armed_ = false; }

  bool Armed() const { return armed_; }

  // True when hover may act on the pointer at (x, y).
  bool Admit(int x, int y, int slopPixels = kDefaultSlopPixels) {
    if (!armed_) return true;
    const int slop = std::max(1, slopPixels);
    const int dx = x > anchorX_ ? x - anchorX_ : anchorX_ - x;
    const int dy = y > anchorY_ ? y - anchorY_ : anchorY_ - y;
    if (dx < slop && dy < slop) return false;
    armed_ = false;
    return true;
  }

 private:
  bool armed_ = false;
  int anchorX_ = 0;
  int anchorY_ = 0;
};

// A busy indicator that only appears once work has been pending for a while.
// Most searches publish within a frame or two; flashing "Searching..." for
// those reads as jitter, so the indicator waits out a short grace period while
// the previous results stay on screen.
class DelayedBusyIndicator {
 public:
  static constexpr std::uint64_t kDefaultDelayMs = 500;

  explicit DelayedBusyIndicator(std::uint64_t delayMs = kDefaultDelayMs)
      : delayMs_(delayMs) {}

  // Records a new request. A request that continues an already pending one
  // keeps the original start time, so steady typing against a slow search
  // still surfaces the indicator.
  void NoteRequest(bool alreadyPending, std::uint64_t now) {
    if (!alreadyPending || !started_) {
      since_ = now;
      started_ = true;
    }
  }

  bool Visible(bool pending, std::uint64_t now) const {
    return pending && started_ && now >= since_ && now - since_ >= delayMs_;
  }

  // Milliseconds until a still pending request should show the indicator.
  std::uint64_t RemainingMs(std::uint64_t now) const {
    if (!started_ || now < since_) return delayMs_;
    const std::uint64_t elapsed = now - since_;
    return elapsed >= delayMs_ ? 0 : delayMs_ - elapsed;
  }

  std::uint64_t DelayMs() const { return delayMs_; }

 private:
  std::uint64_t delayMs_ = kDefaultDelayMs;
  std::uint64_t since_ = 0;
  bool started_ = false;
};

// An undo offer for a routine action. It is only honored while the status
// message that advertises it is still the one on screen; typing, navigation,
// or any newer message silently withdraws it.
template <typename Snapshot>
class StatusUndoOffer {
 public:
  void Offer(std::wstring statusText, Snapshot snapshot) {
    statusText_ = std::move(statusText);
    snapshot_ = std::move(snapshot);
  }

  bool Available(const std::wstring* visibleStatus) const {
    return snapshot_.has_value() && visibleStatus &&
           *visibleStatus == statusText_;
  }

  // Returns the snapshot to restore when the offer is still visible. Any call
  // consumes the offer, so a stale offer can never be redeemed later.
  std::optional<Snapshot> Take(const std::wstring* visibleStatus) {
    const bool available = Available(visibleStatus);
    std::optional<Snapshot> snapshot = std::move(snapshot_);
    Clear();
    if (!available) return std::nullopt;
    return snapshot;
  }

  void Clear() {
    snapshot_.reset();
    statusText_.clear();
  }

 private:
  std::wstring statusText_;
  std::optional<Snapshot> snapshot_;
};

inline bool StablePointerTargetMatches(
    int expectedType, int expectedIndex, std::uint64_t expectedGeneration,
    std::wstring_view expectedKey, int currentType, int currentIndex,
    std::uint64_t currentGeneration, std::wstring_view currentKey,
    bool pointerInside, bool activationAllowed) {
  return pointerInside && activationAllowed && expectedType == currentType &&
         expectedIndex == currentIndex &&
         expectedGeneration == currentGeneration && expectedKey == currentKey;
}

}  // namespace feathercast::interaction
