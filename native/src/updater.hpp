#pragma once

#include "extension_protocol.hpp"

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::updater {

struct Version {
  std::array<int, 3> parts{0, 0, 0};
};

struct ReleaseAsset {
  std::wstring name;
  std::wstring browserDownloadUrl;
};

struct ReleaseInfo {
  std::wstring tagName;
  std::wstring name;
  std::wstring htmlUrl;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

inline std::wstring TrimWide(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring LowerWide(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

inline std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

inline bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
  const std::wstring lowerValue = LowerWide(value);
  const std::wstring lowerSuffix = LowerWide(suffix);
  return lowerValue.size() >= lowerSuffix.size() &&
         lowerValue.compare(lowerValue.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

inline std::optional<Version> ParseVersion(std::wstring value) {
  value = TrimWide(std::move(value));
  if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) value.erase(value.begin());
  if (value.empty()) return std::nullopt;

  Version version;
  size_t part = 0;
  size_t start = 0;
  while (start <= value.size() && part < version.parts.size()) {
    const size_t dot = value.find(L'.', start);
    const size_t end = dot == std::wstring::npos ? value.size() : dot;
    if (end == start) return std::nullopt;

    int parsed = 0;
    for (size_t i = start; i < end; ++i) {
      if (!std::iswdigit(value[i])) return std::nullopt;
      parsed = parsed * 10 + static_cast<int>(value[i] - L'0');
    }
    version.parts[part++] = parsed;

    if (dot == std::wstring::npos) {
      start = value.size();
      break;
    }
    start = dot + 1;
  }

  if (part == 0) return std::nullopt;
  if (start < value.size()) return std::nullopt;
  return version;
}

inline int CompareVersions(const Version& left, const Version& right) {
  for (size_t i = 0; i < left.parts.size(); ++i) {
    if (left.parts[i] < right.parts[i]) return -1;
    if (left.parts[i] > right.parts[i]) return 1;
  }
  return 0;
}

inline int CompareVersionStrings(const std::wstring& left, const std::wstring& right) {
  const auto parsedLeft = ParseVersion(left);
  const auto parsedRight = ParseVersion(right);
  if (!parsedLeft || !parsedRight) return 0;
  return CompareVersions(*parsedLeft, *parsedRight);
}

inline bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& candidateTag) {
  const auto current = ParseVersion(currentVersion);
  const auto candidate = ParseVersion(candidateTag);
  return current && candidate && CompareVersions(*candidate, *current) > 0;
}

inline std::wstring AssetVersionFromTag(std::wstring tagName) {
  tagName = TrimWide(std::move(tagName));
  if (!tagName.empty() && (tagName.front() == L'v' || tagName.front() == L'V')) tagName.erase(tagName.begin());
  return tagName;
}

inline std::optional<std::filesystem::path> InstalledRootFromExecutable(
    const std::filesystem::path& executable) {
  if (executable.empty()) return std::nullopt;
  if (LowerWide(executable.filename().wstring()) != L"feathercast.exe") {
    return std::nullopt;
  }
  const auto binDirectory = executable.parent_path();
  if (LowerWide(binDirectory.filename().wstring()) != L"bin") {
    return std::nullopt;
  }
  const auto root = binDirectory.parent_path();
  return root.empty() ? std::nullopt
                      : std::optional<std::filesystem::path>(root);
}

inline bool IsInstalledLayout(const std::filesystem::path& installRoot) {
  if (installRoot.empty()) return false;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(installRoot / L"Uninstall.exe", ec)) {
    return false;
  }
  ec.clear();
  return std::filesystem::is_regular_file(
      installRoot / L"bin" / L"FeatherCast.exe", ec);
}

inline std::wstring QuoteWindowsCommandLineArgument(std::wstring_view value) {
  if (value.empty()) return L"\"\"";

  const bool needsQuotes = value.find_first_of(L" \t\n\v\"") !=
                           std::wstring_view::npos;
  if (!needsQuotes) return std::wstring(value);

  std::wstring quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back(L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'\"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(ch);
    }
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

inline std::wstring NsisInstallDirectoryArgument(
    const std::filesystem::path& installRoot) {
  return L"/D=" + QuoteWindowsCommandLineArgument(installRoot.wstring());
}

inline std::optional<ReleaseInfo> ParseGitHubReleaseJson(const std::string& json) {
  auto tagName = feathercast::extensions::JsonString(json, "tag_name");
  if (!tagName || tagName->empty()) return std::nullopt;

  ReleaseInfo release;
  release.tagName = feathercast::extensions::Utf8ToWide(*tagName);
  release.draft = feathercast::extensions::JsonBool(json, "draft", false);
  release.prerelease = feathercast::extensions::JsonBool(json, "prerelease", false);
  if (auto name = feathercast::extensions::JsonString(json, "name")) release.name = feathercast::extensions::Utf8ToWide(*name);
  if (auto html = feathercast::extensions::JsonString(json, "html_url")) release.htmlUrl = feathercast::extensions::Utf8ToWide(*html);

  for (const auto& object : feathercast::extensions::JsonObjectArray(json, "assets")) {
    auto name = feathercast::extensions::JsonString(object, "name");
    auto url = feathercast::extensions::JsonString(object, "browser_download_url");
    if (!name || !url || name->empty() || url->empty()) continue;
    release.assets.push_back({feathercast::extensions::Utf8ToWide(*name),
                              feathercast::extensions::Utf8ToWide(*url)});
  }
  return release;
}

inline bool IsEligibleRelease(const ReleaseInfo& release, const std::wstring& currentVersion) {
  return !release.draft && !release.prerelease && IsNewerVersion(currentVersion, release.tagName);
}

inline std::wstring ExpectedInstallerAssetName(const std::wstring& tagName) {
  return L"FeatherCast-" + AssetVersionFromTag(tagName) + L"-win64.exe";
}

inline std::optional<ReleaseAsset> SelectInstallerAsset(const ReleaseInfo& release) {
  const std::wstring expected = LowerWide(ExpectedInstallerAssetName(release.tagName));
  const std::wstring version = LowerWide(AssetVersionFromTag(release.tagName));

  for (const auto& asset : release.assets) {
    if (LowerWide(asset.name) == expected) return asset;
  }
  for (const auto& asset : release.assets) {
    const std::wstring name = LowerWide(asset.name);
    if (EndsWithInsensitive(asset.name, L".exe") &&
        name.find(L"feathercast") != std::wstring::npos &&
        name.find(version) != std::wstring::npos &&
        name.find(L"win64") != std::wstring::npos) {
      return asset;
    }
  }
  return std::nullopt;
}

inline std::optional<ReleaseAsset> SelectSha256Asset(const ReleaseInfo& release, const ReleaseAsset& installer) {
  const std::wstring exact = LowerWide(installer.name + L".sha256");
  for (const auto& asset : release.assets) {
    if (LowerWide(asset.name) == exact) return asset;
  }

  const std::wstring installerName = LowerWide(installer.name);
  for (const auto& asset : release.assets) {
    const std::wstring name = LowerWide(asset.name);
    if (EndsWithInsensitive(asset.name, L".sha256") && name.find(installerName) != std::wstring::npos) {
      return asset;
    }
  }
  return std::nullopt;
}

inline bool IsHexChar(char ch) {
  return (ch >= '0' && ch <= '9') ||
         (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

inline std::optional<std::string> ExtractSha256Hex(std::string_view text) {
  for (size_t i = 0; i + 64 <= text.size(); ++i) {
    bool allHex = true;
    for (size_t j = 0; j < 64; ++j) {
      if (!IsHexChar(text[i + j])) {
        allHex = false;
        break;
      }
    }
    if (allHex) return LowerAscii(std::string(text.substr(i, 64)));
  }
  return std::nullopt;
}

// Incremental SHA-256 on top of CryptoAPI, shared by the path and handle
// variants below so both hash exactly the same way.
class Sha256Hasher {
 public:
  Sha256Hasher() {
    if (!CryptAcquireContextW(&provider_, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        !CryptAcquireContextW(&provider_, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
      provider_ = 0;
      return;
    }
    if (!CryptCreateHash(provider_, CALG_SHA_256, 0, 0, &hash_)) hash_ = 0;
  }

  ~Sha256Hasher() {
    if (hash_) CryptDestroyHash(hash_);
    if (provider_) CryptReleaseContext(provider_, 0);
  }

  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;

  bool Ok() const { return hash_ != 0; }

  bool Update(const void* data, size_t size) {
    if (!hash_) return false;
    const auto* bytes = static_cast<const BYTE*>(data);
    while (size > 0) {
      const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(size, size_t{1} << 30));
      if (!CryptHashData(hash_, bytes, chunk, 0)) return false;
      bytes += chunk;
      size -= chunk;
    }
    return true;
  }

  std::optional<std::string> FinishHex() {
    if (!hash_) return std::nullopt;
    BYTE bytes[32]{};
    DWORD size = static_cast<DWORD>(sizeof(bytes));
    if (!CryptGetHashParam(hash_, HP_HASHVAL, bytes, &size, 0) || size != sizeof(bytes)) {
      return std::nullopt;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(bytes) * 2);
    for (const BYTE byte : bytes) {
      out.push_back(kHex[(byte >> 4) & 0x0F]);
      out.push_back(kHex[byte & 0x0F]);
    }
    return out;
  }

 private:
  HCRYPTPROV provider_ = 0;
  HCRYPTHASH hash_ = 0;
};

inline std::optional<std::string> Sha256FileHex(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;

  Sha256Hasher hasher;
  if (!hasher.Ok()) return std::nullopt;

  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = file.gcount();
    if (read > 0 && !hasher.Update(buffer.data(), static_cast<size_t>(read))) return std::nullopt;
  }
  // A read that stopped for any reason other than end-of-file must not yield a
  // digest of a truncated file.
  if (!file.eof()) return std::nullopt;
  return hasher.FinishHex();
}

inline bool VerifyFileSha256(const std::filesystem::path& path, std::string_view expectedText) {
  const auto expected = ExtractSha256Hex(expectedText);
  const auto actual = Sha256FileHex(path);
  return expected && actual && *expected == *actual;
}

// SHA-256 of an already opened file, read from offset 0 to end of file through
// the handle itself. Lets a caller that holds the installer open (for example
// with FILE_SHARE_READ so nobody can write or replace it) hash the very bytes
// it will later run, instead of re-resolving the path. The handle must be
// opened for synchronous reads (no FILE_FLAG_OVERLAPPED) with GENERIC_READ;
// the file pointer is moved and reset to 0 on success.
inline std::optional<std::string> Sha256HandleHex(HANDLE file) {
  if (file == nullptr || file == INVALID_HANDLE_VALUE) return std::nullopt;
  const LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return std::nullopt;

  Sha256Hasher hasher;
  if (!hasher.Ok()) return std::nullopt;

  std::vector<BYTE> buffer(64 * 1024);
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      return std::nullopt;
    }
    if (read == 0) break;
    if (!hasher.Update(buffer.data(), read)) return std::nullopt;
  }
  auto digest = hasher.FinishHex();
  SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
  return digest;
}

// Handle counterpart of VerifyFileSha256(path, ...). Named differently on
// purpose: HANDLE is void*, so an overload would silently capture wchar_t*.
inline bool VerifyHandleSha256(HANDLE file, std::string_view expectedText) {
  const auto expected = ExtractSha256Hex(expectedText);
  const auto actual = Sha256HandleHex(file);
  return expected && actual && *expected == *actual;
}

// Simple display name (usually the organization) of a certificate.
inline std::optional<std::wstring> CertificatePublisher(PCCERT_CONTEXT certificate) {
  if (!certificate) return std::nullopt;
  const DWORD chars = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
  if (chars <= 1) return std::nullopt;
  std::wstring publisher(chars, L'\0');
  CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, publisher.data(), chars);
  if (!publisher.empty() && publisher.back() == L'\0') publisher.pop_back();
  if (publisher.empty()) return std::nullopt;
  return publisher;
}

// Lowercase hex SHA-256 of the DER-encoded certificate.
inline std::optional<std::wstring> CertificateSha256(PCCERT_CONTEXT certificate) {
  if (!certificate) return std::nullopt;
  BYTE hash[32]{};
  DWORD hashSize = static_cast<DWORD>(sizeof(hash));
  if (!CryptHashCertificate(0, CALG_SHA_256, 0, certificate->pbCertEncoded,
                            certificate->cbCertEncoded, hash, &hashSize) ||
      hashSize != sizeof(hash)) {
    return std::nullopt;
  }
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring out;
  out.reserve(hashSize * 2);
  for (DWORD i = 0; i < hashSize; ++i) {
    out.push_back(kHex[(hash[i] >> 4) & 0x0F]);
    out.push_back(kHex[hash[i] & 0x0F]);
  }
  return out;
}

inline std::optional<std::wstring> AuthenticodePublisher(const std::filesystem::path& path) {
  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  DWORD encoding = 0;
  DWORD contentType = 0;
  DWORD formatType = 0;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType,
                        &formatType, &store, &message, nullptr)) {
    return std::nullopt;
  }
  auto close = [&] {
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
  };

  DWORD signerSize = 0;
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) || signerSize == 0) {
    close();
    return std::nullopt;
  }
  std::vector<BYTE> signerBuffer(signerSize);
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(), &signerSize)) {
    close();
    return std::nullopt;
  }
  const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signerBuffer.data());
  CERT_INFO certificateInfo{};
  certificateInfo.Issuer = signer->Issuer;
  certificateInfo.SerialNumber = signer->SerialNumber;
  PCCERT_CONTEXT certificate = CertFindCertificateInStore(
      store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
  if (!certificate) {
    close();
    return std::nullopt;
  }

  auto publisher = CertificatePublisher(certificate);
  CertFreeCertificateContext(certificate);
  close();
  return publisher;
}

