#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "phone_service.hpp"
#include "file_transfer.hpp"

#include "phone_crypto.hpp"
#include "phone_messages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>

namespace feathercast::phone {
namespace {

constexpr long long kPairingLifetimeMs = 5 * 60 * 1000;
constexpr DWORD kSessionIdleTimeoutMs = 75 * 1000;
constexpr auto kHandshakeTimeout = std::chrono::seconds(20);
constexpr auto kHttpTransferTimeout = std::chrono::minutes(5);
constexpr DWORD kHttpSendTimeoutMs = 30 * 1000;
constexpr std::size_t kMaxHandshakeBytes = 64 * 1024;
constexpr std::size_t kMaxWorkers = 16;
// Unauthenticated connections (handshakes and APK downloads) may hold only a
// few of the workers, so a slow or hostile client cannot lock out the phone.
constexpr std::size_t kMaxPendingHandshakes = 4;
constexpr std::size_t kMaxQueuedMessages = 1024;
constexpr std::size_t kMaxQueuedBytes = 64 * 1024 * 1024;
// The phone sends nothing when it cannot deliver a photo or file, so requests
// that get no answer end with an error instead of spinning forever.
constexpr long long kRequestTimeoutMs = 2 * 60 * 1000;
constexpr auto kBeaconInterval = std::chrono::seconds(3);

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

long long SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Remaining time until deadline as a socket timeout; 0 once it has passed.
DWORD RemainingMs(std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
  return left <= 0 ? 0 : static_cast<DWORD>(left);
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

// Gather the four-byte header and body in one blocking send, without copying
// a photo/file ciphertext into another frame-sized allocation.
bool SendFrame(SOCKET socket, const Bytes& body) {
  if (body.size() > kMaxFrameBytes) return false;
  const auto size = static_cast<std::uint32_t>(body.size());
  std::array<char, 4> header{
      static_cast<char>(size >> 24), static_cast<char>(size >> 16),
      static_cast<char>(size >> 8), static_cast<char>(size)};
  WSABUF buffers[] = {
      {4, header.data()},
      {static_cast<ULONG>(body.size()),
       reinterpret_cast<char*>(const_cast<std::uint8_t*>(body.data()))},
  };
  DWORD first = 0;
  const DWORD count = body.empty() ? 1 : 2;
  while (first < count) {
    DWORD sent = 0;
    if (WSASend(socket, buffers + first, count - first, &sent, 0,
                nullptr, nullptr) == SOCKET_ERROR || sent == 0) {
      return false;
    }
    while (first < count && sent >= buffers[first].len) {
      sent -= buffers[first].len;
      ++first;
    }
    if (first < count && sent != 0) {
      buffers[first].buf += sent;
      buffers[first].len -= sent;
    }
  }
  return true;
}

bool SendAll(SOCKET socket, std::string_view text) {
  return SendAll(socket, reinterpret_cast<const std::uint8_t*>(text.data()),
                 text.size());
}

void SetSendTimeout(SOCKET socket, DWORD milliseconds) {
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
}

void SetTimeout(SOCKET socket, DWORD milliseconds) {
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
  SetSendTimeout(socket, milliseconds);
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

// Temporary files get a random name of their own next to the target, so they
// never truncate a user's file. A crash leaves only names the next start removes.
constexpr std::wstring_view kTempPrefix = L".feathercast-phone-";
constexpr std::wstring_view kTempSuffix = L".tmp";

// Writes data to a new temporary file in dir. Returns its path, or empty.
std::filesystem::path WriteTempFile(const std::filesystem::path& dir, const Bytes& data,
                                    bool flush) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::filesystem::path temp =
        dir / (std::wstring(kTempPrefix) + Widen(HexEncode(crypto::RandomBytes(8))) +
               std::wstring(kTempSuffix));
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_EXISTS) continue;
      return {};
    }
    bool ok = true;
    for (std::size_t offset = 0; ok && offset < data.size();) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1 << 20));
      DWORD written = 0;
      ok = WriteFile(file, data.data() + offset, chunk, &written, nullptr) && written == chunk;
      offset += written;
    }
    if (ok && flush) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) {
      DeleteFileW(temp.c_str());
      return {};
    }
    return temp;
  }
  return {};
}

