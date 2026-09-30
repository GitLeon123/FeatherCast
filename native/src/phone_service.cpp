#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "phone_service.hpp"

#include "phone_crypto.hpp"
#include "phone_messages.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace feathercast::phone {
namespace {

constexpr long long kPairingLifetimeMs = 5 * 60 * 1000;
constexpr DWORD kSessionIdleTimeoutMs = 75 * 1000;
constexpr DWORD kHandshakeTimeoutMs = 20 * 1000;
constexpr std::size_t kMaxHandshakeBytes = 64 * 1024;
constexpr std::size_t kMaxWorkers = 16;
constexpr auto kBeaconInterval = std::chrono::seconds(3);

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::wstring Widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

std::string Narrow(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                      size, nullptr, nullptr);
  return out;
}

bool SendAll(SOCKET socket, const std::uint8_t* data, size_t size) {
  while (size > 0) {
    const int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
    const int sent = send(socket, reinterpret_cast<const char*>(data), chunk, 0);
    if (sent <= 0) return false;
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

bool SendAll(SOCKET socket, const Bytes& bytes) {
  return SendAll(socket, bytes.data(), bytes.size());
}

bool SendAll(SOCKET socket, std::string_view text) {
  return SendAll(socket, reinterpret_cast<const std::uint8_t*>(text.data()),
                 text.size());
}

void SetTimeout(SOCKET socket, DWORD milliseconds) {
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
}

struct Ipv4Adapter {
  std::string address;
  ULONG broadcast = 0;  // network byte order
  bool hasGateway = false;
};

std::vector<Ipv4Adapter> EnumerateAdapters() {
  std::vector<Ipv4Adapter> out;
  ULONG size = 16 * 1024;
  std::vector<std::uint8_t> buffer(size);
  constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                           GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG result = GetAdaptersAddresses(AF_INET, kFlags, nullptr, addresses, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, kFlags, nullptr, addresses, &size);
  }
  if (result != NO_ERROR) return out;
  for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    for (auto* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      const auto* addr =
          reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      if (addr->sin_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
      const std::string_view view(text);
      if (view.starts_with("169.254.") || view.starts_with("127.")) continue;
      const ULONG prefix = unicast->OnLinkPrefixLength;
      const ULONG hostMask =
          prefix >= 32 ? 0 : (prefix == 0 ? 0xFFFFFFFFu : (0xFFFFFFFFu >> prefix));
      Ipv4Adapter entry;
      entry.address = text;
      entry.broadcast = addr->sin_addr.s_addr | htonl(hostMask);
      entry.hasGateway = adapter->FirstGatewayAddress != nullptr;
      out.push_back(std::move(entry));
    }
  }
  // Adapters with a default gateway are almost always the real Wi-Fi/LAN.
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.hasGateway && !b.hasGateway;
  });
  return out;
}

std::string SanitizeFileName(std::string name) {
  static constexpr std::string_view kInvalid = "<>:\"/\\|?*";
  for (char& ch : name) {
    if (static_cast<unsigned char>(ch) < 0x20 || kInvalid.find(ch) != std::string_view::npos) {
      ch = '_';
    }
  }
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
  while (!name.empty() && name.front() == '.') name.erase(name.begin());
  if (name.size() > 120) name = name.substr(name.size() - 120);
  return name.empty() ? std::string("phone-file") : name;
}

std::optional<Bytes> ReadFileBytes(const std::wstring& path,
                                  std::size_t maxBytes = 1024 * 1024) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary | std::ios::ate);
  if (!in) return std::nullopt;
  const auto size = in.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > maxBytes) return std::nullopt;
  Bytes bytes(static_cast<std::size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!in || in.peek() != std::char_traits<char>::eof()) return std::nullopt;
  return bytes;
}

bool WriteFileBytes(const std::wstring& path, const Bytes& data) {
  const std::filesystem::path target(path);
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::filesystem::path temp = target.wstring() + L".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

class WinsockInit {
 public:
  WinsockInit() { ok_ = WSAStartup(MAKEWORD(2, 2), &data_) == 0; }
  ~WinsockInit() {
    if (ok_) WSACleanup();
  }
  bool ok() const { return ok_; }

 private:
  WSADATA data_{};
  bool ok_ = false;
};

WinsockInit& Winsock() {
  static WinsockInit init;
  return init;
}

}  // namespace

