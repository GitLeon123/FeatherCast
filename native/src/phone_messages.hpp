#pragma once

// Session message catalog for the phone link: parses phone → PC messages
// into Events and builds PC → phone messages. Pure (no sockets or files) so
// both directions are checked against the fixtures that the Kotlin tests in
// android/protocol/src/test use too. Keep in sync with Messages.kt.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "phone_service.hpp"

namespace feathercast::phone {

// Largest file either side sends in one message (the frame limit leaves room
// for the JSON header and AES-GCM overhead).
inline constexpr std::size_t kMaxTransferBytes = 40u * 1024u * 1024u;

namespace detail {

inline const json::Value* JsonArray(const json::Value& root, std::string_view key) {
  const json::Value* value = root.Find(key);
  return value && value->type == json::Value::Type::Array ? value : nullptr;
}

inline std::string JoinRaw(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += ",";
    out += items[i];
  }
  return out + "]";
}

}  // namespace detail

// Parses a decrypted session message. Returns nullopt for "ping" and unknown
// types. For "photo.full", "file.send", and "file.data" the caller saves
// `binary` and fills in Event::path.
inline std::optional<Event> ParseSessionMessage(const json::Value& root,
                                                const Bytes& binary,
                                                long long nowMs) {
  using detail::JsonArray;
  const std::string type = JsonString(root, "type");
  Event event;
  if (type == "status") {
    event.kind = EventKind::Status;
    event.battery = static_cast<int>(JsonInt(root, "battery", -1));
    event.charging = JsonBool(root, "charging");
    event.deviceName = JsonString(root, "name");
    if (const auto* features = JsonArray(root, "features")) {
      for (const auto& item : features->array) {
        if (item.type == json::Value::Type::String) event.features.push_back(item.str);
      }
    }
  } else if (type == "notification.posted") {
    event.kind = EventKind::NotificationPosted;
    auto& info = event.notification;
    info.key = JsonString(root, "key");
    info.app = JsonString(root, "app");
    info.appName = JsonString(root, "appName");
    info.title = JsonString(root, "title");
    info.text = JsonString(root, "text");
    info.time = JsonInt(root, "time", nowMs);
    if (const auto* actions = JsonArray(root, "actions")) {
      for (const auto& item : actions->array) {
        NotificationAction action;
        action.index = static_cast<int>(JsonInt(item, "i", -1));
        action.title = JsonString(item, "title");
        action.reply = JsonBool(item, "reply");
        if (action.index >= 0 && !action.title.empty()) info.actions.push_back(std::move(action));
      }
    }
    info.iconPng = binary;
  } else if (type == "notification.removed") {
    event.kind = EventKind::NotificationRemoved;
    event.text = JsonString(root, "key");
  } else if (type == "notifications.reset") {
    event.kind = EventKind::NotificationsReset;
  } else if (type == "clipboard.set") {
    event.kind = EventKind::Clipboard;
    event.text = JsonString(root, "text");
  } else if (type == "clipboard.history.request") {
    event.kind = EventKind::ClipboardHistoryRequested;
  } else if (type == "photos.list") {
    event.kind = EventKind::PhotoList;
    if (const auto* items = JsonArray(root, "items")) {
      for (const auto& item : items->array) {
        PhotoInfo photo;
        photo.id = JsonString(item, "id");
        photo.name = JsonString(item, "name");
        photo.time = JsonInt(item, "time");
        photo.width = static_cast<int>(JsonInt(item, "w"));
        photo.height = static_cast<int>(JsonInt(item, "h"));
        if (!photo.id.empty()) event.photos.push_back(std::move(photo));
      }
    }
  } else if (type == "photo.thumb") {
    event.kind = EventKind::PhotoThumb;
    event.photo.id = JsonString(root, "id");
    event.photo.thumbJpeg = binary;
  } else if (type == "photo.full" || type == "file.send") {
    event.kind = type == "photo.full" ? EventKind::PhotoSaved : EventKind::FileSaved;
    event.photo.id = JsonString(root, "id");
    event.photo.name = JsonString(root, "name");
  } else if (type == "file.received") {
    event.kind = EventKind::FileDelivered;
    event.id = JsonString(root, "id");
    event.photo.name = JsonString(root, "name");
    event.ok = JsonBool(root, "ok");
    event.text = JsonString(root, "error");
  } else if (type == "ring.state") {
    event.kind = EventKind::RingState;
    event.ok = JsonBool(root, "ringing");
  } else if (type == "media.state") {
    event.kind = EventKind::MediaState;
    auto& media = event.media;
    media.active = true;
    media.app = JsonString(root, "app");
    media.appName = JsonString(root, "appName");
    media.title = JsonString(root, "title");
    media.artist = JsonString(root, "artist");
    media.playing = JsonBool(root, "playing");
    media.position = JsonInt(root, "pos");
    media.duration = JsonInt(root, "dur");
    media.positionAt = JsonInt(root, "posAt", nowMs);
    media.volume = static_cast<int>(JsonInt(root, "vol", -1));
    media.volumeMax = static_cast<int>(JsonInt(root, "volMax"));
    media.artJpeg = binary;
  } else if (type == "media.none") {
    event.kind = EventKind::MediaState;
  } else if (type == "sms.threads") {
    event.kind = EventKind::SmsThreads;
    if (const auto* items = JsonArray(root, "items")) {
      for (const auto& item : items->array) {
        SmsThread thread;
        thread.thread = JsonString(item, "thread");
        thread.address = JsonString(item, "address");
        thread.name = JsonString(item, "name");
        thread.snippet = JsonString(item, "snippet");
        thread.time = JsonInt(item, "time");
        thread.unread = JsonBool(item, "unread");
        if (!thread.thread.empty()) event.smsThreads.push_back(std::move(thread));
      }
    }
  } else if (type == "sms.messages") {
    event.kind = EventKind::SmsMessages;
    event.id = JsonString(root, "thread");
    if (const auto* items = JsonArray(root, "items")) {
      for (const auto& item : items->array) {
        SmsMessage message;
        message.id = JsonString(item, "id");
        message.body = JsonString(item, "body");
        message.time = JsonInt(item, "time");
        message.outgoing = JsonBool(item, "out");
        event.smsMessages.push_back(std::move(message));
      }
    }
  } else if (type == "sms.received") {
    event.kind = EventKind::SmsReceived;
    SmsThread thread;
    thread.thread = JsonString(root, "thread");
    thread.address = JsonString(root, "address");
    thread.name = JsonString(root, "name");
    thread.snippet = JsonString(root, "body");
    thread.time = JsonInt(root, "time", nowMs);
    thread.unread = true;
    event.id = thread.thread;
    event.smsThreads.push_back(std::move(thread));
  } else if (type == "sms.sent") {
    event.kind = EventKind::SmsSent;
    event.id = JsonString(root, "ref");
    event.ok = JsonBool(root, "ok");
    event.text = JsonString(root, "error");
  } else if (type == "call.state") {
    event.kind = EventKind::Call;
    const std::string state = JsonString(root, "state");
    event.call.state = state == "ringing" ? CallState::Ringing
                       : state == "active" ? CallState::Active
                                           : CallState::Idle;
    event.call.number = JsonString(root, "number");
    event.call.name = JsonString(root, "name");
  } else if (type == "files.list") {
    event.kind = EventKind::RemoteFileList;
    event.remotePath = JsonString(root, "path");
    event.text = JsonString(root, "error");
    if (const auto* items = JsonArray(root, "items")) {
      for (const auto& item : items->array) {
        RemoteFile file;
        file.name = JsonString(item, "name");
        file.directory = JsonBool(item, "dir");
        file.size = JsonInt(item, "size");
        file.time = JsonInt(item, "time");
        if (!file.name.empty()) event.files.push_back(std::move(file));
      }
    }
  } else if (type == "file.data") {
    event.kind = EventKind::RemoteFileSaved;
    event.remotePath = JsonString(root, "path");
    event.photo.name = JsonString(root, "name");
    event.text = JsonString(root, "error");
  } else {
    return std::nullopt;  // "ping" and unknown types
  }
  return event;
}