// Replaces path as a whole: readers see the old or the new content, never a part.
bool ReplaceFileContents(const std::wstring& path, const Bytes& data) {
  const std::filesystem::path target(path);
  const auto temp = WriteTempFile(target.parent_path(), data, true);
  if (temp.empty()) return false;
  if (!MoveFileExW(temp.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

// Saves data as a new file in dir and never replaces an existing one: "name.ext",
// then "name (2).ext" and so on. The file carries the mark of the web, so Windows
// and Office treat it like any other download. Returns the final path or empty.
std::wstring SaveNewFile(const std::filesystem::path& dir, const std::string& name,
                         const Bytes& data) {
  const auto temp = WriteTempFile(dir, data, false);
  if (temp.empty()) return {};
  MarkFileFromInternet(temp.wstring());  // the stream moves with the file
  const std::filesystem::path base(Widen(name));
  const std::wstring stem = base.stem().wstring();
  const std::wstring extension = base.extension().wstring();
  for (int i = 1; i < 1000; ++i) {
    const std::filesystem::path candidate =
        dir / (i == 1 ? base.wstring() : stem + L" (" + std::to_wstring(i) + L")" + extension);
    if (MoveFileExW(temp.c_str(), candidate.c_str(), 0)) return candidate.wstring();
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) break;
  }
  DeleteFileW(temp.c_str());
  return {};
}

void RemoveStaleTempFiles(const std::filesystem::path& dir) {
  if (dir.empty()) return;
  const std::wstring pattern =
      (dir / (std::wstring(kTempPrefix) + L"*" + std::wstring(kTempSuffix))).wstring();
  WIN32_FIND_DATAW data{};
  const HANDLE find =
      FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        std::wstring_view(data.cFileName).starts_with(kTempPrefix)) {
      DeleteFileW((dir / data.cFileName).c_str());
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
}

// The storage path the phone resolves a request to (normalizeStoragePath in
// the Kotlin protocol), so its answer can be matched to the request.
std::optional<std::string> NormalizeStoragePath(std::string_view path) {
  std::string out;
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = path.find_first_of("/\\", start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view part = path.substr(start, end - start);
    if (part == "..") return std::nullopt;
    if (!part.empty() && part != ".") {
      out += '/';
      out += part;
    }
    start = end + 1;
  }
  return out.empty() ? std::string("/") : out;
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

bool MarkFileFromInternet(const std::wstring& path) {
  static constexpr char kZone[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
  constexpr DWORD kSize = sizeof(kZone) - 1;
  const HANDLE file = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, kZone, kSize, &written, nullptr) && written == kSize;
  CloseHandle(file);
  return ok;
}

struct PhoneService::Session {
  enum class State { Hello, Auth, Open };
  SOCKET socket = INVALID_SOCKET;
  std::atomic<State> state{State::Hello};
  std::string deviceId;
  std::string deviceName;
  Bytes linkKey;
  Bytes phoneNonce;
  Bytes pcNonce;
  Bytes sendKey;
  Bytes recvKey;
  std::uint64_t recvCounter = 0;
  std::string screenId;
  std::atomic<bool> fileStreamSupported = false;
  std::atomic<std::uint64_t> transferEpoch = 0;
  std::atomic<std::size_t> outgoingTransfers = 0;
  std::mutex cancellationMutex;
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancellations;
  std::atomic<std::shared_ptr<transfer::Receiver>> incomingTransfers;

  // Everything sent after the handshake goes through one writer thread, in
  // order, so callers never block on the network or on each other. A failed or
  // partial write leaves the stream unusable: the session is closed and every
  // queued message fails instead of following a gap.
  struct Outgoing {
    std::string json;
    Bytes binary;
    bool plain = false;  // the welcome, sent before any sealed frame
    std::shared_ptr<std::promise<bool>> delivered;  // for callers that wait
  };
  std::mutex sendMutex;
  std::condition_variable_any sendWake;
  std::deque<Outgoing> outgoing;
  std::size_t outgoingBytes = 0;
  bool sendFailed = false;
  std::uint64_t sendCounter = 0;  // writer thread only
  std::jthread writer;

  // Starts the writer with the welcome as its first frame. Called once, by the
  // connection thread, before the session becomes visible to senders.
  void Open(std::string welcome) {
    {
      std::lock_guard lock(sendMutex);  // Stop() may close the session meanwhile
      Outgoing first;
      first.json = std::move(welcome);
      first.plain = true;
      outgoingBytes += first.json.size();
      outgoing.push_back(std::move(first));
    }
    writer = std::jthread([this](std::stop_token stop) { WriteLoop(stop); });
    state = State::Open;
  }

  // Queues a sealed message. With wait, blocks until there is room and the
  // message was written (true) or the session failed (false).
  bool Enqueue(std::string json, Bytes binary, bool wait) {
    std::shared_ptr<std::promise<bool>> delivered;
    std::future<bool> result;
    if (wait) {
      delivered = std::make_shared<std::promise<bool>>();
      result = delivered->get_future();
    }
    const std::size_t size = json.size() + binary.size();
    {
      std::unique_lock lock(sendMutex);
      const auto fits = [&] {
        return outgoing.empty() ||
               (outgoing.size() < kMaxQueuedMessages && outgoingBytes + size <= kMaxQueuedBytes);
      };
      if (wait) sendWake.wait(lock, [&] { return sendFailed || fits(); });
      if (sendFailed || !fits()) return false;
      outgoing.push_back(Outgoing{std::move(json), std::move(binary), false, delivered});
      outgoingBytes += size;
    }
    sendWake.notify_all();
    return !wait || result.get();
  }

  // Ends the session: queued messages fail and blocked socket calls return.
  void Close() {
    std::deque<Outgoing> dropped;
    {
      std::lock_guard lock(sendMutex);
      sendFailed = true;
      dropped.swap(outgoing);
      outgoingBytes = 0;
    }
    sendWake.notify_all();
    if (socket != INVALID_SOCKET) {
      shutdown(socket, SD_BOTH);
      CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
    }
    for (auto& item : dropped) {
      if (item.delivered) item.delivered->set_value(false);
    }
    if (auto incoming = incomingTransfers.load()) incoming->Cancel();
  }

  void WriteLoop(std::stop_token stop) {
    for (;;) {
      Outgoing item;
      std::uint64_t counter = 0;
      {
        std::unique_lock lock(sendMutex);
        sendWake.wait(lock, stop, [&] { return sendFailed || !outgoing.empty(); });
        if (sendFailed || outgoing.empty()) return;
        item = std::move(outgoing.front());
        outgoing.pop_front();
        outgoingBytes -= item.json.size() + item.binary.size();
        if (!item.plain) counter = sendCounter++;
      }
      sendWake.notify_all();  // room for waiting senders
      bool ok = false;
      if (item.plain) {
        ok = SendFrame(socket, ToBytes(item.json));
      } else {
        Bytes payload = PackPayload(item.json, item.binary);
        item.binary = {};
        const auto sealed = crypto::AesGcmEncrypt(sendKey, CounterNonce(counter), payload);
        payload = {};
        ok = sealed && SendFrame(socket, *sealed);
      }
      if (item.delivered) item.delivered->set_value(ok);
      if (!ok) {
        Close();
        return;
      }
    }
  }

  // Senders and the connection thread retain the session while using its
  // socket. Close only after all of them release it, so a reused handle stays
  // safe. The writer never owns the session, so this never runs on it.
  ~Session() {
    Close();
    if (writer.joinable()) {
      writer.request_stop();
      writer.join();
    }
    if (socket != INVALID_SOCKET) closesocket(socket);
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
  stateReady_ = false;
  RemoveStaleTempFiles(std::filesystem::path(config_.stateFile).parent_path());
  if (!config_.downloadsDir.empty()) RemoveStaleTempFiles(config_.downloadsDir);
  switch (LoadState()) {
    case StateLoad::Loaded:
      break;
    case StateLoad::Missing: {
      auto pair = crypto::EcdhKeyPair::Generate();
      if (!pair) return fail("Could not create the phone link key.");
      std::lock_guard lock(mutex_);
      pcPrivateKey_ = pair->ExportPrivate();
      pcPublicKey_ = pair->PublicKey();
      pcId_ = HexEncode(crypto::RandomBytes(8));
      devices_.clear();
      break;
    }
    case StateLoad::Failed:
      // Never replace pairings that could not be read: the cause (a locked file,
      // an unavailable profile key) is often temporary.
      return fail("Could not read the saved phone pairings (phone-link.dat). The file was left "
                  "unchanged; turn Phone Connection off and on again to retry. If the file is "
                  "damaged, delete it and pair your phones again.");
  }
  stateReady_ = true;
  if (!SaveState()) return fail("Could not save the phone link identity.");

  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return fail("Could not create a network socket.");
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  // Port 0 lets Windows pick a free port (used by the tests).
  bool bound = false;
  for (unsigned candidate = config_.port;
       candidate <= 65535 && candidate < static_cast<unsigned>(config_.port) + 10 && !bound;
       ++candidate) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(candidate));
    bound = bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  }
  sockaddr_in local{};
  int localSize = sizeof(local);
  if (!bound || listen(listener, 8) != 0 ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&local), &localSize) != 0) {
    closesocket(listener);
    return fail("The phone link port is already in use.");
  }
  port_ = ntohs(local.sin_port);
  const WSAEVENT acceptEvent = WSACreateEvent();
  if (acceptEvent == WSA_INVALID_EVENT ||
      WSAEventSelect(listener, acceptEvent, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
    if (acceptEvent != WSA_INVALID_EVENT) WSACloseEvent(acceptEvent);
    closesocket(listener);
    return fail("Could not monitor the phone link port.");
  }
  acceptEvent_ = acceptEvent;
  listenSocket_ = listener;
  running_ = true;
  acceptThread_ = std::jthread([this](std::stop_token stop) { AcceptLoop(stop); });
  beaconThread_ = std::jthread([this](std::stop_token stop) { BeaconLoop(stop); });
  screenSendThread_ = std::jthread([this](std::stop_token stop) { ScreenSendLoop(stop); });
  return true;
}

void PhoneService::Stop() {
  if (!running_.exchange(false)) return;
  acceptThread_.request_stop();
  if (acceptEvent_) WSASetEvent(acceptEvent_);
  beaconThread_.request_stop();
  screenSendThread_.request_stop();
  screenWake_.notify_all();
  // The accept loop checks running_ before adding workers. Join it before
  // collecting workers or closing the listener it is still using.
  if (acceptThread_.joinable()) acceptThread_.join();
  closesocket(static_cast<SOCKET>(listenSocket_));
  listenSocket_ = ~std::uintptr_t{0};
  if (acceptEvent_) WSACloseEvent(acceptEvent_);
  acceptEvent_ = nullptr;
  std::vector<Worker> workers;
  {
    std::lock_guard lock(mutex_);
    for (const auto& session : sessions_) session->Close();
    workers = std::move(workers_);
    workers_.clear();
  }
  if (beaconThread_.joinable()) beaconThread_.join();
  if (screenSendThread_.joinable()) screenSendThread_.join();
  workers.clear();  // joins
  // Sessions are released outside the lock; their destructors join writers.
  std::vector<std::shared_ptr<Session>> ended;
  std::deque<std::pair<std::shared_ptr<Session>, std::string>> commands;
  {
    std::lock_guard lock(mutex_);
    ended = std::move(sessions_);
    sessions_.clear();
    ended.push_back(std::move(active_));
    ended.push_back(std::move(screen_));
    active_.reset();
    screen_.reset();
    screenOwner_.reset();
    screenId_.clear();
    screenKey_.clear();
    screenInputs_.clear();
    commands.swap(screenCommands_);
    pairingToken_.clear();
    pairingExpiresAt_ = 0;
    pendingPhotos_.clear();
    pendingFiles_.clear();
  }
}

void PhoneService::Emit(Event event) {
  if (config_.onEvent) config_.onEvent(std::move(event));
}

// ------------------------------------------------------------------- state

PhoneService::StateLoad PhoneService::LoadState() {
  std::error_code ec;
  const auto status = std::filesystem::status(std::filesystem::path(config_.stateFile), ec);
  if (status.type() == std::filesystem::file_type::not_found) return StateLoad::Missing;
  const auto sealed = ReadFileBytes(config_.stateFile);
  if (!sealed) return StateLoad::Failed;
  const auto plain = crypto::Unprotect(*sealed);
  if (!plain) return StateLoad::Failed;
  const auto root = json::Parse(std::string_view(
      reinterpret_cast<const char*>(plain->data()), plain->size()));
  if (!root || root->type != json::Value::Type::Object) return StateLoad::Failed;
  const auto privateKey = Base64UrlDecode(JsonString(*root, "key"));
  if (!privateKey) return StateLoad::Failed;
  auto pair = crypto::EcdhKeyPair::Import(*privateKey);
  if (!pair) return StateLoad::Failed;
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
  return StateLoad::Loaded;
}

bool PhoneService::SaveState() {
  std::lock_guard saveLock(stateSaveMutex_);
  // Until the saved state was read, writing would replace pairings this run
  // never saw.
  if (!stateReady_) return false;
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
  return sealed && ReplaceFileContents(config_.stateFile, *sealed);
}

// ----------------------------------------------------------------- queries

std::vector<std::string> LocalNetworkAddresses() {
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
  invite.hosts = LocalNetworkAddresses();
  // The phone cannot use an invite without an address; no token is issued.
  if (invite.hosts.empty()) return {};
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
  // "localhost" would point the phone at itself; without a network there is no URL.
  const auto hosts = LocalNetworkAddresses();
  if (hosts.empty()) return {};
  return "http://" + hosts.front() + ":" + std::to_string(port_) + "/app.apk";
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
      if (session->deviceId == deviceId) session->Close();
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

bool PhoneService::Send(std::string json, Bytes binary) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard lock(mutex_);
    session = active_;
  }
  return SendToSession(session, std::move(json), std::move(binary));
}