struct PhoneService::Session {
  enum class State { Hello, Auth, Open };
  SOCKET socket = INVALID_SOCKET;
  State state = State::Hello;
  std::mutex sendMutex;
  std::string deviceId;
  std::string deviceName;
  Bytes linkKey;
  Bytes phoneNonce;
  Bytes pcNonce;
  Bytes sendKey;
  Bytes recvKey;
  std::uint64_t sendCounter = 0;
  std::uint64_t recvCounter = 0;

  // Senders retain the session while using its socket. Close only after the
  // receive worker and all senders release it, so a reused handle stays safe.
  ~Session() {
    if (socket != INVALID_SOCKET) {
      shutdown(socket, SD_BOTH);
      closesocket(socket);
    }
  }
};

PhoneService::PhoneService() = default;

PhoneService::~PhoneService() { Stop(); }

bool PhoneService::Start(ServiceConfig config, std::string* error) {
  Stop();
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!Winsock().ok()) return fail("Windows networking is unavailable.");
  config_ = std::move(config);
  if (!LoadState()) {
    auto pair = crypto::EcdhKeyPair::Generate();
    if (!pair) return fail("Could not create the phone link key.");
    std::lock_guard lock(mutex_);
    pcPrivateKey_ = pair->ExportPrivate();
    pcPublicKey_ = pair->PublicKey();
    pcId_ = HexEncode(crypto::RandomBytes(8));
    devices_.clear();
  }
  if (!SaveState()) return fail("Could not save the phone link identity.");

  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return fail("Could not create a network socket.");
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  bool bound = false;
  for (unsigned candidate = config_.port;
       candidate <= 65535 && candidate < static_cast<unsigned>(config_.port) + 10 && !bound;
       ++candidate) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(candidate));
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      port_ = static_cast<std::uint16_t>(candidate);
      bound = true;
    }
  }
  if (!bound || listen(listener, 8) != 0) {
    closesocket(listener);
    return fail("The phone link port is already in use.");
  }
  listenSocket_ = listener;
  running_ = true;
  acceptThread_ = std::jthread([this](std::stop_token stop) { AcceptLoop(stop); });
  beaconThread_ = std::jthread([this](std::stop_token stop) { BeaconLoop(stop); });
  return true;
}

void PhoneService::Stop() {
  if (!running_.exchange(false)) return;
  acceptThread_.request_stop();
  beaconThread_.request_stop();
  // The accept loop checks running_ before adding workers. Join it before
  // collecting workers or closing the listener it is still using.
  if (acceptThread_.joinable()) acceptThread_.join();
  closesocket(static_cast<SOCKET>(listenSocket_));
  listenSocket_ = ~std::uintptr_t{0};
  std::vector<Worker> workers;
  {
    std::lock_guard lock(mutex_);
    for (const auto& session : sessions_) {
      shutdown(session->socket, SD_BOTH);
    }
    workers = std::move(workers_);
    workers_.clear();
  }
  if (beaconThread_.joinable()) beaconThread_.join();
  workers.clear();  // joins
  std::lock_guard lock(mutex_);
  sessions_.clear();
  active_.reset();
  pairingToken_.clear();
  pairingExpiresAt_ = 0;
}

void PhoneService::Emit(Event event) {
  if (config_.onEvent) config_.onEvent(std::move(event));
}

// ------------------------------------------------------------------- state

