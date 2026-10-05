#pragma once

// Wire format shared by FeatherCast and the FeatherCast Phone Android app.
// Pure helpers only (no sockets, no crypto) so the encoding can be tested.
//
// Stream framing:  [u32 big-endian length][frame bytes]
// Handshake frames are plaintext UTF-8 JSON. After "welcome" every frame is
// AES-256-GCM ciphertext whose plaintext is a Payload:
//   [u32 big-endian JSON length][JSON header][optional binary attachment]

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace feathercast::phone {

using Bytes = std::vector<std::uint8_t>;

inline constexpr int kProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 47800;
inline constexpr std::uint16_t kBeaconPort = 47801;
inline constexpr std::uint32_t kMaxFrameBytes = 48u * 1024u * 1024u;
inline constexpr std::string_view kBeaconPrefix = "FCAST1";

// HKDF info labels and MAC domain separators. Keep in sync with Protocol.kt.
inline constexpr std::string_view kLinkKeyInfo = "feathercast-link-v1";
inline constexpr std::string_view kPairProofLabel = "pair";
inline constexpr std::string_view kPairedProofLabel = "paired";
inline constexpr std::string_view kPhoneAuthLabel = "auth-phone";
inline constexpr std::string_view kPcAuthLabel = "auth-pc";
inline constexpr std::string_view kPhoneToPcInfo = "p2c";
inline constexpr std::string_view kPcToPhoneInfo = "c2p";

inline Bytes ToBytes(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

inline Bytes Concat(std::initializer_list<const Bytes*> parts) {
  Bytes out;
  for (const Bytes* part : parts) out.insert(out.end(), part->begin(), part->end());
  return out;
}

// ---------------------------------------------------------------- encodings

inline std::string Base64UrlEncode(const std::uint8_t* data, std::size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back(kAlphabet[v & 63]);
  }
  if (size - i == 1) {
    const std::uint32_t v = data[i] << 16;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
  } else if (size - i == 2) {
    const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
  }
  return out;
}

inline std::string Base64UrlEncode(const Bytes& data) {
  return Base64UrlEncode(data.data(), data.size());
}

// Accepts both the URL-safe and the standard alphabet, padding optional.
inline std::optional<Bytes> Base64UrlDecode(std::string_view text) {
  const auto value = [](char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-' || ch == '+') return 62;
    if (ch == '_' || ch == '/') return 63;
    return -1;
  };
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  if (text.size() % 4 == 1) return std::nullopt;
  Bytes out;
  out.reserve(text.size() * 3 / 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char ch : text) {
    const int v = value(ch);
    if (v < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

inline std::string HexEncode(const Bytes& data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::uint8_t byte : data) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 15]);
  }
  return out;
}

inline std::optional<Bytes> HexDecode(std::string_view text) {
  if (text.size() % 2) return std::nullopt;
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  Bytes out;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

inline std::string UrlEncode(std::string_view text) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~') {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kDigits[byte >> 4]);
      out.push_back(kDigits[byte & 15]);
    }
  }
  return out;
}

// ------------------------------------------------------------------ framing

