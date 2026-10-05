#include "network_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::network {
namespace {

template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
  ~ScopeExit() { fn_(); }
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  F fn_;
};

struct HttpsUrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

std::optional<HttpsUrlParts> ParseHttpsUrl(const std::wstring& url) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  std::wstring mutableUrl = url;
  if (!WinHttpCrackUrl(mutableUrl.c_str(), 0, 0, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      !components.lpszHostName || components.dwHostNameLength == 0) {
    return std::nullopt;
  }
  HttpsUrlParts parts;
  parts.host.assign(components.lpszHostName, components.dwHostNameLength);
  if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
    parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
    parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (parts.path.empty()) parts.path = L"/";
  parts.port = components.nPort ? components.nPort
                                : INTERNET_DEFAULT_HTTPS_PORT;
  return parts;
}

bool IsSuccessful(HINTERNET request) {
  DWORD status = 0;
  DWORD size = sizeof(status);
  const bool queried = WinHttpQueryHeaders(
                           request,
                           WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                           WINHTTP_NO_HEADER_INDEX) != FALSE;
  return IsSuccessfulStatusQuery(queried, status);
}

// The Content-Length the server declared, if it sent a valid one.
std::optional<unsigned long long> DeclaredContentLength(HINTERNET request) {
  wchar_t text[32]{};
  DWORD size = sizeof(text);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                           WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return std::nullopt;
  }
  const std::wstring_view digits(text, size / sizeof(wchar_t));
  if (digits.empty()) return std::nullopt;
  unsigned long long value = 0;
  for (const wchar_t character : digits) {
    if (character < L'0' || character > L'9') return std::nullopt;
    const unsigned long long digit = static_cast<unsigned long long>(character - L'0');
    if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value;
}

// A sibling of the destination that no other download (or process) shares, so
// concurrent downloads cannot clobber each other's partial file.
std::filesystem::path TemporaryDownloadPath(
    const std::filesystem::path& destination) {
  static std::atomic<unsigned> counter{0};
  auto temp = destination;
  temp += L"." + std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(counter.fetch_add(1)) + L".part";
  return temp;
}

}  // namespace

std::optional<std::string> HttpsGet(const std::wstring& host,
                                    const std::wstring& path) {
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return std::nullopt;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET connect = WinHttpConnect(
      session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect) return std::nullopt;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return std::nullopt;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  static constexpr wchar_t kGitHubHeaders[] =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28";
  WinHttpAddRequestHeaders(
      request, kGitHubHeaders, static_cast<DWORD>(-1),
      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return std::nullopt;
  }
  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) {
      break;
    }
    chunk.resize(read);
    body += chunk;
    if (body.size() > 2 * 1024 * 1024) break;
  }
  return body;
}

std::optional<std::string> HttpsGetUrl(const std::wstring& url,
                                       std::size_t maxBytes) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return std::nullopt;
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return std::nullopt;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET connect = WinHttpConnect(
      session, parts->host.c_str(), parts->port, 0);
  if (!connect) return std::nullopt;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", parts->path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return std::nullopt;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return std::nullopt;
  }
  const auto declaredLength = DeclaredContentLength(request);
  std::string body;
  for (;;) {
    DWORD available = 0;
    // A failing query is a broken connection, not the end of the body.
    if (!WinHttpQueryDataAvailable(request, &available)) return std::nullopt;
    if (available == 0) break;
    if (body.size() + available > maxBytes) return std::nullopt;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) {
      return std::nullopt;
    }
    if (read == 0) break;
    chunk.resize(read);
    body += chunk;
  }
  if (declaredLength && body.size() != *declaredLength) return std::nullopt;
  return body;
}

bool HttpsDownloadToFile(const std::wstring& url,
                         const std::filesystem::path& destination,
                         std::stop_token stopToken, std::size_t maxBytes) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts) return false;
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  const auto temp = TemporaryDownloadPath(destination);
  HINTERNET session = WinHttpOpen(
      L"FeatherCast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  ScopeExit closeSession([&] { WinHttpCloseHandle(session); });
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
  HINTERNET connect = WinHttpConnect(
      session, parts->host.c_str(), parts->port, 0);
  if (!connect) return false;
  ScopeExit closeConnect([&] { WinHttpCloseHandle(connect); });
  HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", parts->path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) return false;
  ScopeExit closeRequest([&] { WinHttpCloseHandle(request); });
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr) || !IsSuccessful(request)) {
    return false;
  }
  // A declared size is checked up front and against what actually arrived.
  const auto declaredLength = DeclaredContentLength(request);
  if (declaredLength && (*declaredLength == 0 || *declaredLength > maxBytes)) {
    return false;
  }

  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  bool completed = false;
  // Declared last so it runs first: a failed or canceled download never
  // leaves its partial file behind.
  ScopeExit removePartial([&] {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!completed) DeleteFileW(temp.c_str());
  });

  std::vector<char> buffer(64 * 1024);
  unsigned long long total = 0;
  for (;;) {
    if (stopToken.stop_requested()) return false;
    DWORD available = 0;
    // A failing query is a broken connection, not the end of the body.
    if (!WinHttpQueryDataAvailable(request, &available)) return false;
    if (available == 0) break;
    DWORD read = 0;
    const DWORD wanted =
        std::min(available, static_cast<DWORD>(buffer.size()));
    if (!WinHttpReadData(request, buffer.data(), wanted, &read)) return false;
    if (read == 0) break;
    if (total + read > maxBytes) return false;
    DWORD written = 0;
    if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
        written != read) {
      return false;
    }
    total += read;
  }
  if (total == 0) return false;
  if (declaredLength && total != *declaredLength) return false;
  if (!FlushFileBuffers(file)) return false;
  const bool closed = CloseHandle(file) != FALSE;
  file = INVALID_HANDLE_VALUE;
  if (!closed) return false;
  if (!MoveFileExW(temp.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return false;
  }
  completed = true;
  return true;
}

}  // namespace feathercast::network
