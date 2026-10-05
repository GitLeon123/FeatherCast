#include "IExtension.h"
#include "extension_protocol.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using ApiVersionFn = uint32_t (*)();
using HandleJsonFn = uint32_t (*)(const char*, char*, uint32_t);

uint32_t CallPluginSeh(HandleJsonFn fn, const char* request, char* response, uint32_t capacity) {
  __try {
    return fn(request, response, capacity);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

uint32_t CallPlugin(HandleJsonFn fn, const char* request, char* response, uint32_t capacity) {
  try {
    return CallPluginSeh(fn, request, response, capacity);
  } catch (...) {
    return 0;
  }
}

std::string ErrorJson(const char* code) {
  return std::string("{\"error\":\"") + code + "\"}";
}

std::string AdaptRequestForPluginApi(std::string request, uint32_t pluginApiVersion) {
  if (pluginApiVersion == feathercast::extensions::kApiVersion) return request;

  const std::string marker = "\"apiVersion\"";
  size_t pos = request.find(marker);
  if (pos == std::string::npos) return request;
  pos = request.find(':', pos + marker.size());
  if (pos == std::string::npos) return request;
  ++pos;
  while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t')) ++pos;
  const size_t start = pos;
  while (pos < request.size() && request[pos] >= '0' && request[pos] <= '9') ++pos;
  if (start == pos) return request;
  request.replace(start, pos - start, std::to_string(pluginApiVersion));
  return request;
}

// The plugin is called exactly once per request. The buffer already has the
// maximum allowed response size, so the plugin is never called a second time
// with a larger buffer, which would run activations and other side effects
// twice.
std::string HandleRequest(HandleJsonFn fn, uint32_t pluginApiVersion, const std::string& request,
                          std::vector<char>& buffer) {
  const std::string adaptedRequest = AdaptRequestForPluginApi(request, pluginApiVersion);
  buffer[0] = '\0';
  const uint32_t required =
      CallPlugin(fn, adaptedRequest.c_str(), buffer.data(), static_cast<uint32_t>(buffer.size()));
  if (!feathercast::extensions::ResponseSizeAllowed(required) || required > buffer.size()) {
    return ErrorJson("plugin-call-failed");
  }

  // buffer[0] was cleared above, so a plugin that reports success without
  // writing anything yields an empty string instead of a stale response.
  const size_t length = strnlen_s(buffer.data(), buffer.size());
  if (length == 0 || length >= buffer.size()) return ErrorJson("plugin-call-failed");
  auto line = feathercast::extensions::FrameResponseLine(std::string(buffer.data(), length));
  if (!line) return ErrorJson("invalid-response");
  return std::move(*line);
}

// Points one standard stream (the Win32 handle and the CRT descriptor) at NUL.
bool RedirectToNul(int descriptor, DWORD standardHandle, DWORD access, int flags) {
  const HANDLE nul = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (nul == INVALID_HANDLE_VALUE) return false;
  const int nulDescriptor = _open_osfhandle(reinterpret_cast<intptr_t>(nul), flags);
  if (nulDescriptor < 0) {
    CloseHandle(nul);
    return false;
  }
  const bool redirected = _dup2(nulDescriptor, descriptor) == 0;
  _close(nulDescriptor);
  if (!redirected) return false;
  const intptr_t redirectedHandle = _get_osfhandle(descriptor);
  return redirectedHandle != -1 &&
         SetStdHandle(standardHandle, reinterpret_cast<HANDLE>(redirectedHandle)) != FALSE;
}

// Moves the protocol pipes to private, non-inheritable handles and points the
// process-wide stdin/stdout at NUL before the plugin DLL is loaded. A plugin
// that prints (printf, std::cout, WriteFile on GetStdHandle) or reads stdin
// can then neither corrupt nor consume protocol lines.
bool IsolateProtocolChannel(HANDLE& input, HANDLE& output) {
  const HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &input, 0, FALSE,
                       DUPLICATE_SAME_ACCESS) ||
      !DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &output, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    return false;
  }
  return RedirectToNul(0, STD_INPUT_HANDLE, GENERIC_READ, _O_RDONLY | _O_BINARY) &&
         RedirectToNul(1, STD_OUTPUT_HANDLE, GENERIC_WRITE, _O_WRONLY | _O_BINARY);
}

bool WriteLine(HANDLE output, std::string line) {
  line.push_back('\n');
  size_t offset = 0;
  while (offset < line.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(line.size() - offset, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(output, line.data() + offset, chunk, &written, nullptr) || written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

// Reads newline-terminated requests from the private protocol input. A final
// line without a newline is still returned, like std::getline.
class LineReader {
 public:
  explicit LineReader(HANDLE input) : input_(input) {}

  bool Next(std::string& line) {
    for (;;) {
      if (const size_t newline = buffer_.find('\n', scanned_); newline != std::string::npos) {
        line.assign(buffer_, 0, newline);
        buffer_.erase(0, newline + 1);
        scanned_ = 0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      scanned_ = buffer_.size();
      char chunk[4096];
      DWORD read = 0;
      if (!ReadFile(input_, chunk, static_cast<DWORD>(sizeof(chunk)), &read, nullptr) || read == 0) {
        if (buffer_.empty()) return false;
        line.swap(buffer_);
        buffer_.clear();
        scanned_ = 0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      buffer_.append(chunk, read);
    }
  }

 private:
  HANDLE input_;
  std::string buffer_;
  size_t scanned_ = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace feathercast::extensions;
  if (argc == 2 && argv[1] && wcscmp(argv[1], L"--self-test") == 0) {
    return 0;
  }
  if (argc < 2 || !argv[1] || !argv[1][0]) return static_cast<int>(kHostExitMissingArgument);

  HANDLE protocolInput = nullptr;
  HANDLE protocolOutput = nullptr;
  if (!IsolateProtocolChannel(protocolInput, protocolOutput)) {
    return static_cast<int>(kHostExitChannelFailed);
  }

  HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) return static_cast<int>(kHostExitLoadFailed);

  auto apiVersion = reinterpret_cast<ApiVersionFn>(GetProcAddress(dll, "FeatherCastExtensionApiVersion"));
  auto handleJson = reinterpret_cast<HandleJsonFn>(GetProcAddress(dll, "FeatherCastExtensionHandleJson"));
  if (!apiVersion || !handleJson) {
    FreeLibrary(dll);
    return static_cast<int>(kHostExitMissingExports);
  }

  const uint32_t pluginApiVersion = apiVersion();
  if (pluginApiVersion < kMinSupportedApiVersion || pluginApiVersion > kApiVersion) {
    FreeLibrary(dll);
    return static_cast<int>(kHostExitUnsupportedApi);
  }

  std::vector<char> responseBuffer(kMaxResponseBytes, '\0');
  LineReader requests(protocolInput);
  std::string request;
  bool connected = WriteLine(protocolOutput, BuildHostReadyJson(pluginApiVersion));
  while (connected && requests.Next(request)) {
    connected = WriteLine(protocolOutput,
                          request.empty() ? ErrorJson("empty-request")
                                          : HandleRequest(handleJson, pluginApiVersion, request,
                                                          responseBuffer));
  }

  FreeLibrary(dll);
  CloseHandle(protocolInput);
  CloseHandle(protocolOutput);
  return 0;
}