// Queues a sealed message for an authenticated session. Without wait, true
// means queued; the writer closes the session if it cannot be delivered.
bool PhoneService::SendToSession(const std::shared_ptr<Session>& session, std::string json,
                                 Bytes binary, bool wait) {
  if (!session || binary.size() > kMaxTransferBytes ||
      json.size() > kMaxFrameBytes - 20 - binary.size()) return false;
  {
    std::lock_guard lock(mutex_);
    if (!running_ || session->state != Session::State::Open ||
        (active_ != session && screen_ != session)) return false;
  }
  return session->Enqueue(std::move(json), std::move(binary), wait);
}

bool PhoneService::SendClipboard(const std::string& text) {
  return Send(message::Clipboard(text, NowMs()));
}

bool PhoneService::SendClipboardHistory(
    const std::vector<ClipboardHistoryItem>& items) {
  return Send(message::ClipboardHistory(items));
}

bool PhoneService::RequestPhotos() { return Send(message::PhotosRequest(60)); }

// Registered before sending, so even an immediate answer finds its request.
bool PhoneService::RequestPhoto(const std::string& id) {
  {
    std::lock_guard lock(mutex_);
    pendingPhotos_[id] = SteadyMs() + kRequestTimeoutMs;
  }
  if (Send(message::PhotoRequest(id))) return true;
  std::lock_guard lock(mutex_);
  pendingPhotos_.erase(id);
  return false;
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
  if (size > static_cast<std::uintmax_t>(transfer::kMaximumBytes)) return fail("The file is larger than 8 GB.");
  const std::string id = HexEncode(crypto::RandomBytes(6));
  const std::string name = Narrow(file.filename().wstring());
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard lock(mutex_);
  if (!running_) return fail("Phone Connection is off.");
  const auto session = active_;
  if (!session) return fail("Your phone is not connected.");
  const bool stream = session->fileStreamSupported.load();
  if (!stream && size > kMaxTransferBytes) return fail("Update the phone app to send files larger than 40 MB.");
  const auto epoch = session->transferEpoch.load();
  if (session->outgoingTransfers.fetch_add(1) >= transfer::kMaximumConcurrent) {
    --session->outgoingTransfers;
    return fail("Up to four files can be sent at once. Wait for a transfer to finish.");
  }
  const auto cancelled = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard cancellationLock(session->cancellationMutex);
    session->cancellations[id] = cancelled;
  }
  std::erase_if(workers_, [](const Worker& worker) { return worker.done->load(); });
  if (workers_.size() >= kMaxWorkers) {
    --session->outgoingTransfers;
    std::lock_guard cancellationLock(session->cancellationMutex);
    session->cancellations.erase(id);
    return fail("Too many phone transfers are in progress.");
  }
  workers_.push_back(Worker{
      std::jthread([this, path, id, name, done, session, stream, epoch, size, cancelled] {
        struct Cleanup {
          std::shared_ptr<Session> session;
          std::string id;
          ~Cleanup() {
            --session->outgoingTransfers;
            std::lock_guard lock(session->cancellationMutex);
            session->cancellations.erase(id);
          }
        } cleanup{session, id};
        if (stream) {
          std::ifstream file(std::filesystem::path(path), std::ios::binary);
          bool ok = static_cast<bool>(file) && SendToSession(session,
              Json("file.begin").Str("id", id).Str("name", name)
                  .Str("purpose", "file").Int("size", static_cast<long long>(size)).Build(), {}, true);
          long long offset = 0;
          int lastPercent = -1;
          while (ok && offset < static_cast<long long>(size) && session->transferEpoch.load() == epoch && !cancelled->load()) {
            Bytes bytes(static_cast<std::size_t>(std::min<long long>(
                static_cast<long long>(transfer::kChunkBytes), static_cast<long long>(size) - offset)));
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ok = file.gcount() == static_cast<std::streamsize>(bytes.size());
            if (ok) ok = SendToSession(session, Json("file.chunk").Str("id", id).Int("offset", offset).Build(), std::move(bytes), true);
            if (!ok) break;
            offset += file.gcount();
            const int percent = size == 0 ? 100 : static_cast<int>(offset * 100 / static_cast<long long>(size));
            if (percent != lastPercent) {
              lastPercent = percent;
              Event progress;
              progress.kind = EventKind::TransferProgress;
              progress.id = id;
              progress.photo.name = name;
              progress.transferredBytes = offset;
              progress.totalBytes = static_cast<long long>(size);
              Emit(std::move(progress));
            }
          }
          ok = ok && session->transferEpoch.load() == epoch && !cancelled->load() && offset == static_cast<long long>(size);
          if (ok) ok = SendToSession(session, Json("file.end").Str("id", id).Build(), {}, true);
          if (!ok) {
            SendToSession(session, Json("file.cancel").Str("id", id).Build());
            Event error;
            error.kind = EventKind::Error;
            error.id = id;
            error.text = session->transferEpoch.load() != epoch || cancelled->load() ? "File transfer cancelled." : "Could not send " + name + ".";
            Emit(std::move(error));
          }
          done->store(true);
          return;
        }
        auto bytes = ReadFileBytes(path, kMaxTransferBytes);
        const bool read = bytes.has_value();
        // Waits until the file was written to the connection or the link failed.
        if (!read || !SendToSession(session, message::FileSend(id, name), std::move(*bytes), true)) {
          Event event;
          event.kind = EventKind::Error;
          event.id = id;
          event.text = read ? "Could not send " + name + " to your phone."
                            : "Could not read " + name + ".";
          Emit(std::move(event));
        }
        done->store(true);
      }),
      done});
  return id;
}