inline std::optional<std::wstring> AuthenticodeSignerSha256(
    const std::filesystem::path& path) {
  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  DWORD encoding = 0;
  DWORD contentType = 0;
  DWORD formatType = 0;
  if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                        CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType,
                        &formatType, &store, &message, nullptr)) {
    return std::nullopt;
  }
  auto close = [&] {
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
  };

  DWORD signerSize = 0;
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) ||
      signerSize == 0) {
    close();
    return std::nullopt;
  }
  std::vector<BYTE> signerBuffer(signerSize);
  if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(),
                        &signerSize)) {
    close();
    return std::nullopt;
  }
  const auto* signer =
      reinterpret_cast<const CMSG_SIGNER_INFO*>(signerBuffer.data());
  CERT_INFO certificateInfo{};
  certificateInfo.Issuer = signer->Issuer;
  certificateInfo.SerialNumber = signer->SerialNumber;
  PCCERT_CONTEXT certificate = CertFindCertificateInStore(
      store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
  if (!certificate) {
    close();
    return std::nullopt;
  }

  auto thumbprint = CertificateSha256(certificate);
  CertFreeCertificateContext(certificate);
  close();
  return thumbprint;
}

inline std::vector<std::wstring> ParseSignerThumbprints(
    const std::wstring& configured) {
  std::vector<std::wstring> out;
  std::wstring current;
  auto finish = [&] {
    std::wstring normalized;
    for (const wchar_t ch : current) {
      if (std::iswxdigit(ch)) {
        normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
      }
    }
    if (normalized.size() == 64) out.push_back(std::move(normalized));
    current.clear();
  };
  for (const wchar_t ch : configured) {
    if (ch == L';' || ch == L',') finish();
    else current.push_back(ch);
  }
  finish();
  return out;
}