// ------------------------------------------------------- PC → phone messages

namespace message {

inline std::string Clipboard(std::string_view text, long long time) {
  return Json("clipboard.set").Str("text", text).Int("time", time).Build();
}

inline std::string ClipboardHistory(const std::vector<ClipboardHistoryItem>& items) {
  std::vector<std::string> list;
  for (const auto& item : items) {
    list.push_back(Json().Str("text", item.text).Int("time", item.time).Build());
  }
  return Json("clipboard.history").Raw("items", detail::JoinRaw(list)).Build();
}

inline std::string PhotosRequest(int limit) {
  return Json("photos.request").Int("limit", limit).Build();
}

inline std::string PhotoRequest(std::string_view id) {
  return Json("photo.request").Str("id", id).Build();
}

inline std::string NotificationDismiss(std::string_view key) {
  return Json("notification.dismiss").Str("key", key).Build();
}

inline std::string NotificationAction(std::string_view key, int index,
                                      std::string_view text) {
  Json json("notification.action");
  json.Str("key", key).Int("i", index);
  if (!text.empty()) json.Str("text", text);
  return json.Build();
}

inline std::string FileSend(std::string_view id, std::string_view name) {
  return Json("file.send").Str("id", id).Str("name", name).Build();
}

inline std::string Ring(bool start) {
  return Json(start ? "ring.start" : "ring.stop").Build();
}

inline std::string MediaCommand(std::string_view command, long long position) {
  Json json("media.command");
  json.Str("cmd", command);
  if (position >= 0) json.Int("pos", position);
  return json.Build();
}

inline std::string MediaVolume(int volume) {
  return Json("media.volume").Int("vol", volume).Build();
}

inline std::string SmsThreadsRequest() { return Json("sms.threads.request").Build(); }

inline std::string SmsMessagesRequest(std::string_view thread, int limit) {
  return Json("sms.messages.request").Str("thread", thread).Int("limit", limit).Build();
}

inline std::string SmsSend(std::string_view ref, std::string_view address,
                           std::string_view body) {
  return Json("sms.send").Str("ref", ref).Str("address", address).Str("body", body).Build();
}

inline std::string CallReject() { return Json("call.reject").Build(); }
inline std::string CallSilence() { return Json("call.silence").Build(); }

inline std::string FilesListRequest(std::string_view path) {
  return Json("files.list.request").Str("path", path).Build();
}

inline std::string FileRequest(std::string_view path) {
  return Json("file.request").Str("path", path).Build();
}

}  // namespace message

// Joins a phone storage path ("/" is the shared-storage root) with a child.
inline std::string RemotePathJoin(std::string_view parent, std::string_view child) {
  std::string out(parent.empty() ? "/" : parent);
  if (out.back() != '/') out.push_back('/');
  out += child;
  return out;
}

// Parent of a phone storage path; "/" stays "/".
inline std::string RemotePathParent(std::string_view path) {
  std::string out(path);
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  const auto slash = out.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return out.substr(0, slash);
}

}  // namespace feathercast::phone
