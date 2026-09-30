#pragma once

// UI-thread model of the data received from the linked phone. The launcher
// overlay reads it to show notifications, photos, and clipboard entries as
// browse views. Pure (no Win32 or Direct2D) so it can be unit tested.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phone_service.hpp"

namespace feathercast::phone {

using SharedBytes = std::shared_ptr<const Bytes>;

struct StoredNotification {
  NotificationInfo info;  // iconPng is moved out into icon
  SharedBytes icon;
};

struct StoredPhoto {
  PhotoInfo info;  // thumbJpeg is moved out into thumb
  SharedBytes thumb;
  bool downloading = false;
};

struct StoredClip {
  std::uint64_t serial = 0;
  std::string text;
  long long time = 0;  // Unix milliseconds
};

struct StoredSms {
  SmsMessage message;
  std::string ref;       // set for messages sent from the PC
  bool pending = false;  // sent from the PC, not confirmed yet
  bool failed = false;
};

struct StoreChanges {
  bool connection = false;
  bool notifications = false;
  bool photos = false;
  bool clips = false;
  bool media = false;
  bool ring = false;
  bool smsThreads = false;
  bool smsMessages = false;
  bool call = false;
  bool files = false;
  bool status = false;        // battery, charging, or feature list
  bool lowBattery = false;    // the battery just dropped to the alert level
  bool incomingCall = false;  // a call just started ringing
  std::string failedSmsRef;   // an SMS sent from the PC failed
  std::string thumbId;        // photo whose thumbnail arrived
  std::string savedPhotoId;   // photo whose full image was saved
};

class PhoneStore {
 public:
  static constexpr std::size_t kMaxNotifications = 100;
  static constexpr std::size_t kMaxNotificationIcons = 30;
  static constexpr std::size_t kMaxClips = 50;
  static constexpr std::size_t kMaxSmsThreads = 200;

