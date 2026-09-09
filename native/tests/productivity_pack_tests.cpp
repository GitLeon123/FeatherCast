#include "clock_utilities.hpp"
#include "audio_volume.hpp"
#include "capture_service.hpp"
#include "command_catalog.hpp"
#include "core.hpp"
#include "network_client.hpp"
#include "search_pipeline.hpp"
#include "test_framework.hpp"
#include "system_settings.hpp"
#include "ui_state.hpp"
#include "window_layout.hpp"
#include "uuid_utilities.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using feathercast::window_layout::Layout;
using feathercast::window_layout::Rect;

bool HasAction(const std::vector<feathercast::app::DisplayItem>& actions,
               feathercast::app::ActionKind kind) {
  return std::any_of(actions.begin(), actions.end(), [&](const auto& item) {
    return item.isAction && item.action == kind;
  });
}

void AssertTextActions(const feathercast::app::DisplayItem& item,
                       const std::wstring& expectedValue) {
  const auto actions = feathercast::commands::BuildActions(
      item, feathercast::app::Settings{});
  assert(actions.size() == (item.isClipboard ? 3 : (item.isSnippet ? 4 : 2)));
  assert(actions[0].action == feathercast::app::ActionKind::CopyText);
  assert(actions[1].action == feathercast::app::ActionKind::PasteText);
  for (const auto& action : actions) {
    if (action.action == feathercast::app::ActionKind::PinClipboard) {
      assert(std::get<feathercast::app::ClipboardEntry>(action.actionTarget).text == expectedValue);
      continue;
    }
    if (action.action == feathercast::app::ActionKind::EditAlias ||
        action.action == feathercast::app::ActionKind::PinInvocation) {
      const auto* target =
          std::get_if<feathercast::app::AliasTarget>(&action.actionTarget);
      assert(target && target->invocationKey == item.InvocationKey());
      continue;
    }
    const auto* payload =
        std::get_if<feathercast::app::TextActionPayload>(&action.actionTarget);
    assert(payload && payload->value == expectedValue);
  }
}

}  // namespace