inline bool VerifyAuthenticodePublisher(const std::filesystem::path& path,
                                        const std::wstring& expectedPublisher) {
  WINTRUST_FILE_INFO fileInfo{};
  fileInfo.cbStruct = sizeof(fileInfo);
  fileInfo.pcwszFilePath = path.c_str();

  WINTRUST_DATA trustData{};
  trustData.cbStruct = sizeof(trustData);
  trustData.dwUIChoice = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trustData.dwUnionChoice = WTD_CHOICE_FILE;
  trustData.pFile = &fileInfo;
  trustData.dwStateAction = WTD_STATEACTION_VERIFY;
  trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trustData);
  if (status != ERROR_SUCCESS) return false;
  if (expectedPublisher.empty()) return true;
  const auto publisher = AuthenticodePublisher(path);
  return publisher && LowerWide(*publisher) == LowerWide(expectedPublisher);
}

inline bool VerifyAuthenticodeSigner(
    const std::filesystem::path& path, const std::wstring& expectedPublisher,
    const std::wstring& allowedThumbprints) {
  const auto pins = ParseSignerThumbprints(allowedThumbprints);
  if (pins.empty()) return false;
  if (!VerifyAuthenticodePublisher(path, expectedPublisher)) return false;
  const auto thumbprint = AuthenticodeSignerSha256(path);
  return thumbprint &&
         std::find(pins.begin(), pins.end(), LowerWide(*thumbprint)) != pins.end();
}