bool PhoneService::LoadState() {
  const auto sealed = ReadFileBytes(config_.stateFile);
  if (!sealed) return false;
  const auto plain = crypto::Unprotect(*sealed);
  if (!plain) return false;
  const auto root = json::Parse(std::string_view(
      reinterpret_cast<const char*>(plain->data()), plain->size()));
  if (!root || root->type != json::Value::Type::Object) return false;
  const auto privateKey = Base64UrlDecode(JsonString(*root, "key"));
  if (!privateKey) return false;
  auto pair = crypto::EcdhKeyPair::Import(*privateKey);
  if (!pair) return false;
  std::vector<PairedDevice> devices;
  if (const auto* list = root->Find("devices");
      list && list->type == json::Value::Type::Array) {
    for (const auto& item : list->array) {
      PairedDevice device;
      device.id = JsonString(item, "id");
      device.name = JsonString(item, "name");
      device.pairedAt = JsonInt(item, "pairedAt");
      if (auto key = Base64UrlDecode(JsonString(item, "key"));
          key && key->size() == 32 && !device.id.empty()) {
        device.linkKey = std::move(*key);
        devices.push_back(std::move(device));
      }
    }
  }
  std::lock_guard lock(mutex_);
  pcId_ = JsonString(*root, "pcId");
  if (pcId_.empty()) pcId_ = HexEncode(crypto::RandomBytes(8));
  pcPrivateKey_ = *privateKey;
  pcPublicKey_ = pair->PublicKey();
  devices_ = std::move(devices);
  return true;
}

bool PhoneService::SaveState() {
  std::lock_guard saveLock(stateSaveMutex_);
  std::string devices = "[";
  std::string document;
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < devices_.size(); ++i) {
      if (i) devices += ",";
      devices += Json()
                     .Str("id", devices_[i].id)
                     .Str("name", devices_[i].name)
                     .Str("key", Base64UrlEncode(devices_[i].linkKey))
                     .Int("pairedAt", devices_[i].pairedAt)
                     .Build();
    }
    devices += "]";
    document = Json()
                   .Int("version", 1)
                   .Str("pcId", pcId_)
                   .Str("key", Base64UrlEncode(pcPrivateKey_))
                   .Raw("devices", devices)
                   .Build();
  }
  const auto sealed = crypto::Protect(ToBytes(document));
  SecureZeroMemory(document.data(), document.size());
  return sealed && WriteFileBytes(config_.stateFile, *sealed);
}

// ----------------------------------------------------------------- queries

std::vector<std::string> PhoneService::LocalAddresses() const {
  std::vector<std::string> out;
  for (const auto& adapter : EnumerateAdapters()) {
    if (std::find(out.begin(), out.end(), adapter.address) == out.end()) {
      out.push_back(adapter.address);
    }
  }
  return out;
}

std::string PhoneService::CreatePairingUri() {
  PairingInvite invite;
  invite.pcName = config_.pcName;
  invite.port = port_;
  invite.hosts = LocalAddresses();
  if (invite.hosts.size() > 4) invite.hosts.resize(4);
  std::lock_guard lock(mutex_);
  pairingToken_ = crypto::RandomBytes(16);
  pairingExpiresAt_ = NowMs() + kPairingLifetimeMs;
  invite.pcId = pcId_;
  invite.pcPublicKey = pcPublicKey_;
  invite.token = pairingToken_;
  return BuildPairingUri(invite);
}

std::string PhoneService::ApkUrl() const {
  const auto hosts = LocalAddresses();
  const std::string host = hosts.empty() ? std::string("localhost") : hosts.front();
  return "http://" + host + ":" + std::to_string(port_) + "/app.apk";
}

std::vector<PairedDevice> PhoneService::Devices() const {
  std::lock_guard lock(mutex_);
  return devices_;
}

void PhoneService::Forget(const std::string& deviceId) {
  {
    std::lock_guard lock(mutex_);
    std::erase_if(devices_, [&](const auto& device) { return device.id == deviceId; });
    for (const auto& session : sessions_) {
      if (session->deviceId == deviceId) shutdown(session->socket, SD_BOTH);
    }
  }
  if (!SaveState()) {
    Event event;
    event.text = "Could not save the removed pairing. It may return after restarting FeatherCast.";
    Emit(std::move(event));
  }
}

bool PhoneService::Connected() const {
  std::lock_guard lock(mutex_);
  return active_ != nullptr;
}

std::string PhoneService::ConnectedDeviceName() const {
  std::lock_guard lock(mutex_);
  return active_ ? active_->deviceName : std::string{};
}

// ------------------------------------------------------------------ sending

