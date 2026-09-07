#include "capture_service.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

namespace feathercast::capture {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

constexpr std::uint32_t kFramesPerSecond = 30;
constexpr std::uint64_t kHundredNanosecondsPerSecond = 10'000'000;

struct ComApartment {
  ComApartment()
      : result(CoInitializeEx(
            nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)) {
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
      winrt::check_hresult(result);
    }
  }
  ~ComApartment() {
    if (SUCCEEDED(result)) CoUninitialize();
  }
  HRESULT result;
};

struct MediaFoundationPlatform {
  void Start() {
    winrt::check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    started = true;
  }
  ~MediaFoundationPlatform() {
    if (started) MFShutdown();
  }
  bool started = false;
};

std::wstring HResultMessage(HRESULT result) {
  wchar_t* text = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0,
      reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring message =
      length && text ? std::wstring(text, length) : std::format(L"0x{:08X}", result);
  if (text) LocalFree(text);
  while (!message.empty() &&
         (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return message;
}

std::filesystem::path KnownCaptureFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  winrt::check_hresult(
      SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw));
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  result /= L"FeatherCast";
  std::filesystem::create_directories(result);
  return result;
}

struct OutputPaths {
  std::filesystem::path finalPath;
  std::filesystem::path partialPath;
};

OutputPaths NextOutputPaths(CaptureOperation operation) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  const auto folder = KnownCaptureFolder(
      operation == CaptureOperation::Screenshot ? FOLDERID_Pictures
                                                : FOLDERID_Videos);
  for (unsigned collision = 1;; ++collision) {
    OutputPaths result{
        CaptureOutputPath(folder, operation, now, collision, false),
        CaptureOutputPath(folder, operation, now, collision, true)};
    if (!std::filesystem::exists(result.finalPath) &&
        !std::filesystem::exists(result.partialPath)) {
      return result;
    }
  }
}

bool PublishClipboard(const std::vector<std::uint8_t>& pixels, int width,
                      int height) {
  const std::size_t pixelBytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  const std::size_t allocationBytes = sizeof(BITMAPV5HEADER) + pixelBytes;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, allocationBytes);
  if (!memory) return false;
  auto* data = static_cast<std::uint8_t*>(GlobalLock(memory));
  if (!data) {
    GlobalFree(memory);
    return false;
  }

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = width;
  header.bV5Height = -height;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5SizeImage = static_cast<DWORD>(pixelBytes);
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0;
  header.bV5CSType = LCS_sRGB;
  std::memcpy(data, &header, sizeof(header));
  std::memcpy(data + sizeof(header), pixels.data(), pixelBytes);
  GlobalUnlock(memory);

  if (!OpenClipboard(nullptr)) {
    GlobalFree(memory);
    return false;
  }
  const bool published =
      EmptyClipboard() && SetClipboardData(CF_DIBV5, memory) != nullptr;
  CloseClipboard();
  if (!published) GlobalFree(memory);
  return published;
}

std::vector<std::uint8_t> CaptureDesktopPixels(PixelRect bounds) {
  const int width = bounds.Width();
  const int height = bounds.Height();
  const std::uint64_t pixelBytes =
      static_cast<std::uint64_t>(std::max(0, width)) *
      static_cast<std::uint64_t>(std::max(0, height)) * 4;
  if (width <= 0 || height <= 0 ||
      pixelBytes > std::numeric_limits<UINT>::max() ||
      pixelBytes > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("Invalid screenshot dimensions.");
  }

  HDC screen = GetDC(nullptr);
  if (!screen) throw std::runtime_error("Could not access the desktop.");
  HDC memory = CreateCompatibleDC(screen);
  if (!memory) {
    ReleaseDC(nullptr, screen);
    throw std::runtime_error("Could not create the screenshot surface.");
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap =
      CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
  const bool copied =
      bitmap && previous &&
      BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top,
             SRCCOPY | CAPTUREBLT);

  std::vector<std::uint8_t> pixels;
  if (copied) {
    const std::size_t byteCount =
        static_cast<std::size_t>(width) * height * 4;
    pixels.assign(static_cast<std::uint8_t*>(bits),
                  static_cast<std::uint8_t*>(bits) + byteCount);
  }
  if (previous) SelectObject(memory, previous);
  if (bitmap) DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (!copied) throw std::runtime_error("Windows could not capture the screen.");
  return pixels;
}

