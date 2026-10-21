#pragma once

// Pure presentation rules for the FeatherCast Phone windows: the truthful link
// status line, when transfer progress is worth showing, byte amounts, and
// hairline pixel snapping. No Win32 or Direct2D here, so tests can run them.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <string>

namespace feathercast::phone_ui {

// ------------------------------------------------------------- link status

enum class LinkTone { Neutral, Good, Warning, Bad };

struct LinkFacts {
  bool enabled = false;
  bool running = false;
  std::size_t pairedCount = 0;
  bool connected = false;
  int battery = -1;  // -1: not reported yet
  bool charging = false;
  // When a phone last dropped its connection in this session; 0 = not this session.
  long long lastSeenMs = 0;
  std::wstring lastSeenName;  // that phone's name, if known
  long long nowMs = 0;
};

struct LinkStatus {
  LinkTone tone = LinkTone::Neutral;
  std::wstring text;
  bool operator==(const LinkStatus&) const = default;
};

// The phone app retries every 2 s and backs off to 30 s. Within this window a
// dropped link is most likely coming back; after it the phone is more likely
// asleep, out of range or closed, so the status stops promising a reconnect.
inline constexpr long long kReconnectWindowMs = 60'000;

// "just now", "5 min ago", "2 hours ago", "3 days ago".
inline std::wstring FormatAgo(long long elapsedMs) {
  const long long minutes = std::max(0LL, elapsedMs) / 60'000;
  if (minutes < 1) return L"just now";
  if (minutes < 60) return std::to_wstring(minutes) + L" min ago";
  const long long hours = minutes / 60;
  if (hours < 24) return std::to_wstring(hours) + (hours == 1 ? L" hour ago" : L" hours ago");
  const long long days = hours / 24;
  return std::to_wstring(days) + (days == 1 ? L" day ago" : L" days ago");
}

inline LinkStatus DescribeLink(const LinkFacts& facts) {
  if (!facts.enabled) return {LinkTone::Neutral, L"Phone connection is off"};
  if (!facts.running) {
    return {LinkTone::Bad, L"Not available – phone connection could not start"};
  }
  if (facts.connected) {
    std::wstring text = L"Connected";
    if (facts.battery >= 0) {
      text += L"  ·  Battery " + std::to_wstring(facts.battery) + L"%";
      if (facts.charging) text += L" (charging)";
    }
    return {LinkTone::Good, text};
  }
  if (facts.pairedCount == 0) return {LinkTone::Neutral, L"No phone paired yet"};
  const bool several = facts.pairedCount > 1;
  if (facts.lastSeenMs > 0) {
    const long long since = facts.nowMs - facts.lastSeenMs;
    const std::wstring who =
        several && !facts.lastSeenName.empty() ? facts.lastSeenName : std::wstring(L"your phone");
    if (since < kReconnectWindowMs) {
      return {LinkTone::Warning, L"Connection lost – waiting for " + who + L" to reconnect"};
    }
    std::wstring text = L"Last connected " + FormatAgo(since);
    if (several && !facts.lastSeenName.empty()) text += L" (" + facts.lastSeenName + L")";
    return {LinkTone::Neutral, text + L" – open FeatherCast Phone on it"};
  }
  if (!several) {
    return {LinkTone::Neutral, L"Waiting for your phone – open FeatherCast Phone on it"};
  }
  return {LinkTone::Neutral, L"Waiting for one of your " + std::to_wstring(facts.pairedCount) +
                                 L" paired phones – open FeatherCast Phone on it"};
}

// ---------------------------------------------------------------- transfers

// Progress only for work that lasts: nothing under a second (the action's own
// toast covers it), a percentage only for transfers that take ten seconds or more.
inline constexpr long long kTransferRevealMs = 1'000;
inline constexpr long long kTransferPercentMs = 10'000;

inline bool TransferRevealed(long long startedAtMs, long long nowMs) {
  return nowMs - startedAtMs >= kTransferRevealMs;
}

// True once a transfer has run ten seconds, or earlier when its rate so far
// says it will. Unknown sizes never get a percentage.
inline bool TransferWantsPercent(long long startedAtMs, long long nowMs, long long done,
                                 long long total) {
  if (total <= 0) return false;
  const long long elapsed = std::max(0LL, nowMs - startedAtMs);
  if (elapsed >= kTransferPercentMs) return true;
  if (elapsed < kTransferRevealMs || done <= 0) return false;
  const double projected = static_cast<double>(elapsed) * static_cast<double>(total) /
                           static_cast<double>(std::min(done, total));
  return projected >= static_cast<double>(kTransferPercentMs);
}

// 0..100, or -1 when the size is unknown.
inline int TransferPercent(long long done, long long total) {
  if (total <= 0) return -1;
  return static_cast<int>(std::clamp<long long>(std::max(0LL, done) * 100 / total, 0, 100));
}

struct ByteAmount {
  std::wstring number;
  const wchar_t* unit = L"B";
};

// Binary units like Explorer. Values round down, so progress never overstates.
inline ByteAmount SplitBytes(long long bytes) {
  bytes = std::max(0LL, bytes);
  if (bytes < 1024) return {std::to_wstring(bytes), L"B"};
  static constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  wchar_t buffer[32]{};
  if (value < 10.0) {
    std::swprintf(buffer, std::size(buffer), L"%.1f", std::floor(value * 10.0) / 10.0);
  } else {
    std::swprintf(buffer, std::size(buffer), L"%.0f", std::floor(value));
  }
  return {buffer, kUnits[unit]};
}

inline std::wstring FormatBytes(long long bytes) {
  const ByteAmount amount = SplitBytes(bytes);
  return amount.number + L" " + amount.unit;
}

// "1.2 of 4.5 MB", "820 KB of 4.5 MB", or just "1.2 MB" for an unknown size.
inline std::wstring TransferAmount(long long done, long long total) {
  if (total <= 0) return FormatBytes(done);
  const ByteAmount sent = SplitBytes(std::min(std::max(0LL, done), total));
  const ByteAmount size = SplitBytes(total);
  if (std::wcscmp(sent.unit, size.unit) == 0) {
    return sent.number + L" of " + size.number + L" " + size.unit;
  }
  return sent.number + L" " + sent.unit + L" of " + size.number + L" " + size.unit;
}

// ----------------------------------------------------------- pixel snapping

struct SnappedStroke {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  float width = 1.0f;  // DIPs
};

// Geometry for stroking the inside of a rectangle so the line covers whole
// device pixels at any scale: the outer edge lands on a pixel boundary and the
// width is a whole number of pixels (at least one). Inputs and outputs are DIPs.
inline SnappedStroke SnapStroke(float left, float top, float right, float bottom,
                                float widthDip, float scale) {
  scale = std::max(scale, 0.01f);
  const float pixels = std::max(1.0f, std::round(widthDip * scale));
  const float half = pixels / 2.0f;
  const float l = std::round(left * scale);
  const float t = std::round(top * scale);
  const float r = std::max(l + pixels, std::round(right * scale));
  const float b = std::max(t + pixels, std::round(bottom * scale));
  return {(l + half) / scale, (t + half) / scale, (r - half) / scale, (b - half) / scale,
          pixels / scale};
}

// Centre of a horizontal hairline whose bottom edge sits on `edgeDip`, snapped
// so it fills whole device pixel rows.
inline float SnapHairlineAbove(float edgeDip, float widthDip, float scale) {
  scale = std::max(scale, 0.01f);
  const float pixels = std::max(1.0f, std::round(widthDip * scale));
  return (std::round(edgeDip * scale) - pixels / 2.0f) / scale;
}

// ------------------------------------------------------------- phone screen

// Headline of the phone screen window while no frame is shown. It follows the
// link first, so it never reads "Ready to share" when sharing cannot start.
inline std::wstring ScreenEmptyTitle(bool connected, bool available, const std::wstring& phase) {
  if (!connected) return L"Phone not connected";
  if (!available) return L"Screen sharing is off on your phone";
  return phase;
}

}  // namespace feathercast::phone_ui
