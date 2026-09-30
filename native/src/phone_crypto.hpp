#pragma once

// Windows CNG (BCrypt) primitives for the phone link: P-256 ECDH, HMAC-SHA256,
// HKDF-SHA256 and AES-256-GCM, plus DPAPI helpers for secrets at rest.

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "phone_protocol.hpp"

namespace feathercast::phone::crypto {

Bytes RandomBytes(std::size_t size);

Bytes HmacSha256(const Bytes& key, const Bytes& data);
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, std::string_view info,
           std::size_t length);
bool ConstantTimeEquals(const Bytes& a, const Bytes& b);

// Returns ciphertext || 16-byte tag, or nullopt on failure.
std::optional<Bytes> AesGcmEncrypt(const Bytes& key, const Bytes& nonce,
                                   const Bytes& plaintext);
// Expects ciphertext || tag. Returns nullopt if authentication fails.
std::optional<Bytes> AesGcmDecrypt(const Bytes& key, const Bytes& nonce,
                                   const Bytes& sealed);

// P-256 key pair. Public keys are 65-byte uncompressed points (0x04||X||Y).
class EcdhKeyPair {
 public:
  static std::optional<EcdhKeyPair> Generate();
  // Imports a previously exported private blob (see ExportPrivate).
  static std::optional<EcdhKeyPair> Import(const Bytes& privateBlob);

  EcdhKeyPair(EcdhKeyPair&&) noexcept;
  EcdhKeyPair& operator=(EcdhKeyPair&&) noexcept;
  ~EcdhKeyPair();

  const Bytes& PublicKey() const { return publicKey_; }
  Bytes ExportPrivate() const;
  // Shared secret: the 32-byte big-endian X coordinate (same as Java's
  // KeyAgreement "ECDH").
  std::optional<Bytes> Agree(const Bytes& peerPublicKey) const;

 private:
  EcdhKeyPair() = default;
  void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
  Bytes publicKey_;
};

// DPAPI (current user). Returns nullopt on failure.
std::optional<Bytes> Protect(const Bytes& plaintext);
std::optional<Bytes> Unprotect(const Bytes& sealed);

}  // namespace feathercast::phone::crypto