void EncodePng(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& pixels, int width,
               int height) {
  ComPtr<IWICImagingFactory> factory;
  winrt::check_hresult(CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(factory.GetAddressOf())));
  ComPtr<IWICStream> stream;
  winrt::check_hresult(factory->CreateStream(stream.GetAddressOf()));
  winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder;
  winrt::check_hresult(factory->CreateEncoder(
      GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
  winrt::check_hresult(
      encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame;
  winrt::check_hresult(
      encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
  winrt::check_hresult(frame->Initialize(nullptr));
  winrt::check_hresult(frame->SetSize(static_cast<UINT>(width),
                                     static_cast<UINT>(height)));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  winrt::check_hresult(frame->SetPixelFormat(&format));
  if (format != GUID_WICPixelFormat32bppBGRA) {
    throw std::runtime_error("The PNG encoder rejected the screen format.");
  }
  const UINT stride = static_cast<UINT>(width) * 4;
  winrt::check_hresult(frame->WritePixels(
      static_cast<UINT>(height), stride, static_cast<UINT>(pixels.size()),
      const_cast<BYTE*>(pixels.data())));
  winrt::check_hresult(frame->Commit());
  winrt::check_hresult(encoder->Commit());
}

struct SinkWriter {
  ComPtr<IMFSinkWriter> writer;
  DWORD stream = 0;
};

SinkWriter CreateSinkWriter(const std::filesystem::path& path,
                            std::uint32_t width, std::uint32_t height,
                            bool hardware) {
  ComPtr<IMFAttributes> attributes;
  winrt::check_hresult(MFCreateAttributes(attributes.GetAddressOf(), 2));
  winrt::check_hresult(attributes->SetUINT32(
      MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware));
  winrt::check_hresult(
      attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));

  SinkWriter sink;
  winrt::check_hresult(MFCreateSinkWriterFromURL(
      path.c_str(), nullptr, attributes.Get(), sink.writer.GetAddressOf()));

  ComPtr<IMFMediaType> output;
  winrt::check_hresult(MFCreateMediaType(output.GetAddressOf()));
  winrt::check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  winrt::check_hresult(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  const std::uint32_t bitrate = static_cast<std::uint32_t>(std::clamp(
      8'000'000.0 * static_cast<double>(pixels) / (1920.0 * 1080.0),
      4'000'000.0, 40'000'000.0));
  winrt::check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE, bitrate));
  winrt::check_hresult(
      output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  winrt::check_hresult(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width,
                                         height));
  winrt::check_hresult(MFSetAttributeRatio(
      output.Get(), MF_MT_FRAME_RATE, kFramesPerSecond, 1));
  winrt::check_hresult(
      MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  winrt::check_hresult(
      sink.writer->AddStream(output.Get(), &sink.stream));

  ComPtr<IMFMediaType> input;
  winrt::check_hresult(MFCreateMediaType(input.GetAddressOf()));
  winrt::check_hresult(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  winrt::check_hresult(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
  winrt::check_hresult(
      input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  winrt::check_hresult(
      MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height));
  winrt::check_hresult(MFSetAttributeRatio(
      input.Get(), MF_MT_FRAME_RATE, kFramesPerSecond, 1));
  winrt::check_hresult(
      MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  winrt::check_hresult(
      input->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4));
  winrt::check_hresult(
      sink.writer->SetInputMediaType(sink.stream, input.Get(), nullptr));
  winrt::check_hresult(sink.writer->BeginWriting());
  return sink;
}

void WriteVideoFrame(SinkWriter& sink,
                     const std::vector<std::uint8_t>& topDownPixels,
                     std::uint32_t width, std::uint32_t height,
                     std::uint64_t frameNumber) {
  const DWORD stride = width * 4;
  const DWORD byteCount = stride * height;
  ComPtr<IMFMediaBuffer> buffer;
  winrt::check_hresult(
      MFCreateMemoryBuffer(byteCount, buffer.GetAddressOf()));
  BYTE* destination = nullptr;
  winrt::check_hresult(buffer->Lock(&destination, nullptr, nullptr));
  std::memcpy(destination, topDownPixels.data(), byteCount);
  winrt::check_hresult(buffer->Unlock());
  winrt::check_hresult(buffer->SetCurrentLength(byteCount));

  ComPtr<IMFSample> sample;
  winrt::check_hresult(MFCreateSample(sample.GetAddressOf()));
  winrt::check_hresult(sample->AddBuffer(buffer.Get()));
  const LONGLONG start = static_cast<LONGLONG>(
      frameNumber * kHundredNanosecondsPerSecond / kFramesPerSecond);
  const LONGLONG end = static_cast<LONGLONG>(
      (frameNumber + 1) * kHundredNanosecondsPerSecond / kFramesPerSecond);
  winrt::check_hresult(sample->SetSampleTime(start));
  winrt::check_hresult(sample->SetSampleDuration(end - start));
  winrt::check_hresult(sink.writer->WriteSample(sink.stream, sample.Get()));
}

IDirect3DDevice CreateCaptureDevice(ComPtr<ID3D11Device>& device,
                                    ComPtr<ID3D11DeviceContext>& context) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL selected{};
  constexpr D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
      device.GetAddressOf(), &selected, context.GetAddressOf());
  if (result == E_INVALIDARG) {
    result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1,
        static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
        device.GetAddressOf(), &selected, context.GetAddressOf());
  }
  winrt::check_hresult(result);
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
  ComPtr<IDXGIDevice> dxgi;
  winrt::check_hresult(device.As(&dxgi));
  winrt::com_ptr<IInspectable> inspectable;
  winrt::check_hresult(
      CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
  return inspectable.as<IDirect3DDevice>();
}

struct MonitorCapture {
  MonitorSlice slice;
  GraphicsCaptureItem item{nullptr};
  Direct3D11CaptureFramePool pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  ComPtr<ID3D11Texture2D> staging;
  std::vector<std::uint8_t> latest;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool received = false;
};

void DrainLatestFrame(MonitorCapture& source, ID3D11Device* device,
                      ID3D11DeviceContext* context) {
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame newest{nullptr};
  while (auto frame = source.pool.TryGetNextFrame()) newest = std::move(frame);
  if (!newest) return;

  auto access = newest.Surface().as<
      Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  ComPtr<ID3D11Texture2D> texture;
  winrt::check_hresult(
      access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())));
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  if (!description.Width || !description.Height) return;
  if (!source.staging || description.Width != source.width ||
      description.Height != source.height) {
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.Usage = D3D11_USAGE_STAGING;
    source.staging.Reset();
    winrt::check_hresult(
        device->CreateTexture2D(&description, nullptr,
                                source.staging.GetAddressOf()));
    source.width = description.Width;
    source.height = description.Height;
    source.latest.resize(
        static_cast<std::size_t>(source.width) * source.height * 4);
  }
  context->CopyResource(source.staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  winrt::check_hresult(
      context->Map(source.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  const std::size_t rowBytes = static_cast<std::size_t>(source.width) * 4;
  for (std::uint32_t row = 0; row < source.height; ++row) {
    std::memcpy(source.latest.data() + row * rowBytes,
                static_cast<const std::uint8_t*>(mapped.pData) +
                    row * mapped.RowPitch,
                rowBytes);
  }
  context->Unmap(source.staging.Get(), 0);
  source.received = true;
}

void ComposeFrame(const std::vector<MonitorCapture>& sources, PixelRect bounds,
                  std::uint32_t outputWidth, std::uint32_t outputHeight,
                  std::vector<std::uint8_t>& output) {
  output.assign(static_cast<std::size_t>(outputWidth) * outputHeight * 4, 0);
  for (const auto& source : sources) {
    if (!source.received) continue;
    const PixelRect& slice = source.slice.intersection;
    const int sourceX = slice.left - source.slice.monitorBounds.left;
    const int sourceY = slice.top - source.slice.monitorBounds.top;
    const int destinationX = slice.left - bounds.left;
    const int destinationY = slice.top - bounds.top;
    const int copyWidth = std::min(
        {slice.Width(), static_cast<int>(source.width) - sourceX,
         static_cast<int>(outputWidth) - destinationX});
    const int copyHeight = std::min(
        {slice.Height(), static_cast<int>(source.height) - sourceY,
         static_cast<int>(outputHeight) - destinationY});
    if (sourceX < 0 || sourceY < 0 || destinationX < 0 ||
        destinationY < 0 || copyWidth <= 0 || copyHeight <= 0) {
      continue;
    }
    for (int row = 0; row < copyHeight; ++row) {
      std::memcpy(
          output.data() +
              (static_cast<std::size_t>(destinationY + row) * outputWidth +
               destinationX) *
                  4,
          source.latest.data() +
              (static_cast<std::size_t>(sourceY + row) * source.width +
               sourceX) *
                  4,
          static_cast<std::size_t>(copyWidth) * 4);
    }
  }

  const int selectedWidth = bounds.Width();
  const int selectedHeight = bounds.Height();
  if (outputWidth > static_cast<std::uint32_t>(selectedWidth)) {
    for (int row = 0; row < selectedHeight; ++row) {
      auto* destination =
          output.data() +
          (static_cast<std::size_t>(row) * outputWidth + selectedWidth) * 4;
      std::memcpy(destination, destination - 4, 4);
    }
  }
  if (outputHeight > static_cast<std::uint32_t>(selectedHeight)) {
    std::memcpy(output.data() +
                    static_cast<std::size_t>(selectedHeight) * outputWidth * 4,
                output.data() +
                    static_cast<std::size_t>(selectedHeight - 1) * outputWidth *
                        4,
                static_cast<std::size_t>(outputWidth) * 4);
  }
}

std::wstring CurrentExceptionMessage() {
  try {
    throw;
  } catch (const winrt::hresult_error& error) {
    return error.message().empty() ? HResultMessage(error.code())
                                   : std::wstring(error.message());
  } catch (const std::filesystem::filesystem_error& error) {
    const int count =
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
    if (count <= 1) return L"Capture file operation failed.";
    std::wstring message(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), count);
    message.resize(static_cast<std::size_t>(count - 1));
    return message;
  } catch (const std::exception& error) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
    if (count <= 1) return L"Capture failed.";
    std::wstring message(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), count);
    message.resize(static_cast<std::size_t>(count - 1));
    return message;
  } catch (...) {
    return L"Capture failed.";
  }
}

}  // namespace