void PhoneService::CancelTransfers() {
  std::shared_ptr<Session> session;
  std::vector<Event> cancelled;
  {
    std::lock_guard lock(mutex_);
    session = active_;
    for (const auto& [id, unused] : pendingPhotos_) {
      Event event;
      event.kind = EventKind::Error;
      event.photo.id = id;
      event.text = "Photo transfer cancelled.";
      cancelled.push_back(std::move(event));
    }
    for (const auto& [path, unused] : pendingFiles_) {
      Event event;
      event.kind = EventKind::Error;
      event.remotePath = path;
      event.text = "File transfer cancelled.";
      cancelled.push_back(std::move(event));
    }
    pendingPhotos_.clear();
    pendingFiles_.clear();
  }
  EmitAll(std::move(cancelled));
  if (!session) return;
  ++session->transferEpoch;
  if (auto incoming = session->incomingTransfers.load()) incoming->RequestCancel();
  SendToSession(session, Json("file.cancel").Str("id", "").Build());
  Event event;
  event.kind = EventKind::Error;
  event.id = "*";
  event.text = "File transfers cancelled.";
  Emit(std::move(event));
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
  {
    std::lock_guard lock(mutex_);
    pendingFiles_[remotePath] = PendingFile{
        NormalizeStoragePath(remotePath).value_or(remotePath), SteadyMs() + kRequestTimeoutMs};
  }
  if (Send(message::FileRequest(remotePath))) return true;
  std::lock_guard lock(mutex_);
  pendingFiles_.erase(remotePath);
  return false;
}

