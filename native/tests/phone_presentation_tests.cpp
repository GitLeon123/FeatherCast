#include "phone_presentation.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace feathercast::phone_ui;

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

LinkFacts Running(std::size_t paired) {
  LinkFacts facts;
  facts.enabled = true;
  facts.running = true;
  facts.pairedCount = paired;
  facts.nowMs = 1'790'000'000'000;  // a realistic Unix-epoch clock in ms
  return facts;
}

void TestLinkStatusPhases() {
  LinkFacts off;
  assert(DescribeLink(off).text == L"Phone connection is off");
  assert(DescribeLink(off).tone == LinkTone::Neutral);

  LinkFacts failed;
  failed.enabled = true;
  assert(DescribeLink(failed).tone == LinkTone::Bad);
  assert(DescribeLink(failed).text.find(L"could not start") != std::wstring::npos);

  assert(DescribeLink(Running(0)).text == L"No phone paired yet");

  auto connected = Running(1);
  connected.connected = true;
  assert(DescribeLink(connected).tone == LinkTone::Good);
  assert(DescribeLink(connected).text == L"Connected");
  connected.battery = 82;
  connected.charging = true;
  assert(DescribeLink(connected).text == L"Connected  ·  Battery 82% (charging)");
  // A connected phone wins over an old disconnect.
  connected.lastSeenMs = connected.nowMs - 5'000;
  assert(DescribeLink(connected).tone == LinkTone::Good);

  assert(DescribeLink(Running(1)).text ==
         L"Waiting for your phone – open FeatherCast Phone on it");
  assert(DescribeLink(Running(3)).text ==
         L"Waiting for one of your 3 paired phones – open FeatherCast Phone on it");
}

void TestLinkStatusLastSeen() {
  auto lost = Running(1);
  lost.lastSeenMs = lost.nowMs - 4'000;
  lost.lastSeenName = L"Pixel";
  const auto recent = DescribeLink(lost);
  assert(recent.tone == LinkTone::Warning);
  assert(recent.text == L"Connection lost – waiting for your phone to reconnect");

  // With several phones the name says which one dropped.
  auto several = lost;
  several.pairedCount = 2;
  assert(DescribeLink(several).text == L"Connection lost – waiting for Pixel to reconnect");

  // After the reconnect window the status stops promising a reconnect.
  lost.lastSeenMs = lost.nowMs - kReconnectWindowMs;
  assert(DescribeLink(lost).tone == LinkTone::Neutral);
  assert(DescribeLink(lost).text ==
         L"Last connected 1 min ago – open FeatherCast Phone on it");
  several.lastSeenMs = several.nowMs - 3 * 3'600'000LL;
  assert(DescribeLink(several).text ==
         L"Last connected 3 hours ago (Pixel) – open FeatherCast Phone on it");

  // A clock that went backwards still reads as a fresh disconnect.
  lost.lastSeenMs = lost.nowMs + 50'000;
  assert(DescribeLink(lost).tone == LinkTone::Warning);
}