inline void AppendU32(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

inline std::uint32_t ReadU32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

inline Bytes EncodeFrame(const Bytes& body) {
  Bytes out;
  out.reserve(body.size() + 4);
  AppendU32(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// Incremental decoder for a byte stream. Push received bytes, then pop
// complete frames. A frame larger than kMaxFrameBytes poisons the decoder.
class FrameDecoder {
 public:
  void Push(const std::uint8_t* data, std::size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
  }

  std::optional<Bytes> Next(std::size_t maxFrameBytes = kMaxFrameBytes) {
    if (failed_ || buffer_.size() - offset_ < 4) return std::nullopt;
    const std::uint32_t length = ReadU32(buffer_.data() + offset_);
    if (length > maxFrameBytes) {
      failed_ = true;
      return std::nullopt;
    }
    if (buffer_.size() - offset_ - 4 < length) return std::nullopt;
    Bytes frame(buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + 4),
                buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + 4 + length));
    offset_ += 4 + length;
    if (offset_ == buffer_.size()) {
      buffer_.clear();
      offset_ = 0;
    } else if (offset_ > 1024 * 1024) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
      offset_ = 0;
    }
    // Do not keep a large photo/file transfer buffer alive for the rest of the session.
    if (buffer_.capacity() > kRetainedBufferBytes &&
        buffer_.size() <= kRetainedBufferBytes) {
      Bytes(buffer_.begin() + static_cast<std::ptrdiff_t>(offset_), buffer_.end())
          .swap(buffer_);
      offset_ = 0;
    }
    return frame;
  }

  std::size_t Capacity() const { return buffer_.capacity(); }

  bool Failed() const { return failed_; }

 private:
  static constexpr std::size_t kRetainedBufferBytes = 256 * 1024;

  Bytes buffer_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

// 12-byte AES-GCM nonce: four zero bytes followed by a big-endian counter.
inline Bytes CounterNonce(std::uint64_t counter) {
  Bytes nonce(12, 0);
  for (int i = 0; i < 8; ++i) {
    nonce[11 - i] = static_cast<std::uint8_t>(counter >> (8 * i));
  }
  return nonce;
}

// ------------------------------------------------------------------ payload

struct Payload {
  std::string json;
  Bytes binary;
};

inline Bytes PackPayload(std::string_view json, const Bytes& binary = {}) {
  Bytes out;
  out.reserve(4 + json.size() + binary.size());
  AppendU32(out, static_cast<std::uint32_t>(json.size()));
  out.insert(out.end(), json.begin(), json.end());
  out.insert(out.end(), binary.begin(), binary.end());
  return out;
}

inline std::optional<Payload> UnpackPayload(const Bytes& data) {
  if (data.size() < 4) return std::nullopt;
  const std::uint32_t length = ReadU32(data.data());
  if (length > data.size() - 4) return std::nullopt;
  Payload payload;
  payload.json.assign(reinterpret_cast<const char*>(data.data() + 4), length);
  payload.binary.assign(data.begin() + 4 + length, data.end());
  return payload;
}

// ------------------------------------------------------------- JSON writing

inline std::string JsonQuote(std::string_view value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[8]{};
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
  return out;
}

// Tiny builder for flat-ish JSON objects: Json("clipboard.set").Str("text", t).
class Json {
 public:
  explicit Json(std::string_view type) { Str("type", type); }
  Json() = default;

  Json& Str(std::string_view key, std::string_view value) {
    return Raw(key, JsonQuote(value));
  }
  Json& Int(std::string_view key, long long value) {
    return Raw(key, std::to_string(value));
  }
  Json& Bool(std::string_view key, bool value) {
    return Raw(key, value ? "true" : "false");
  }
  Json& Raw(std::string_view key, std::string_view raw) {
    if (!body_.empty()) body_.push_back(',');
    body_ += JsonQuote(key);
    body_.push_back(':');
    body_ += raw;
    return *this;
  }
  std::string Build() const { return "{" + body_ + "}"; }

 private:
  std::string body_;
};

inline std::string JsonString(const json::Value& root, std::string_view key) {
  const json::Value* value = root.Find(key);
  return value && value->type == json::Value::Type::String ? value->str
                                                            : std::string{};
}

// Integral JSON numbers only: fractions, NaN/infinity and values outside the
// target range yield the fallback instead of an undefined conversion.
inline long long JsonInt(const json::Value& root, std::string_view key,
                         long long fallback = 0) {
  const json::Value* value = root.Find(key);
  if (!value || value->type != json::Value::Type::Number) return fallback;
  const double number = value->number;
  // 2^63 is exactly representable; the valid range is [-2^63, 2^63).
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < -9223372036854775808.0 || number >= 9223372036854775808.0) {
    return fallback;
  }
  return static_cast<long long>(number);
}

inline int JsonInt32(const json::Value& root, std::string_view key,
                     int fallback = 0) {
  const long long value = JsonInt(root, key, fallback);
  return value < std::numeric_limits<int>::min() ||
                 value > std::numeric_limits<int>::max()
             ? fallback
             : static_cast<int>(value);
}

inline bool JsonBool(const json::Value& root, std::string_view key,
                     bool fallback = false) {
  const json::Value* value = root.Find(key);
  return value && value->type == json::Value::Type::Bool ? value->boolean
                                                          : fallback;
}

// ------------------------------------------------------------ pairing codes

struct PairingInvite {
  std::string pcName;
  std::string pcId;                 // hex
  std::vector<std::string> hosts;   // IPv4 addresses
  std::uint16_t port = kDefaultPort;
  Bytes pcPublicKey;                // 65-byte uncompressed P-256 point
  Bytes token;                      // 16 random bytes, single use
};

inline std::string BuildPairingUri(const PairingInvite& invite) {
  std::string hosts;
  for (const auto& host : invite.hosts) {
    if (!hosts.empty()) hosts.push_back(',');
    hosts += host;
  }
  return "feathercast://pair?v=" + std::to_string(kProtocolVersion) +
         "&n=" + UrlEncode(invite.pcName) + "&id=" + invite.pcId +
         "&h=" + UrlEncode(hosts) + "&p=" + std::to_string(invite.port) +
         "&k=" + Base64UrlEncode(invite.pcPublicKey) +
         "&t=" + Base64UrlEncode(invite.token);
}

inline std::string BuildBeacon(std::string_view pcId, std::uint16_t port) {
  return std::string(kBeaconPrefix) + " " + std::string(pcId) + " " +
         std::to_string(port);
}

inline bool LooksLikeHttp(const std::uint8_t* data, std::size_t size) {
  return size >= 4 && data[0] == 'G' && data[1] == 'E' && data[2] == 'T' &&
         data[3] == ' ';
}

}  // namespace feathercast::phone