  StoreChanges Apply(const Event& event, long long nowMs) {
    StoreChanges changes;
    switch (event.kind) {
      case EventKind::Connected:
        if (!dataDeviceId_.empty() && event.deviceId != dataDeviceId_) {
          Clear();
          changes.notifications = changes.photos = changes.clips = true;
          changes.media = changes.smsThreads = changes.smsMessages = changes.files = true;
        }
        dataDeviceId_ = event.deviceId;
        deviceName_ = event.deviceName;
        connected_ = true;
        changes.connection = true;
        break;
      case EventKind::Disconnected:
        connected_ = false;
        for (auto& photo : photos_) {
          if (photo.downloading) changes.photos = true;
          photo.downloading = false;
        }
        changes.files = !downloadingFiles_.empty() || filesLoading_;
        downloadingFiles_.clear();
        filesLoading_ = false;
        changes.ring = ringing_;
        ringing_ = false;
        changes.call = call_.state != CallState::Idle;
        call_ = {};
        changes.connection = true;
        break;
      case EventKind::Status: {
        if (!event.deviceName.empty()) deviceName_ = event.deviceName;
        features_ = event.features;
        battery_ = event.battery;
        charging_ = event.charging;
        const bool low = lowBatteryPercent_ > 0 && battery_ >= 0 && !charging_ &&
                         battery_ <= lowBatteryPercent_;
        if (low && !lowBatteryAlerted_) changes.lowBattery = true;
        // Alert again only after the phone charged or recovered.
        if (low) lowBatteryAlerted_ = true;
        else if (charging_ || battery_ > lowBatteryPercent_) lowBatteryAlerted_ = false;
        changes.status = true;
        break;
      }
      case EventKind::NotificationPosted: {
        RemoveNotification(event.notification.key);
        StoredNotification item;
        item.info = event.notification;
        if (!item.info.iconPng.empty()) {
          item.icon = std::make_shared<const Bytes>(std::move(item.info.iconPng));
          item.info.iconPng = {};
        }
        notifications_.push_front(std::move(item));
        std::stable_sort(notifications_.begin(), notifications_.end(),
                         [](const auto& a, const auto& b) {
                           return a.info.time > b.info.time;
                         });
        while (notifications_.size() > kMaxNotifications) notifications_.pop_back();
        for (std::size_t i = kMaxNotificationIcons; i < notifications_.size(); ++i) {
          notifications_[i].icon.reset();
        }
        changes.notifications = true;
        break;
      }
      case EventKind::NotificationRemoved:
        changes.notifications = RemoveNotification(event.text);
        break;
      case EventKind::NotificationsReset:
        changes.notifications = !notifications_.empty();
        notifications_.clear();
        break;
      case EventKind::Clipboard:
        if (!event.text.empty() &&
            (clips_.empty() || clips_.front().text != event.text)) {
          clips_.push_front(StoredClip{++clipSerial_, event.text, nowMs});
          while (clips_.size() > kMaxClips) clips_.pop_back();
          changes.clips = true;
        }
        break;
      case EventKind::PhotoList: {
        std::vector<StoredPhoto> next;
        next.reserve(event.photos.size());
        for (const auto& info : event.photos) {
          StoredPhoto item;
          item.info = info;
          item.info.thumbJpeg = {};
          if (const auto* existing = FindPhoto(info.id)) {
            item.thumb = existing->thumb;
            item.downloading = existing->downloading;
          }
          next.push_back(std::move(item));
        }
        photos_ = std::move(next);
        photosFetchedAt_ = nowMs;
        changes.photos = true;
        break;
      }
      case EventKind::PhotoThumb:
        if (auto* photo = FindPhoto(event.photo.id);
            photo && !event.photo.thumbJpeg.empty()) {
          photo->thumb = std::make_shared<const Bytes>(event.photo.thumbJpeg);
          changes.photos = true;
          changes.thumbId = event.photo.id;
        }
        break;
      case EventKind::PhotoSaved:
        if (auto* photo = FindPhoto(event.photo.id)) {
          photo->downloading = false;
          changes.photos = true;
        }
        changes.savedPhotoId = event.photo.id;
        break;
      case EventKind::Error:
        for (auto& photo : photos_) {
          if (photo.downloading) changes.photos = true;
          photo.downloading = false;
        }
        if (!downloadingFiles_.empty()) changes.files = true;
        downloadingFiles_.clear();
        break;
      case EventKind::RingState:
        changes.ring = ringing_ != event.ok;
        ringing_ = event.ok;
        break;
      case EventKind::MediaState: {
        // Cover art is only sent when the track changes.
        const bool sameTrack = media_.active && event.media.title == media_.title &&
                               event.media.artist == media_.artist;
        media_ = event.media;
        media_.artJpeg = {};
        if (!event.media.artJpeg.empty()) {
          mediaArt_ = std::make_shared<const Bytes>(event.media.artJpeg);
        } else if (!sameTrack) {
          mediaArt_.reset();
        }
        mediaReceivedAt_ = nowMs;
        changes.media = true;
        break;
      }
      case EventKind::SmsThreads:
        smsThreads_ = event.smsThreads;
        SortThreads();
        smsFetchedAt_ = nowMs;
        changes.smsThreads = true;
        break;
      case EventKind::SmsMessages:
        if (event.id == smsThread_) {
          // Keep messages sent from this PC that the phone has not listed yet.
          std::vector<StoredSms> next;
          for (const auto& message : event.smsMessages) next.push_back(StoredSms{message});
          for (const auto& stored : smsMessages_) {
            if (stored.pending || stored.failed) next.push_back(stored);
          }
          smsMessages_ = std::move(next);
          SortMessages();
          smsMessagesLoaded_ = true;
          changes.smsMessages = true;
        }
        break;
      case EventKind::SmsReceived: {
        if (event.smsThreads.empty()) break;
        const SmsThread& incoming = event.smsThreads.front();
        auto it = std::find_if(smsThreads_.begin(), smsThreads_.end(),
                               [&](const auto& t) { return t.thread == incoming.thread; });
        if (it == smsThreads_.end()) {
          smsThreads_.push_back(incoming);
        } else {
          it->snippet = incoming.snippet;
          it->time = incoming.time;
          it->unread = true;
          if (!incoming.name.empty()) it->name = incoming.name;
        }
        SortThreads();
        changes.smsThreads = true;
        if (incoming.thread == smsThread_) {
          SmsMessage message;
          message.id = "in:" + std::to_string(incoming.time);
          message.body = incoming.snippet;
          message.time = incoming.time;
          smsMessages_.push_back(StoredSms{std::move(message)});
          SortMessages();
          changes.smsMessages = true;
        }
        break;
      }
      case EventKind::SmsSent:
        for (auto& stored : smsMessages_) {
          if (stored.ref != event.id) continue;
          stored.pending = false;
          stored.failed = !event.ok;
          changes.smsMessages = true;
        }
        if (!event.ok) changes.failedSmsRef = event.id;
        break;
      case EventKind::Call:
        changes.incomingCall = event.call.state == CallState::Ringing &&
                               call_.state != CallState::Ringing;
        call_ = event.call;
        changes.call = true;
        break;
      case EventKind::RemoteFileList:
        // Replies for folders the user already left are ignored.
        if (event.remotePath == filesPath_) {
          files_ = event.files;
          std::stable_sort(files_.begin(), files_.end(), [](const auto& a, const auto& b) {
            if (a.directory != b.directory) return a.directory;
            return a.name < b.name;
          });
          filesError_ = event.text;
          filesLoading_ = false;
          changes.files = true;
        }
        break;
      case EventKind::RemoteFileSaved:
        changes.files = downloadingFiles_.erase(event.remotePath) > 0;
        break;
      default:
        break;
    }
    return changes;
  }