// Identity of the certificate that signed a file, as seen by WinVerifyTrust.
struct AuthenticodeSigner {
  std::wstring publisher;           // simple display name of the signing certificate
  std::wstring certificateSha256;   // lowercase hex SHA-256 of the DER signing certificate
};

// Runs the Authenticode policy (same flags as VerifyAuthenticodePublisher) on
// an already opened file and, on success, returns the signer taken from that
// same WinVerifyTrust run. The trust decision and the signer identity are
// therefore about the same bytes, and there is no second open of the path that
// could race with a replacement. `file` needs GENERIC_READ; `displayPath` is
// only the name WinVerifyTrust reports and uses for catalog lookups, the bytes
// always come from the handle.
inline std::optional<AuthenticodeSigner> VerifyHandleAuthenticode(
    HANDLE file, const std::filesystem::path& displayPath) {
  if (file == nullptr || file == INVALID_HANDLE_VALUE) return std::nullopt;

  WINTRUST_FILE_INFO fileInfo{};
  fileInfo.cbStruct = sizeof(fileInfo);
  fileInfo.pcwszFilePath = displayPath.c_str();
  fileInfo.hFile = file;

  WINTRUST_DATA trustData{};
  trustData.cbStruct = sizeof(trustData);
  trustData.dwUIChoice = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trustData.dwUnionChoice = WTD_CHOICE_FILE;
  trustData.pFile = &fileInfo;
  trustData.dwStateAction = WTD_STATEACTION_VERIFY;
  trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);

  std::optional<AuthenticodeSigner> signer;
  if (status == ERROR_SUCCESS && trustData.hWVTStateData != nullptr) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trustData.hWVTStateData);
    CRYPT_PROVIDER_SGNR* primary =
        provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    // Chain element 0 is the end-entity (signing) certificate.
    CRYPT_PROVIDER_CERT* leaf = primary ? WTHelperGetProvCertFromChain(primary, 0) : nullptr;
    if (leaf && leaf->pCert) {
      auto publisher = CertificatePublisher(leaf->pCert);
      auto thumbprint = CertificateSha256(leaf->pCert);
      if (publisher && thumbprint) {
        signer = AuthenticodeSigner{std::move(*publisher), std::move(*thumbprint)};
      }
    }
  }

  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trustData);
  return signer;
}

// Handle counterpart of VerifyAuthenticodeSigner(path, ...): the signature must
// verify, the publisher must match (when one is configured) and the signing
// certificate must be one of the pinned SHA-256 thumbprints. Everything is
// evaluated on the opened file; see VerifyHandleAuthenticode.
inline bool VerifyHandleAuthenticodeSigner(
    HANDLE file, const std::filesystem::path& displayPath,
    const std::wstring& expectedPublisher, const std::wstring& allowedThumbprints) {
  const auto pins = ParseSignerThumbprints(allowedThumbprints);
  if (pins.empty()) return false;
  const auto signer = VerifyHandleAuthenticode(file, displayPath);
  if (!signer) return false;
  if (!expectedPublisher.empty() &&
      LowerWide(signer->publisher) != LowerWide(expectedPublisher)) {
    return false;
  }
  return std::find(pins.begin(), pins.end(), LowerWide(signer->certificateSha256)) != pins.end();
}

}  // namespace feathercast::updater
