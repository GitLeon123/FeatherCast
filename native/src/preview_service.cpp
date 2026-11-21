#include "preview_service.hpp"

#include "core.hpp"
#include "file_content.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iterator>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace feathercast::preview {
namespace {

constexpr std::uint64_t kMaxImageBytes = 25ULL * 1024 * 1024;
constexpr std::uint64_t kMaxImagePixels = 40ULL * 1000 * 1000;
constexpr std::uint32_t kMaxPreviewDimension = 1024;
constexpr std::size_t kMaxTextExcerptBytes = 16 * 1024;
constexpr std::size_t kMaxTextExcerptChars =
    kMaxTextExcerptBytes / sizeof(wchar_t);

// Formats a file time in the user's locale and local time zone (including the
// daylight saving rules that applied on that date). Returns an empty string
// for missing or unrepresentable times.
std::wstring FormatLocalFileTime(const FILETIME& fileTime) {
  if (fileTime.dwLowDateTime == 0 && fileTime.dwHighDateTime == 0) return {};
  SYSTEMTIME utc{};
  SYSTEMTIME local{};
  if (!FileTimeToSystemTime(&fileTime, &utc) ||
      !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
    return {};
  }
  wchar_t date[128]{};
  wchar_t time[128]{};
  if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local,
                      nullptr, date, static_cast<int>(std::size(date)),
                      nullptr) == 0 ||
      GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local,
                      nullptr, time, static_cast<int>(std::size(time))) == 0) {
    return {};
  }
  return std::wstring(date) + L" " + time;
}

std::wstring MetadataDetail(const std::filesystem::path& path,
                            std::uintmax_t bytes,
                            const FILETIME& lastWriteTime) {
  const std::wstring modified = FormatLocalFileTime(lastWriteTime);
  std::wostringstream out;
  out << path.wstring() << L"\n" << bytes << L" bytes\nModified "
      << (modified.empty() ? std::wstring(L"unknown") : modified);
  return out.str();
}

std::wstring Excerpt(const std::wstring& text, const std::wstring& terms) {
  if (text.size() <= kMaxTextExcerptChars) return text;
  std::wstring token;
  for (const wchar_t ch : terms) {
    if (std::iswalnum(ch) || ch == L'_') token.push_back(ch);
    else if (!token.empty()) break;
  }
  if (token.empty()) return text.substr(0, kMaxTextExcerptChars);
  const std::wstring lowered = core::Lower(text);
  const std::size_t match = lowered.find(core::Lower(token));
  if (match == std::wstring::npos) return text.substr(0, kMaxTextExcerptChars);
  const std::size_t before = kMaxTextExcerptChars / 3;
  const std::size_t start = match > before ? match - before : 0;
  return text.substr(start, kMaxTextExcerptChars);
}

bool DecodeImage(const std::filesystem::path& path, Result& result,
                 std::stop_token token, IWICImagingFactory** cachedFactory) {
  ComPtr<IWICImagingFactory> localFactory;
  IWICImagingFactory* factory = cachedFactory ? *cachedFactory : nullptr;
  if (!factory) {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&localFactory)))) {
      return false;
    }
    factory = localFactory.Get();
    if (cachedFactory) {
      *cachedFactory = localFactory.Detach();
    }
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          &decoder))) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
      static_cast<std::uint64_t>(width) * height > kMaxImagePixels) {
    return false;
  }
  if (token.stop_requested()) return false;
  const double scale = std::min(
      1.0, static_cast<double>(kMaxPreviewDimension) /
               static_cast<double>(std::max(width, height)));
  const UINT targetWidth = std::max<UINT>(1, static_cast<UINT>(width * scale));
  const UINT targetHeight = std::max<UINT>(1, static_cast<UINT>(height * scale));
  ComPtr<IWICBitmapSource> source = frame;
  ComPtr<IWICBitmapScaler> scaler;
  if (targetWidth != width || targetHeight != height) {
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeFant))) {
      return false;
    }
    source = scaler;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  result.width = targetWidth;
  result.height = targetHeight;
  result.stride = targetWidth * 4;
  result.pixels.resize(static_cast<std::size_t>(result.stride) * targetHeight);
  return SUCCEEDED(converter->CopyPixels(
      nullptr, result.stride, static_cast<UINT>(result.pixels.size()),
      reinterpret_cast<BYTE*>(result.pixels.data())));
}

}  // namespace

PreviewService::PreviewService(ResultSink sink, ErrorSink errors)
    : sink_(std::move(sink)), errors_(std::move(errors)) {}

PreviewService::~PreviewService() { Stop(); }

void PreviewService::Start() {
  std::lock_guard lock(mutex_);
  if (started_) return;
  stopping_ = false;
  started_ = true;
}