// Matches a photo or storage answer to its request and stops the request's
// timeout. A storage answer carries the path the phone resolved; the event
// gets the path the PC asked for, so the UI can match it exactly.
void PhoneService::SettleRequest(Event& event) {
  std::lock_guard lock(mutex_);
  if (event.kind == EventKind::PhotoSaved) {
    pendingPhotos_.erase(event.photo.id);
    return;
  }
  if (event.kind != EventKind::RemoteFileSaved || event.remotePath.empty()) return;
  if (pendingFiles_.erase(event.remotePath) > 0) return;
  const auto normalized = NormalizeStoragePath(event.remotePath);
  for (auto it = pendingFiles_.begin(); it != pendingFiles_.end(); ++it) {
    if (normalized && it->second.normalized == *normalized) {
      event.remotePath = it->first;
      pendingFiles_.erase(it);
      return;
    }
  }
}

// Takes the requests that ran out of time (all: every one, because the
// connection that would answer them ended). Called with mutex_ held, in the
// same section that ends a connection, so a request made for the next one stays.
std::vector<Event> PhoneService::TakeExpiredLocked(bool all) {
  std::vector<Event> expired;
  const long long now = SteadyMs();
  for (auto it = pendingPhotos_.begin(); it != pendingPhotos_.end();) {
    if (!all && it->second > now) { ++it; continue; }
    Event event;
    event.kind = EventKind::Error;
    event.photo.id = it->first;
    event.text = all ? "Your phone disconnected before the photo arrived."
                     : "Your phone did not send the photo in time.";
    expired.push_back(std::move(event));
    it = pendingPhotos_.erase(it);
  }
  for (auto it = pendingFiles_.begin(); it != pendingFiles_.end();) {
    if (!all && it->second.deadline > now) { ++it; continue; }
    Event event;
    event.kind = EventKind::Error;
    event.remotePath = it->first;
    event.text = all ? "Your phone disconnected before the file arrived."
                     : "Your phone did not send the file in time.";
    expired.push_back(std::move(event));
    it = pendingFiles_.erase(it);
  }
  return expired;
}

void PhoneService::EmitAll(std::vector<Event> events) {
  for (auto& event : events) {
    if (running_) Emit(std::move(event));
  }
}

void PhoneService::ExpireRequests() {
  std::vector<Event> expired;
  {
    std::lock_guard lock(mutex_);
    expired = TakeExpiredLocked(false);
  }
  EmitAll(std::move(expired));
}

void PhoneService::EmitScreenState(std::string id, std::string state, std::string detail) {
  ScreenPacket packet;
  packet.sessionId = std::move(id);
  packet.state = std::move(state);
  packet.detail = std::move(detail);
  if (config_.onScreenPacket) config_.onScreenPacket(std::move(packet));
}

std::string PhoneService::StartScreen(bool audio) {
  StopScreen();
  const auto id = HexEncode(crypto::RandomBytes(16));
  {
    std::lock_guard lock(mutex_);
    if (!running_ || !active_ || screenCommands_.size() >= 8) return {};
    screenId_ = id;
    screenKey_ = crypto::RandomBytes(32);
    if (screenKey_.size() != 32) { screenId_.clear(); return {}; }
    screenOwner_ = active_;
    screenExpiresAt_ = SteadyMs() + kScreenRequestLifetimeMs;
    screenCommands_.emplace_back(active_, Json("screen.start").Str("session", id)
        .Str("key", Base64UrlEncode(screenKey_)).Bool("audio", audio).Build());
  }
  EmitScreenState(id, "pending", "Open FeatherCast on your phone and approve screen sharing. The request expires in 60 seconds.");
  screenWake_.notify_all();
  return id;
}

void PhoneService::StopScreen(bool report) {
  std::string id;
  {
    std::lock_guard lock(mutex_);
    id = std::exchange(screenId_, {});
    screenKey_.clear();
    screenInputs_.clear();
    screenOwner_.reset();
    screenExpiresAt_ = 0;
    if (screen_) screen_->Close();
    screen_.reset();
    if (!id.empty() && active_ && screenCommands_.size() < 8) {
      screenCommands_.emplace_back(active_, Json("screen.stop").Str("session", id).Build());
    }
  }
  if (report && !id.empty()) EmitScreenState(std::move(id), "stopped", "Screen sharing stopped.");
  screenWake_.notify_all();
}

bool PhoneService::SendScreenInput(ScreenInput input) {
  if (!ValidScreenInput(input)) return false;
  {
    std::lock_guard lock(mutex_);
    if (!screen_ || input.sessionId != screenId_ || screenOwner_.lock() != active_) return false;
    if (input.action == "move" && !screenInputs_.empty() && screenInputs_.back().action == "move") {
      screenInputs_.back() = std::move(input);
    } else {
      if (screenInputs_.size() >= 128) {
        // Never replay stale gestures or text after a congested connection.
        screenInputs_.clear();
        input.action = "cancel";
        input.text.clear();
      }
      screenInputs_.push_back(std::move(input));
    }
  }
  screenWake_.notify_all();
  return true;
}

