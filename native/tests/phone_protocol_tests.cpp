#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "phone_crypto.hpp"
#include "phone_messages.hpp"
#include "phone_protocol.hpp"
#include "phone_service.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <barrier>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace feathercast::phone;
namespace crypto = feathercast::phone::crypto;

namespace {

Bytes Hex(std::string_view text) { return *HexDecode(text); }

std::string QueryParam(const std::string& uri, const std::string& key) {
  const std::string needle = key + "=";
  size_t pos = uri.find("?" + needle);
  if (pos == std::string::npos) pos = uri.find("&" + needle);
  if (pos == std::string::npos) return {};
  pos += needle.size() + 1;
  const size_t end = uri.find('&', pos);
  return uri.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

void TestEncodings() {
  const Bytes data = ToBytes("any carnal pleas");
  assert(Base64UrlEncode(data) == "YW55IGNhcm5hbCBwbGVhcw");
  assert(*Base64UrlDecode("YW55IGNhcm5hbCBwbGVhcw") == data);
  assert(*Base64UrlDecode("YW55IGNhcm5hbCBwbGVhcw==") == data);
  const Bytes binary{0xfb, 0xff, 0x00, 0x10};
  assert(Base64UrlEncode(binary) == "-_8AEA");
  assert(*Base64UrlDecode("-_8AEA") == binary);
  assert(!Base64UrlDecode("a"));
  assert(HexEncode(binary) == "fbff0010");
  assert(*HexDecode("FBFF0010") == binary);
  assert(UrlEncode("My PC #1") == "My%20PC%20%231");
}

void TestFraming() {
  FrameDecoder decoder;
  const Bytes one = EncodeFrame(ToBytes("hello"));
  const Bytes two = EncodeFrame(ToBytes("world!"));
  Bytes stream = one;
  stream.insert(stream.end(), two.begin(), two.end());
  decoder.Push(stream.data(), 3);
  assert(!decoder.Next());
  decoder.Push(stream.data() + 3, stream.size() - 3);
  assert(*decoder.Next() == ToBytes("hello"));
  assert(*decoder.Next() == ToBytes("world!"));
  assert(!decoder.Next());

  // A large transfer must not leave its buffer allocated afterwards.
  FrameDecoder large;
  const Bytes big = EncodeFrame(Bytes(4 * 1024 * 1024, 0x5a));
  for (size_t at = 0; at < big.size(); at += 64 * 1024) {
    large.Push(big.data() + at, std::min<size_t>(64 * 1024, big.size() - at));
  }
  const auto bigFrame = large.Next();
  assert(bigFrame && bigFrame->size() == 4 * 1024 * 1024);
  assert(large.Capacity() <= 256 * 1024);
  large.Push(one.data(), one.size());
  assert(*large.Next() == ToBytes("hello"));

  FrameDecoder hostile;
  const std::uint8_t huge[4] = {0xff, 0xff, 0xff, 0xff};
  hostile.Push(huge, 4);
  assert(!hostile.Next());
  assert(hostile.Failed());

  FrameDecoder handshake;
  Bytes largeHandshake;
  AppendU32(largeHandshake, 64 * 1024 + 1);
  handshake.Push(largeHandshake.data(), largeHandshake.size());
  assert(!handshake.Next(64 * 1024));
  assert(handshake.Failed());

  const Bytes packed = PackPayload("{\"type\":\"x\"}", Bytes{1, 2, 3});
  const auto payload = UnpackPayload(packed);
  assert(payload && payload->json == "{\"type\":\"x\"}");
  assert((payload->binary == Bytes{1, 2, 3}));
  assert(!UnpackPayload(Bytes{0, 0, 0, 9, 'a'}));

  const Bytes nonce = CounterNonce(0x0102);
  assert(nonce.size() == 12 && nonce[10] == 0x01 && nonce[11] == 0x02 && nonce[0] == 0);

  const std::string json =
      Json("t").Str("s", "a\"b\n").Int("n", -5).Bool("b", true).Build();
  const auto parsed = feathercast::json::Parse(json);
  assert(parsed && JsonString(*parsed, "s") == "a\"b\n");
  assert(JsonInt(*parsed, "n") == -5 && JsonBool(*parsed, "b"));
}

void TestCrypto() {
  // RFC 4231 test case 2.
  assert(HexEncode(crypto::HmacSha256(ToBytes("Jefe"),
                                      ToBytes("what do ya want for nothing?"))) ==
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // RFC 5869 test case 1.
  const Bytes okm = crypto::Hkdf(Bytes(22, 0x0b), Hex("000102030405060708090a0b0c"),
                                 std::string_view("\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9", 10),
                                 42);
  assert(HexEncode(okm) ==
         "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
         "34007208d5b887185865");

  // AES-256-GCM, NIST test vector (key/iv all zero, 16 zero bytes).
  const auto sealed = crypto::AesGcmEncrypt(Bytes(32, 0), Bytes(12, 0), Bytes(16, 0));
  assert(sealed && HexEncode(*sealed) ==
                       "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919");
  const Bytes key = crypto::RandomBytes(32);
  const auto message = crypto::AesGcmEncrypt(key, CounterNonce(7), ToBytes("secret"));
  assert(message);
  assert(*crypto::AesGcmDecrypt(key, CounterNonce(7), *message) == ToBytes("secret"));
  assert(!crypto::AesGcmDecrypt(key, CounterNonce(8), *message));
  Bytes tampered = *message;
  tampered[0] ^= 1;
  assert(!crypto::AesGcmDecrypt(key, CounterNonce(7), tampered));
  assert(crypto::AesGcmEncrypt(key, CounterNonce(1), Bytes{}));

  auto a = crypto::EcdhKeyPair::Generate();
  auto b = crypto::EcdhKeyPair::Generate();
  assert(a && b && a->PublicKey().size() == 65 && a->PublicKey()[0] == 0x04);
  const auto ab = a->Agree(b->PublicKey());
  const auto ba = b->Agree(a->PublicKey());
  assert(ab && ba && *ab == *ba && ab->size() == 32);
  auto restored = crypto::EcdhKeyPair::Import(a->ExportPrivate());
  assert(restored && restored->PublicKey() == a->PublicKey());
  assert(*restored->Agree(b->PublicKey()) == *ab);
  assert(!a->Agree(Bytes(65, 0)));

  const auto sealedSecret = crypto::Protect(ToBytes("link"));
  assert(sealedSecret && *crypto::Unprotect(*sealedSecret) == ToBytes("link"));
}

// A minimal phone client used to exercise the real service over loopback.
class FakePhone {
 public:
  explicit FakePhone(std::uint16_t port) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    connected_ = connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    DWORD timeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
  }
  ~FakePhone() { closesocket(socket_); }

  bool Connected() const { return connected_; }

  void SendPlain(const std::string& json) {
    const Bytes frame = EncodeFrame(ToBytes(json));
    send(socket_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  }

  std::optional<Bytes> ReadFrame() {
    for (;;) {
      if (auto frame = decoder_.Next()) return frame;
      char buffer[4096];
      const int received = recv(socket_, buffer, sizeof(buffer), 0);
      if (received <= 0) return std::nullopt;
      decoder_.Push(reinterpret_cast<std::uint8_t*>(buffer), static_cast<size_t>(received));
    }
  }

  std::optional<feathercast::json::Value> ReadPlain() {
    const auto frame = ReadFrame();
    if (!frame) return std::nullopt;
    return feathercast::json::Parse(
        std::string_view(reinterpret_cast<const char*>(frame->data()), frame->size()));
  }

  void SendSealed(const std::string& json, const Bytes& binary = {}) {
    const auto sealed =
        crypto::AesGcmEncrypt(sendKey, CounterNonce(sendCounter++), PackPayload(json, binary));
    const Bytes frame = EncodeFrame(*sealed);
    send(socket_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  }

  std::optional<Payload> ReadSealed() {
    const auto frame = ReadFrame();
    if (!frame) return std::nullopt;
    const auto plain = crypto::AesGcmDecrypt(recvKey, CounterNonce(recvCounter++), *frame);
    if (!plain) return std::nullopt;
    return UnpackPayload(*plain);
  }

  bool Authenticate(const std::string& deviceId, const Bytes& linkKey) {
    return BeginAuthentication(deviceId, linkKey) && CompleteAuthentication();
  }

  bool BeginAuthentication(const std::string& deviceId, const Bytes& linkKey) {
    linkKey_ = linkKey;
    phoneNonce_ = crypto::RandomBytes(16);
    SendPlain(Json("hello").Str("deviceId", deviceId).Str("nonce", Base64UrlEncode(phoneNonce_)).Build());
    const auto challenge = ReadPlain();
    if (!challenge || JsonString(*challenge, "type") != "challenge") return false;
    pcNonce_ = *Base64UrlDecode(JsonString(*challenge, "nonce"));
    return true;
  }

  bool CompleteAuthentication() {
    const Bytes phoneLabel = ToBytes(kPhoneAuthLabel);
    const Bytes pcLabel = ToBytes(kPcAuthLabel);
    SendPlain(Json("auth")
                  .Str("mac", Base64UrlEncode(crypto::HmacSha256(
                                  linkKey_, Concat({&phoneLabel, &phoneNonce_, &pcNonce_}))))
                  .Build());
    const auto welcome = ReadPlain();
    if (!welcome || JsonString(*welcome, "type") != "welcome") return false;
    const Bytes expected =
        crypto::HmacSha256(linkKey_, Concat({&pcLabel, &phoneNonce_, &pcNonce_}));
    if (*Base64UrlDecode(JsonString(*welcome, "mac")) != expected) return false;
    const Bytes salt = Concat({&phoneNonce_, &pcNonce_});
    sendKey = crypto::Hkdf(linkKey_, salt, kPhoneToPcInfo, 32);
    recvKey = crypto::Hkdf(linkKey_, salt, kPcToPhoneInfo, 32);
    return true;
  }

  Bytes sendKey;
  Bytes recvKey;
  std::uint64_t sendCounter = 0;
  std::uint64_t recvCounter = 0;

 private:
  SOCKET socket_ = INVALID_SOCKET;
  bool connected_ = false;
  FrameDecoder decoder_;
  Bytes linkKey_;
  Bytes phoneNonce_;
  Bytes pcNonce_;
};

// The fixtures below are shared with MessagesTest.kt in android/protocol/src/test:
// phone → PC strings are exactly what the Kotlin builders produce, and PC →
// phone strings are exactly what these builders produce.
Event ParseFixture(std::string_view text, const Bytes& binary = {}) {
  const auto root = feathercast::json::Parse(text);
  assert(root);
  const auto event = ParseSessionMessage(*root, binary, 5000);
  assert(event);
  return *event;
}

void TestMessageFixtures() {
  {
    const auto event = ParseFixture(
        R"({"type":"status","battery":76,"charging":true,"name":"Pixel","features":["media","sms"]})");
    assert(event.kind == EventKind::Status && event.battery == 76 && event.charging);
    assert(event.deviceName == "Pixel");
    assert((event.features == std::vector<std::string>{"media", "sms"}));
  }
  {
    const auto event = ParseFixture(
        R"({"type":"notification.posted","key":"k1","app":"com.whatsapp","appName":"WhatsApp","title":"Anna","text":"Hi","time":1000,"actions":[{"i":0,"title":"Reply","reply":true},{"i":1,"title":"Mark as read","reply":false}]})",
        Bytes{1, 2});
    assert(event.kind == EventKind::NotificationPosted);
    const auto& info = event.notification;
    assert(info.key == "k1" && info.appName == "WhatsApp" && info.time == 1000);
    assert(info.actions.size() == 2 && info.actions[0].reply && !info.actions[1].reply);
    assert(info.actions[1].index == 1 && info.actions[1].title == "Mark as read");
    assert(info.iconPng.size() == 2);
  }
  {
    const auto event = ParseFixture(
        R"({"type":"file.received","id":"ab12","name":"a.txt","ok":false,"error":"Storage full"})");
    assert(event.kind == EventKind::FileDelivered && event.id == "ab12" && !event.ok);
    assert(event.text == "Storage full" && event.photo.name == "a.txt");
  }
  {
    const auto event = ParseFixture(R"({"type":"ring.state","ringing":true})");
    assert(event.kind == EventKind::RingState && event.ok);
  }
  {
    const auto event = ParseFixture(
        R"({"type":"media.state","app":"com.spotify.music","appName":"Spotify","title":"Song","artist":"Band","playing":true,"pos":1500,"dur":200000,"posAt":4000,"vol":7,"volMax":15})",
        Bytes{9});
    const auto& media = event.media;
    assert(event.kind == EventKind::MediaState && media.active && media.playing);
    assert(media.title == "Song" && media.artist == "Band" && media.appName == "Spotify");
    assert(media.position == 1500 && media.duration == 200000 && media.positionAt == 4000);
    assert(media.volume == 7 && media.volumeMax == 15 && media.artJpeg.size() == 1);
    const auto none = ParseFixture(R"({"type":"media.none"})");
    assert(none.kind == EventKind::MediaState && !none.media.active);
  }
  {
    const auto event = ParseFixture(
        R"({"type":"sms.threads","items":[{"thread":"12","address":"+4917012345","name":"Anna","snippet":"See you","time":3000,"unread":true}]})");
    assert(event.kind == EventKind::SmsThreads && event.smsThreads.size() == 1);
    const auto& thread = event.smsThreads[0];
    assert(thread.thread == "12" && thread.address == "+4917012345" && thread.name == "Anna");
    assert(thread.snippet == "See you" && thread.time == 3000 && thread.unread);
  }
  {
    const auto event = ParseFixture(
        R"({"type":"sms.messages","thread":"12","items":[{"id":"7","body":"Hello","time":2000,"out":true}]})");
    assert(event.kind == EventKind::SmsMessages && event.id == "12");
    assert(event.smsMessages.size() == 1 && event.smsMessages[0].outgoing &&
           event.smsMessages[0].body == "Hello");
  }
  {
    const auto event = ParseFixture(
        R"({"type":"sms.received","thread":"12","address":"+4917012345","name":"Anna","body":"Hi","time":6000})");
    assert(event.kind == EventKind::SmsReceived && event.id == "12");
    assert(event.smsThreads.size() == 1 && event.smsThreads[0].snippet == "Hi");
  }
  {
    const auto event = ParseFixture(R"({"type":"sms.sent","ref":"r1","ok":true})");
    assert(event.kind == EventKind::SmsSent && event.id == "r1" && event.ok);
  }
  {
    const auto event = ParseFixture(
        R"({"type":"call.state","state":"ringing","number":"+4917012345","name":"Anna"})");
    assert(event.kind == EventKind::Call && event.call.state == CallState::Ringing);
    assert(event.call.name == "Anna" && event.call.number == "+4917012345");
  }
  {
    const auto event = ParseFixture(
        R"({"type":"files.list","path":"/Download","items":[{"name":"Music","dir":true,"size":0,"time":100},{"name":"a.pdf","dir":false,"size":2048,"time":200}]})");
    assert(event.kind == EventKind::RemoteFileList && event.remotePath == "/Download");
    assert(event.files.size() == 2 && event.files[0].directory && event.files[1].size == 2048);
    const auto denied = ParseFixture(
        R"({"type":"files.list","path":"/","error":"No access","items":[]})");
    assert(denied.text == "No access" && denied.files.empty());
  }
  {
    const auto event =
        ParseFixture(R"({"type":"file.data","path":"/Download/a.pdf","name":"a.pdf"})");
    assert(event.kind == EventKind::RemoteFileSaved && event.remotePath == "/Download/a.pdf");
  }
  {
    const auto root = feathercast::json::Parse(R"({"type":"future.thing"})");
    assert(!ParseSessionMessage(*root, {}, 0));
  }

  // PC → phone.
  assert(message::NotificationAction("k1", 0, "On my way") ==
         R"({"type":"notification.action","key":"k1","i":0,"text":"On my way"})");
  assert(message::NotificationAction("k1", 1, "") ==
         R"({"type":"notification.action","key":"k1","i":1})");
  assert(message::FileSend("ab12", "a.txt") ==
         R"({"type":"file.send","id":"ab12","name":"a.txt"})");
  assert(message::Ring(true) == R"({"type":"ring.start"})");
  assert(message::Ring(false) == R"({"type":"ring.stop"})");
  assert(message::MediaCommand("toggle", -1) == R"({"type":"media.command","cmd":"toggle"})");
  assert(message::MediaCommand("seek", 30000) ==
         R"({"type":"media.command","cmd":"seek","pos":30000})");
  assert(message::MediaVolume(9) == R"({"type":"media.volume","vol":9})");
  assert(message::SmsThreadsRequest() == R"({"type":"sms.threads.request"})");
  assert(message::SmsMessagesRequest("12", 50) ==
         R"({"type":"sms.messages.request","thread":"12","limit":50})");
  assert(message::SmsSend("r1", "+4917012345", "Hi") ==
         R"({"type":"sms.send","ref":"r1","address":"+4917012345","body":"Hi"})");
  assert(message::CallReject() == R"({"type":"call.reject"})");
  assert(message::CallSilence() == R"({"type":"call.silence"})");
  assert(message::FilesListRequest("/Download") ==
         R"({"type":"files.list.request","path":"/Download"})");
  assert(message::FileRequest("/Download/a.pdf") ==
         R"({"type":"file.request","path":"/Download/a.pdf"})");

  assert(RemotePathJoin("/", "Download") == "/Download");
  assert(RemotePathJoin("/Download", "a.pdf") == "/Download/a.pdf");
  assert(RemotePathParent("/Download/Music") == "/Download");
  assert(RemotePathParent("/Download") == "/");
  assert(RemotePathParent("/") == "/");
}

void TestServiceEndToEnd() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("feathercast-phone-test-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(dir);

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Event> events;
  const auto waitFor = [&](EventKind kind) -> std::optional<Event> {
    std::unique_lock lock(mutex);
    const bool found = cv.wait_for(lock, std::chrono::seconds(5), [&] {
      for (const auto& event : events) {
        if (event.kind == kind) return true;
      }
      return false;
    });
    if (!found) return std::nullopt;
    for (auto it = events.begin(); it != events.end(); ++it) {
      if (it->kind == kind) {
        Event event = *it;
        events.erase(it);
        return event;
      }
    }
    return std::nullopt;
  };

  PhoneService service;
  ServiceConfig config;
  config.pcName = "Test PC";
  config.stateFile = (dir / L"phone-link.dat").wstring();
  config.downloadsDir = (dir / L"downloads").wstring();
  config.apkPath = (dir / L"missing.apk").wstring();
  config.port = 47890;
  config.onEvent = [&](Event event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
    cv.notify_all();
  };
  std::string error;
  assert(service.Start(config, &error));

  const std::string uri = service.CreatePairingUri();
  assert(uri.starts_with("feathercast://pair?v=1&n=Test%20PC"));
  const Bytes pcPublic = *Base64UrlDecode(QueryParam(uri, "k"));
  const Bytes token = *Base64UrlDecode(QueryParam(uri, "t"));
  assert(pcPublic.size() == 65 && token.size() == 16);

  auto phoneKey = crypto::EcdhKeyPair::Generate();
  const Bytes pairLabel = ToBytes(kPairProofLabel);
  const std::string deviceId = "0123456789abcdef";
  Bytes linkKey;
  {
    FakePhone phone(service.Port());
    assert(phone.Connected());
    // A wrong proof is rejected.
    phone.SendPlain(Json("pair")
                        .Str("deviceId", deviceId)
                        .Str("name", "Pixel")
                        .Str("pub", Base64UrlEncode(phoneKey->PublicKey()))
                        .Str("proof", Base64UrlEncode(Bytes(32, 1)))
                        .Build());
    const auto rejected = phone.ReadPlain();
    assert(rejected && JsonString(*rejected, "type") == "error");
  }
  {
    FakePhone phone(service.Port());
    const Bytes proof = crypto::HmacSha256(
        token, Concat({&pairLabel, &phoneKey->PublicKey(), &pcPublic}));
    phone.SendPlain(Json("pair")
                        .Str("deviceId", deviceId)
                        .Str("name", "Pixel")
                        .Str("pub", Base64UrlEncode(phoneKey->PublicKey()))
                        .Str("proof", Base64UrlEncode(proof))
                        .Build());
    const auto paired = phone.ReadPlain();
    assert(paired && JsonString(*paired, "type") == "paired");
    const auto shared = phoneKey->Agree(pcPublic);
    linkKey = crypto::Hkdf(*shared, token, kLinkKeyInfo, 32);
    const Bytes pairedLabel = ToBytes(kPairedProofLabel);
    assert(*Base64UrlDecode(JsonString(*paired, "proof")) ==
           crypto::HmacSha256(linkKey, Concat({&pairedLabel, &phoneKey->PublicKey()})));
    assert(waitFor(EventKind::Paired));
    assert(service.Devices().size() == 1);

    assert(phone.Authenticate(deviceId, linkKey));
    assert(waitFor(EventKind::Connected));
    assert(service.Connected());

    phone.SendSealed(Json("notification.posted")
                         .Str("key", "k1")
                         .Str("appName", "Messages")
                         .Str("title", "Anna")
                         .Str("text", "Hi there")
                         .Int("time", 1000)
                         .Build(),
                     Bytes{0x89, 'P', 'N', 'G'});
    const auto posted = waitFor(EventKind::NotificationPosted);
    assert(posted && posted->notification.title == "Anna" &&
           posted->notification.iconPng.size() == 4);

    phone.SendSealed(Json("clipboard.set").Str("text", "from phone").Build());
    const auto clip = waitFor(EventKind::Clipboard);
    assert(clip && clip->text == "from phone");

    phone.SendSealed(Json("file.send").Str("name", "../evil.txt").Build(), ToBytes("data"));
    const auto saved = waitFor(EventKind::FileSaved);
    assert(saved && std::filesystem::path(saved->path).parent_path() ==
                        std::filesystem::path(config.downloadsDir));
    assert(std::filesystem::file_size(saved->path) == 4);

    assert(service.SendClipboard("from pc"));
    const auto received = phone.ReadSealed();
    assert(received);
    const auto root = feathercast::json::Parse(received->json);
    assert(root && JsonString(*root, "type") == "clipboard.set" &&
           JsonString(*root, "text") == "from pc");

    // PC → phone file transfer and delivery receipt.
    const auto outgoing = dir / L"to-phone.txt";
    std::ofstream(outgoing, std::ios::binary) << "hello phone";
    const std::string transferId = service.SendFile(outgoing.wstring());
    assert(!transferId.empty());
    const auto fileMessage = phone.ReadSealed();
    assert(fileMessage && fileMessage->binary == ToBytes("hello phone"));
    const auto fileRoot = feathercast::json::Parse(fileMessage->json);
    assert(fileRoot && JsonString(*fileRoot, "type") == "file.send" &&
           JsonString(*fileRoot, "id") == transferId &&
           JsonString(*fileRoot, "name") == "to-phone.txt");
    phone.SendSealed(Json("file.received").Str("id", transferId).Bool("ok", true).Build());
    const auto delivered = waitFor(EventKind::FileDelivered);
    assert(delivered && delivered->ok && delivered->id == transferId);

    // Files requested from phone storage are saved to Downloads.
    phone.SendSealed(
        Json("file.data").Str("path", "/Download/report.pdf").Str("name", "report.pdf").Build(),
        ToBytes("pdf!"));
    const auto remote = waitFor(EventKind::RemoteFileSaved);
    assert(remote && remote->remotePath == "/Download/report.pdf" &&
           std::filesystem::file_size(remote->path) == 4);
    phone.SendSealed(
        Json("file.data").Str("path", "/x").Str("name", "x").Str("error", "Too large").Build());
    const auto remoteError = waitFor(EventKind::Error);
    assert(remoteError && remoteError->text == "Too large");

    phone.SendSealed(Json("ping").Build());
    const auto pong = phone.ReadSealed();
    assert(pong && pong->json.find("pong") != std::string::npos);
  }
  assert(waitFor(EventKind::Disconnected));

  // The pairing token is single use.
  {
    FakePhone phone(service.Port());
    const Bytes proof = crypto::HmacSha256(
        token, Concat({&pairLabel, &phoneKey->PublicKey(), &pcPublic}));
    phone.SendPlain(Json("pair")
                        .Str("deviceId", "other")
                        .Str("pub", Base64UrlEncode(phoneKey->PublicKey()))
                        .Str("proof", Base64UrlEncode(proof))
                        .Build());
    const auto rejected = phone.ReadPlain();
    assert(rejected && JsonString(*rejected, "code") == "expired");
  }
  // Unknown devices are refused.
  {
    FakePhone phone(service.Port());
    assert(!phone.Authenticate("unknown-device", linkKey));
  }

  // Racing valid requests must consume an invite exactly once.
  {
    const auto racingUri = service.CreatePairingUri();
    const auto racingToken = *Base64UrlDecode(QueryParam(racingUri, "t"));
    const auto proof = crypto::HmacSha256(
        racingToken, Concat({&pairLabel, &phoneKey->PublicKey(), &pcPublic}));
    std::barrier ready(8);
    std::atomic<int> pairedCount = 0;
    std::vector<std::jthread> clients;
    for (int i = 0; i < 8; ++i) {
      clients.emplace_back([&, i] {
        FakePhone phone(service.Port());
        ready.arrive_and_wait();
        phone.SendPlain(Json("pair")
                            .Str("deviceId", "racing-" + std::to_string(i))
                            .Str("pub", Base64UrlEncode(phoneKey->PublicKey()))
                            .Str("proof", Base64UrlEncode(proof)).Build());
        const auto reply = phone.ReadPlain();
        if (reply && JsonString(*reply, "type") == "paired") ++pairedCount;
      });
    }
    clients.clear();
    assert(pairedCount == 1);
    for (const auto& device : service.Devices()) {
      if (device.id.starts_with("racing-")) service.Forget(device.id);
    }
    assert(service.Devices().size() == 1);
  }

  // Identity and devices survive a restart.
  service.Stop();
  PhoneService restarted;
  assert(restarted.Start(config, &error));
  assert(restarted.Devices().size() == 1);
  {
    FakePhone phone(restarted.Port());
    assert(phone.Authenticate(deviceId, linkKey));
  }
  // Unpairing also revokes a challenge that was already issued.
  {
    FakePhone pending(restarted.Port());
    assert(pending.BeginAuthentication(deviceId, linkKey));
    restarted.Forget(deviceId);
    assert(!pending.CompleteAuthentication());
  }
  assert(restarted.Devices().empty());
  restarted.Stop();

  // Stop must join idle connection workers, and an invite cannot survive it.
  assert(restarted.Start(config, &error));
  const auto stoppedUri = restarted.CreatePairingUri();
  {
    FakePhone idle(restarted.Port());
    const auto before = std::chrono::steady_clock::now();
    restarted.Stop();
    assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
  }
  assert(restarted.Start(config, &error));
  {
    FakePhone phone(restarted.Port());
    const auto stoppedToken = *Base64UrlDecode(QueryParam(stoppedUri, "t"));
    const auto proof = crypto::HmacSha256(
        stoppedToken, Concat({&pairLabel, &phoneKey->PublicKey(), &pcPublic}));
    phone.SendPlain(Json("pair").Str("deviceId", "old-invite")
                        .Str("pub", Base64UrlEncode(phoneKey->PublicKey()))
                        .Str("proof", Base64UrlEncode(proof)).Build());
    const auto reply = phone.ReadPlain();
    assert(reply && JsonString(*reply, "code") == "expired");
  }
  restarted.Stop();

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
  TestEncodings();
  TestFraming();
  TestCrypto();
  TestMessageFixtures();
  TestServiceEndToEnd();
  std::puts("phone protocol tests passed");
  return 0;
}