bool PhoneService::Send(const std::string& json, const Bytes& binary) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard lock(mutex_);
    session = active_;
  }
  return SendToSession(session, json, binary);
}

bool PhoneService::SendToSession(const std::shared_ptr<Session>& session,
                                  const std::string& json, const Bytes& binary) {
  if (!session || binary.size() > kMaxTransferBytes ||
      json.size() > kMaxFrameBytes - 20 - binary.size()) return false;
  {
    std::lock_guard lock(mutex_);
    if (!running_ || active_ != session) return false;
  }
  std::lock_guard lock(session->sendMutex);
  const auto sealed = crypto::AesGcmEncrypt(
      session->sendKey, CounterNonce(session->sendCounter++),
      PackPayload(json, binary));
  return sealed && SendAll(session->socket, EncodeFrame(*sealed));
}

bool PhoneService::SendClipboard(const std::string& text) {
  return Send(message::Clipboard(text, NowMs()));
}

bool PhoneService::SendClipboardHistory(
    const std::vector<ClipboardHistoryItem>& items) {
  return Send(message::ClipboardHistory(items));
}

bool PhoneService::RequestPhotos() { return Send(message::PhotosRequest(60)); }

bool PhoneService::RequestPhoto(const std::string& id) {
  return Send(message::PhotoRequest(id));
}

bool PhoneService::DismissNotification(const std::string& key) {
  return Send(message::NotificationDismiss(key));
}

bool PhoneService::NotificationAction(const std::string& key, int index,
                                      const std::string& text) {
  return Send(message::NotificationAction(key, index, text));
}

std::string PhoneService::SendFile(const std::wstring& path, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return std::string{};
  };
  if (!Connected()) return fail("Your phone is not connected.");
  std::error_code ec;
  const std::filesystem::path file(path);
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || !std::filesystem::is_regular_file(file, ec)) {
    return fail("Only files can be sent to the phone.");
  }
  if (size > kMaxTransferBytes) return fail("The file is larger than 40 MB.");
  const std::string id = HexEncode(crypto::RandomBytes(6));
  const std::string name = Narrow(file.filename().wstring());
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard lock(mutex_);
  if (!running_) return fail("Phone Connection is off.");
  const auto session = active_;
  if (!session) return fail("Your phone is not connected.");
  std::erase_if(workers_, [](const Worker& worker) { return worker.done->load(); });
  if (workers_.size() >= kMaxWorkers) return fail("Too many phone transfers are in progress.");
  workers_.push_back(Worker{
      std::jthread([this, path, id, name, done, session] {
        const auto bytes = ReadFileBytes(path, kMaxTransferBytes);
        if (!bytes || !SendToSession(session, message::FileSend(id, name), *bytes)) {
          Event event;
          event.kind = EventKind::Error;
          event.id = id;
          event.text = bytes ? "Could not send " + name + " to your phone."
                             : "Could not read " + name + ".";
          Emit(std::move(event));
        }
        done->store(true);
      }),
      done});
  return id;
}

bool PhoneService::Ring(bool start) { return Send(message::Ring(start)); }

bool PhoneService::MediaCommand(const std::string& command, long long position) {
  return Send(message::MediaCommand(command, position));
}

bool PhoneService::MediaVolume(int volume) {
  return Send(message::MediaVolume(volume));
}

bool PhoneService::RequestSmsThreads() { return Send(message::SmsThreadsRequest()); }

bool PhoneService::RequestSmsMessages(const std::string& thread, int limit) {
  return Send(message::SmsMessagesRequest(thread, limit));
}

bool PhoneService::SendSms(const std::string& ref, const std::string& address,
                           const std::string& body) {
  return Send(message::SmsSend(ref, address, body));
}

bool PhoneService::CallReject() { return Send(message::CallReject()); }
bool PhoneService::CallSilence() { return Send(message::CallSilence()); }

bool PhoneService::ListFiles(const std::string& remotePath) {
  return Send(message::FilesListRequest(remotePath));
}

bool PhoneService::RequestFile(const std::string& remotePath) {
  return Send(message::FileRequest(remotePath));
}