PixelRect NormalizeRect(PixelRect rect) noexcept {
  if (rect.left > rect.right) std::swap(rect.left, rect.right);
  if (rect.top > rect.bottom) std::swap(rect.top, rect.bottom);
  return rect;
}

std::optional<PixelRect> IntersectRects(PixelRect first,
                                        PixelRect second) noexcept {
  first = NormalizeRect(first);
  second = NormalizeRect(second);
  PixelRect result{std::max(first.left, second.left),
                   std::max(first.top, second.top),
                   std::min(first.right, second.right),
                   std::min(first.bottom, second.bottom)};
  return result.Empty() ? std::nullopt : std::optional(result);
}

std::vector<MonitorSlice> GetMonitorSlices(PixelRect bounds) {
  bounds = NormalizeRect(bounds);
  std::vector<MonitorSlice> result;
  struct Context {
    PixelRect bounds;
    std::vector<MonitorSlice>* result;
  } context{bounds, &result};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(parameter);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;
        const PixelRect monitorBounds{info.rcMonitor.left, info.rcMonitor.top,
                                      info.rcMonitor.right,
                                      info.rcMonitor.bottom};
        if (auto intersection =
                IntersectRects(context.bounds, monitorBounds)) {
          context.result->push_back(
              MonitorSlice{monitor, monitorBounds, *intersection});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  return result;
}

std::pair<std::uint32_t, std::uint32_t> EvenRecordingSize(
    PixelRect bounds) noexcept {
  bounds = NormalizeRect(bounds);
  const auto width = static_cast<std::uint32_t>(std::max(0, bounds.Width()));
  const auto height = static_cast<std::uint32_t>(std::max(0, bounds.Height()));
  return {width + (width & 1u), height + (height & 1u)};
}

std::chrono::nanoseconds RecordingTimestamp(
    std::uint64_t encodedFrameCount) noexcept {
  return std::chrono::nanoseconds(
      encodedFrameCount * 1'000'000'000ull / kFramesPerSecond);
}

std::filesystem::path CaptureOutputPath(
    const std::filesystem::path& folder, CaptureOperation operation,
    const SYSTEMTIME& localTime, unsigned collision, bool partial) {
  const wchar_t* kind =
      operation == CaptureOperation::Screenshot ? L"Screenshot" : L"Recording";
  const wchar_t* extension =
      operation == CaptureOperation::Screenshot ? L".png" : L".mp4";
  std::wstring name = std::format(
      L"FeatherCast {} {:04}-{:02}-{:02} {:02}-{:02}-{:02}", kind,
      localTime.wYear, localTime.wMonth, localTime.wDay, localTime.wHour,
      localTime.wMinute, localTime.wSecond);
  if (collision > 1) name += std::format(L" ({})", collision);
  if (partial && operation == CaptureOperation::Recording) name += L".partial";
  if (partial && operation == CaptureOperation::Screenshot) name += L".tmp";
  name += extension;
  return folder / name;
}

std::optional<std::wstring> RecordingSupportError() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto* rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  RTL_OSVERSIONINFOW version{sizeof(version)};
  if (!rtlGetVersion || rtlGetVersion(&version) != 0 ||
      version.dwMajorVersion < 10 || version.dwBuildNumber < 19041) {
    return L"Screen recording requires Windows 10 version 2004 or newer.";
  }
  try {
    if (!GraphicsCaptureSession::IsSupported()) {
      return L"Windows Graphics Capture is unavailable on this system.";
    }
  } catch (...) {
    return L"Windows Graphics Capture is unavailable on this system.";
  }
  return std::nullopt;
}

bool ExcludeWindowFromCapture(HWND window) noexcept {
  if (!window || !SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
    return false;
  }
  DWORD affinity = WDA_NONE;
  return GetWindowDisplayAffinity(window, &affinity) &&
         affinity == WDA_EXCLUDEFROMCAPTURE;
}

class CaptureService::Impl {
 public:
  explicit Impl(Callback callback) : callback_(std::move(callback)) {}
  ~Impl() { Shutdown(); }

  void SetCallback(Callback callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
  }

  bool StartScreenshot(PixelRect bounds, CaptureScope scope) {
    bounds = NormalizeRect(bounds);
    if (bounds.Width() < 2 || bounds.Height() < 2) return false;
    return StartWorker(CaptureState::StartingScreenshot,
                       CaptureOperation::Screenshot, scope, bounds,
                       [this, bounds, scope](std::stop_token token) {
                         ScreenshotWorker(bounds, scope, token);
                       });
  }

  bool StartRecording(PixelRect bounds, CaptureScope scope) {
    bounds = NormalizeRect(bounds);
    if (bounds.Width() < 48 || bounds.Height() < 48 ||
        bounds.Width() > 4096 || bounds.Height() > 2304) {
      return false;
    }
    return StartWorker(CaptureState::StartingRecording,
                       CaptureOperation::Recording, scope, bounds,
                       [this, bounds, scope](std::stop_token token) {
                         RecordingWorker(bounds, scope, token);
                       });
  }

  bool Pause() {
    CaptureState expected = CaptureState::Recording;
    if (!state_.compare_exchange_strong(expected, CaptureState::Paused)) {
      return false;
    }
    Emit({CaptureEventKind::Paused, CaptureOperation::Recording, scope_,
          bounds_, {}, {}, Elapsed(), false});
    wake_.notify_all();
    return true;
  }

  bool Resume() {
    CaptureState expected = CaptureState::Paused;
    if (!state_.compare_exchange_strong(expected, CaptureState::Recording)) {
      return false;
    }
    Emit({CaptureEventKind::Resumed, CaptureOperation::Recording, scope_,
          bounds_, {}, {}, Elapsed(), false});
    wake_.notify_all();
    return true;
  }

  bool Stop() {
    CaptureState current = state_.load();
    while (current != CaptureState::Idle &&
           current != CaptureState::Stopping) {
      if (state_.compare_exchange_weak(current, CaptureState::Stopping)) {
        stopRequested_ = true;
        Emit({CaptureEventKind::Stopping, operation_, scope_, bounds_, {}, {},
              Elapsed(), false});
        wake_.notify_all();
        return true;
      }
    }
    return false;
  }

  void Shutdown() {
    Stop();
    std::jthread worker;
    {
      std::lock_guard lock(mutex_);
      worker = std::move(worker_);
    }
    if (worker.joinable()) {
      worker.request_stop();
      wake_.notify_all();
      worker.join();
    }
    state_ = CaptureState::Idle;
  }

  CaptureState State() const noexcept { return state_.load(); }

 private:
  std::chrono::milliseconds Elapsed() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        RecordingTimestamp(encodedFrames_));
  }

  template <typename Work>
  bool StartWorker(CaptureState startingState, CaptureOperation operation,
                   CaptureScope scope, PixelRect bounds, Work work) {
    std::jthread finished;
    {
      std::lock_guard lock(mutex_);
      if (state_ != CaptureState::Idle) return false;
      if (worker_.joinable() &&
          worker_.get_id() == std::this_thread::get_id()) {
        return false;
      }
      finished = std::move(worker_);
    }
    if (finished.joinable()) finished.join();
    {
      std::lock_guard lock(mutex_);
      if (state_ != CaptureState::Idle) return false;
      stopRequested_ = false;
      encodedFrames_ = 0;
      operation_ = operation;
      scope_ = scope;
      bounds_ = bounds;
      state_ = startingState;
      worker_ = std::jthread(std::move(work));
    }
    return true;
  }

  void Emit(CaptureEvent event) noexcept {
    Callback callback;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
    }
    if (callback) {
      try {
        callback(std::move(event));
      } catch (...) {
      }
    }
  }

  void Complete(CaptureEvent event) {
    state_ = CaptureState::Idle;
    Emit(std::move(event));
  }

  void Fail(CaptureOperation operation, CaptureScope scope, PixelRect bounds,
            const std::filesystem::path& path = {}) {
    const std::wstring message = CurrentExceptionMessage();
    state_ = CaptureState::Idle;
    Emit({CaptureEventKind::Failed, operation, scope, bounds, path, message,
          Elapsed(), false});
  }

  void ScreenshotWorker(PixelRect bounds, CaptureScope scope,
                        std::stop_token token) {
    std::filesystem::path temporary;
    try {
      ComApartment apartment;
      Emit({CaptureEventKind::Started, CaptureOperation::Screenshot, scope,
            bounds});
      if (token.stop_requested() || stopRequested_) {
        Complete({CaptureEventKind::Completed, CaptureOperation::Screenshot,
                  scope, bounds, {}, L"Screenshot canceled."});
        return;
      }
      auto pixels = CaptureDesktopPixels(bounds);
      for (std::size_t index = 3; index < pixels.size(); index += 4) {
        pixels[index] = 0xFF;
      }
      const auto output = NextOutputPaths(CaptureOperation::Screenshot);
      temporary = output.partialPath;
      EncodePng(temporary, pixels, bounds.Width(), bounds.Height());
      if (!MoveFileExW(temporary.c_str(), output.finalPath.c_str(),
                       MOVEFILE_WRITE_THROUGH)) {
        winrt::check_hresult(HRESULT_FROM_WIN32(GetLastError()));
      }
      temporary.clear();
      const bool clipboard =
          PublishClipboard(pixels, bounds.Width(), bounds.Height());
      Complete({CaptureEventKind::Completed, CaptureOperation::Screenshot,
                scope, bounds, output.finalPath,
                clipboard ? L""
                          : L"Screenshot saved, but the clipboard was busy.",
                {}, clipboard});
    } catch (...) {
      if (!temporary.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
      }
      Fail(CaptureOperation::Screenshot, scope, bounds);
    }
  }

  void RecordingWorker(PixelRect bounds, CaptureScope scope,
                       std::stop_token token) {
    std::filesystem::path partialPath;
    bool keepPartial = false;
    try {
      ComApartment apartment;
      MediaFoundationPlatform mediaFoundation;
      if (auto error = RecordingSupportError()) {
        throw winrt::hresult_error(E_NOTIMPL, *error);
      }
      auto slices = GetMonitorSlices(bounds);
      if (slices.empty()) {
        throw std::runtime_error("The selected area is not on an active display.");
      }

      ComPtr<ID3D11Device> d3dDevice;
      ComPtr<ID3D11DeviceContext> d3dContext;
      auto captureDevice = CreateCaptureDevice(d3dDevice, d3dContext);
      auto interop = winrt::get_activation_factory<
          GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      std::vector<MonitorCapture> sources;
      sources.reserve(slices.size());
      for (const auto& slice : slices) {
        MonitorCapture source;
        source.slice = slice;
        winrt::check_hresult(interop->CreateForMonitor(
            slice.monitor, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(source.item)));
        source.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            source.item.Size());
        source.session = source.pool.CreateCaptureSession(source.item);
        source.session.IsCursorCaptureEnabled(true);
        sources.push_back(std::move(source));
      }
      for (auto& source : sources) source.session.StartCapture();

      const auto initialDeadline = std::chrono::steady_clock::now() + 5s;
      while (!stopRequested_ && !token.stop_requested()) {
        bool ready = true;
        for (auto& source : sources) {
          DrainLatestFrame(source, d3dDevice.Get(), d3dContext.Get());
          ready = ready && source.received;
        }
        if (ready) break;
        if (std::chrono::steady_clock::now() >= initialDeadline) {
          throw std::runtime_error(
              "Windows did not provide the first recording frame.");
        }
        std::this_thread::sleep_for(10ms);
      }
      if (stopRequested_ || token.stop_requested()) {
        Complete({CaptureEventKind::Completed, CaptureOperation::Recording,
                  scope, bounds, {}, L"Recording canceled."});
        return;
      }

      mediaFoundation.Start();
      const auto output = NextOutputPaths(CaptureOperation::Recording);
      partialPath = output.partialPath;
      const auto [outputWidth, outputHeight] = EvenRecordingSize(bounds);
      SinkWriter sink;
      try {
        sink =
            CreateSinkWriter(partialPath, outputWidth, outputHeight, true);
      } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(partialPath, ignored);
        sink =
            CreateSinkWriter(partialPath, outputWidth, outputHeight, false);
      }
      CaptureState expected = CaptureState::StartingRecording;
      if (!state_.compare_exchange_strong(expected, CaptureState::Recording)) {
        sink.writer->Finalize();
        sink.writer.Reset();
        std::error_code ignored;
        std::filesystem::remove(partialPath, ignored);
        partialPath.clear();
        Complete({CaptureEventKind::Completed, CaptureOperation::Recording,
                  scope, bounds, {}, L"Recording canceled."});
        return;
      }
      keepPartial = true;
      Emit({CaptureEventKind::Started, CaptureOperation::Recording, scope,
            bounds, partialPath});

      std::vector<std::uint8_t> composed;
      auto nextFrame = std::chrono::steady_clock::now();
      bool finalizeAttempted = false;
      try {
        while (!stopRequested_ && !token.stop_requested()) {
          if (state_ == CaptureState::Paused) {
            std::unique_lock lock(waitMutex_);
            wake_.wait(lock, [&] {
              return stopRequested_.load() || token.stop_requested() ||
                     state_.load() != CaptureState::Paused;
            });
            nextFrame = std::chrono::steady_clock::now();
            continue;
          }
          for (auto& source : sources) {
            DrainLatestFrame(source, d3dDevice.Get(), d3dContext.Get());
          }
          ComposeFrame(sources, bounds, outputWidth, outputHeight, composed);
          const std::uint64_t frameNumber = encodedFrames_.load();
          WriteVideoFrame(sink, composed, outputWidth, outputHeight,
                          frameNumber);
          encodedFrames_ = frameNumber + 1;
          nextFrame += std::chrono::nanoseconds(
              1'000'000'000ll / kFramesPerSecond);
          std::unique_lock lock(waitMutex_);
          wake_.wait_until(lock, nextFrame, [&] {
            return stopRequested_.load() || token.stop_requested() ||
                   state_.load() == CaptureState::Paused;
          });
        }
        state_ = CaptureState::Stopping;
        finalizeAttempted = true;
        winrt::check_hresult(sink.writer->Finalize());
      } catch (...) {
        auto failure = std::current_exception();
        if (!finalizeAttempted) {
          try {
            sink.writer->Finalize();
          } catch (...) {
          }
        }
        std::rethrow_exception(failure);
      }
      if (!MoveFileExW(partialPath.c_str(), output.finalPath.c_str(),
                       MOVEFILE_WRITE_THROUGH)) {
        winrt::check_hresult(HRESULT_FROM_WIN32(GetLastError()));
      }
      partialPath.clear();
      Complete({CaptureEventKind::Completed, CaptureOperation::Recording,
                scope, bounds, output.finalPath, {},
                Elapsed(), false});
    } catch (...) {
      if (!keepPartial && !partialPath.empty()) {
        std::error_code ignored;
        std::filesystem::remove(partialPath, ignored);
        partialPath.clear();
      }
      Fail(CaptureOperation::Recording, scope, bounds, partialPath);
    }
  }

  mutable std::mutex mutex_;
  std::mutex waitMutex_;
  std::condition_variable wake_;
  Callback callback_;
  std::jthread worker_;
  std::atomic<CaptureState> state_ = CaptureState::Idle;
  std::atomic<bool> stopRequested_ = false;
  std::atomic<std::uint64_t> encodedFrames_ = 0;
  CaptureOperation operation_ = CaptureOperation::Screenshot;
  CaptureScope scope_ = CaptureScope::Region;
  PixelRect bounds_;
};

CaptureService::CaptureService(Callback callback)
    : impl_(new Impl(std::move(callback))) {}
CaptureService::~CaptureService() { delete impl_; }
void CaptureService::SetCallback(Callback callback) {
  impl_->SetCallback(std::move(callback));
}
bool CaptureService::StartScreenshot(PixelRect bounds, CaptureScope scope) {
  return impl_->StartScreenshot(bounds, scope);
}
bool CaptureService::StartRecording(PixelRect bounds, CaptureScope scope) {
  return impl_->StartRecording(bounds, scope);
}
bool CaptureService::Pause() { return impl_->Pause(); }
bool CaptureService::Resume() { return impl_->Resume(); }
bool CaptureService::Stop() { return impl_->Stop(); }
void CaptureService::Shutdown() { impl_->Shutdown(); }
CaptureState CaptureService::State() const noexcept { return impl_->State(); }

}  // namespace feathercast::capture
