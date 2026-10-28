#include <winsock2.h>

#include "background_services.hpp"
#include "network_client.hpp"
#include "test_framework.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <fstream>
#include <mutex>

namespace {

// Stopping an operation whose HTTPS request is stuck in the TLS handshake (a
// local listener accepts the connection and never answers) must end it right
// away rather than after the 8 s / 30 s WinHTTP timeouts. Everything stays on
// the loopback interface.
void TestNetworkRequestsStopPromptly() {
  using namespace std::chrono_literals;
  WSADATA wsa{};
  assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(listener != INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  assert(bind(listener, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) == 0);
  assert(listen(listener, 4) == 0);
  int addressLength = sizeof(address);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                     &addressLength) == 0);
  const std::wstring url = L"https://127.0.0.1:" +
                           std::to_wstring(ntohs(address.sin_port)) +
                           L"/never-answers";
  const auto destination =
      std::filesystem::temp_directory_path() /
      (L"feathercast-network-stop-" + std::to_wstring(GetCurrentProcessId())) /
      L"download.bin";

  // A request that is already stopped gives up before touching the network.
  {
    std::stop_source stopped;
    stopped.request_stop();
    const auto started = std::chrono::steady_clock::now();
    assert(!feathercast::network::HttpsGetUrl(url, 1024, stopped.get_token()));
    assert(!feathercast::network::HttpsGet(L"127.0.0.1", L"/",
                                           stopped.get_token()));
    assert(!feathercast::network::HttpsDownloadToFile(url, destination,
                                                      stopped.get_token()));
    assert(std::chrono::steady_clock::now() - started < 1s);
  }

  std::vector<SOCKET> accepted;
  const auto stopWhileBlocked =
      [&](std::function<bool(std::stop_token)> request) {
    std::atomic<bool> finished = false;
    std::atomic<bool> succeeded = false;
    feathercast::runtime::SingleOperationService<int> operation([](int) {});
    assert(operation.Run([&](std::stop_token token) -> std::optional<int> {
      succeeded = request(token);
      finished = true;
      return std::nullopt;
    }));
    // Wait for WinHTTP to connect, then give it time to send its TLS hello
    // and block on the answer. A proxy could keep it from connecting at all;
    // the stop must be prompt either way.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{5, 0};
    const bool connected = select(0, &readable, nullptr, nullptr, &timeout) == 1;
    if (connected) {
      accepted.push_back(accept(listener, nullptr, nullptr));
      std::this_thread::sleep_for(200ms);
      assert(!finished);
    }
    const auto stopRequested = std::chrono::steady_clock::now();
    operation.Stop();
    assert(std::chrono::steady_clock::now() - stopRequested < 2s);
    assert(finished && !succeeded);
  };
  stopWhileBlocked([&](std::stop_token token) {
    return feathercast::network::HttpsGetUrl(url, 1024, token).has_value();
  });
  stopWhileBlocked([&](std::stop_token token) {
    return feathercast::network::HttpsDownloadToFile(url, destination, token);
  });
  std::error_code ec;
  assert(!std::filesystem::exists(destination, ec));
  std::filesystem::remove_all(destination.parent_path(), ec);

  for (const SOCKET client : accepted) {
    if (client != INVALID_SOCKET) closesocket(client);
  }
  closesocket(listener);
  WSACleanup();
}

}  // namespace

