#include "network_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
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

struct Timeouts {
  int resolve = 0;
  int connect = 0;
  int send = 0;
  int receive = 0;
};

// One HTTPS GET on an asynchronous WinHTTP request. Every step that can block
// (proxy discovery, DNS, connect, TLS, headers, body) waits for its completion
// callback or for a stop request, whichever comes first. A stop closes the
// request, which makes WinHTTP cancel the outstanding operation, so stopping
// the owning service never waits out a network timeout. Only the calling
// thread touches the handles. What the callback touches lives in shared state
// the callback keeps alive until WinHTTP reports the request handle closed, so
// a late callback (or a canceled read still filling the buffer) never reaches
// freed memory.
class AsyncHttpsRequest {
 public:
  static constexpr DWORD kReadChunk = 64 * 1024;

  AsyncHttpsRequest(const std::wstring& host, INTERNET_PORT port,
                    const std::wstring& path, Timeouts timeouts) {
    session_ = WinHttpOpen(L"FeatherCast/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                           WINHTTP_FLAG_ASYNC);
    if (!session_) return;
    WinHttpSetTimeouts(session_, timeouts.resolve, timeouts.connect,
                       timeouts.send, timeouts.receive);
    connect_ = WinHttpConnect(session_, host.c_str(), port, 0);
    if (!connect_) return;
    HINTERNET request = WinHttpOpenRequest(
        connect_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return;
    auto context = std::make_unique<CallbackContext>(CallbackContext{state_});
    DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(context.get());
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &contextValue,
                          sizeof(contextValue)) ||
        WinHttpSetStatusCallback(
            request, &AsyncHttpsRequest::OnStatus,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                WINHTTP_CALLBACK_FLAG_HANDLES,
            0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      // No callback was installed, so none will run for this handle.
      WinHttpCloseHandle(request);
      return;
    }
    // From here on the HANDLE_CLOSING callback owns and deletes the context.
    context.release();
    contextValue_ = contextValue;
    request_ = request;
  }

  ~AsyncHttpsRequest() {
    CloseRequest();
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
  }

  AsyncHttpsRequest(const AsyncHttpsRequest&) = delete;
  AsyncHttpsRequest& operator=(const AsyncHttpsRequest&) = delete;

  [[nodiscard]] bool IsOpen() const noexcept { return request_ != nullptr; }
  [[nodiscard]] HINTERNET Handle() const noexcept { return request_; }

  // Sends the request and waits for the response headers.
  bool Send(std::stop_token stopToken) {
    const DWORD_PTR context = contextValue_;
    if (!Run(stopToken, [&] {
          return WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0,
                                    context) != FALSE;
        })) {
      return false;
    }
    return Run(stopToken, [&] {
             return WinHttpReceiveResponse(request_, nullptr) != FALSE;
           }).has_value();
  }

  // Bytes ready to read, 0 at the end of the body, or nullopt when the
  // connection broke or the request was stopped.
  std::optional<DWORD> QueryDataAvailable(std::stop_token stopToken) {
    // Asynchronous WinHTTP reports the count through the callback only.
    return Run(stopToken, [&] {
      return WinHttpQueryDataAvailable(request_, nullptr) != FALSE;
    });
  }

  // Reads up to `wanted` bytes (at most kReadChunk). The view stays valid
  // until the next call on this request.
  std::optional<std::string_view> Read(DWORD wanted,
                                       std::stop_token stopToken) {
    if (!request_) return std::nullopt;
    char* data = state_->buffer.data();
    const DWORD size = std::min(wanted, kReadChunk);
    const auto read = Run(stopToken, [&] {
      return WinHttpReadData(request_, data, size, nullptr) != FALSE;
    });
    if (!read || *read > size) return std::nullopt;
    return std::string_view(data, *read);
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable_any cv;
    bool done = false;
    bool succeeded = false;
    DWORD value = 0;
    bool closed = false;
    // Sized once and never reallocated: a canceled read may still write into
    // it until the handle has closed.
    std::vector<char> buffer = std::vector<char>(kReadChunk);
  };

  struct CallbackContext {
    std::shared_ptr<State> state;
  };