  bool RemoveNotification(const std::string& key) {
    const auto before = notifications_.size();
    std::erase_if(notifications_,
                  [&](const auto& item) { return item.info.key == key; });
    return notifications_.size() != before;
  }

  bool SetDownloading(const std::string& id, bool downloading) {
    auto* photo = FindPhoto(id);
    if (!photo) return false;
    photo->downloading = downloading;
    return true;
  }

  void Clear() {
    notifications_.clear();
    photos_.clear();
    clips_.clear();
    photosFetchedAt_ = 0;
    media_ = {};
    mediaArt_.reset();
    smsThreads_.clear();
    smsMessages_.clear();
    smsThread_.clear();
    smsMessagesLoaded_ = false;
    smsFetchedAt_ = 0;
    files_.clear();
    filesError_.clear();
    downloadingFiles_.clear();
    filesLoading_ = false;
    filesPath_ = "/";
    call_ = {};
    ringing_ = false;
    features_.clear();
  }

  // ---- SMS

  // Selects the conversation shown in the thread view; clears the old one.
  void OpenSmsThread(const std::string& thread) {
    if (thread == smsThread_) return;
    smsThread_ = thread;
    smsMessages_.clear();
    smsMessagesLoaded_ = false;
    for (auto& item : smsThreads_) {
      if (item.thread == thread) item.unread = false;
    }
  }

  // Shows an SMS sent from the PC right away; SmsSent confirms or fails it.
  void AddPendingSms(const std::string& ref, const std::string& body, long long nowMs) {
    StoredSms stored;
    stored.message.id = "out:" + ref;
    stored.message.body = body;
    stored.message.time = nowMs;
    stored.message.outgoing = true;
    stored.ref = ref;
    stored.pending = true;
    smsMessages_.push_back(std::move(stored));
    for (auto& item : smsThreads_) {
      if (item.thread != smsThread_) continue;
      item.snippet = body;
      item.time = nowMs;
    }
    SortThreads();
  }

  const SmsThread* FindSmsThread(const std::string& thread) const {
    for (const auto& item : smsThreads_) {
      if (item.thread == thread) return &item;
    }
    return nullptr;
  }

  // ---- Phone storage

  // Starts showing a folder; its listing arrives as RemoteFileList.
  void OpenFolder(const std::string& path) {
    if (path != filesPath_) {
      files_.clear();
      filesError_.clear();
    }
    filesPath_ = path;
    filesLoading_ = true;
  }

  void SetFileDownloading(const std::string& path, bool downloading) {
    if (downloading) downloadingFiles_.push_back(path);
    else downloadingFiles_.erase(path);
  }

  bool FileDownloading(const std::string& path) const {
    return downloadingFiles_.contains(path);
  }

  // ---- Misc

  bool HasFeature(std::string_view feature) const {
    return std::find(features_.begin(), features_.end(), feature) != features_.end();
  }

  // Percentage at which a battery alert fires; 0 turns it off.
  void SetLowBatteryPercent(int percent) { lowBatteryPercent_ = std::clamp(percent, 0, 100); }

