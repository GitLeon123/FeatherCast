#include <windows.h>

#include "app_types.hpp"
#include "layout_contract.hpp"
#include "phone_store.hpp"
#include "search_pipeline.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace feathercast::phone;

namespace {

Event Connected(std::string deviceId, std::string name) {
  Event event;
  event.kind = EventKind::Connected;
  event.deviceId = std::move(deviceId);
  event.deviceName = std::move(name);
  return event;
}

Event Notification(std::string key, long long time, bool withIcon = false) {
  Event event;
  event.kind = EventKind::NotificationPosted;
  event.notification.key = key;
  event.notification.appName = "Messages";
  event.notification.title = "Title " + key;
  event.notification.text = "Body " + key;
  event.notification.time = time;
  if (withIcon) event.notification.iconPng = {1, 2, 3};
  return event;
}

Event Clip(std::string text) {
  Event event;
  event.kind = EventKind::Clipboard;
  event.text = std::move(text);
  return event;
}

Event PhotoList(std::vector<std::string> ids) {
  Event event;
  event.kind = EventKind::PhotoList;
  for (auto& id : ids) {
    PhotoInfo info;
    info.id = std::move(id);
    info.name = info.id + ".jpg";
    event.photos.push_back(std::move(info));
  }
  return event;
}

Event Thumb(std::string id) {
  Event event;
  event.kind = EventKind::PhotoThumb;
  event.photo.id = std::move(id);
  event.photo.thumbJpeg = {0xff, 0xd8, 0xff};
  return event;
}

Event Status(int battery, bool charging, std::vector<std::string> features = {}) {
  Event event;
  event.kind = EventKind::Status;
  event.battery = battery;
  event.charging = charging;
  event.features = std::move(features);
  return event;
}

void TestStatusAndBattery() {
  PhoneStore store;
  store.SetLowBatteryPercent(20);
  store.Apply(Connected("a", "Pixel"), 0);
  auto changes = store.Apply(Status(50, false, {"media", "sms"}), 0);
  assert(changes.status && !changes.lowBattery);
  assert(store.HasFeature("media") && store.HasFeature("sms") && !store.HasFeature("calls"));
  assert(store.Battery() == 50);
  // Fires once when crossing the threshold, not on every status after that.
  assert(store.Apply(Status(20, false), 0).lowBattery);
  assert(!store.Apply(Status(19, false), 0).lowBattery);
  // Charging re-arms it.
  assert(!store.Apply(Status(19, true), 0).lowBattery);
  assert(store.Apply(Status(18, false), 0).lowBattery);
  // Off when the threshold is 0.
  store.SetLowBatteryPercent(0);
  store.Apply(Status(90, false), 0);
  assert(!store.Apply(Status(5, false), 0).lowBattery);
}

void TestMediaRingAndCalls() {
  PhoneStore store;
  store.Apply(Connected("a", "Pixel"), 0);
  Event media;
  media.kind = EventKind::MediaState;
  media.media.active = true;
  media.media.title = "Song";
  media.media.playing = true;
  media.media.artJpeg = {1, 2, 3};
  auto changes = store.Apply(media, 1234);
  assert(changes.media && store.Media().active && store.Media().title == "Song");
  assert(store.MediaArt() && store.MediaArt()->size() == 3 && store.Media().artJpeg.empty());
  assert(store.MediaReceivedAt() == 1234);
  // An update without art keeps the previous cover; "nothing playing" drops it.
  media.media.artJpeg = {};
  media.media.playing = false;
  store.Apply(media, 0);
  assert(store.MediaArt() && !store.Media().playing);
  // A new track without art drops the old cover.
  media.media.title = "Next";
  store.Apply(media, 0);
  assert(!store.MediaArt());
  Event none;
  none.kind = EventKind::MediaState;
  store.Apply(none, 0);
  assert(!store.Media().active && !store.MediaArt());

  Event ring;
  ring.kind = EventKind::RingState;
  ring.ok = true;
  assert(store.Apply(ring, 0).ring && store.Ringing());

  Event call;
  call.kind = EventKind::Call;
  call.call.state = CallState::Ringing;
  call.call.name = "Anna";
  changes = store.Apply(call, 0);
  assert(changes.call && changes.incomingCall && store.Call().name == "Anna");
  assert(!store.Apply(call, 0).incomingCall);  // still the same call
  call.call.state = CallState::Active;
  assert(!store.Apply(call, 0).incomingCall);

  Event disconnected;
  disconnected.kind = EventKind::Disconnected;
  changes = store.Apply(disconnected, 0);
  assert(changes.call && changes.ring && !store.Ringing());
  assert(store.Call().state == CallState::Idle);
}

void TestSms() {
  PhoneStore store;
  store.Apply(Connected("a", "Pixel"), 0);
  Event threads;
  threads.kind = EventKind::SmsThreads;
  threads.smsThreads = {SmsThread{"1", "+111", "Anna", "old", 100, false},
                        SmsThread{"2", "+222", "Ben", "newer", 200, true}};
  auto changes = store.Apply(threads, 50);
  assert(changes.smsThreads && store.SmsThreads().size() == 2);
  assert(store.SmsThreads()[0].thread == "2");  // newest first
  assert(store.SmsFetchedAt() == 50);

  store.OpenSmsThread("2");
  assert(!store.FindSmsThread("2")->unread && !store.SmsMessagesLoaded());
  Event messages;
  messages.kind = EventKind::SmsMessages;
  messages.id = "1";  // a stale reply for another thread is ignored
  messages.smsMessages = {SmsMessage{"9", "ignored", 10, false}};
  assert(!store.Apply(messages, 0).smsMessages);
  messages.id = "2";
  messages.smsMessages = {SmsMessage{"5", "second", 300, true}, SmsMessage{"4", "first", 150, false}};
  changes = store.Apply(messages, 0);
  assert(changes.smsMessages && store.SmsMessagesLoaded());
  assert(store.SmsMessages().size() == 2 && store.SmsMessages()[0].message.body == "first");

  // Sent from the PC: shown at once, confirmed later, and kept across reloads.
  store.AddPendingSms("r1", "on my way", 400);
  assert(store.SmsMessages().back().pending && store.SmsThreads()[0].snippet == "on my way");
  changes = store.Apply(messages, 0);
  assert(store.SmsMessages().size() == 3 && store.SmsMessages().back().ref == "r1");
  Event sent;
  sent.kind = EventKind::SmsSent;
  sent.id = "r1";
  sent.ok = false;
  changes = store.Apply(sent, 0);
  assert(changes.smsMessages && changes.failedSmsRef == "r1");
  assert(store.SmsMessages().back().failed && !store.SmsMessages().back().pending);

  // Incoming message: thread moves to the top and the open chat grows.
  Event received;
  received.kind = EventKind::SmsReceived;
  received.smsThreads = {SmsThread{"1", "+111", "Anna", "hello again", 500, true}};
  changes = store.Apply(received, 0);
  assert(changes.smsThreads && !changes.smsMessages);
  assert(store.SmsThreads()[0].thread == "1" && store.SmsThreads()[0].unread);
  received.smsThreads = {SmsThread{"2", "+222", "Ben", "reply", 600, true}};
  changes = store.Apply(received, 0);
  assert(changes.smsMessages && store.SmsMessages().back().message.body == "reply");
  received.smsThreads = {SmsThread{"3", "+333", "", "new person", 700, true}};
  store.Apply(received, 0);
  assert(store.SmsThreads().size() == 3 && store.SmsThreads()[0].thread == "3");
}

void TestRemoteFiles() {
  PhoneStore store;
  store.Apply(Connected("a", "Pixel"), 0);
  assert(store.FilesPath() == "/");
  store.OpenFolder("/Download");
  assert(store.FilesLoading());
  Event list;
  list.kind = EventKind::RemoteFileList;
  list.remotePath = "/";  // late reply for the folder we left
  list.files = {RemoteFile{"x", false, 1, 0}};
  assert(!store.Apply(list, 0).files && store.FilesLoading());
  list.remotePath = "/Download";
  list.files = {RemoteFile{"b.txt", false, 1, 0}, RemoteFile{"Music", true, 0, 0},
                RemoteFile{"a.txt", false, 1, 0}};
  assert(store.Apply(list, 0).files && !store.FilesLoading());
  // Folders first, then by name.
  assert(store.Files()[0].name == "Music" && store.Files()[1].name == "a.txt");

  store.SetFileDownloading("/Download/a.txt", true);
  assert(store.FileDownloading("/Download/a.txt"));
  Event saved;
  saved.kind = EventKind::RemoteFileSaved;
  saved.remotePath = "/Download/a.txt";
  assert(store.Apply(saved, 0).files && !store.FileDownloading("/Download/a.txt"));

  list.text = "No access";
  list.files.clear();
  store.OpenFolder("/Download");
  store.Apply(list, 0);
  assert(store.FilesError() == "No access" && store.Files().empty());
}

void TestNotifications() {
  PhoneStore store;
  auto changes = store.Apply(Connected("a", "Pixel"), 0);
  assert(changes.connection && store.Connected() && store.DeviceName() == "Pixel");

  changes = store.Apply(Notification("n1", 100, true), 0);
  assert(changes.notifications);
  store.Apply(Notification("n2", 300), 0);
  store.Apply(Notification("n3", 200), 0);
  assert(store.Notifications().size() == 3);
  // Newest first by posting time.
  assert(store.Notifications()[0].info.key == "n2");
  assert(store.Notifications()[1].info.key == "n3");
  assert(store.Notifications()[2].info.key == "n1");
  // Icon bytes move out of the info into shared storage.
  assert(store.NotificationIcon("n1") && store.NotificationIcon("n1")->size() == 3);
  assert(store.Notifications()[2].info.iconPng.empty());
  assert(!store.NotificationIcon("n2"));

  // Re-posting a key replaces it instead of adding a duplicate.
  store.Apply(Notification("n1", 400), 0);
  assert(store.Notifications().size() == 3);
  assert(store.Notifications()[0].info.key == "n1");

  Event removed;
  removed.kind = EventKind::NotificationRemoved;
  removed.text = "n3";
  assert(store.Apply(removed, 0).notifications);
  assert(store.Notifications().size() == 2);
  assert(!store.Apply(removed, 0).notifications);

  assert(store.RemoveNotification("n2"));
  assert(!store.RemoveNotification("n2"));

  for (int i = 0; i < 130; ++i) {
    store.Apply(Notification("bulk" + std::to_string(i), 1000 + i, true), 0);
  }
  assert(store.Notifications().size() == PhoneStore::kMaxNotifications);
  std::size_t withIcons = 0;
  for (const auto& item : store.Notifications()) {
    if (item.icon) ++withIcons;
  }
  assert(withIcons == PhoneStore::kMaxNotificationIcons);

  Event reset;
  reset.kind = EventKind::NotificationsReset;
  assert(store.Apply(reset, 0).notifications);
  assert(store.Notifications().empty());
}

void TestClipsAndDeviceSwitch() {
  PhoneStore store;
  store.Apply(Connected("a", "Pixel"), 0);
  assert(store.Apply(Clip("hello"), 10).clips);
  assert(!store.Apply(Clip("hello"), 20).clips);
  assert(store.Apply(Clip("world"), 30).clips);
  assert(!store.Apply(Clip(""), 40).clips);
  assert(store.Clips().size() == 2);
  assert(store.Clips()[0].text == "world" && store.Clips()[0].time == 30);
  assert(store.Clips()[0].serial != store.Clips()[1].serial);
  for (int i = 0; i < 80; ++i) store.Apply(Clip("c" + std::to_string(i)), i);
  assert(store.Clips().size() == PhoneStore::kMaxClips);

  store.Apply(Notification("n", 1), 0);
  // Disconnecting keeps cached data but marks the store offline.
  Event disconnected;
  disconnected.kind = EventKind::Disconnected;
  store.Apply(disconnected, 0);
  assert(!store.Connected() && !store.Clips().empty());
  // The same phone reconnecting keeps its data...
  store.Apply(Connected("a", "Pixel"), 0);
  assert(!store.Clips().empty() && !store.Notifications().empty());
  // ...another phone clears it.
  const auto changes = store.Apply(Connected("b", "Galaxy"), 0);
  assert(changes.clips && changes.notifications && changes.photos);
  assert(store.Clips().empty() && store.Notifications().empty());
  assert(store.DeviceName() == "Galaxy");
}

void TestPhotos() {
  PhoneStore store;
  store.Apply(Connected("a", "Pixel"), 0);
  auto changes = store.Apply(PhotoList({"p1", "p2"}), 5000);
  assert(changes.photos && store.Photos().size() == 2 && store.PhotosFetchedAt() == 5000);

  changes = store.Apply(Thumb("p2"), 0);
  assert(changes.photos && changes.thumbId == "p2");
  assert(store.PhotoThumb("p2") && store.PhotoThumb("p2")->size() == 3);
  assert(store.Apply(Thumb("unknown"), 0).thumbId.empty());

  assert(store.SetDownloading("p2", true));
  assert(!store.SetDownloading("missing", true));
  // A refreshed list keeps thumbnails and download state of known photos.
  store.Apply(PhotoList({"p0", "p2"}), 9000);
  assert(store.Photos().size() == 2 && store.Photos()[1].info.id == "p2");
  assert(store.PhotoThumb("p2") && store.Photos()[1].downloading);
  assert(!store.PhotoThumb("p1"));

  Event saved;
  saved.kind = EventKind::PhotoSaved;
  saved.photo.id = "p2";
  changes = store.Apply(saved, 0);
  assert(changes.savedPhotoId == "p2" && !store.Photos()[1].downloading);

  store.SetDownloading("p0", true);
  Event error;
  error.kind = EventKind::Error;
  assert(store.Apply(error, 0).photos);
  assert(!store.Photos()[0].downloading);
}

void TestGridMath() {
  using feathercast::layout::FitResultGrid;
  using feathercast::layout::GridMove;
  const auto grid = FitResultGrid(600.0f, 112.0f, 8.0f);
  assert(grid.columns == 5);
  assert(grid.cell >= 112.0f);
  assert(std::abs(grid.cell * 5 + 8.0f * 4 - 600.0f) < 0.01f);
  assert(grid.Rows(0) == 0 && grid.Rows(5) == 1 && grid.Rows(6) == 2);
  assert(grid.RowOf(7) == 1 && grid.ColumnOf(7) == 2);
  assert(std::abs(grid.Height(6) - 2.0f * grid.Stride()) < 0.01f);
  assert(FitResultGrid(50.0f).columns == 1);

  // 7 items in 3 columns:  0 1 2 / 3 4 5 / 6
  assert(GridMove(0, 7, 3, 1, 0) == 1);
  assert(GridMove(0, 7, 3, -1, 0) == 0);
  assert(GridMove(6, 7, 3, 1, 0) == 6);
  assert(GridMove(1, 7, 3, 0, 1) == 4);
  assert(GridMove(4, 7, 3, 0, 1) == 6);  // lands on the shorter last row
  assert(GridMove(6, 7, 3, 0, 1) == 6);
  assert(GridMove(6, 7, 3, 0, -1) == 3);
  assert(GridMove(1, 7, 3, 0, -1) == 1);
  assert(GridMove(0, 0, 3, 1, 0) == 0);
}

feathercast::app::DisplayItem PhoneItem(feathercast::app::PhoneItemKind kind,
                                        std::wstring id, std::wstring title,
                                        std::wstring text,
                                        std::wstring appName = L"") {
  feathercast::app::DisplayItem item;
  item.isPhone = true;
  item.phone.kind = kind;
  item.phone.id = std::move(id);
  item.phone.title = std::move(title);
  item.phone.text = std::move(text);
  item.phone.appName = std::move(appName);
  return item;
}

void TestSearchPipeline() {
  using feathercast::app::BrowseView;
  using feathercast::app::PhoneItemKind;
  feathercast::app::QueryRequest request;
  request.limit = 20;
  request.browseView = BrowseView::PhoneNotifications;
  request.phoneSectionTitle = L"Phone Notifications · Pixel";
  request.phoneItems = {
      PhoneItem(PhoneItemKind::Notification, L"k1", L"Dinner tonight?", L"See you at 7", L"Messages"),
      PhoneItem(PhoneItemKind::Notification, L"k2", L"Build passed", L"main is green", L"GitHub"),
  };
  request.empty = true;
  auto results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.sections.size() == 1);
  assert(results.sections[0].title == L"Phone Notifications · Pixel");
  assert(results.flatItems.size() == 2);
  assert(results.flatItems[0].Key() == L"phone:n:k1");
  assert(results.flatItems[0].Name() == L"Dinner tonight?");

