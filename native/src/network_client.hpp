#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace feathercast::network {

constexpr bool IsSuccessfulStatusQuery(bool headerQuerySucceeded,
                                       unsigned long status) {
  return headerQuerySucceeded && status >= 200 && status < 300;
}

// Every request returns the complete body or nothing: a broken connection, a
// body over the size limit, or one shorter than its Content-Length is an
// error. A stop request aborts whichever phase is in flight (proxy discovery,
// DNS, connect, TLS, headers or body) promptly instead of waiting out the
// network timeouts, so a service can join its worker without freezing.
std::optional<std::string> HttpsGet(const std::wstring& host,
                                    const std::wstring& path,
                                    std::stop_token stopToken = {});
std::optional<std::string> HttpsGetUrl(
    const std::wstring& url, std::size_t maxBytes = 2 * 1024 * 1024,
    std::stop_token stopToken = {});
bool HttpsDownloadToFile(
    const std::wstring& url, const std::filesystem::path& destination,
    std::stop_token stopToken, std::size_t maxBytes = 250 * 1024 * 1024);

}  // namespace feathercast::network