void PhoneService::ScreenSendLoop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::shared_ptr<Session> target;
    std::string json, expired;
    bool input = false;
    {
      std::unique_lock lock(mutex_);
      while (!stop.stop_requested() && screenCommands_.empty() &&
             screenInputs_.empty()) {
        const auto id = screenId_;
        const auto expiry = screenExpiresAt_;
        const auto connection = screen_.get();
        const auto changed = [&] {
          return !screenCommands_.empty() || !screenInputs_.empty() ||
                 screenId_ != id || screenExpiresAt_ != expiry ||
                 screen_.get() != connection;
        };
        if (!id.empty() && !connection) {
          const auto remaining = expiry - SteadyMs();
          if (remaining <= 0) break;
          screenWake_.wait_for(lock, stop, std::chrono::milliseconds(remaining),
                                changed);
        } else {
          screenWake_.wait(lock, stop, changed);
        }
      }
      if (stop.stop_requested()) break;
      if (!screenId_.empty() && !screen_ && SteadyMs() >= screenExpiresAt_) {
        expired = std::exchange(screenId_, {});
        screenExpiresAt_ = 0;
        screenKey_.clear();
        screenOwner_.reset();
        if (active_) screenCommands_.emplace_back(active_, Json("screen.stop").Str("session", expired).Build());
      }
      if (!screenInputs_.empty()) {
        auto next = std::move(screenInputs_.front());
        screenInputs_.pop_front();
        target = screen_;
        json = ScreenInputJson(next);
        input = true;
      } else if (!screenCommands_.empty()) {
        target = std::move(screenCommands_.front().first);
        json = std::move(screenCommands_.front().second);
        screenCommands_.pop_front();
      }
    }
    if (!expired.empty()) EmitScreenState(expired, "stopped", "Screen sharing request expired. Start again and approve it on your phone.");
    if (!target) continue;
    // Input waits for the screen connection, so moves keep coalescing while it
    // is congested; a failed write already closed that connection. Commands to
    // the phone connection are queued, and a full queue only drops the command.
    if (input) {
      SendToSession(target, std::move(json), {}, true);
    } else {
      SendToSession(target, std::move(json));
    }
  }
}

// ------------------------------------------------------------------ threads

void PhoneService::AcceptLoop(std::stop_token stop) {
  const SOCKET listener = static_cast<SOCKET>(listenSocket_);
  const WSAEVENT event = acceptEvent_;
  while (!stop.stop_requested()) {
    if (WSAWaitForMultipleEvents(1, &event, FALSE, WSA_INFINITE, FALSE) !=
            WSA_WAIT_EVENT_0 || stop.stop_requested()) break;
    WSANETWORKEVENTS events{};
    if (WSAEnumNetworkEvents(listener, event, &events) == SOCKET_ERROR ||
        (events.lNetworkEvents & FD_CLOSE)) break;
    if (!(events.lNetworkEvents & FD_ACCEPT)) continue;
    if (events.iErrorCode[FD_ACCEPT_BIT] != 0) break;
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    // Accepted sockets inherit the listener's event association and
    // nonblocking mode. Session timeouts and I/O require a blocking socket.
    u_long blocking = 0;
    if (WSAEventSelect(client, nullptr, 0) == SOCKET_ERROR ||
        ioctlsocket(client, FIONBIO, &blocking) == SOCKET_ERROR) {
      closesocket(client);
      continue;
    }
    BOOL noDelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(mutex_);
    std::erase_if(workers_, [](const Worker& worker) { return worker.done->load(); });
    if (!running_ || workers_.size() >= kMaxWorkers ||
        pendingHandshakes_ >= kMaxPendingHandshakes) {
      closesocket(client);
      continue;
    }
    ++pendingHandshakes_;  // released by HandleConnection
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
    ExpireRequests();
    std::unique_lock lock(waitMutex);
    wake.wait_for(lock, stop, kBeaconInterval, [] { return false; });
  }
  closesocket(udp);
}