// ------------------------------------------------------------------ threads

void PhoneService::AcceptLoop(std::stop_token stop) {
  const SOCKET listener = static_cast<SOCKET>(listenSocket_);
  while (!stop.stop_requested()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{0, 500 * 1000};
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) break;
    if (ready == 0) continue;
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    BOOL noDelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(mutex_);
    std::erase_if(workers_, [](const Worker& worker) { return worker.done->load(); });
    if (!running_ || workers_.size() >= kMaxWorkers) {
      closesocket(client);
      continue;
    }
    workers_.push_back(Worker{
        std::jthread([this, client, done] {
          HandleConnection(static_cast<std::uintptr_t>(client));
          done->store(true);
        }),
        done});
  }
}

void PhoneService::BeaconLoop(std::stop_token stop) {
  const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp == INVALID_SOCKET) return;
  BOOL broadcast = TRUE;
  setsockopt(udp, SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
  std::mutex waitMutex;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    bool anyDevice = false;
    std::string pcId;
    {
      std::lock_guard lock(mutex_);
      anyDevice = !devices_.empty() || !pairingToken_.empty();
      pcId = pcId_;
    }
    if (anyDevice) {
      const std::string beacon = BuildBeacon(pcId, port_);
      std::vector<ULONG> targets{htonl(INADDR_BROADCAST)};
      for (const auto& adapter : EnumerateAdapters()) {
        targets.push_back(adapter.broadcast);
      }
      for (const ULONG target : targets) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kBeaconPort);
        address.sin_addr.s_addr = target;
        sendto(udp, beacon.data(), static_cast<int>(beacon.size()), 0,
               reinterpret_cast<sockaddr*>(&address), sizeof(address));
      }
    }
    std::unique_lock lock(waitMutex);
    wake.wait_for(lock, stop, kBeaconInterval, [] { return false; });
  }
  closesocket(udp);
}

void PhoneService::HandleConnection(std::uintptr_t rawSocket) {
  const SOCKET socket = static_cast<SOCKET>(rawSocket);
  auto session = std::make_shared<Session>();
  session->socket = socket;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return;
    }
    sessions_.push_back(session);
  }
  SetTimeout(socket, kHandshakeTimeoutMs);

  FrameDecoder decoder;
  std::uint8_t buffer[64 * 1024];
  bool first = true;
  Bytes firstBytes;
  bool open = true;
  while (open && running_) {
    const int received = recv(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (received <= 0) break;
    if (first) {
      firstBytes.insert(firstBytes.end(), buffer, buffer + received);
      if (firstBytes.size() < 4) continue;
      first = false;
      if (LooksLikeHttp(firstBytes.data(), firstBytes.size())) {
        ServeHttp(rawSocket, firstBytes);
        open = false;
        break;
      }
      decoder.Push(firstBytes.data(), firstBytes.size());
      firstBytes.clear();
    } else {
      decoder.Push(buffer, static_cast<size_t>(received));
    }
    while (open) {
      auto frame = decoder.Next(session->state == Session::State::Open
                                    ? kMaxFrameBytes : kMaxHandshakeBytes);
      if (!frame) {
        if (decoder.Failed()) open = false;
        break;
      }
      if (session->state == Session::State::Open) {
        HandleSessionFrame(session, *frame);
      } else if (!HandlePlainFrame(session, *frame)) {
        open = false;
      } else if (session->state == Session::State::Open) {
        SetTimeout(socket, kSessionIdleTimeoutMs);
      }
    }
  }

  bool wasActive = false;
  std::string deviceName;
  {
    std::lock_guard lock(mutex_);
    std::erase(sessions_, session);
    if (active_ == session) {
      active_.reset();
      wasActive = true;
      deviceName = session->deviceName;
    }
  }
  shutdown(socket, SD_BOTH);
  if (wasActive && running_) {
    Event event;
    event.kind = EventKind::Disconnected;
    event.deviceId = session->deviceId;
    event.deviceName = deviceName;
    Emit(std::move(event));
  }
}

