#include "phone_crypto.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace feathercast::phone::crypto {
namespace {

constexpr ULONG kP256Bytes = 32;

bool Ok(NTSTATUS status) { return status >= 0; }

class Algorithm {
 public:
  Algorithm(LPCWSTR id, ULONG flags = 0) {
    if (!Ok(BCryptOpenAlgorithmProvider(&handle_, id, nullptr, flags))) {
      handle_ = nullptr;
    }
  }
  ~Algorithm() {
    if (handle_) BCryptCloseAlgorithmProvider(handle_, 0);
  }
  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;
  BCRYPT_ALG_HANDLE get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class KeyHandle {
 public:
  KeyHandle() = default;
  explicit KeyHandle(BCRYPT_KEY_HANDLE handle) : handle_(handle) {}
  ~KeyHandle() {
    if (handle_) BCryptDestroyKey(handle_);
  }
  KeyHandle(const KeyHandle&) = delete;
  KeyHandle& operator=(const KeyHandle&) = delete;
  BCRYPT_KEY_HANDLE* out() { return &handle_; }
  BCRYPT_KEY_HANDLE get() const { return handle_; }

 private:
  BCRYPT_KEY_HANDLE handle_ = nullptr;
};

std::optional<Bytes> ExportBlob(BCRYPT_KEY_HANDLE key, LPCWSTR type) {
  ULONG size = 0;
  if (!Ok(BCryptExportKey(key, nullptr, type, nullptr, 0, &size, 0))) {
    return std::nullopt;
  }
  Bytes blob(size);
  if (!Ok(BCryptExportKey(key, nullptr, type, blob.data(), size, &size, 0))) {
    return std::nullopt;
  }
  blob.resize(size);
  return blob;
}

std::optional<Bytes> PublicPointFromKey(BCRYPT_KEY_HANDLE key) {
  const auto blob = ExportBlob(key, BCRYPT_ECCPUBLIC_BLOB);
  if (!blob || blob->size() < sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kP256Bytes) {
    return std::nullopt;
  }
  Bytes point{0x04};
  point.insert(point.end(), blob->begin() + sizeof(BCRYPT_ECCKEY_BLOB),
               blob->begin() + sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kP256Bytes);
  return point;
}

}  // namespace

Bytes RandomBytes(std::size_t size) {
  Bytes out(size);
  if (size && !Ok(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(size),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    out.clear();
  }
  return out;
}

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
  static const Algorithm algorithm(BCRYPT_SHA256_ALGORITHM,
                                   BCRYPT_ALG_HANDLE_HMAC_FLAG);
  Bytes digest(32);
  if (!algorithm ||
      !Ok(BCryptHash(algorithm.get(), const_cast<PUCHAR>(key.data()),
                     static_cast<ULONG>(key.size()),
                     const_cast<PUCHAR>(data.data()),
                     static_cast<ULONG>(data.size()), digest.data(),
                     static_cast<ULONG>(digest.size())))) {
    return {};
  }
  return digest;
}

Bytes Hkdf(const Bytes& ikm, const Bytes& salt, std::string_view info,
           std::size_t length) {
  const Bytes prk = HmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
  if (prk.empty()) return {};
  Bytes okm;
  Bytes previous;
  for (std::uint8_t counter = 1; okm.size() < length; ++counter) {
    Bytes block = previous;
    block.insert(block.end(), info.begin(), info.end());
    block.push_back(counter);
    previous = HmacSha256(prk, block);
    if (previous.empty()) return {};
    okm.insert(okm.end(), previous.begin(), previous.end());
  }
  okm.resize(length);
  return okm;
}

bool ConstantTimeEquals(const Bytes& a, const Bytes& b) {
  if (a.size() != b.size()) return false;
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

namespace {

std::optional<Bytes> AesGcm(bool encrypt, const Bytes& key, const Bytes& nonce,
                            const Bytes& input) {
  static const Algorithm algorithm(BCRYPT_AES_ALGORITHM);
  static const bool gcmReady =
      algorithm &&
      Ok(BCryptSetProperty(algorithm.get(), BCRYPT_CHAINING_MODE,
                           reinterpret_cast<PUCHAR>(
                               const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
  constexpr std::size_t kTagBytes = 16;
  if (!gcmReady || key.size() != 32 || nonce.size() != 12) return std::nullopt;
  if (!encrypt && input.size() < kTagBytes) return std::nullopt;

  KeyHandle handle;
  if (!Ok(BCryptGenerateSymmetricKey(algorithm.get(), handle.out(), nullptr, 0,
                                     const_cast<PUCHAR>(key.data()),
                                     static_cast<ULONG>(key.size()), 0))) {
    return std::nullopt;
  }

  const std::size_t dataSize = encrypt ? input.size() : input.size() - kTagBytes;
  Bytes tag(kTagBytes);
  if (!encrypt) {
    std::copy(input.end() - kTagBytes, input.end(), tag.begin());
  }
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(nonce.data());
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbTag = tag.data();
  info.cbTag = static_cast<ULONG>(tag.size());

  Bytes output(dataSize);
  ULONG written = 0;
  // BCrypt rejects null buffers for empty payloads, so give it a dummy byte.
  std::uint8_t dummy = 0;
  PUCHAR in = dataSize ? const_cast<PUCHAR>(input.data()) : &dummy;
  PUCHAR out = dataSize ? output.data() : &dummy;
  const NTSTATUS status =
      encrypt ? BCryptEncrypt(handle.get(), in, static_cast<ULONG>(dataSize),
                              &info, nullptr, 0, out,
                              static_cast<ULONG>(dataSize), &written, 0)
              : BCryptDecrypt(handle.get(), in, static_cast<ULONG>(dataSize),
                              &info, nullptr, 0, out,
                              static_cast<ULONG>(dataSize), &written, 0);
  if (!Ok(status)) return std::nullopt;
  if (encrypt) output.insert(output.end(), tag.begin(), tag.end());
  return output;
}

}  // namespace

std::optional<Bytes> AesGcmEncrypt(const Bytes& key, const Bytes& nonce,
                                   const Bytes& plaintext) {
  return AesGcm(true, key, nonce, plaintext);
}

std::optional<Bytes> AesGcmDecrypt(const Bytes& key, const Bytes& nonce,
                                   const Bytes& sealed) {
  return AesGcm(false, key, nonce, sealed);
}

// ------------------------------------------------------------------- ECDH

namespace {
const Algorithm& EcdhAlgorithm() {
  static const Algorithm algorithm(BCRYPT_ECDH_P256_ALGORITHM);
  return algorithm;
}
}  // namespace

EcdhKeyPair::EcdhKeyPair(EcdhKeyPair&& other) noexcept
    : key_(std::exchange(other.key_, nullptr)),
      publicKey_(std::move(other.publicKey_)) {}

EcdhKeyPair& EcdhKeyPair::operator=(EcdhKeyPair&& other) noexcept {
  if (this != &other) {
    if (key_) BCryptDestroyKey(key_);
    key_ = std::exchange(other.key_, nullptr);
    publicKey_ = std::move(other.publicKey_);
  }
  return *this;
}

EcdhKeyPair::~EcdhKeyPair() {
  if (key_) BCryptDestroyKey(key_);
}

std::optional<EcdhKeyPair> EcdhKeyPair::Generate() {
  const Algorithm& algorithm = EcdhAlgorithm();
  if (!algorithm) return std::nullopt;
  EcdhKeyPair pair;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (!Ok(BCryptGenerateKeyPair(algorithm.get(), &key, 256, 0))) {
    return std::nullopt;
  }
  pair.key_ = key;
  if (!Ok(BCryptFinalizeKeyPair(key, 0))) return std::nullopt;
  auto point = PublicPointFromKey(key);
  if (!point) return std::nullopt;
  pair.publicKey_ = std::move(*point);
  return pair;
}

std::optional<EcdhKeyPair> EcdhKeyPair::Import(const Bytes& privateBlob) {
  const Algorithm& algorithm = EcdhAlgorithm();
  if (!algorithm || privateBlob.empty()) return std::nullopt;
  EcdhKeyPair pair;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (!Ok(BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB,
                              &key, const_cast<PUCHAR>(privateBlob.data()),
                              static_cast<ULONG>(privateBlob.size()), 0))) {
    return std::nullopt;
  }
  pair.key_ = key;
  auto point = PublicPointFromKey(key);
  if (!point) return std::nullopt;
  pair.publicKey_ = std::move(*point);
  return pair;
}

Bytes EcdhKeyPair::ExportPrivate() const {
  if (!key_) return {};
  return ExportBlob(key_, BCRYPT_ECCPRIVATE_BLOB).value_or(Bytes{});
}

std::optional<Bytes> EcdhKeyPair::Agree(const Bytes& peerPublicKey) const {
  const Algorithm& algorithm = EcdhAlgorithm();
  if (!algorithm || !key_ || peerPublicKey.size() != 1 + 2 * kP256Bytes ||
      peerPublicKey[0] != 0x04) {
    return std::nullopt;
  }
  Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
  auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
  header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
  header->cbKey = kP256Bytes;
  blob.insert(blob.end(), peerPublicKey.begin() + 1, peerPublicKey.end());

  KeyHandle peer;
  if (!Ok(BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                              peer.out(), blob.data(),
                              static_cast<ULONG>(blob.size()), 0))) {
    return std::nullopt;
  }
  BCRYPT_SECRET_HANDLE secret = nullptr;
  if (!Ok(BCryptSecretAgreement(key_, peer.get(), &secret, 0))) {
    return std::nullopt;
  }
  Bytes raw(kP256Bytes);
  ULONG size = 0;
  const NTSTATUS status =
      BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, raw.data(),
                      static_cast<ULONG>(raw.size()), &size, 0);
  BCryptDestroySecret(secret);
  if (!Ok(status) || size != kP256Bytes) return std::nullopt;
  // CNG returns the raw secret in little-endian order.
  std::reverse(raw.begin(), raw.end());
  return raw;
}

// ------------------------------------------------------------------ DPAPI

std::optional<Bytes> Protect(const Bytes& plaintext) {
  DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                  const_cast<BYTE*>(plaintext.data())};
  DATA_BLOB output{};
  const DWORD flags = CRYPTPROTECT_UI_FORBIDDEN;
  if (!CryptProtectData(&input, L"FeatherCast phone link", nullptr, nullptr,
                        nullptr, flags, &output)) {
    // Mirrors storage.hpp: isolated CTest accounts have no user master key.
    wchar_t testFallback[2]{};
    const bool testFallbackEnabled =
        GetEnvironmentVariableW(L"FEATHERCAST_TEST_DPAPI_FALLBACK", testFallback,
                                static_cast<DWORD>(std::size(testFallback))) > 0;
    if (!testFallbackEnabled ||
        !CryptProtectData(&input, L"FeatherCast phone link", nullptr, nullptr,
                          nullptr, flags | CRYPTPROTECT_LOCAL_MACHINE,
                          &output)) {
      return std::nullopt;
    }
  }
  Bytes sealed(output.pbData, output.pbData + output.cbData);
  LocalFree(output.pbData);
  return sealed;
}

std::optional<Bytes> Unprotect(const Bytes& sealed) {
  DATA_BLOB input{static_cast<DWORD>(sealed.size()),
                  const_cast<BYTE*>(sealed.data())};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  Bytes plaintext(output.pbData, output.pbData + output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return plaintext;
}

}  // namespace feathercast::phone::crypto