  void SetRinging(bool ringing) { ringing_ = ringing; }

  SharedBytes NotificationIcon(const std::string& key) const {
    for (const auto& item : notifications_) {
      if (item.info.key == key) return item.icon;
    }
    return nullptr;
  }

  SharedBytes PhotoThumb(const std::string& id) const {
    const auto* photo = FindPhoto(id);
    return photo ? photo->thumb : nullptr;
  }

  const std::deque<StoredNotification>& Notifications() const { return notifications_; }
  const std::vector<StoredPhoto>& Photos() const { return photos_; }
  const std::deque<StoredClip>& Clips() const { return clips_; }
  const MediaInfo& Media() const { return media_; }
  SharedBytes MediaArt() const { return mediaArt_; }
  long long MediaReceivedAt() const { return mediaReceivedAt_; }
  const std::vector<SmsThread>& SmsThreads() const { return smsThreads_; }
  const std::vector<StoredSms>& SmsMessages() const { return smsMessages_; }
  const std::string& SmsThreadId() const { return smsThread_; }
  bool SmsMessagesLoaded() const { return smsMessagesLoaded_; }
  long long SmsFetchedAt() const { return smsFetchedAt_; }
  const CallInfo& Call() const { return call_; }
  bool Ringing() const { return ringing_; }
  const std::string& FilesPath() const { return filesPath_; }
  const std::vector<RemoteFile>& Files() const { return files_; }
  const std::string& FilesError() const { return filesError_; }
  bool FilesLoading() const { return filesLoading_; }
  int Battery() const { return battery_; }
  bool Charging() const { return charging_; }
  bool Connected() const { return connected_; }
  const std::string& DeviceName() const { return deviceName_; }
  long long PhotosFetchedAt() const { return photosFetchedAt_; }

 private:
  // Small set of strings; a vector keeps the header dependency-free.
  struct PathSet {
    std::vector<std::string> items;
    void push_back(const std::string& path) {
      if (!contains(path)) items.push_back(path);
    }
    std::size_t erase(const std::string& path) { return std::erase(items, path); }
    bool contains(const std::string& path) const {
      return std::find(items.begin(), items.end(), path) != items.end();
    }
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }
  };

  void SortThreads() {
    std::stable_sort(smsThreads_.begin(), smsThreads_.end(),
                     [](const auto& a, const auto& b) { return a.time > b.time; });
    if (smsThreads_.size() > kMaxSmsThreads) smsThreads_.resize(kMaxSmsThreads);
  }

  // Oldest first, like a chat.
  void SortMessages() {
    std::stable_sort(smsMessages_.begin(), smsMessages_.end(), [](const auto& a, const auto& b) {
      return a.message.time < b.message.time;
    });
  }

  StoredPhoto* FindPhoto(const std::string& id) {
    for (auto& photo : photos_) {
      if (photo.info.id == id) return &photo;
    }
    return nullptr;
  }

  const StoredPhoto* FindPhoto(const std::string& id) const {
    for (const auto& photo : photos_) {
      if (photo.info.id == id) return &photo;
    }
    return nullptr;
  }

  std::deque<StoredNotification> notifications_;
  std::vector<StoredPhoto> photos_;
  std::deque<StoredClip> clips_;
  std::uint64_t clipSerial_ = 0;
  std::string dataDeviceId_;
  std::string deviceName_;
  bool connected_ = false;
  long long photosFetchedAt_ = 0;
  std::vector<std::string> features_;
  int battery_ = -1;
  bool charging_ = false;
  int lowBatteryPercent_ = 0;
  bool lowBatteryAlerted_ = false;
  bool ringing_ = false;
  MediaInfo media_;
  SharedBytes mediaArt_;
  long long mediaReceivedAt_ = 0;
  std::vector<SmsThread> smsThreads_;
  std::vector<StoredSms> smsMessages_;
  std::string smsThread_;
  bool smsMessagesLoaded_ = false;
  long long smsFetchedAt_ = 0;
  CallInfo call_;
  std::string filesPath_ = "/";
  std::vector<RemoteFile> files_;
  std::string filesError_;
  bool filesLoading_ = false;
  PathSet downloadingFiles_;
};

}  // namespace feathercast::phone