bool PhoneService::HandlePlainFrame(const std::shared_ptr<Session>& session,
                                    const Bytes& frame) {
  const auto root = json::Parse(std::string_view(
      reinterpret_cast<const char*>(frame.data()), frame.size()));
  if (!root || root->type != json::Value::Type::Object) return false;
  const std::string type = JsonString(*root, "type");
  const auto sendPlain = [&](const std::string& json) {
    return SendAll(session->socket, EncodeFrame(ToBytes(json)));
  };
  const auto reject = [&](std::string_view code, std::string_view message) {
    sendPlain(Json("error").Str("code", code).Str("message", message).Build());
    return false;
  };

  if (type == "pair" && session->state == Session::State::Hello) {
    const auto phonePublic = Base64UrlDecode(JsonString(*root, "pub"));
    const auto proof = Base64UrlDecode(JsonString(*root, "proof"));
    const std::string deviceId = JsonString(*root, "deviceId");
    std::string name = JsonString(*root, "name");
    if (name.empty()) name = "Android phone";
    if (name.size() > 80) name.resize(80);
    if (!phonePublic || !proof || deviceId.empty() || deviceId.size() > 64) {
      return reject("bad-request", "Malformed pairing request.");
    }
    Bytes token;
    Bytes privateKey;
    Bytes publicKey;
    {
      std::lock_guard lock(mutex_);
      if (!pairingToken_.empty() && NowMs() <= pairingExpiresAt_) token = pairingToken_;
      privateKey = pcPrivateKey_;
      publicKey = pcPublicKey_;
    }
    if (token.empty()) {
      return reject("expired", "This pairing code expired. Show a new code on your PC.");
    }
    const Bytes pairLabel = ToBytes(kPairProofLabel);
    const Bytes expected = crypto::HmacSha256(
        token, Concat({&pairLabel, &*phonePublic, &publicKey}));
    if (!crypto::ConstantTimeEquals(expected, *proof)) {
      return reject("bad-proof", "This pairing code is not valid anymore.");
    }
    auto pair = crypto::EcdhKeyPair::Import(privateKey);
    const auto shared = pair ? pair->Agree(*phonePublic) : std::nullopt;
    if (!shared) return reject("bad-key", "The phone key could not be used.");
    PairedDevice device;
    device.id = deviceId;
    device.name = name;
    device.linkKey = crypto::Hkdf(*shared, token, kLinkKeyInfo, 32);
    device.pairedAt = NowMs();
    const Bytes pairedLabel = ToBytes(kPairedProofLabel);
    const Bytes confirm = crypto::HmacSha256(
        device.linkKey, Concat({&pairedLabel, &*phonePublic}));
    bool consumed = false;
    {
      std::lock_guard lock(mutex_);
      if (pairingToken_ == token && NowMs() <= pairingExpiresAt_) {
        pairingToken_.clear();
        pairingExpiresAt_ = 0;
        std::erase_if(devices_, [&](const auto& existing) { return existing.id == deviceId; });
        devices_.push_back(device);
        consumed = true;
      }
    }
    if (!consumed) return reject("expired", "This pairing code has already been used or replaced.");
    if (!SaveState()) {
      std::lock_guard lock(mutex_);
      std::erase_if(devices_, [&](const auto& existing) {
        return existing.id == deviceId && existing.linkKey == device.linkKey;
      });
      return reject("storage-error", "Could not save the pairing on the PC. Show a new code and try again.");
    }
    Event event;
    event.kind = EventKind::Paired;
    event.deviceId = device.id;
    event.deviceName = device.name;
    Emit(std::move(event));
    return sendPlain(Json("paired")
                         .Str("proof", Base64UrlEncode(confirm))
                         .Str("pcName", config_.pcName)
                         .Str("pcId", pcId_)
                         .Build());
  }

  if (type == "hello" && session->state == Session::State::Hello) {
    const std::string deviceId = JsonString(*root, "deviceId");
    const auto nonce = Base64UrlDecode(JsonString(*root, "nonce"));
    if (!nonce || nonce->size() != 16) return reject("bad-request", "Malformed hello.");
    std::optional<PairedDevice> device;
    {
      std::lock_guard lock(mutex_);
      for (const auto& candidate : devices_) {
        if (candidate.id == deviceId) device = candidate;
      }
      if (device) {
        session->deviceId = device->id;
        session->deviceName = device->name;
        session->linkKey = device->linkKey;
      }
    }
    if (!device) {
      return reject("unpaired", "This phone is not paired with this PC anymore.");
    }
    session->phoneNonce = *nonce;
    session->pcNonce = crypto::RandomBytes(16);
    session->state = Session::State::Auth;
    return sendPlain(Json("challenge")
                         .Str("nonce", Base64UrlEncode(session->pcNonce))
                         .Str("pcName", config_.pcName)
                         .Build());
  }

  if (type == "auth" && session->state == Session::State::Auth) {
    const auto mac = Base64UrlDecode(JsonString(*root, "mac"));
    const Bytes phoneLabel = ToBytes(kPhoneAuthLabel);
    const Bytes pcLabel = ToBytes(kPcAuthLabel);
    const Bytes expected = crypto::HmacSha256(
        session->linkKey, Concat({&phoneLabel, &session->phoneNonce, &session->pcNonce}));
    if (!mac || !crypto::ConstantTimeEquals(expected, *mac)) {
      return reject("bad-auth", "Authentication failed. Pair the phone again.");
    }
    const Bytes salt = Concat({&session->phoneNonce, &session->pcNonce});
    session->recvKey = crypto::Hkdf(session->linkKey, salt, kPhoneToPcInfo, 32);
    session->sendKey = crypto::Hkdf(session->linkKey, salt, kPcToPhoneInfo, 32);
    const Bytes pcMac = crypto::HmacSha256(
        session->linkKey, Concat({&pcLabel, &session->phoneNonce, &session->pcNonce}));
    {
      std::lock_guard lock(session->sendMutex);
      if (!sendPlain(Json("welcome")
                         .Str("mac", Base64UrlEncode(pcMac))
                         .Str("pcName", config_.pcName)
                         .Build())) {
        return false;
      }
      session->state = Session::State::Open;
    }
    std::shared_ptr<Session> previous;
    {
      std::lock_guard lock(mutex_);
      const bool stillPaired = std::any_of(devices_.begin(), devices_.end(), [&](const auto& device) {
        return device.id == session->deviceId && device.linkKey == session->linkKey;
      });
      if (!running_ || !stillPaired) return false;
      previous = std::exchange(active_, session);
    }
    if (previous && previous != session) shutdown(previous->socket, SD_BOTH);
    Event event;
    event.kind = EventKind::Connected;
    event.deviceId = session->deviceId;
    event.deviceName = session->deviceName;
    Emit(std::move(event));
    return true;
  }

  return reject("bad-state", "Unexpected message.");
}

