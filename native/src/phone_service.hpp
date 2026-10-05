#pragma once

// LAN server that links FeatherCast with the FeatherCast Phone Android app.
// Runs its own socket threads; every event is reported through the callback
// from a worker thread, so the owner must marshal it to the UI thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "phone_protocol.hpp"
#include "phone_screen_protocol.hpp"

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
  TransferProgress,
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
  long long transferredBytes = 0;
  long long totalBytes = 0;
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
  std::uint16_t port = kDefaultPort;  // tries up to 10 ports from here; 0 = any free port
  std::function<void(Event)> onEvent;
  // Screen packets bypass the general UI event queue; the receiver must bound
  // media buffering and marshal its own window notifications.
  std::function<void(ScreenPacket)> onScreenPacket;
};

struct ClipboardHistoryItem {
  std::string text;
  long long time = 0;
};

// Marks a received file as downloaded from the internet (Zone.Identifier), so
// Windows and Office apply their usual protections. False on volumes without streams.
bool MarkFileFromInternet(const std::wstring& path);
// IPv4 addresses a phone can reach this PC at, best candidates first. Empty
// without a usable network.
std::vector<std::string> LocalNetworkAddresses();

class PhoneService {
 public:
  PhoneService();
  ~PhoneService();
  PhoneService(const PhoneService&) = delete;
  PhoneService& operator=(const PhoneService&) = delete;

  // Loads (or creates) the PC identity and starts listening. Returns false
  // with a readable reason in *error when the port cannot be opened or saved
  // pairings exist but cannot be read (they are then left untouched).
  bool Start(ServiceConfig config, std::string* error = nullptr);
  void Stop();
  bool Running() const { return running_.load(); }
  std::uint16_t Port() const { return port_; }

  // Creates a new single-use pairing invite, valid for five minutes. Empty
  // when this PC has no network address a phone could reach.
  std::string CreatePairingUri();
  std::string ApkUrl() const;  // empty without a network address

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
  void CancelTransfers();
  std::string StartScreen(bool audio = true);
  void StopScreen(bool report = true);
  bool SendScreenInput(ScreenInput input);

  struct Session;

 private:
  void AcceptLoop(std::stop_token stop);
  void BeaconLoop(std::stop_token stop);
  void ScreenSendLoop(std::stop_token stop);
  void EmitScreenState(std::string id, std::string state, std::string detail = {});
  void HandleConnection(std::uintptr_t socket);
  void ServeHttp(std::uintptr_t socket, const Bytes& firstBytes,
                 std::chrono::steady_clock::time_point deadline);
  bool HandlePlainFrame(const std::shared_ptr<Session>& session,
                        const Bytes& frame);
  void HandleSessionFrame(const std::shared_ptr<Session>& session,
                          const Bytes& frame);
  // Queue a message on the session's writer. True means queued; a failed write
  // later closes the session. wait blocks while the queue is full.
  bool Send(std::string json, Bytes binary = {});
  bool SendToSession(const std::shared_ptr<Session>& session, std::string json,
                     Bytes binary = {}, bool wait = false);
  bool SaveState();
  enum class StateLoad { Loaded, Missing, Failed };
  StateLoad LoadState();
  void Emit(Event event);
  // Matches a received photo or storage file to its request.
  void SettleRequest(Event& event);
  // Reports photo and file requests the phone did not answer in time.
  void ExpireRequests();
  std::vector<Event> TakeExpiredLocked(bool all);  // mutex_ held
  void EmitAll(std::vector<Event> events);

  ServiceConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stateReady_{false};  // SaveState never overwrites unread pairings
  std::uint16_t port_ = 0;
  std::uintptr_t listenSocket_ = ~std::uintptr_t{0};
  void* acceptEvent_ = nullptr;
  std::jthread acceptThread_;
  std::jthread beaconThread_;
  std::jthread screenSendThread_;
  std::condition_variable_any screenWake_;

  mutable std::mutex mutex_;
  std::mutex stateSaveMutex_;
  std::string pcId_;
  Bytes pcPrivateKey_;
  Bytes pcPublicKey_;
  std::vector<PairedDevice> devices_;
  Bytes pairingToken_;
  long long pairingExpiresAt_ = 0;
  std::shared_ptr<Session> active_;
  std::shared_ptr<Session> screen_;
  std::weak_ptr<Session> screenOwner_;
  std::string screenId_;
  Bytes screenKey_;
  long long screenExpiresAt_ = 0;
  std::deque<ScreenInput> screenInputs_;
  std::deque<std::pair<std::shared_ptr<Session>, std::string>> screenCommands_;
  std::vector<std::shared_ptr<Session>> sessions_;
  std::size_t pendingHandshakes_ = 0;  // unauthenticated connections
  // Outstanding photo.request ids and file.get paths with their deadlines.
  std::map<std::string, long long> pendingPhotos_;
  struct PendingFile {
    std::string normalized;  // the path the phone answers with
    long long deadline = 0;
  };
  std::map<std::string, PendingFile> pendingFiles_;
  struct Worker {
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Worker> workers_;
};

}  // namespace feathercast::phone
