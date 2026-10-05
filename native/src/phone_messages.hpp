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
    event.battery = JsonInt32(root, "battery", -1);
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
        action.index = JsonInt32(item, "i", -1);
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
    event.text = JsonString(root, "error");
    event.id = JsonString(root, "access");
    if (const auto* items = JsonArray(root, "items")) {
      for (const auto& item : items->array) {
        PhotoInfo photo;
        photo.id = JsonString(item, "id");
        photo.name = JsonString(item, "name");
        photo.time = JsonInt(item, "time");
        photo.width = JsonInt32(item, "w");
        photo.height = JsonInt32(item, "h");
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
    media.volume = JsonInt32(root, "vol", -1);
    media.volumeMax = JsonInt32(root, "volMax");
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

// ------------------------------------------------------------- saved files

inline constexpr std::size_t kMaxSavedNameBytes = 120;

// Turns a name chosen by the phone into one safe file name for the downloads
// folder. Path separators, characters Windows rejects, control characters,
// invalid UTF-8, and bidirectional overrides (which can disguise "exe.jpg" as
// "gpj.exe") become '_'. Leading dots and spaces, trailing dots and spaces,
// and reserved device names (CON, NUL, COM1, ...) are neutralized, and long
// names are shortened at a character boundary, keeping the extension.
inline std::string SanitizeFileName(std::string_view name,
                                    std::string_view fallback = "phone-file") {
  static constexpr std::string_view kInvalid = "<>:\"/\\|?*";
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size();) {
    const auto lead = static_cast<unsigned char>(name[i]);
    std::size_t length = lead < 0x80 ? 1
                         : (lead & 0xE0) == 0xC0 ? 2
                         : (lead & 0xF0) == 0xE0 ? 3
                         : (lead & 0xF8) == 0xF0 ? 4
                                                 : 0;
    std::uint32_t cp = length == 1 ? lead
                       : length == 2 ? (lead & 0x1Fu)
                       : length == 3 ? (lead & 0x0Fu)
                                     : (lead & 0x07u);
    bool valid = length > 0 && i + length <= name.size();
    for (std::size_t k = 1; valid && k < length; ++k) {
      const auto next = static_cast<unsigned char>(name[i + k]);
      valid = (next & 0xC0) == 0x80;
      cp = (cp << 6) | (next & 0x3Fu);
    }
    if (valid && ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) ||
                  (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                  (cp >= 0xD800 && cp <= 0xDFFF))) {
      valid = false;  // overlong, out of range, or a surrogate
    }
    if (!valid) {
      out.push_back('_');
      ++i;
      continue;
    }
    const bool unsafe = cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
                        cp == 0x061C || cp == 0x200E || cp == 0x200F ||
                        (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) ||
                        (cp < 0x80 && kInvalid.find(static_cast<char>(cp)) != std::string_view::npos);
    if (unsafe) {
      out.push_back('_');
    } else {
      out.append(name.substr(i, length));
    }
    i += length;
  }
  const auto trim = [](std::string& text) {
    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == '.' || text[start] == ' ')) ++start;
    text.erase(0, start);
  };
  trim(out);
  if (out.size() > kMaxSavedNameBytes) {
    const auto dot = out.find_last_of('.');
    const std::string extension =
        dot != std::string::npos && dot > 0 && out.size() - dot <= 16 ? out.substr(dot) : std::string{};
    std::size_t keep = kMaxSavedNameBytes - extension.size();
    // Never cut a multi-byte UTF-8 sequence in half.
    while (keep > 0 && (static_cast<unsigned char>(out[keep]) & 0xC0) == 0x80) --keep;
    out = out.substr(0, keep) + extension;
    trim(out);
  }
  if (out.empty()) out = std::string(fallback);
  // Windows maps these names to devices, with or without an extension.
  std::string stem = out.substr(0, out.find('.'));
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  for (char& ch : stem) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  bool reserved = stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                  stem == "CONIN$" || stem == "CONOUT$" || stem == "CLOCK$";
  if (!reserved && (stem.starts_with("COM") || stem.starts_with("LPT"))) {
    const std::string_view number = std::string_view(stem).substr(3);
    reserved = (number.size() == 1 && number[0] >= '0' && number[0] <= '9') ||
               number == "\xC2\xB9" || number == "\xC2\xB2" || number == "\xC2\xB3";
  }
  if (reserved) out.insert(out.begin(), '_');
  return out;
}

// File name for a file fetched from phone storage: the last segment of the
// requested path, never a separate name the phone reports.
inline std::string RemoteFileName(std::string_view remotePath) {
  while (!remotePath.empty() && remotePath.back() == '/') remotePath.remove_suffix(1);
  const auto slash = remotePath.find_last_of('/');
  return SanitizeFileName(slash == std::string_view::npos ? remotePath
                                                          : remotePath.substr(slash + 1));
}

// Canonical extension for image bytes, or empty when the format is unknown.
inline std::string_view DetectImageExtension(const Bytes& data) {
  const auto at = [&](std::size_t offset, std::string_view magic) {
    if (data.size() < offset + magic.size()) return false;
    for (std::size_t i = 0; i < magic.size(); ++i) {
      if (data[offset + i] != static_cast<std::uint8_t>(magic[i])) return false;
    }
    return true;
  };
  if (at(0, "\xFF\xD8\xFF")) return ".jpg";
  if (at(0, "\x89" "PNG\r\n\x1A\n")) return ".png";
  if (at(0, "GIF87a") || at(0, "GIF89a")) return ".gif";
  if (at(0, "RIFF") && at(8, "WEBP")) return ".webp";
  if (at(0, std::string_view("II*\0", 4)) || at(0, std::string_view("MM\0*", 4))) return ".tif";
  if (at(0, "BM")) return ".bmp";
  if (at(4, "ftyp")) {
    if (at(8, "avif") || at(8, "avis")) return ".avif";
    for (const std::string_view brand : {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1"}) {
      if (at(8, brand)) return ".heic";
    }
  }
  return {};
}

// Saved photo name whose extension matches the image bytes, so a photo named
// "invoice.pdf.exe" by the phone can only be saved and opened as an image.
// Unknown formats are saved as ".bin" and therefore never run when opened.
inline std::string PhotoFileName(std::string_view name, const Bytes& data) {
  std::string safe = SanitizeFileName(name, "phone-photo");
  const std::string_view detected = DetectImageExtension(data);
  const std::string_view wanted = detected.empty() ? std::string_view(".bin") : detected;
  const auto dot = safe.find_last_of('.');
  const bool hasExtension = dot != std::string::npos && dot > 0;
  std::string extension = hasExtension ? safe.substr(dot) : std::string{};
  for (char& ch : extension) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  const bool matches = extension == wanted ||
                       (wanted == ".jpg" && (extension == ".jpeg" || extension == ".jpe")) ||
                       (wanted == ".tif" && (extension == ".tiff" || extension == ".dng")) ||
                       (wanted == ".heic" && extension == ".heif");
  if (matches) return safe;
  return (hasExtension ? safe.substr(0, dot) : safe) + std::string(wanted);
}

}  // namespace feathercast::phone