  static void CALLBACK OnStatus(HINTERNET, DWORD_PTR contextValue,
                                DWORD status, LPVOID information,
                                DWORD informationLength) {
    auto* context = reinterpret_cast<CallbackContext*>(contextValue);
    if (!context) return;
    State& state = *context->state;
    {
      // Notify while holding the lock: the waiting thread may destroy its
      // request as soon as it sees the result.
      std::lock_guard lock(state.mutex);
      switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
          state.done = true;
          state.succeeded = true;
          state.value = 0;
          break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
          state.done = true;
          state.succeeded = information != nullptr;
          state.value = information ? *static_cast<const DWORD*>(information)
                                    : 0;
          break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
          state.done = true;
          state.succeeded = true;
          state.value = informationLength;
          break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
          state.done = true;
          state.succeeded = false;
          state.value = 0;
          break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
          state.closed = true;
          break;
        default:
          return;
      }
      state.cv.notify_all();
    }
    // HANDLE_CLOSING is the last callback for the handle.
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) delete context;
  }

  // Starts one asynchronous operation and waits for its completion. A stop
  // request closes the request, which cancels the operation.
  template <typename Start>
  std::optional<DWORD> Run(std::stop_token stopToken, Start start) {
    if (!request_ || stopToken.stop_requested()) return std::nullopt;
    {
      std::lock_guard lock(state_->mutex);
      state_->done = false;
      state_->succeeded = false;
      state_->value = 0;
    }
    // A call that fails right away queues no completion.
    if (!start()) return std::nullopt;
    std::unique_lock lock(state_->mutex);
    if (!state_->cv.wait(lock, stopToken, [&] { return state_->done; })) {
      lock.unlock();
      CloseRequest();
      return std::nullopt;
    }
    if (!state_->succeeded) return std::nullopt;
    return state_->value;
  }

  void CloseRequest() {
    if (!request_) return;
    WinHttpCloseHandle(request_);
    request_ = nullptr;
    // Close the connection and session only after the request has gone, but
    // never let a misbehaving network stack hold shutdown hostage; the shared
    // state outlives this object either way.
    std::unique_lock lock(state_->mutex);
    state_->cv.wait_for(lock, std::chrono::seconds(2),
                        [&] { return state_->closed; });
  }

  std::shared_ptr<State> state_ = std::make_shared<State>();
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  HINTERNET request_ = nullptr;
  DWORD_PTR contextValue_ = 0;
};

// Reads a complete response body. Any failure, a body larger than maxBytes,
// or one shorter than the declared Content-Length is an error rather than a
// partial result.
std::optional<std::string> ReadBody(AsyncHttpsRequest& request,
                                    std::size_t maxBytes,
                                    std::stop_token stopToken) {
  if (!request.Send(stopToken) || !IsSuccessful(request.Handle())) {
    return std::nullopt;
  }
  const auto declaredLength = DeclaredContentLength(request.Handle());
  if (declaredLength && *declaredLength > maxBytes) return std::nullopt;
  std::string body;
  for (;;) {
    // A failing query is a broken connection, not the end of the body.
    const auto available = request.QueryDataAvailable(stopToken);
    if (!available) return std::nullopt;
    if (*available == 0) break;
    if (body.size() + *available > maxBytes) return std::nullopt;
    const auto chunk = request.Read(*available, stopToken);
    if (!chunk) return std::nullopt;
    if (chunk->empty()) break;
    body.append(chunk->data(), chunk->size());
  }
  if (declaredLength && body.size() != *declaredLength) return std::nullopt;
  return body;
}

constexpr Timeouts kRequestTimeouts{8000, 8000, 8000, 8000};
constexpr Timeouts kDownloadTimeouts{10000, 10000, 30000, 30000};

}  // namespace

std::optional<std::string> HttpsGet(const std::wstring& host,
                                    const std::wstring& path,
                                    std::stop_token stopToken) {
  if (stopToken.stop_requested()) return std::nullopt;
  AsyncHttpsRequest request(host, INTERNET_DEFAULT_HTTPS_PORT, path,
                            kRequestTimeouts);
  if (!request.IsOpen()) return std::nullopt;
  static constexpr wchar_t kGitHubHeaders[] =
      L"Accept: application/vnd.github+json\r\n"
      L"X-GitHub-Api-Version: 2022-11-28";
  WinHttpAddRequestHeaders(
      request.Handle(), kGitHubHeaders, static_cast<DWORD>(-1),
      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  return ReadBody(request, 2 * 1024 * 1024, stopToken);
}

std::optional<std::string> HttpsGetUrl(const std::wstring& url,
                                       std::size_t maxBytes,
                                       std::stop_token stopToken) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts || stopToken.stop_requested()) return std::nullopt;
  AsyncHttpsRequest request(parts->host, parts->port, parts->path,
                            kRequestTimeouts);
  if (!request.IsOpen()) return std::nullopt;
  return ReadBody(request, maxBytes, stopToken);
}

bool HttpsDownloadToFile(const std::wstring& url,
                         const std::filesystem::path& destination,
                         std::stop_token stopToken, std::size_t maxBytes) {
  const auto parts = ParseHttpsUrl(url);
  if (!parts || stopToken.stop_requested()) return false;
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  const auto temp = TemporaryDownloadPath(destination);
  AsyncHttpsRequest request(parts->host, parts->port, parts->path,
                            kDownloadTimeouts);
  if (!request.IsOpen()) return false;
  if (!request.Send(stopToken) || !IsSuccessful(request.Handle())) {
    return false;
  }
  // A declared size is checked up front and against what actually arrived.
  const auto declaredLength = DeclaredContentLength(request.Handle());
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

  unsigned long long total = 0;
  for (;;) {
    if (stopToken.stop_requested()) return false;
    // A failing query is a broken connection, not the end of the body.
    const auto available = request.QueryDataAvailable(stopToken);
    if (!available) return false;
    if (*available == 0) break;
    const auto chunk = request.Read(*available, stopToken);
    if (!chunk) return false;
    if (chunk->empty()) break;
    const auto read = static_cast<DWORD>(chunk->size());
    if (total + read > maxBytes) return false;
    DWORD written = 0;
    if (!WriteFile(file, chunk->data(), read, &written, nullptr) ||
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