void PhoneService::HandleConnection(std::uintptr_t rawSocket) {
  const SOCKET socket = static_cast<SOCKET>(rawSocket);
  // The slot AcceptLoop reserved for an unauthenticated connection; returned
  // once the session is authenticated or the connection ends.
  struct PendingSlot {
    PhoneService* service;
    bool held = true;
    void Release() {
      if (!held) return;
      held = false;
      std::lock_guard lock(service->mutex_);
      --service->pendingHandshakes_;
    }
    ~PendingSlot() { Release(); }
  } pending{this};
  auto session = std::make_shared<Session>();
  session->socket = socket;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return;
    }
    sessions_.push_back(session);
  }
  // One deadline for the whole handshake, not per read, so a client that
  // trickles bytes cannot hold the connection open.
  const auto handshakeDeadline = std::chrono::steady_clock::now() + kHandshakeTimeout;

  FrameDecoder decoder;
  std::uint8_t buffer[64 * 1024];
  bool first = true;
  Bytes firstBytes;
  bool open = true;
  while (open && running_) {
    if (session->state != Session::State::Open) {
      const DWORD left = RemainingMs(handshakeDeadline);
      if (left == 0) break;
      SetTimeout(socket, left);
    }
    const int received = recv(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (received <= 0) break;
    if (first) {
      firstBytes.insert(firstBytes.end(), buffer, buffer + received);
      if (firstBytes.size() < 4) continue;
      first = false;
      if (LooksLikeHttp(firstBytes.data(), firstBytes.size())) {
        ServeHttp(rawSocket, firstBytes, handshakeDeadline);
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
                                    ? (session->screenId.empty() ? kMaxFrameBytes : kMaxScreenPacketBytes + 8192 + 20)
                                    : kMaxHandshakeBytes);
      if (!frame) {
        if (decoder.Failed()) open = false;
        break;
      }
      if (session->state == Session::State::Open) {
        HandleSessionFrame(session, *frame);
      } else if (!HandlePlainFrame(session, *frame)) {
        open = false;
      } else if (session->state == Session::State::Open) {
        SetTimeout(socket, session->screenId.empty() ? kSessionIdleTimeoutMs : 5000);
        pending.Release();
      }
    }
  }

  bool wasActive = false;
  std::string deviceName;
  std::string endedScreen;
  std::vector<Event> unanswered;
  {
    std::lock_guard lock(mutex_);
    std::erase(sessions_, session);
    if (screen_ == session || (active_ == session && !screenId_.empty())) {
      endedScreen = std::exchange(screenId_, {});
      screenKey_.clear();
      screenOwner_.reset();
      screenInputs_.clear();
      if (screen_ && screen_ != session) screen_->Close();
      screen_.reset();
    }
    if (active_ == session) {
      active_.reset();
      wasActive = true;
      deviceName = session->deviceName;
      unanswered = TakeExpiredLocked(true);  // answers would have come over this connection
    }
  }
  session->Close();
  if (!endedScreen.empty()) screenWake_.notify_all();
  if (!endedScreen.empty() && running_) EmitScreenState(endedScreen, "stopped", "The screen connection was lost. Start a new session to reconnect.");
  EmitAll(std::move(unanswered));
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
    session->screenId = JsonString(*root, "screen");
    if (!session->screenId.empty()) {
      bool allowed = false;
      {
        std::lock_guard lock(mutex_);
        allowed = active_ && active_->deviceId == deviceId && screenOwner_.lock() == active_ &&
            screenId_ == session->screenId && screenKey_.size() == 32 && !screen_ &&
            SteadyMs() < screenExpiresAt_;
        if (allowed) {
          session->deviceId = deviceId;
          session->deviceName = active_->deviceName;
          session->linkKey = screenKey_;
        }
      }
      if (!allowed) return reject("screen-expired", "This screen sharing request is no longer active.");
      session->phoneNonce = *nonce;
      session->pcNonce = crypto::RandomBytes(16);
      session->state = Session::State::Auth;
      return sendPlain(Json("challenge").Str("nonce", Base64UrlEncode(session->pcNonce))
          .Str("pcName", config_.pcName).Build());
    }
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
    // Session::Open queues the welcome as the first frame and only then makes
    // the session sendable, so no sealed frame can precede it. Senders find the
    // session (active_ or screen_) only after that.
    if (session->screenId.empty()) {
      session->incomingTransfers.store(std::make_shared<transfer::Receiver>(config_.downloadsDir));
    }
    std::string welcome = Json("welcome")
                              .Str("mac", Base64UrlEncode(pcMac))
                              .Str("pcName", config_.pcName)
                              .Bool("fileStream", true)
                              .Build();
    if (!session->screenId.empty()) {
      std::lock_guard lock(mutex_);
      if (!running_ || !active_ || screenOwner_.lock() != active_ ||
          active_->deviceId != session->deviceId || screenId_ != session->screenId ||
          screenKey_ != session->linkKey || screen_ || SteadyMs() >= screenExpiresAt_) return false;
      screenKey_.clear();  // one authentication only, including concurrent challenges
      session->Open(std::move(welcome));
      screen_ = session;
      screenWake_.notify_all();
      return true;  // a media connection never replaces the normal phone connection
    }
    std::shared_ptr<Session> previous;
    std::string endedScreen;
    std::vector<Event> unanswered;
    {
      std::lock_guard lock(mutex_);
      const bool stillPaired = std::any_of(devices_.begin(), devices_.end(), [&](const auto& device) {
        return device.id == session->deviceId && device.linkKey == session->linkKey;
      });
      if (!running_ || !stillPaired) return false;
      session->Open(std::move(welcome));
      previous = std::exchange(active_, session);
      // Answers would have come over the replaced connection.
      if (previous && previous != session) unanswered = TakeExpiredLocked(true);
      if (previous && !screenId_.empty()) {
        if (screen_) screen_->Close();
        screen_.reset();
        screenOwner_.reset();
        screenKey_.clear();
        endedScreen = std::exchange(screenId_, {});
        screenInputs_.clear();
      }
    }
    if (previous && previous != session) previous->Close();
    if (!endedScreen.empty()) screenWake_.notify_all();
    EmitAll(std::move(unanswered));
    if (!endedScreen.empty()) EmitScreenState(endedScreen, "stopped", "The phone connection changed. Start a new screen sharing session.");
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
    if (!session->screenId.empty()) {
      if (!running_ || screen_ != session || screenId_ != session->screenId ||
          !active_ || screenOwner_.lock() != active_ || active_->deviceId != session->deviceId) return;
    } else if (!running_ || active_ != session ||
        std::none_of(devices_.begin(), devices_.end(), [&](const auto& device) {
          return device.id == session->deviceId && device.linkKey == session->linkKey;
        })) return;
  }
  const auto plain = crypto::AesGcmDecrypt(
      session->recvKey, CounterNonce(session->recvCounter++), frame);
  if (!plain) {
    session->Close();
    return;
  }
  const auto payload = UnpackPayload(*plain);
  if (!payload) return;
  if (!session->screenId.empty()) {
    auto packet = ParseScreenPacket(*payload);
    if (!packet || packet->sessionId != session->screenId) {
      session->Close();
      return;
    }
    if (config_.onScreenPacket) config_.onScreenPacket(std::move(*packet));
    return;
  }
  const auto root = json::Parse(payload->json);
  if (!root || root->type != json::Value::Type::Object) return;
  if (const auto receiver = session->incomingTransfers.load()) receiver->Maintain();
  if (JsonString(*root, "type") == "ping") {
    SendToSession(session, Json("pong").Int("time", NowMs()).Build());
    return;
  }
  if (JsonString(*root, "type") == "screen.state") {
    auto packet = ParseScreenPacket(*payload);
    bool matches = false;
    {
      std::lock_guard lock(mutex_);
      matches = packet && packet->sessionId == screenId_ && screenOwner_.lock() == session;
    }
    if (matches && (packet->state == "stopped" || packet->state == "error")) StopScreen(false);
    if (matches && config_.onScreenPacket) config_.onScreenPacket(*packet);
    return;
  }
  const auto type = JsonString(*root, "type");
  if (type == "file.cancel") {
    const auto id = JsonString(*root, "id");
    if (id.empty()) ++session->transferEpoch;
    else {
      std::lock_guard lock(session->cancellationMutex);
      if (const auto found = session->cancellations.find(id); found != session->cancellations.end()) found->second->store(true);
    }
    if (auto incoming = session->incomingTransfers.load()) {
      for (const auto& progress : incoming->Cancel(id)) {
        Event event;
        event.id = progress.id;
        event.remotePath = progress.purpose == "storage" ? progress.reference : "";
        event.photo.id = progress.purpose == "photo" ? progress.reference : "";
        event.kind = progress.purpose == "storage" ? EventKind::RemoteFileSaved : EventKind::PhotoSaved;
        SettleRequest(event);
        event.kind = EventKind::Error;
        event.text = "File transfer cancelled on your phone.";
        Emit(std::move(event));
      }
    }
    if (id.empty()) {
      Event event;
      event.kind = EventKind::Error;
      event.id = "*";
      event.text = "File transfers cancelled on your phone.";
      Emit(std::move(event));
    }
    return;
  }
  if (type == "file.begin" || type == "file.chunk" || type == "file.end") {
    auto incoming = session->incomingTransfers.load();
    if (!incoming) return;
    transfer::Result result;
    if (type == "file.begin") {
      result = incoming->Begin(*root, SanitizeFileName(JsonString(*root, "name")));
    } else if (type == "file.chunk") {
      result = incoming->Chunk(*root, payload->binary);
    } else {
      result = incoming->Finish(JsonString(*root, "id"));
    }
    if (!result.error.empty() || result.complete) {
      Event event;
      event.id = result.progress.id;
      event.deviceId = session->deviceId;
      event.deviceName = session->deviceName;
      event.photo.name = result.progress.name;
      event.path = result.path;
      event.ok = result.complete;
      event.text = result.error;
      event.kind = result.complete ? EventKind::FileSaved : EventKind::Error;
      if (result.progress.purpose == "storage") {
        event.remotePath = result.progress.reference;
        event.kind = EventKind::RemoteFileSaved;
        SettleRequest(event);
        if (!result.complete) event.kind = EventKind::Error;
      } else if (result.progress.purpose == "photo") {
        event.photo.id = result.progress.reference;
        event.kind = EventKind::PhotoSaved;
        SettleRequest(event);
        if (!result.complete) event.kind = EventKind::Error;
      }
      if (result.complete) MarkFileFromInternet(result.path);
      SendToSession(session, Json("file.received").Str("id", result.progress.id)
          .Str("name", result.progress.name).Bool("ok", result.complete)
          .Str("error", result.error).Build());
      if (!result.complete) SendToSession(session, Json("file.cancel").Str("id", result.progress.id).Build());
      Emit(std::move(event));
    } else if (result.reportProgress) {
      {
        std::lock_guard lock(mutex_);
        if (auto pending = pendingFiles_.find(result.progress.reference); pending != pendingFiles_.end()) {
          pending->second.deadline = SteadyMs() + kRequestTimeoutMs;
        }
      }
      Event progress;
      progress.kind = EventKind::TransferProgress;
      progress.id = result.progress.id;
      progress.photo.name = result.progress.name;
      progress.transferredBytes = result.progress.bytes;
      progress.totalBytes = result.progress.total;
      Emit(std::move(progress));
    }
    return;
  }
  auto parsed = ParseSessionMessage(*root, payload->binary, NowMs());
  if (!parsed) return;  // unknown message types are ignored for forward compatibility
  Event event = std::move(*parsed);
  const std::string reportedName = std::move(event.deviceName);
  event.deviceId = session->deviceId;
  event.deviceName = session->deviceName;

  if (event.kind == EventKind::Status) {
    session->fileStreamSupported.store(std::find(event.features.begin(), event.features.end(),
        "file.stream.v1") != event.features.end());
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
    const bool incomingFile = event.kind == EventKind::FileSaved;
    SettleRequest(event);
    if (event.kind == EventKind::RemoteFileSaved && !event.text.empty()) {
      event.kind = EventKind::Error;  // the phone could not read the file
    } else if (payload->binary.size() > kMaxTransferBytes) {
      event.kind = EventKind::Error;
      event.text = "The file from your phone is larger than 40 MB.";
    } else {
      // Names come from the phone. They are made safe for Windows, a photo's
      // extension follows its content, and a storage file is named after the
      // requested path, as listed in the browser.
      const std::string name =
          event.kind == EventKind::PhotoSaved
              ? PhotoFileName(event.photo.name, payload->binary)
              : event.kind == EventKind::RemoteFileSaved ? RemoteFileName(event.remotePath)
                                                         : SanitizeFileName(event.photo.name);
      event.path = SaveNewFile(config_.downloadsDir, name, payload->binary);
      if (event.path.empty()) {
        event.kind = EventKind::Error;
        event.text = "Could not save a file from your phone.";
      }
    }
    // A failed incoming transfer has no photo; its id must not match one.
    if (incomingFile && event.kind == EventKind::Error) event.photo.id.clear();
  }
  Emit(std::move(event));
}

void PhoneService::ServeHttp(std::uintptr_t rawSocket, const Bytes& firstBytes,
                             std::chrono::steady_clock::time_point deadline) {
  const SOCKET socket = static_cast<SOCKET>(rawSocket);
  std::string request(firstBytes.begin(), firstBytes.end());
  char buffer[2048];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
    const DWORD left = RemainingMs(deadline);
    if (left == 0) return;
    SetTimeout(socket, left);
    const int received = recv(socket, buffer, sizeof(buffer), 0);
    if (received <= 0) return;
    request.append(buffer, static_cast<size_t>(received));
  }
  // Each write may wait a while for a slow phone, the whole download not forever.
  const auto transferDeadline = std::chrono::steady_clock::now() + kHttpTransferTimeout;
  SetSendTimeout(socket, kHttpSendTimeoutMs);
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
      while (apk && RemainingMs(transferDeadline) > 0) {
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