int main() {
  using feathercast::ui::CapturePhase;
  using feathercast::ui::CaptureShortcutTarget;
  using feathercast::ui::CaptureUiController;
  using feathercast::ui::CaptureUiState;
  using feathercast::ui::PixelPoint;
  using feathercast::ui::PixelRect;

  CaptureUiState capture;
  assert(CaptureUiController::Begin(
      capture, CaptureShortcutTarget::ScreenshotRegion));
  assert(capture.phase == CapturePhase::SelectingScreenshot);
  assert(!CaptureUiController::Begin(
      capture, CaptureShortcutTarget::RecordFullscreen));
  assert(CaptureUiController::BeginSelection(capture, PixelPoint{100, 80}));
  assert(CaptureUiController::UpdateSelection(capture, PixelPoint{-50, -20}));
  assert(CaptureUiController::SelectionRect(capture) ==
         (PixelRect{-50, -20, 100, 80}));
  assert(CaptureUiController::FinishSelection(capture, PixelPoint{-60, 90}));
  assert(capture.phase == CapturePhase::StartingScreenshot);
  assert(CaptureUiController::Complete(capture));
  assert(CaptureUiController::Complete(capture));
  assert(capture.phase == CapturePhase::Idle &&
         capture.target == CaptureShortcutTarget::None);

  assert(CaptureUiController::Begin(
      capture, CaptureShortcutTarget::RecordRegion));
  assert(CaptureUiController::CancelSelection(capture));
  assert(CaptureUiController::CancelSelection(capture));
  assert(capture.phase == CapturePhase::Idle);

  assert(CaptureUiController::Begin(
      capture, CaptureShortcutTarget::RecordFullscreen));
  assert(capture.phase == CapturePhase::StartingRecording);
  assert(!CaptureUiController::Pause(capture));
  assert(CaptureUiController::RecordingStarted(capture));
  assert(CaptureUiController::RecordingStarted(capture));
  assert(CaptureUiController::AddElapsed(capture, 1000));
  assert(CaptureUiController::Pause(capture));
  assert(CaptureUiController::Pause(capture));
  assert(!CaptureUiController::AddElapsed(capture, 500));
  assert(CaptureUiController::Resume(capture));
  assert(CaptureUiController::Resume(capture));
  assert(CaptureUiController::AddElapsed(capture, 250));
  assert(capture.elapsedMilliseconds == 1250);
  CaptureUiController::SetControlFocus(capture, 9);
  CaptureUiController::SetControlHover(capture, -9);
  assert(capture.controlFocus == 1 && capture.controlHover == -1);
  assert(CaptureUiController::Stop(capture));
  assert(CaptureUiController::Stop(capture));
  assert(capture.phase == CapturePhase::Stopping);
  assert(!CaptureUiController::Resume(capture));
  assert(CaptureUiController::Complete(capture));
  assert(capture.phase == CapturePhase::Idle &&
         capture.elapsedMilliseconds == 0);

  assert(CaptureUiController::Begin(
      capture, CaptureShortcutTarget::ScreenshotFullscreen));
  assert(!CaptureUiController::RecordingStarted(capture));
  assert(CaptureUiController::Fail(capture));
  assert(CaptureUiController::Fail(capture));
  assert(capture.phase == CapturePhase::Idle);

  using feathercast::capture::CaptureOperation;
  using CaptureRect = feathercast::capture::PixelRect;

  const CaptureRect normalized =
      feathercast::capture::NormalizeRect({100, 80, -50, -20});
  assert(normalized.left == -50 && normalized.top == -20 &&
         normalized.right == 100 && normalized.bottom == 80);

  const auto leftSlice = feathercast::capture::IntersectRects(
      {-2000, -100, 100, 1200}, {-1920, -200, 0, 1080});
  const auto rightSlice = feathercast::capture::IntersectRects(
      {-2000, -100, 100, 1200}, {0, 0, 1920, 1080});
  assert(leftSlice && leftSlice->left == -1920 && leftSlice->top == -100 &&
         leftSlice->right == 0 && leftSlice->bottom == 1080);
  assert(rightSlice && rightSlice->left == 0 && rightSlice->top == 0 &&
         rightSlice->right == 100 && rightSlice->bottom == 1080);
  assert(!feathercast::capture::IntersectRects(
      {1920, 0, 3000, 500}, {0, 0, 1920, 1080}));
  assert(!feathercast::capture::IntersectRects(
      {1920, 0, 3000, 500}, {1920, 500, 3000, 1580}));
  assert(!feathercast::capture::IntersectRects(
      {0, 0, 100, 100}, {100, 0, 200, 100}));

  assert((feathercast::capture::EvenRecordingSize({0, 0, 100, 100}) ==
          std::pair<std::uint32_t, std::uint32_t>{100, 100}));
  assert((feathercast::capture::EvenRecordingSize({101, 99, 0, 0}) ==
          std::pair<std::uint32_t, std::uint32_t>{102, 100}));
  assert(feathercast::capture::RecordingTimestamp(0) ==
         std::chrono::nanoseconds::zero());
  assert(feathercast::capture::RecordingTimestamp(30) ==
         std::chrono::seconds(1));
  assert(feathercast::capture::RecordingTimestamp(31) >
         feathercast::capture::RecordingTimestamp(30));
  assert(feathercast::capture::RecordingTimestamp(60) ==
         std::chrono::seconds(2));

  SYSTEMTIME captureTime{};
  captureTime.wYear = 2026;
  captureTime.wMonth = 7;
  captureTime.wDay = 29;
  captureTime.wHour = 14;
  captureTime.wMinute = 5;
  captureTime.wSecond = 6;
  const std::filesystem::path captureFolder = LR"(C:\Captures)";
  assert(feathercast::capture::CaptureOutputPath(
             captureFolder, CaptureOperation::Screenshot, captureTime)
             .filename() ==
         L"FeatherCast Screenshot 2026-07-29 14-05-06.png");
  assert(feathercast::capture::CaptureOutputPath(
             captureFolder, CaptureOperation::Screenshot, captureTime, 2)
             .filename() ==
         L"FeatherCast Screenshot 2026-07-29 14-05-06 (2).png");
  assert(feathercast::capture::CaptureOutputPath(
             captureFolder, CaptureOperation::Recording, captureTime)
             .filename() ==
         L"FeatherCast Recording 2026-07-29 14-05-06.mp4");
  assert(feathercast::capture::CaptureOutputPath(
             captureFolder, CaptureOperation::Recording, captureTime, 3, true)
             .filename() ==
         L"FeatherCast Recording 2026-07-29 14-05-06 (3).partial.mp4");

  feathercast::capture::CaptureService invalidCapture;
  assert(!invalidCapture.StartScreenshot({0, 0, 1, 100}));
  assert(!invalidCapture.StartRecording({0, 0, 47, 100}));
  assert(!invalidCapture.StartRecording({0, 0, 100, 47}));
  assert(!invalidCapture.StartRecording({0, 0, 4097, 100}));
  assert(!invalidCapture.StartRecording({0, 0, 100, 2305}));
  assert(invalidCapture.State() == feathercast::capture::CaptureState::Idle);

  assert(feathercast::audio::ClampPercent(-1) == 0);
  assert(feathercast::audio::ClampPercent(42) == 42);
  assert(feathercast::audio::ClampPercent(101) == 100);
  assert(feathercast::audio::AdjustPercent(0, -1) == 0);
  assert(feathercast::audio::AdjustPercent(50, -10) == 40);
  assert(feathercast::audio::AdjustPercent(50, 10) == 60);
  assert(feathercast::audio::AdjustPercent(100, 1) == 100);
  assert(feathercast::audio::PercentFromTrack(-10.0f, 0.0f, 200.0f) == 0);
  assert(feathercast::audio::PercentFromTrack(100.0f, 0.0f, 200.0f) == 50);
  assert(feathercast::audio::PercentFromTrack(210.0f, 0.0f, 200.0f) == 100);
  assert(feathercast::audio::PercentFromTrack(10.0f, 10.0f, 10.0f) == 0);

  const Rect work{-1920, 0, 1, 1081};
  const Rect window{-1700, 100, -700, 800};
  assert(feathercast::window_layout::Compute(Layout::LeftHalf, window, work,
                                              work) ==
         (Rect{-1920, 0, -960, 1081}));
  assert(feathercast::window_layout::Compute(Layout::RightHalf, window, work,
                                              work) ==
         (Rect{-960, 0, 1, 1081}));
  assert(feathercast::window_layout::Compute(Layout::TopHalf, window, work,
                                              work) ==
         (Rect{-1920, 0, 1, 540}));
  assert(feathercast::window_layout::Compute(Layout::BottomHalf, window, work,
                                              work) ==
         (Rect{-1920, 540, 1, 1081}));
  assert(feathercast::window_layout::Compute(Layout::LeftThird, window, work,
                                              work) ==
         (Rect{-1920, 0, -1280, 1081}));
  assert(feathercast::window_layout::Compute(Layout::CenterThird, window, work,
                                              work) ==
         (Rect{-1280, 0, -640, 1081}));
  assert(feathercast::window_layout::Compute(Layout::RightThird, window, work,
                                              work) ==
         (Rect{-640, 0, 1, 1081}));
  assert(feathercast::window_layout::Compute(Layout::TopLeft, window, work,
                                              work) ==
         (Rect{-1920, 0, -960, 540}));
  assert(feathercast::window_layout::Compute(Layout::BottomRight, window, work,
                                              work) ==
         (Rect{-960, 540, 1, 1081}));
  assert(feathercast::window_layout::Compute(
             Layout::Center, Rect{0, 0, 3000, 2000}, work, work) == work);

  const Rect nextWork{0, -200, 1280, 824};
  const Rect moved = feathercast::window_layout::Compute(
      Layout::NextDisplay, window, work, nextWork);
  assert(moved.Width() == window.Width());
  assert(moved.Height() == window.Height());
  assert(moved.left >= nextWork.left && moved.right <= nextWork.right);
  assert(moved.top >= nextWork.top && moved.bottom <= nextWork.bottom);
  assert(feathercast::window_layout::PositionLess(
      Rect{-1920, 0, 0, 1080}, Rect{0, -200, 1280, 824}));
  assert(feathercast::window_layout::NextIndex(0, 3) == 1);
  assert(feathercast::window_layout::NextIndex(2, 3) == 0);
  assert(feathercast::window_layout::NextIndex(0, 0) == 0);
  assert(feathercast::window_layout::PreviousIndex(0, 3) == 2);
  assert(feathercast::window_layout::PreviousIndex(2, 3) == 1);
  assert(feathercast::window_layout::PreviousIndex(0, 0) == 0);
  assert(feathercast::window_layout::Compute(
             Layout::NextDisplay, Rect{200, 200, 400, 400},
             Rect{0, 0, 1000, 1000}, Rect{1000, 100, 3000, 1100}) ==
         (Rect{1500, 300, 1700, 500}));
  assert(feathercast::network::IsSuccessfulStatusQuery(true, 200));
  assert(feathercast::network::IsSuccessfulStatusQuery(true, 299));
  assert(!feathercast::network::IsSuccessfulStatusQuery(true, 300));
  assert(!feathercast::network::IsSuccessfulStatusQuery(true, 404));
  assert(!feathercast::network::IsSuccessfulStatusQuery(false, 204));

  feathercast::clock_utilities::ClockSnapshot clock;
  clock.localDate = std::chrono::year{2026} / std::chrono::July / 17;
  clock.localHour = 9;
  clock.localMinute = 5;
  clock.localSecond = 7;
  clock.epochSeconds = 1784288707;
  assert(feathercast::clock_utilities::Evaluate(L" time ", clock)->value ==
         L"09:05:07");
  assert(feathercast::clock_utilities::Evaluate(L"today", clock)->value ==
         L"2026-07-17");
  assert(feathercast::clock_utilities::Evaluate(L"week number", clock)->value ==
         L"2026-W29");
  assert(feathercast::clock_utilities::Evaluate(L"unix timestamp", clock)->value ==
         L"1784288707");
  assert(!feathercast::clock_utilities::Evaluate(L"not a utility", clock));

  GUID knownGuid{0x00112233, 0x4455, 0x4677,
                 {0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
  assert(feathercast::uuid_utilities::Format(knownGuid) ==
         L"00112233-4455-4677-8899-aabbccddeeff");

  clock.localDate = std::chrono::year{2021} / std::chrono::January / 1;
  assert(feathercast::clock_utilities::Evaluate(L"iso week", clock)->value ==
         L"2020-W53");
  clock.localDate = std::chrono::year{2024} / std::chrono::February / 29;
  assert(feathercast::clock_utilities::Evaluate(L"date", clock)->value ==
         L"2024-02-29");

  const std::vector<std::tuple<feathercast::app::CommandKind, std::wstring,
                               std::wstring>> expectedCommands = {
      {feathercast::app::CommandKind::VolumeControl, L"volume-control",
       L"volume"},
      {feathercast::app::CommandKind::VolumeUp, L"volume-up", L"audio"},
      {feathercast::app::CommandKind::VolumeDown, L"volume-down", L"audio"},
      {feathercast::app::CommandKind::MediaPlayPause, L"media-play-pause",
       L"media"},
      {feathercast::app::CommandKind::MediaNext, L"media-next", L"next"},
      {feathercast::app::CommandKind::MediaPrevious, L"media-previous",
       L"previous"},
      {feathercast::app::CommandKind::ShowDesktop, L"show-desktop",
       L"desktop"},
      {feathercast::app::CommandKind::GenerateUuid, L"generate-uuid", L"uuid"},
  };
  for (const auto& [kind, stableId, keyword] : expectedCommands) {
    const auto* descriptor = feathercast::commands::Find(kind);
    assert(descriptor && descriptor->stableId == stableId);
    assert(!descriptor->confirmation);
    assert(std::find(descriptor->keywords.begin(), descriptor->keywords.end(),
                     keyword) != descriptor->keywords.end());
  }
  assert(feathercast::commands::Find(L"volume-up") ==
         feathercast::commands::Find(feathercast::app::CommandKind::VolumeUp));

  const std::map<std::wstring, std::wstring> commandAliases = {
      {L"volume-up", L"Louder Now"},
  };
  const auto aliasedCommands =
      feathercast::commands::BuildCommandItems(commandAliases);
  const auto aliasedVolume = std::find_if(
      aliasedCommands.begin(), aliasedCommands.end(), [](const auto& item) {
        return item.command == feathercast::app::CommandKind::VolumeUp;
      });
  assert(aliasedVolume != aliasedCommands.end());
  assert(aliasedVolume->commandStableId == L"volume-up");
  assert(aliasedVolume->InvocationKey() == L"command:volume-up");
  assert(std::find(aliasedVolume->commandKeywords.begin(),
                   aliasedVolume->commandKeywords.end(), L"Louder Now") !=
         aliasedVolume->commandKeywords.end());
  const auto aliasedVolumeSearch =
      feathercast::commands::BuildSearchItem(*aliasedVolume, commandAliases);
  assert(aliasedVolumeSearch.id == L"command:volume-up");
  assert(aliasedVolumeSearch.aliases ==
         std::vector<std::wstring>{L"Louder Now"});
  const auto preparedAliasedVolume =
      feathercast::core::PrepareSearchItem(aliasedVolumeSearch);
  assert(feathercast::core::ScorePreparedItem(
             L"louder now", feathercast::core::Tokens(L"louder now"),
             preparedAliasedVolume, {}) > 0.0);

  feathercast::app::Settings invocationSettings;
  invocationSettings.commandAliases = commandAliases;
  invocationSettings.pinnedItems = {L"command:volume-up"};
  const auto commandActions = feathercast::commands::BuildActions(
      *aliasedVolume, invocationSettings);
  assert(HasAction(commandActions, feathercast::app::ActionKind::EditAlias));
  assert(HasAction(commandActions,
                   feathercast::app::ActionKind::UnpinInvocation));
  const auto editAlias = std::find_if(
      commandActions.begin(), commandActions.end(), [](const auto& item) {
        return item.action == feathercast::app::ActionKind::EditAlias;
      });
  assert(editAlias != commandActions.end());
  const auto* aliasTarget = std::get_if<feathercast::app::AliasTarget>(
      &editAlias->actionTarget);
  assert(aliasTarget && aliasTarget->stableId == L"volume-up" &&
         aliasTarget->invocationKey == L"command:volume-up" &&
         aliasTarget->currentAlias == L"Louder Now");
  assert(feathercast::commands::IsPersonalizableInvocation(*aliasedVolume));
  assert(feathercast::commands::RecordsRecentActivation(*aliasedVolume));

  const auto settingsTargets = feathercast::system_settings::Catalog();
  assert(settingsTargets.size() == 5);
  std::set<std::wstring> settingsIds;
  std::vector<feathercast::core::SearchItem> settingsSearchItems;
  for (const auto& target : settingsTargets) {
    assert(target.id.starts_with(L"windows-settings:"));
    assert(settingsIds.insert(target.id).second);
    assert(target.source == L"windows-settings");
    assert(target.launchType == feathercast::app::LaunchType::Shell);
    assert(target.launchTarget.starts_with(L"ms-settings:"));
    assert(target.systemEssential);
    feathercast::core::SearchItem searchItem;
    searchItem.id = target.id;
    searchItem.name = target.name;
    searchItem.keywords = target.keywords;
    settingsSearchItems.push_back(std::move(searchItem));
  }
  for (const auto& [query, expectedId] :
       std::vector<std::pair<std::wstring, std::wstring>>{
           {L"display", L"windows-settings:display"},
           {L"sound", L"windows-settings:sound"},
           {L"bluetooth", L"windows-settings:bluetooth"},
           {L"installed apps", L"windows-settings:installed-apps"},
           {L"windows update", L"windows-settings:windows-update"},
       }) {
    const auto matches = feathercast::core::Search(query, settingsSearchItems);
    assert(!matches.empty());
    assert(settingsSearchItems[matches.front()].id == expectedId);
  }

  feathercast::app::DisplayItem windowItem;
  windowItem.isWindow = true;
  windowItem.window.hwnd = reinterpret_cast<HWND>(0x1234);
  windowItem.window.name = L"Test Window";
  const auto windowActions = feathercast::commands::BuildActions(
      windowItem, feathercast::app::Settings{});
  assert(HasAction(windowActions,
                   feathercast::app::ActionKind::ArrangeWindow));
  const auto arrange = std::find_if(
      windowActions.begin(), windowActions.end(), [](const auto& item) {
        return item.action == feathercast::app::ActionKind::ArrangeWindow;
      });
  assert(arrange != windowActions.end());
  const auto arrangeActions = feathercast::commands::BuildActions(
      *arrange, feathercast::app::Settings{});
  for (const auto kind : {
           feathercast::app::ActionKind::MoveWindowLeftHalf,
           feathercast::app::ActionKind::MoveWindowRightHalf,
           feathercast::app::ActionKind::MoveWindowTopHalf,
           feathercast::app::ActionKind::MoveWindowBottomHalf,
           feathercast::app::ActionKind::MoveWindowLeftThird,
           feathercast::app::ActionKind::MoveWindowCenterThird,
           feathercast::app::ActionKind::MoveWindowRightThird,
           feathercast::app::ActionKind::MoveWindowTopLeft,
           feathercast::app::ActionKind::MoveWindowTopRight,
           feathercast::app::ActionKind::MoveWindowBottomLeft,
           feathercast::app::ActionKind::MoveWindowBottomRight,
           feathercast::app::ActionKind::CenterWindow,
           feathercast::app::ActionKind::MoveWindowPreviousDisplay,
           feathercast::app::ActionKind::MoveWindowNextDisplay,
  }) {
    assert(HasAction(arrangeActions, kind));
  }
  for (const auto& action : arrangeActions) {
    const auto* target =
        std::get_if<feathercast::app::WindowEntry>(&action.actionTarget);
    assert(target && target->hwnd == windowItem.window.hwnd);
  }

  feathercast::app::DisplayItem appItem;
  appItem.app.id = L"app:test";
  appItem.app.name = L"Test App";
  appItem.app.source = L"start-menu";
  const auto appActions = feathercast::commands::BuildActions(
      appItem, feathercast::app::Settings{});
  assert(HasAction(appActions,
                   feathercast::app::ActionKind::EditAppAlias));

  feathercast::app::DisplayItem calculation;
  calculation.isCalculator = true;
  calculation.calculationExpression = L"6 * 7";
  calculation.calculationResult = L"42";
  const auto calculationActions = feathercast::commands::BuildActions(
      calculation, feathercast::app::Settings{});
  assert(calculationActions.size() == 3);
  assert(HasAction(calculationActions,
                   feathercast::app::ActionKind::CopyText));
  assert(HasAction(calculationActions,
                   feathercast::app::ActionKind::PasteText));
  const auto* complete = std::get_if<feathercast::app::TextActionPayload>(
      &calculationActions[1].actionTarget);
  assert(complete && complete->value == L"6 * 7 = 42");

  feathercast::app::DisplayItem conversion = calculation;
  conversion.isCalculator = false;
  conversion.isConversion = true;
  conversion.calculationExpression = L"1 km to m";
  conversion.calculationResult = L"1,000 m";
  const auto conversionActions = feathercast::commands::BuildActions(
      conversion, feathercast::app::Settings{});
  assert(conversionActions.size() == 3);
  const auto* conversionComplete =
      std::get_if<feathercast::app::TextActionPayload>(
          &conversionActions[1].actionTarget);
  assert(conversionComplete &&
         conversionComplete->value == L"1 km to m = 1,000 m");

  feathercast::app::DisplayItem snippet;
  snippet.isSnippet = true;
  snippet.snippet.keyword = L"reuse";
  snippet.snippet.name = L"Reusable text";
  snippet.snippet.text = L"Reusable text";
  AssertTextActions(snippet, snippet.snippet.text);
  assert(snippet.InvocationKey() == L"snippet:reuse");
  assert(feathercast::commands::RecordsRecentActivation(snippet));
  feathercast::app::DisplayItem quicklink;
  quicklink.app.id = L"quicklink:docs";
  quicklink.app.name = L"Documentation";
  quicklink.app.source = L"quicklink";
  const auto quicklinkActions = feathercast::commands::BuildActions(
      quicklink, feathercast::app::Settings{});
  assert(HasAction(quicklinkActions,
                   feathercast::app::ActionKind::EditAlias));
  assert(HasAction(quicklinkActions,
                   feathercast::app::ActionKind::PinInvocation));
  assert(quicklink.InvocationKey() == L"quicklink:docs");
  assert(feathercast::commands::RecordsRecentActivation(quicklink));
  feathercast::app::DisplayItem clipboard;
  clipboard.isClipboard = true;
  clipboard.clipboard.text = L"Clipboard text";
  AssertTextActions(clipboard, clipboard.clipboard.text);
  feathercast::app::DisplayItem symbol;
  symbol.isSymbol = true;
  symbol.symbol.value = L"\u221e";
  AssertTextActions(symbol, symbol.symbol.value);
  feathercast::app::DisplayItem utility;
  utility.utility = feathercast::app::UtilityResult{
      feathercast::app::UtilityKind::LocalTime, L"local-time", L"Local Time",
      L"12:34:56", {L"time"}};
  AssertTextActions(utility, utility.utility->value);

  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  feathercast::app::DisplayItem notepad;
  notepad.app.id = L"app:notepad";
  notepad.app.name = L"Notepad";
  notepad.app.source = L"shortcut";
  snapshot->pool.push_back(notepad);
  feathercast::core::SearchItem searchItem;
  searchItem.id = notepad.app.id;
  searchItem.name = notepad.app.name;
  searchItem.source = notepad.app.source;
  searchItem.kind = L"app";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(searchItem));

  feathercast::app::DisplayItem pinnedApp;
  pinnedApp.app.id = L"app:pinned";
  pinnedApp.app.name = L"Pinned Launcher";
  pinnedApp.app.source = L"shortcut";
  feathercast::app::DisplayItem recentApp;
  recentApp.app.id = L"app:recent";
  recentApp.app.name = L"Recent Launcher";
  recentApp.app.source = L"shortcut";
  feathercast::app::DisplayItem remainingApp;
  remainingApp.app.id = L"app:remaining";
  remainingApp.app.name = L"Remaining Launcher";
  remainingApp.app.source = L"shortcut";
  feathercast::app::DisplayItem indexedFile;
  indexedFile.app.id = L"file:indexed";
  indexedFile.app.name = L"Indexed file";
  indexedFile.app.source = L"file";
  feathercast::app::DisplayItem windowsSetting;
  windowsSetting.app.id = L"windows-settings:test";
  windowsSetting.app.name = L"Windows Settings";
  windowsSetting.app.source = L"windows-settings";
  snapshot->pinned = {*aliasedVolume, pinnedApp};
  snapshot->recent = {snippet, recentApp, *aliasedVolume, quicklink};
  snapshot->appItems = {
      pinnedApp, recentApp, remainingApp, indexedFile, windowsSetting,
      quicklink};

  feathercast::app::QueryRequest emptyRequest;
  emptyRequest.empty = true;
  emptyRequest.limit = 20;
  emptyRequest.snapshot = snapshot;
  const auto emptyResults =
      feathercast::search_pipeline::ComputeResults(emptyRequest);
  assert(!emptyResults.sections.empty());
  assert(emptyResults.sections[0].title == L"Favorites");
  assert(emptyResults.sections[0].items.size() == 2);
  assert(emptyResults.sections[0].items[0].commandStableId == L"volume-up");
  assert(emptyResults.sections[0].items[1].app.id == L"app:pinned");
  assert(emptyResults.sections[1].title == L"Recent");
  assert(emptyResults.sections[1].items.size() == 3);
  assert(emptyResults.sections[1].items[0].snippet.keyword == L"reuse");
  assert(emptyResults.sections[1].items[1].app.id == L"app:recent");
  assert(emptyResults.sections[1].items[2].app.id == L"quicklink:docs");
  assert(emptyResults.sections[2].title == L"Apps");
  assert(emptyResults.sections[2].items.size() == 1);
  assert(emptyResults.sections[2].items[0].app.id == L"app:remaining");
  assert(emptyResults.sections[3].title == L"More tools");
  assert(emptyResults.sections[3].items.size() == 1);
  assert(emptyResults.sections[3].items[0].isCapability);
  assert(emptyResults.sections[3].items[0].capability.stableId ==
         L"empty:more-tools");

  const auto hasSection = [](const auto& results, const std::wstring& title) {
    return std::any_of(results.sections.begin(), results.sections.end(),
                       [&](const auto& section) { return section.title == title; });
  };
  assert(hasSection(emptyResults, L"Favorites"));
  assert(hasSection(emptyResults, L"Recent"));
  assert(hasSection(emptyResults, L"More tools"));
  assert(!hasSection(emptyResults, L"Snippets"));
  assert(!hasSection(emptyResults, L"Commands"));

  feathercast::app::DisplayItem volumeCommand = *aliasedVolume;
  snapshot->pool.push_back(volumeCommand);
  const auto commandSearchItem = feathercast::commands::BuildSearchItem(
      volumeCommand, commandAliases);
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(commandSearchItem));

  feathercast::app::DisplayItem volumeControlCommand;
  volumeControlCommand.isCommand = true;
  volumeControlCommand.command = feathercast::app::CommandKind::VolumeControl;
  volumeControlCommand.commandName = L"Volume Control";
  snapshot->pool.push_back(volumeControlCommand);
  feathercast::core::SearchItem volumeControlSearchItem;
  volumeControlSearchItem.id = L"cmd:volume-control";
  volumeControlSearchItem.name = volumeControlCommand.commandName;
  volumeControlSearchItem.keywords = {L"volume", L"audio", L"speaker"};
  volumeControlSearchItem.source = L"command";
  volumeControlSearchItem.kind = L"command";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(volumeControlSearchItem));

  snapshot->pool.push_back(utility);
  feathercast::core::SearchItem utilitySearchItem;
  utilitySearchItem.id = utility.Key();
  utilitySearchItem.name = utility.Name();
  utilitySearchItem.keywords = utility.utility->keywords;
  utilitySearchItem.source = L"utility";
  utilitySearchItem.kind = L"utility";
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(utilitySearchItem));
  snapshot->pool.push_back(notepad);
  snapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(searchItem));

  auto typedSnapshot =
      std::make_shared<feathercast::app::SearchSnapshot>();
  const auto addSearchable =
      [&](feathercast::app::DisplayItem item, const std::wstring& kind,
          const std::wstring& source) {
        feathercast::core::SearchItem searchable;
        searchable.id = item.Key();
        searchable.name = item.Name();
        searchable.kind = kind;
        searchable.source = source;
        typedSnapshot->pool.push_back(std::move(item));
        typedSnapshot->searchItems.push_back(
            feathercast::core::PrepareSearchItem(searchable));
      };
  feathercast::app::DisplayItem updateApp;
  updateApp.app.id = L"app:update-tool";
  updateApp.app.name = L"Update Tool";
  updateApp.app.source = L"shortcut";
  addSearchable(updateApp, L"app", L"shortcut");
  feathercast::app::DisplayItem storeApp;
  storeApp.app.id = L"app:store-update-tool";
  storeApp.app.name = L"Store Update Tool";
  storeApp.app.source = L"appx";
  storeApp.app.launchType = feathercast::app::LaunchType::AppsFolder;
  addSearchable(storeApp, L"app", L"appx");
  feathercast::app::DisplayItem updateGame;
  updateGame.app.id = L"game:update";
  updateGame.app.name = L"Update Game";
  updateGame.app.source = L"game";
  updateGame.app.isGame = true;
  addSearchable(updateGame, L"game", L"game");
  feathercast::app::DisplayItem updateSetting;
  updateSetting.app.id = L"windows-settings:update";
  updateSetting.app.name = L"Windows Update";
  updateSetting.app.source = L"windows-settings";
  addSearchable(updateSetting, L"app", L"windows-settings");
  feathercast::app::DisplayItem updateCommand;
  updateCommand.isCommand = true;
  updateCommand.command = feathercast::app::CommandKind::VolumeUp;
  updateCommand.commandName = L"Update Command";
  addSearchable(updateCommand, L"command", L"command");

  feathercast::app::QueryRequest typedRequest;
  typedRequest.query = L"update";
  typedRequest.limit = 20;
  typedRequest.snapshot = typedSnapshot;
  const auto typedResults =
      feathercast::search_pipeline::ComputeResults(typedRequest);
  assert(!typedResults.sections.empty());
  assert(typedResults.sections.front().title == L"Apps");
  const auto appsSection = std::find_if(
      typedResults.sections.begin(), typedResults.sections.end(),
      [](const auto& section) { return section.title == L"Apps"; });
  const auto gamesSection = std::find_if(
      typedResults.sections.begin(), typedResults.sections.end(),
      [](const auto& section) { return section.title == L"Games"; });
  const auto settingsSection = std::find_if(
      typedResults.sections.begin(), typedResults.sections.end(),
      [](const auto& section) {
        return section.title == L"FeatherCast Settings";
      });
  assert(appsSection != typedResults.sections.end());
  assert(gamesSection != typedResults.sections.end());
  assert(settingsSection != typedResults.sections.end());
  assert(appsSection < gamesSection && gamesSection < settingsSection);
  assert(std::any_of(appsSection->items.begin(), appsSection->items.end(),
                     [](const auto& item) {
                       return item.app.id == L"app:store-update-tool";
                     }));
  assert(std::any_of(gamesSection->items.begin(), gamesSection->items.end(),
                     [](const auto& item) { return item.app.isGame; }));

  auto manyAppsSnapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  for (int index = 0; index < 7; ++index) {
    feathercast::app::DisplayItem item;
    item.app.id = L"app:launcher-" + std::to_wstring(index);
    item.app.name = L"Launcher " + std::to_wstring(index);
    item.app.source = L"shortcut";
    feathercast::core::SearchItem searchable;
    searchable.id = item.app.id;
    searchable.name = item.app.name;
    searchable.kind = L"app";
    searchable.source = L"shortcut";
    manyAppsSnapshot->pool.push_back(item);
    manyAppsSnapshot->searchItems.push_back(
        feathercast::core::PrepareSearchItem(searchable));
  }
  feathercast::app::QueryRequest collapsedRequest;
  collapsedRequest.query = L"launcher";
  collapsedRequest.limit = 20;
  collapsedRequest.snapshot = manyAppsSnapshot;
  const auto collapsedResults =
      feathercast::search_pipeline::ComputeResults(collapsedRequest);
  assert(!collapsedResults.sections.empty());
  assert(collapsedResults.sections.front().title == L"Apps");
  const auto collapsedAppsSection = std::find_if(
      collapsedResults.sections.begin(), collapsedResults.sections.end(),
      [](const auto& section) { return section.title == L"Apps"; });
  assert(collapsedAppsSection != collapsedResults.sections.end());
  assert(collapsedAppsSection->items.size() == 6);
  assert(collapsedAppsSection->items.back().isSectionExpander);
  assert(collapsedAppsSection->items.back().hiddenResultCount == 2);

  collapsedRequest.expandedSections.insert(L"Apps");
  const auto expandedResults =
      feathercast::search_pipeline::ComputeResults(collapsedRequest);
  const auto expandedAppsSection = std::find_if(
      expandedResults.sections.begin(), expandedResults.sections.end(),
      [](const auto& section) { return section.title == L"Apps"; });
  assert(expandedAppsSection != expandedResults.sections.end());
  assert(expandedAppsSection->items.size() == 7);
  assert(!expandedAppsSection->items.back().isSectionExpander);

  auto manyGamesSnapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  for (int index = 0; index < 7; ++index) {
    feathercast::app::DisplayItem item;
    item.app.id = L"game:launcher-" + std::to_wstring(index);
    item.app.name = L"Launcher Game " + std::to_wstring(index);
    item.app.source = L"game";
    item.app.isGame = true;
    manyGamesSnapshot->gameItems.push_back(item);
  }
  feathercast::app::QueryRequest gamesRequest;
  gamesRequest.empty = true;
  gamesRequest.snapshot = manyGamesSnapshot;
  gamesRequest.browseView = feathercast::app::BrowseView::Games;
  const auto browseGames =
      feathercast::search_pipeline::ComputeResults(gamesRequest);
  assert(browseGames.sections.size() == 1);
  assert(browseGames.sections.front().items.size() == 6);
  assert(browseGames.sections.front().items.back().isSectionExpander);
  gamesRequest.expandedSections.insert(L"Games");
  const auto expandedGames =
      feathercast::search_pipeline::ComputeResults(gamesRequest);
  assert(expandedGames.sections.front().items.size() == 7);
  assert(!expandedGames.sections.front().items.back().isSectionExpander);

  auto noAppSnapshot =
      std::make_shared<feathercast::app::SearchSnapshot>();
  feathercast::app::DisplayItem onlyCommand;
  onlyCommand.isCommand = true;
  onlyCommand.command = feathercast::app::CommandKind::VolumeDown;
  onlyCommand.commandName = L"Settings Command";
  feathercast::core::SearchItem onlyCommandSearch;
  onlyCommandSearch.id = onlyCommand.Key();
  onlyCommandSearch.name = onlyCommand.Name();
  onlyCommandSearch.kind = L"command";
  onlyCommandSearch.source = L"command";
  noAppSnapshot->pool.push_back(onlyCommand);
  noAppSnapshot->searchItems.push_back(
      feathercast::core::PrepareSearchItem(onlyCommandSearch));
  feathercast::app::QueryRequest noAppRequest;
  noAppRequest.query = L"settings";
  noAppRequest.limit = 20;
  noAppRequest.snapshot = noAppSnapshot;
  const auto noAppResults =
      feathercast::search_pipeline::ComputeResults(noAppRequest);
  assert(!noAppResults.sections.empty());
  assert(noAppResults.sections.front().title == L"Best match");
  assert(noAppResults.flatItems.front().isCommand);
  assert(!hasSection(noAppResults, L"Apps"));
  assert(!hasSection(noAppResults, L"Games"));

  feathercast::app::QueryRequest request;
  request.generation = 44;
  request.limit = 200;
  request.snapshot = snapshot;
  request.query = L"time";
  request.clock = clock;
  request.clock.localDate =
      std::chrono::year{2026} / std::chrono::December / 31;
  request.clock.localHour = 23;
  request.clock.localMinute = 59;
  request.clock.localSecond = 58;
  const auto utilityResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(utilityResults.generation == request.generation);
  assert(!utilityResults.sections.empty());
  assert(utilityResults.sections.front().title == L"Utilities");
  assert(utilityResults.flatItems.front().utility);
  assert(utilityResults.flatItems.front().utility->value == L"23:59:58");
  assert(std::count_if(utilityResults.flatItems.begin(),
                       utilityResults.flatItems.end(), [](const auto& item) {
                         return item.Key() == L"utility:local-time";
                       }) == 1);
  const auto repeated = feathercast::search_pipeline::ComputeResults(request);
  assert(repeated.flatItems.front().utility->value == L"23:59:58");

  for (const auto& [query, expected] :
       std::vector<std::pair<std::wstring, std::wstring>>{
           {L"date", L"2026-12-31"},
           {L"iso week", L"2026-W53"},
           {L"unix time", L"1784288707"},
       }) {
    request.query = query;
    const auto results = feathercast::search_pipeline::ComputeResults(request);
    assert(!results.flatItems.empty() && results.flatItems.front().utility);
    assert(results.flatItems.front().utility->value == expected);
  }

  request.query = L"notepad";
  const auto appResults = feathercast::search_pipeline::ComputeResults(request);
  assert(std::count_if(appResults.flatItems.begin(), appResults.flatItems.end(),
                       [](const auto& item) {
                         return item.app.id == L"app:notepad";
                       }) == 1);

  request.query = L"volume up";
  const auto commandResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!commandResults.flatItems.empty());
  assert(commandResults.flatItems.front().isCommand);
  assert(commandResults.flatItems.front().command ==
         feathercast::app::CommandKind::VolumeUp);

  request.query = L"volume control";
  const auto volumeControlResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!volumeControlResults.flatItems.empty());
  assert(volumeControlResults.flatItems.front().isCommand);
  assert(volumeControlResults.flatItems.front().command ==
         feathercast::app::CommandKind::VolumeControl);

  request.query = L"louder now";
  const auto rootAliasResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!rootAliasResults.flatItems.empty());
  assert(rootAliasResults.flatItems.front().commandStableId == L"volume-up");
  request.scope = feathercast::search_scope::Scope::Commands;
  const auto scopedAliasResults =
      feathercast::search_pipeline::ComputeResults(request);
  assert(!scopedAliasResults.flatItems.empty());
  assert(scopedAliasResults.flatItems.front().commandStableId == L"volume-up");
  request.scope = feathercast::search_scope::Scope::All;

  request.searchEngines = {
      {L"docs", L"https://example.com/search?q=%s"},
  };
  request.query = L"docs overlay crash";
  const auto webResults =
      feathercast::search_pipeline::ComputeResults(request);
  const auto webResult = std::find_if(
      webResults.flatItems.begin(), webResults.flatItems.end(),
      [](const auto& item) { return item.isWebSearch; });
  assert(webResult != webResults.flatItems.end());
  assert(webResult->webSearchUrl ==
         L"https://example.com/search?q=overlay+crash");

  std::atomic<unsigned long long> newest{request.generation + 1};
  request.query = L"notepad";
  request.latestGeneration = &newest;
  const auto staleResults = feathercast::search_pipeline::ComputeResults(request);
  assert(std::none_of(staleResults.flatItems.begin(), staleResults.flatItems.end(),
                      [](const auto& item) {
                        return item.app.id == L"app:notepad";
                      }));

  return 0;
}