void TestFormatAgo() {
  assert(FormatAgo(-5) == L"just now");
  assert(FormatAgo(59'999) == L"just now");
  assert(FormatAgo(60'000) == L"1 min ago");
  assert(FormatAgo(59 * 60'000LL) == L"59 min ago");
  assert(FormatAgo(60 * 60'000LL) == L"1 hour ago");
  assert(FormatAgo(23 * 3'600'000LL) == L"23 hours ago");
  assert(FormatAgo(24 * 3'600'000LL) == L"1 day ago");
  assert(FormatAgo(72 * 3'600'000LL) == L"3 days ago");
}

void TestTransferTiming() {
  // Nothing for the first second.
  assert(!TransferRevealed(1'000, 1'999));
  assert(TransferRevealed(1'000, 2'000));

  // Fast transfers never get a percentage.
  assert(!TransferWantsPercent(0, 500, 400, 1'000));
  assert(!TransferWantsPercent(0, 2'000, 500, 1'000));  // projects 4 s
  // Slow ones get it as soon as the rate says ten seconds or more.
  assert(TransferWantsPercent(0, 2'000, 100, 1'000));   // projects 20 s
  // And always after ten seconds, unless the size is unknown.
  assert(TransferWantsPercent(0, 10'000, 999, 1'000));
  assert(!TransferWantsPercent(0, 60'000, 999, 0));
  // No bytes yet: wait for a rate (or the ten seconds).
  assert(!TransferWantsPercent(0, 3'000, 0, 1'000));
  // Done larger than total (bad report) does not divide by more than total.
  assert(!TransferWantsPercent(0, 2'000, 5'000, 1'000));

  assert(TransferPercent(0, 0) == -1);
  assert(TransferPercent(50, 200) == 25);
  assert(TransferPercent(-5, 200) == 0);
  assert(TransferPercent(500, 200) == 100);
}

void TestByteAmounts() {
  assert(FormatBytes(0) == L"0 B");
  assert(FormatBytes(-3) == L"0 B");
  assert(FormatBytes(1023) == L"1023 B");
  assert(FormatBytes(1024) == L"1.0 KB");
  assert(FormatBytes(1536) == L"1.5 KB");
  assert(FormatBytes(10 * 1024 - 1) == L"9.9 KB");  // rounds down, never "10.0"
  assert(FormatBytes(820 * 1024) == L"820 KB");
  assert(FormatBytes(1024LL * 1024 - 1) == L"1023 KB");
  assert(FormatBytes(static_cast<long long>(4.5 * 1024 * 1024)) == L"4.5 MB");
  assert(FormatBytes(3LL * 1024 * 1024 * 1024) == L"3.0 GB");

  const long long mb = 1024LL * 1024;
  assert(TransferAmount(static_cast<long long>(1.25 * mb), static_cast<long long>(4.5 * mb)) ==
         L"1.2 of 4.5 MB");
  assert(TransferAmount(820 * 1024, static_cast<long long>(4.5 * mb)) == L"820 KB of 4.5 MB");
  assert(TransferAmount(static_cast<long long>(2.5 * mb), 0) == L"2.5 MB");
  // A report past the size reads as complete, not more.
  assert(TransferAmount(9 * mb, 4 * mb) == L"4.0 of 4.0 MB");
}

void TestPixelSnapping() {
  // 100 %: the classic half-pixel inset of a one pixel line.
  auto s = SnapStroke(10.0f, 20.0f, 110.0f, 60.0f, 1.0f, 1.0f);
  assert(Near(s.left, 10.5f) && Near(s.top, 20.5f));
  assert(Near(s.right, 109.5f) && Near(s.bottom, 59.5f));
  assert(Near(s.width, 1.0f));

  // 150 %: a 1 DIP line becomes 2 whole device pixels (1.5 rounds to 2),
  // and every outer edge lands on a device pixel boundary.
  const auto onGrid = [](const SnappedStroke& stroke, float scale) {
    const auto whole = [&](float dip) { return Near(dip * scale, std::round(dip * scale)); };
    const float half = stroke.width / 2.0f;
    return whole(stroke.left - half) && whole(stroke.top - half) &&
           whole(stroke.right + half) && whole(stroke.bottom + half) && whole(stroke.width);
  };
  s = SnapStroke(10.0f, 20.0f, 110.0f, 60.0f, 1.0f, 1.5f);
  assert(onGrid(s, 1.5f));
  assert(Near(s.width * 1.5f, 2.0f));

  // 125 %: fractional input edges are pulled onto the pixel grid.
  s = SnapStroke(10.3f, 20.7f, 50.1f, 40.2f, 1.0f, 1.25f);
  assert(onGrid(s, 1.25f));
  assert(Near(s.width * 1.25f, 1.0f));

  // A two DIP focus ring at 175 % is a whole number of pixels too.
  s = SnapStroke(0.0f, 0.0f, 40.0f, 40.0f, 2.0f, 1.75f);
  assert(onGrid(s, 1.75f));

  // Degenerate rectangles keep room for the line.
  s = SnapStroke(5.0f, 5.0f, 5.0f, 5.0f, 1.0f, 1.0f);
  assert(s.right >= s.left && s.bottom >= s.top);

  // Header divider: one pixel row directly above the edge.
  assert(Near(SnapHairlineAbove(84.0f, 1.0f, 1.0f), 83.5f));
  assert(Near(SnapHairlineAbove(84.0f, 1.0f, 1.5f) * 1.5f, 125.0f));   // 126 px edge, 2 px line
  assert(Near(SnapHairlineAbove(84.0f, 1.0f, 1.25f) * 1.25f, 104.5f));  // 105 px edge, 1 px line
}

void TestScreenEmptyTitle() {
  assert(ScreenEmptyTitle(false, false, L"Ready to share") == L"Phone not connected");
  assert(ScreenEmptyTitle(false, true, L"Ready to share") == L"Phone not connected");
  assert(ScreenEmptyTitle(true, false, L"Ready to share") ==
         L"Screen sharing is off on your phone");
  assert(ScreenEmptyTitle(true, true, L"Screen sharing stopped") == L"Screen sharing stopped");
}

}  // namespace

int main() {
  TestLinkStatusPhases();
  TestLinkStatusLastSeen();
  TestFormatAgo();
  TestTransferTiming();
  TestByteAmounts();
  TestPixelSnapping();
  TestScreenEmptyTitle();
  std::puts("phone presentation tests passed");
  return 0;
}
