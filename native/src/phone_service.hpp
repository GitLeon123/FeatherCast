#pragma once

// LAN server that links FeatherCast with the FeatherCast Phone Android app.
// Runs its own socket threads; every event is reported through the callback
// from a worker thread, so the owner must marshal it to the UI thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "phone_protocol.hpp"

namespace feathercast::phone {

struct PairedDevice {
  std::string id;
  std::string name;
  Bytes linkKey;
  long long pairedAt = 0;
};

struct NotificationAction {
  int index = 0;
  std::string title;
  bool reply = false;  // takes typed text (RemoteInput)
};

struct NotificationInfo {
  std::string key;
  std::string app;
  std::string appName;
  std::string title;
  std::string text;
  long long time = 0;
  std::vector<NotificationAction> actions;
  Bytes iconPng;
};

struct PhotoInfo {
  std::string id;
  std::string name;
  long long time = 0;
  int width = 0;
  int height = 0;
  Bytes thumbJpeg;
};

struct MediaInfo {
  bool active = false;
  std::string app;
  std::string appName;
  std::string title;
  std::string artist;
  bool playing = false;
  long long position = 0;    // milliseconds at positionAt
  long long duration = 0;    // milliseconds, 0 when unknown
  long long positionAt = 0;  // Unix milliseconds
  int volume = -1;
  int volumeMax = 0;
  Bytes artJpeg;
};

struct SmsThread {
  std::string thread;
  std::string address;
  std::string name;
  std::string snippet;
  long long time = 0;
  bool unread = false;
};

struct SmsMessage {
  std::string id;
  std::string body;
  long long time = 0;
  bool outgoing = false;
};

enum class CallState { Idle, Ringing, Active };

struct CallInfo {
  CallState state = CallState::Idle;
  std::string number;
  std::string name;
};

struct RemoteFile {
  std::string name;
  bool directory = false;
  long long size = 0;
  long long time = 0;
};

enum class EventKind {
  Connected,
  Disconnected,
  Paired,
  Status,
  NotificationPosted,
  NotificationRemoved,
  NotificationsReset,
  Clipboard,
  PhotoList,
  PhotoThumb,
  PhotoSaved,
  FileSaved,
  ClipboardHistoryRequested,
  FileDelivered,    // the phone saved a file sent from the PC (ok/text)
  RingState,        // ok = ringing
  MediaState,       // media.active == false when nothing plays
  SmsThreads,
  SmsMessages,
  SmsReceived,
  SmsSent,          // id = ref, ok
  Call,
  RemoteFileList,   // remotePath, files, text = error
  RemoteFileSaved,  // remotePath, path
  Error,
};

struct Event {
  EventKind kind = EventKind::Error;
  std::string deviceId;
  std::string deviceName;
  std::string text;  // clipboard text, error message, or notification key
  NotificationInfo notification;
  PhotoInfo photo;
  std::vector<PhotoInfo> photos;
  int battery = -1;
  bool charging = false;
  std::wstring path;  // saved photo or file
  std::vector<std::string> features;  // Status: what the phone app supports
  std::string id;          // file transfer id, SMS ref, or thread id
  bool ok = false;
  MediaInfo media;
  std::vector<SmsThread> smsThreads;  // SmsThreads, or one entry for SmsReceived
  std::vector<SmsMessage> smsMessages;
  CallInfo call;
  std::string remotePath;
  std::vector<RemoteFile> files;
};

struct ServiceConfig {
  std::string pcName;
  std::wstring stateFile;     // DPAPI-sealed identity and paired devices
  std::wstring downloadsDir;  // where received photos and files are saved
  std::wstring apkPath;       // served at http://<pc>:<port>/app.apk
  std::uint16_t port = kDefaultPort;
  std::function<void(Event)> onEvent;
};

struct ClipboardHistoryItem {
  std::string text;
  long long time = 0;
};

class PhoneService {
 public:
  PhoneService();
  ~PhoneService();
  PhoneService(const PhoneService&) = delete;
  PhoneService& operator=(const PhoneService&) = delete;

  // Loads (or creates) the PC identity and starts listening. Returns false
  // with a readable reason in *error when the port cannot be opened.
  bool Start(ServiceConfig config, std::string* error = nullptr);
  void Stop();
  bool Running() const { return running_.load(); }
  std::uint16_t Port() const { return port_; }

  // Creates a new single-use pairing invite, valid for five minutes.
  std::string CreatePairingUri();
  std::string ApkUrl() const;
  std::vector<std::string> LocalAddresses() const;

  std::vector<PairedDevice> Devices() const;
  void Forget(const std::string& deviceId);
  bool Connected() const;
  std::string ConnectedDeviceName() const;

  bool SendClipboard(const std::string& text);
  bool SendClipboardHistory(const std::vector<ClipboardHistoryItem>& items);
  bool RequestPhotos();
  bool RequestPhoto(const std::string& id);
  bool DismissNotification(const std::string& key);
  bool NotificationAction(const std::string& key, int index,
                          const std::string& text = {});
  // Reads the file on a worker thread; the result arrives as FileDelivered or
  // Error. Returns the transfer id, or empty when not connected / too large.
  std::string SendFile(const std::wstring& path, std::string* error = nullptr);
  bool Ring(bool start);
  bool MediaCommand(const std::string& command, long long position = -1);
  bool MediaVolume(int volume);
  bool RequestSmsThreads();
  bool RequestSmsMessages(const std::string& thread, int limit = 50);
  bool SendSms(const std::string& ref, const std::string& address,
               const std::string& body);
  bool CallReject();
  bool CallSilence();
  bool ListFiles(const std::string& remotePath);
  bool RequestFile(const std::string& remotePath);

  struct Session;

 private:
  void AcceptLoop(std::stop_token stop);
  void BeaconLoop(std::stop_token stop);
  void HandleConnection(std::uintptr_t socket);
  void ServeHttp(std::uintptr_t socket, const Bytes& firstBytes);
  bool HandlePlainFrame(const std::shared_ptr<Session>& session,
                        const Bytes& frame);
  void HandleSessionFrame(const std::shared_ptr<Session>& session,
                          const Bytes& frame);
  bool Send(const std::string& json, const Bytes& binary = {});
  bool SendToSession(const std::shared_ptr<Session>& session,
                     const std::string& json, const Bytes& binary = {});
  bool SaveState();
  bool LoadState();
  void Emit(Event event);
  std::wstring UniqueDownloadPath(const std::string& name) const;

  ServiceConfig config_;
  std::atomic<bool> running_{false};
  std::uint16_t port_ = 0;
  std::uintptr_t listenSocket_ = ~std::uintptr_t{0};
  std::jthread acceptThread_;
  std::jthread beaconThread_;

  mutable std::mutex mutex_;
  std::mutex stateSaveMutex_;
  std::string pcId_;
  Bytes pcPrivateKey_;
  Bytes pcPublicKey_;
  std::vector<PairedDevice> devices_;
  Bytes pairingToken_;
  long long pairingExpiresAt_ = 0;
  std::shared_ptr<Session> active_;
  std::vector<std::shared_ptr<Session>> sessions_;
  struct Worker {
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Worker> workers_;
};

}  // namespace feathercast::phone