  request.empty = false;
  request.query = L"github";
  results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.size() == 1 && results.flatItems[0].phone.id == L"k2");

  request.query = L"dinner";
  results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.size() == 1 && results.flatItems[0].phone.id == L"k1");

  request.query = L"zzzz";
  results = feathercast::search_pipeline::ComputeResults(request);
  assert(results.flatItems.empty());

  // Icon keys only exist when image bytes are available.
  auto photo = PhoneItem(PhoneItemKind::Photo, L"p1", L"IMG_1.jpg", L"");
  assert(photo.IconKey().empty());
  photo.phone.hasImage = true;
  assert(photo.IconKey() == L"phone-thumb:p1");
  auto clip = PhoneItem(PhoneItemKind::Clip, L"3", L"hello", L"hello");
  assert(clip.Key() == L"phone:c:3");
}

void TestPhoneSuggestions() {
  using feathercast::app::CommandKind;
  using feathercast::search_pipeline::MatchesPhoneSuggestion;
  auto command = [](CommandKind kind, std::wstring id, std::wstring name,
                    std::vector<std::wstring> keywords) {
    feathercast::app::DisplayItem item;
    item.isCommand = true;
    item.command = kind;
    item.commandStableId = std::move(id);
    item.commandName = std::move(name);
    item.commandKeywords = std::move(keywords);
    return item;
  };
  const auto notifications = command(CommandKind::PhoneNotifications, L"phone-notifications",
                                     L"Phone Notifications", {L"notifications", L"alerts", L"messages"});
  const auto photos = command(CommandKind::PhonePhotos, L"phone-photos", L"Phone Photos",
                              {L"photos", L"pictures", L"gallery"});
  // Only a near-complete name or keyword suggests a phone view.
  assert(MatchesPhoneSuggestion(L"notificatio", notifications));
  assert(MatchesPhoneSuggestion(L"Notifications", notifications));
  assert(MatchesPhoneSuggestion(L"phone notificat", notifications));
  assert(MatchesPhoneSuggestion(L"messag", notifications));
  assert(MatchesPhoneSuggestion(L"phone photo", photos));
  assert(!MatchesPhoneSuggestion(L"notif", notifications));
  assert(!MatchesPhoneSuggestion(L"phone not", notifications));
  assert(!MatchesPhoneSuggestion(L"mess", notifications));
  assert(!MatchesPhoneSuggestion(L"phone", photos));
  assert(!MatchesPhoneSuggestion(L"no", notifications));
  assert(!MatchesPhoneSuggestion(L"notepad", notifications));
  assert(!MatchesPhoneSuggestion(L"phone xyz", notifications));
  assert(!MatchesPhoneSuggestion(L"", notifications));

  feathercast::app::QueryRequest request;
  request.limit = 20;
  request.query = L"notifications";
  request.empty = false;
  request.phoneSuggestions = {notifications, photos};
  auto results = feathercast::search_pipeline::ComputeResults(request);
  assert(!results.sections.empty() && results.sections[0].title == L"Phone");
  assert(results.sections[0].items.size() == 1);
  assert(results.sections[0].items[0].command == CommandKind::PhoneNotifications);

  // An app whose name starts with the query stays first.
  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  feathercast::app::DisplayItem app;
  app.app.id = L"photos-app";
  app.app.name = L"Photos";
  app.app.source = L"appx";
  snapshot->pool.push_back(app);
  feathercast::core::SearchItem searchItem;
  searchItem.id = app.Key();
  searchItem.name = L"Photos";
  searchItem.kind = L"app";
  searchItem.source = L"appx";
  snapshot->searchItems.push_back(feathercast::core::PrepareSearchItem(searchItem));
  request.snapshot = snapshot;
  request.query = L"photos";
  results = feathercast::search_pipeline::ComputeResults(request);
  bool phoneSection = false;
  for (const auto& section : results.sections) {
    if (section.title == L"Phone") {
      phoneSection = true;
      assert(section.items[0].command == CommandKind::PhonePhotos);
    }
  }
  assert(phoneSection);
  assert(results.sections[0].title != L"Phone");
}

}  // namespace

int main() {
  TestStatusAndBattery();
  TestMediaRingAndCalls();
  TestSms();
  TestRemoteFiles();
  TestNotifications();
  TestClipsAndDeviceSwitch();
  TestPhotos();
  TestGridMath();
  TestSearchPipeline();
  TestPhoneSuggestions();
  std::puts("phone store tests passed");
  return 0;
}