int main() {
  TestNetworkRequestsStopPromptly();

  const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  assert(SUCCEEDED(coResult));
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    assert(SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))));
    const auto iconPath = std::filesystem::temp_directory_path() /
                          (L"feathercast-icon-decode-" +
                           std::to_wstring(GetCurrentProcessId()) + L".png");
    std::error_code ec;
    std::filesystem::remove(iconPath, ec);
    Microsoft::WRL::ComPtr<IWICStream> stream;
    assert(SUCCEEDED(factory->CreateStream(&stream)));
    assert(SUCCEEDED(
        stream->InitializeFromFilename(iconPath.c_str(), GENERIC_WRITE)));
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    assert(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                            &encoder)));
    assert(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    assert(SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)));
    assert(SUCCEEDED(frame->Initialize(properties.Get())));
    assert(SUCCEEDED(frame->SetSize(2, 1)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    assert(SUCCEEDED(frame->SetPixelFormat(&format)));
    const std::uint8_t pixels[] = {0, 0, 255, 255, 0, 255, 0, 255};
    assert(SUCCEEDED(frame->WritePixels(1, 8, sizeof(pixels),
                                        const_cast<BYTE*>(pixels))));
    assert(SUCCEEDED(frame->Commit()));
    assert(SUCCEEDED(encoder->Commit()));
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    const auto decoded = feathercast::runtime::DecodePngIcon(
        factory.Get(), iconPath, L"test-icon");
    assert(decoded && decoded->key == L"test-icon");
    assert(decoded->width == 2 && decoded->height == 1 &&
           decoded->stride == 8 && decoded->pixels.size() == 8);
    assert(decoded->pixels[2] == 255 && decoded->pixels[3] == 255);
    {
      std::ofstream truncated(iconPath, std::ios::binary | std::ios::trunc);
      truncated.write("\x89PNG", 4);
    }
    assert(!feathercast::runtime::DecodePngIcon(
        factory.Get(), iconPath, L"corrupt-cache-entry"));
    std::filesystem::remove(iconPath, ec);
  }
  {
    // Images from the phone are decoded through DecodeImageBytes, which must
    // scale large sources down to the requested edge and reject bad input.
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    assert(SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))));
    Microsoft::WRL::ComPtr<IStream> memory;
    assert(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)));
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    assert(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                            &encoder)));
    assert(SUCCEEDED(encoder->Initialize(memory.Get(), WICBitmapEncoderNoCache)));
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    assert(SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)));
    assert(SUCCEEDED(frame->Initialize(properties.Get())));
    constexpr UINT kWidth = 1024;
    constexpr UINT kHeight = 512;
    assert(SUCCEEDED(frame->SetSize(kWidth, kHeight)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    assert(SUCCEEDED(frame->SetPixelFormat(&format)));
    std::vector<BYTE> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4, 0x80);
    assert(SUCCEEDED(frame->WritePixels(kHeight, kWidth * 4,
                                        static_cast<UINT>(pixels.size()),
                                        pixels.data())));
    assert(SUCCEEDED(frame->Commit()));
    assert(SUCCEEDED(encoder->Commit()));
    STATSTG stat{};
    assert(SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)));
    std::vector<std::uint8_t> png(static_cast<std::size_t>(stat.cbSize.QuadPart));
    assert(SUCCEEDED(memory->Seek({}, STREAM_SEEK_SET, nullptr)));
    ULONG read = 0;
    assert(SUCCEEDED(memory->Read(png.data(), static_cast<ULONG>(png.size()), &read)) &&
           read == png.size());

    const auto scaled =
        feathercast::runtime::DecodeImageBytes(factory.Get(), png, L"", 128);
    assert(scaled && scaled->width == 128 && scaled->height == 64 &&
           scaled->stride == 128 * 4 &&
           scaled->pixels.size() == static_cast<std::size_t>(128) * 64 * 4);
    const auto unscaled =
        feathercast::runtime::DecodeImageBytes(factory.Get(), png, L"", 4096);
    assert(unscaled && unscaled->width == kWidth && unscaled->height == kHeight);
    assert(!feathercast::runtime::DecodeImageBytes(factory.Get(), png, L"", 0));
    std::vector<std::uint8_t> truncatedPng(png.begin(), png.begin() + 16);
    assert(!feathercast::runtime::DecodeImageBytes(factory.Get(), truncatedPng, L"", 128));
  }

  feathercast::runtime::LaunchService launch;
  launch.Start(1);
  std::promise<void> ran;
  auto future = ran.get_future();
  assert(launch.Submit([&](std::stop_token) { ran.set_value(); }));
  assert(future.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  launch.Stop();

  std::mutex mutex;
  std::condition_variable cv;
  int resolved = 0;
  int completed = 0;
  feathercast::runtime::IconResolver icons(
      [&](feathercast::runtime::DecodedIcon) {
    {
      std::lock_guard lock(mutex);
      ++completed;
    }
    cv.notify_all();
      });
  icons.Start(2, [&](const std::wstring& key, std::stop_token) {
    ++resolved;
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });
  assert(icons.Queue(L"same"));
  assert(icons.Queue(L"same"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return completed == 1; }));
  }
  assert(resolved == 1);
  icons.Stop();
  assert(!icons.Queue(L"stopped"));

  std::atomic<int> iconErrors = 0;
  bool iconRecovered = false;
  feathercast::runtime::IconResolver resilientIcons(
      [&](feathercast::runtime::DecodedIcon icon) {
        {
          std::lock_guard lock(mutex);
          iconRecovered = icon.key == L"good";
        }
        cv.notify_all();
      },
      [&](std::exception_ptr) {
        ++iconErrors;
        cv.notify_all();
      });
  resilientIcons.Start(1, [](const std::wstring& key, std::stop_token) {
    if (key == L"bad") throw std::runtime_error("icon failure");
    feathercast::runtime::DecodedIcon icon;
    icon.key = key;
    icon.width = 1;
    icon.height = 1;
    icon.stride = 4;
    icon.pixels.resize(4);
    return std::optional{std::move(icon)};
  });
  assert(resilientIcons.Queue(L"bad"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return iconErrors == 1; }));
  }
  assert(resilientIcons.Queue(L"good"));
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return iconRecovered; }));
  }
  resilientIcons.Stop();

  std::promise<int> operationCompleted;
  auto operationFuture = operationCompleted.get_future();
  feathercast::runtime::SingleOperationService<int> operation(
      [&](int value) { operationCompleted.set_value(value); });
  assert(operation.Run([](std::stop_token) -> std::optional<int> {
    return 42;
  }));
  assert(operationFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  assert(operationFuture.get() == 42);
  operation.Stop();

  std::promise<void> operationFailed;
  auto failedFuture = operationFailed.get_future();
  feathercast::runtime::SingleOperationService<int> failingOperation(
      [](int) {}, [&](std::exception_ptr) { operationFailed.set_value(); });
  assert(failingOperation.Run([](std::stop_token) -> std::optional<int> {
    throw std::runtime_error("operation failure");
  }));
  assert(failedFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  failingOperation.Stop();

  std::promise<void> canceled;
  auto canceledFuture = canceled.get_future();
  feathercast::runtime::SingleOperationService<int> cancelable(
      [](int) {});
  assert(cancelable.Run([&](std::stop_token token) -> std::optional<int> {
    while (!token.stop_requested()) std::this_thread::yield();
    canceled.set_value();
    return std::nullopt;
  }));
  cancelable.Stop();
  assert(canceledFuture.wait_for(std::chrono::seconds(2)) ==
         std::future_status::ready);
  CoUninitialize();
  return 0;
}