void PhoneService::HandleSessionFrame(const std::shared_ptr<Session>& session,
                                      const Bytes& frame) {
  {
    std::lock_guard lock(mutex_);
    if (!running_ || active_ != session ||
        std::none_of(devices_.begin(), devices_.end(), [&](const auto& device) {
          return device.id == session->deviceId && device.linkKey == session->linkKey;
        })) return;
  }
  const auto plain = crypto::AesGcmDecrypt(
      session->recvKey, CounterNonce(session->recvCounter++), frame);
  if (!plain) {
    shutdown(session->socket, SD_BOTH);
    return;
  }
  const auto payload = UnpackPayload(*plain);
  if (!payload) return;
  const auto root = json::Parse(payload->json);
  if (!root || root->type != json::Value::Type::Object) return;
  if (JsonString(*root, "type") == "ping") {
    SendToSession(session, Json("pong").Int("time", NowMs()).Build());
    return;
  }
  auto parsed = ParseSessionMessage(*root, payload->binary, NowMs());
  if (!parsed) return;  // unknown message types are ignored for forward compatibility
  Event event = std::move(*parsed);
  const std::string reportedName = std::move(event.deviceName);
  event.deviceId = session->deviceId;
  event.deviceName = session->deviceName;

  if (event.kind == EventKind::Status) {
    if (!reportedName.empty() && reportedName != session->deviceName &&
        reportedName.size() <= 80) {
      event.deviceName = reportedName;
      {
        std::lock_guard lock(mutex_);
        session->deviceName = reportedName;
        for (auto& device : devices_) {
          if (device.id == session->deviceId) device.name = reportedName;
        }
      }
      SaveState();
    }
  } else if (event.kind == EventKind::PhotoSaved ||
             event.kind == EventKind::FileSaved ||
             event.kind == EventKind::RemoteFileSaved) {
    if (event.kind == EventKind::RemoteFileSaved && !event.text.empty()) {
      event.kind = EventKind::Error;  // the phone could not read the file
    } else if (payload->binary.size() > kMaxTransferBytes) {
      event.kind = EventKind::Error;
      event.text = "The file from your phone is larger than 40 MB.";
    } else {
      std::string name = event.photo.name;
      if (name.empty()) {
        name = event.kind == EventKind::PhotoSaved ? "phone-photo.jpg" : "phone-file";
      }
      event.path = UniqueDownloadPath(name);
      if (!WriteFileBytes(event.path, payload->binary)) {
        event.kind = EventKind::Error;
        event.text = "Could not save a file from your phone.";
      }
    }
  }
  Emit(std::move(event));
}