void PreviewService::Stop() {
  std::jthread worker;
  {
    std::lock_guard lock(mutex_);
    if (!started_ && !worker_.joinable()) return;
    stopping_ = true;
    started_ = false;
    pending_.reset();
    if (worker_.joinable()) worker_.request_stop();
    worker = std::move(worker_);
  }
  cv_.notify_all();
  if (worker.joinable()) worker.join();
  std::lock_guard lock(mutex_);
  workerActive_ = false;
  stopping_ = false;
}

bool PreviewService::Load(Request request) {
  generation_.store(request.generation, std::memory_order_release);
  std::jthread finishedWorker;
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !started_) return false;
    pending_ = std::move(request);
    if (!workerActive_) {
      if (worker_.joinable()) finishedWorker = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread([this](std::stop_token token) { WorkerLoop(token); });
    } else {
      notify = true;
    }
  }
  if (notify) cv_.notify_one();
  if (finishedWorker.joinable()) finishedWorker.join();
  return true;
}

void PreviewService::Invalidate(std::uint64_t generation) {
  generation_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  pending_.reset();
}

void PreviewService::WorkerLoop(std::stop_token token) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  // Only image decoding needs COM. Without it DecodeImage fails and the pane
  // says the image could not be decoded, so a failed initialization is not
  // fatal, but only a successful one may be balanced by CoUninitialize.
  const bool comInitialized =
      SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IWICImagingFactory* cachedFactory = nullptr;
  // An exception escaping this thread terminates the app, so neither a
  // failing preview nor a failing sink may leave the loop.
  const auto report = [this](std::exception_ptr error) noexcept {
    try {
      if (errors_) errors_(error);
    } catch (...) {
    }
  };
  constexpr auto kIdleTimeout = std::chrono::seconds(30);
  for (;;) {
    Request request;
    {
      std::unique_lock lock(mutex_);
      const bool woke = cv_.wait_for(lock, kIdleTimeout, [&] {
        return stopping_ || token.stop_requested() || pending_.has_value();
      });
      if (stopping_ || token.stop_requested()) {
        workerActive_ = false;
        break;
      }
      if (!woke && !pending_.has_value()) {
        workerActive_ = false;
        break;
      }
      request = std::move(*pending_);
      pending_.reset();
    }
    std::optional<Result> result;
    try {
      result = Build(request, token, comInitialized ? &cachedFactory : nullptr);
    } catch (...) {
      report(std::current_exception());
      // The pane shows "Loading preview..." until a result for the current
      // generation arrives, so a failed build must still answer.
      try {
        Result failed;
        failed.generation = request.generation;
        failed.kind = Kind::Error;
        failed.path = request.path;
        failed.title = request.path.filename().wstring();
        failed.detail = L"The preview could not be loaded.";
        result = std::move(failed);
      } catch (...) {
        report(std::current_exception());
      }
    }
    if (!result || token.stop_requested() ||
        generation_.load(std::memory_order_acquire) != request.generation ||
        !sink_) {
      continue;
    }
    try {
      sink_(std::move(*result));
    } catch (...) {
      report(std::current_exception());
    }
  }
  if (cachedFactory) {
    cachedFactory->Release();
    cachedFactory = nullptr;
  }
  if (comInitialized) CoUninitialize();
}

Result PreviewService::Build(const Request& request,
                             std::stop_token token,
                             IWICImagingFactory** cachedFactory) const {
  Result result;
  result.generation = request.generation;
  result.path = request.path;
  result.title = request.path.filename().wstring();
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (!GetFileAttributesExW(request.path.c_str(), GetFileExInfoStandard,
                            &attributes)) {
    result.kind = Kind::Error;
    result.detail = L"The selected file is no longer available.";
    return result;
  }
  const bool isDirectory =
      (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const std::uintmax_t bytes =
      isDirectory
          ? 0
          : (static_cast<std::uintmax_t>(attributes.nFileSizeHigh) << 32) |
                static_cast<std::uintmax_t>(attributes.nFileSizeLow);
  result.detail = MetadataDetail(request.path, bytes, attributes.ftLastWriteTime);
  if (isDirectory) return result;
  if (token.stop_requested()) return result;

  if (file_content::Supports(request.path)) {
    const auto extraction = file_content::Extract(
        request.path, file_content::kMaxIndexedBytes, token);
    if (extraction.state == file_content::State::Indexed) {
      result.kind = Kind::Text;
      result.text = Excerpt(extraction.text, request.terms);
    } else if (extraction.state != file_content::State::Unsupported) {
      result.detail += L"\nText preview is unavailable.";
    }
    return result;
  }
  if (file_content::IsImage(request.path) && bytes <= kMaxImageBytes) {
    if (DecodeImage(request.path, result, token, cachedFactory)) {
      result.kind = Kind::Image;
    } else {
      result.kind = Kind::Error;
      result.detail += L"\nThe image could not be safely decoded.";
    }
  }
  return result;
}

}  // namespace feathercast::preview