std::wstring PhoneService::UniqueDownloadPath(const std::string& name) const {
  const std::filesystem::path dir(config_.downloadsDir);
  const std::filesystem::path base(Widen(SanitizeFileName(name)));
  std::filesystem::path candidate = dir / base;
  std::error_code ec;
  for (int i = 2; std::filesystem::exists(candidate, ec) && i < 1000; ++i) {
    candidate = dir / (base.stem().wstring() + L" (" + std::to_wstring(i) + L")" +
                       base.extension().wstring());
  }
  return candidate.wstring();
}

void PhoneService::ServeHttp(std::uintptr_t rawSocket, const Bytes& firstBytes) {
  const SOCKET socket = static_cast<SOCKET>(rawSocket);
  std::string request(firstBytes.begin(), firstBytes.end());
  char buffer[2048];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
    const int received = recv(socket, buffer, sizeof(buffer), 0);
    if (received <= 0) return;
    request.append(buffer, static_cast<size_t>(received));
  }
  const size_t pathStart = 4;
  const size_t pathEnd = request.find(' ', pathStart);
  const std::string path =
      pathEnd == std::string::npos ? "/" : request.substr(pathStart, pathEnd - pathStart);

  if (path == "/app.apk" || path == "/FeatherCast-Phone.apk") {
    // Streamed in chunks so the APK is never held in memory as a whole.
    std::error_code ec;
    const std::filesystem::path apkPath(config_.apkPath);
    const auto size = std::filesystem::file_size(apkPath, ec);
    std::ifstream apk(apkPath, std::ios::binary);
    if (!ec && apk) {
      if (!SendAll(socket,
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/vnd.android.package-archive\r\n"
                   "Content-Disposition: attachment; filename=\"FeatherCast-Phone.apk\"\r\n"
                   "Content-Length: " + std::to_string(size) +
                       "\r\nConnection: close\r\n\r\n")) {
        return;
      }
      std::vector<char> chunk(64 * 1024);
      while (apk) {
        apk.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read = static_cast<size_t>(apk.gcount());
        if (read == 0 ||
            !SendAll(socket, reinterpret_cast<const std::uint8_t*>(chunk.data()), read)) {
          break;
        }
      }
      return;
    }
  }
  const std::string body =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>FeatherCast Phone</title><style>body{font-family:sans-serif;"
      "background:#15161b;color:#eee;padding:32px;text-align:center}"
      "a{display:inline-block;margin-top:24px;padding:16px 28px;border-radius:14px;"
      "background:#5b6cff;color:#fff;text-decoration:none;font-size:18px}</style>"
      "</head><body><h1>FeatherCast Phone</h1><p>Install the app, then scan the "
      "pairing code shown on your PC.</p><a href=\"/app.apk\">Download app</a>"
      "</body></html>";
  const bool found = path == "/" || path == "/index.html";
  SendAll(socket, std::string(found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
                      "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                      body);
}

}  // namespace feathercast::phone
