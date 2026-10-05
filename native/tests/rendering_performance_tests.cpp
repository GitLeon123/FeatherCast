#include "text_layout_cache.hpp"
#include "test_framework.hpp"

#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using feathercast::ui::TextLayoutCache;
using feathercast::ui::TextLayoutOptions;
using feathercast::ui::TextLayoutStyle;

namespace {
void Check(HRESULT result) { assert(SUCCEEDED(result)); }

ComPtr<IDWriteTextFormat> Format(IDWriteFactory* factory, float size) {
  ComPtr<IDWriteTextFormat> format;
  Check(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format));
  return format;
}

void TestCache(IDWriteFactory* factory) {
  auto format = Format(factory, 14);
  TextLayoutCache cache(2, 32);
  auto one = cache.Get(factory, L"One", format.Get(), 100, 30);
  assert(one);
  auto two = cache.Get(factory, L"Two", format.Get(), 100, 30);
  assert(two);
  assert(cache.Get(factory, L"One", format.Get(), 100, 30).Get() == one.Get());
  assert(cache.Get(factory, L"Three", format.Get(), 100, 30));
  assert(cache.Size() == 2);
  assert(cache.Get(factory, L"One", format.Get(), 100, 30).Get() == one.Get());
  assert(cache.Get(factory, L"Two", format.Get(), 100, 30).Get() != two.Get());
  cache.Clear();
  assert(cache.Size() == 0 && cache.Characters() == 0);

  one = cache.Get(factory, L"Exact dimensions", format.Get(), 100, 30);
  auto resized = cache.Get(factory, L"Exact dimensions", format.Get(), 100.01f, 30);
  assert(one.Get() != resized.Get() && resized->GetMaxWidth() == 100.01f);
  auto otherFormat = Format(factory, 28);
  auto large = cache.Get(factory, L"Exact dimensions", otherFormat.Get(), 100, 30);
  assert(large.Get() != one.Get());
  float fontSize = 0;
  Check(large->GetFontSize(0, &fontSize));
  assert(fontSize == 28);

  cache.Clear();
  const std::wstring nulText{L'A', L'\0', L'B'};
  one = cache.Get(factory, nulText, format.Get(), 100, 30);
  assert(one && cache.Characters() == 3);
  assert(cache.Get(factory, nulText, format.Get(), 100, 30).Get() == one.Get());
  assert(cache.Get(factory, L"A", format.Get(), 100, 30).Get() != one.Get());
  assert(cache.Get(factory, L"", format.Get(), 100, 30));
  assert(!cache.Get(nullptr, L"Text", format.Get(), 100, 30));
  assert(!cache.Get(factory, L"Text", nullptr, 100, 30));
  assert(!cache.Get(factory, L"Text", format.Get(), 0, 30));
  assert(!cache.Get(factory, L"Text", format.Get(), INFINITY, 30));

  cache.Clear();
  assert(cache.Get(factory, std::wstring(40, L'x'), format.Get(), 100, 30));
  assert(cache.Size() == 0);  // over the aggregate character budget
  TextLayoutCache bounded(16, 12);
  for (const auto* text : {L"aaaa", L"bbbb", L"cccc", L"dddd"}) {
    assert(bounded.Get(factory, text, format.Get(), 100, 30));
  }
  assert(bounded.Size() == 3 && bounded.Characters() == 12);
  assert(bounded.Get(factory, std::wstring(1025, L'x'), format.Get(), 100, 30));
  assert(bounded.Size() == 3 && bounded.Characters() == 12);

  auto trimmed = bounded.Get(factory, L"Wide label", format.Get(), 20, 30,
      {TextLayoutStyle::SingleLineEllipsis, true, true});
  assert(trimmed->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP);
  assert(trimmed->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_CENTER);
  assert(trimmed->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  DWRITE_TRIMMING trimming{};
  ComPtr<IDWriteInlineObject> sign;
  Check(trimmed->GetTrimming(&trimming, &sign));
  assert(trimming.granularity == DWRITE_TRIMMING_GRANULARITY_CHARACTER && sign);
  auto measured = bounded.Get(factory, L"Wide label", format.Get(), 20, 30,
      {TextLayoutStyle::MeasureSingleLine});
  assert(measured.Get() != trimmed.Get());
  Check(measured->GetTrimming(&trimming, &sign));
  assert(trimming.granularity == DWRITE_TRIMMING_GRANULARITY_NONE && !sign);
}

void SavePng(IWICImagingFactory* factory, IWICBitmap* bitmap,
             const std::filesystem::path& path) {
  ComPtr<IWICStream> stream;
  Check(factory->CreateStream(&stream));
  Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder;
  Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
  Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame;
  Check(encoder->CreateNewFrame(&frame, nullptr));
  Check(frame->Initialize(nullptr));
  Check(frame->WriteSource(bitmap, nullptr));
  Check(frame->Commit());
  Check(encoder->Commit());
}

std::vector<BYTE> Render(IDWriteFactory* factory, IWICImagingFactory* wic,
                         ID2D1Factory* d2d, bool cached, float dpi,
                         float textScale, const std::filesystem::path& image) {
  const auto width = static_cast<UINT>(640 * dpi / 96);
  const auto height = static_cast<UINT>(360 * dpi / 96);
  ComPtr<IWICBitmap> bitmap;
  Check(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                         WICBitmapCacheOnLoad, &bitmap));
  ComPtr<ID2D1RenderTarget> target;
  Check(d2d->CreateWicBitmapRenderTarget(bitmap.Get(),
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
          dpi, dpi), &target));
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  ComPtr<ID2D1SolidColorBrush> brush;
  Check(target->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.97f), &brush));
  auto format = Format(factory, 14 * textScale);
  Check(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                               20 * textScale, 15 * textScale));
  TextLayoutCache layouts;
  const std::array<std::wstring, 5> texts{
      L"FeatherCast — Open applications", L"Café · 日本語 · مرحبا · 😀",
      L"A long application name that must use an ellipsis inside a compact control",
      L"A paragraph wraps at the same width and keeps the selected font size. "
      L"Settings and notification text remain readable at larger text sizes.",
      L"Trailing spaces   "};
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0.063f, 0.063f, 0.071f));
  float y = 16;
  for (std::size_t index = 0; index < texts.size(); ++index) {
    const float heightDip = index == 3 ? 100.0f : 50.0f;
    const auto rect = D2D1::RectF(16, y, index == 2 ? 320.0f : 624.0f, y + heightDip);
    const TextLayoutOptions options{
        index == 2 ? TextLayoutStyle::SingleLineEllipsis : TextLayoutStyle::Inherit,
        index == 2, index == 2};
    ComPtr<IDWriteTextLayout> layout;
    if (cached) {
      layout = layouts.Get(factory, texts[index], format.Get(), rect.right - rect.left,
                            heightDip, options);
    } else if (index == 2) {
      // The launcher's original compact-control path.
      Check(factory->CreateTextLayout(texts[index].c_str(), static_cast<UINT32>(texts[index].size()),
                                      format.Get(), 4096, heightDip, &layout));
      Check(layout->SetMaxWidth(rect.right - rect.left));
      Check(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
      ComPtr<IDWriteInlineObject> ellipsis;
      Check(factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
      const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      Check(layout->SetTrimming(&trimming, ellipsis.Get()));
      Check(layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
      Check(layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
    }
    if (layout) {
      target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush.Get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
      target->DrawTextW(texts[index].c_str(), static_cast<UINT32>(texts[index].size()),
                        format.Get(), rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    y += heightDip + 6;
  }
  Check(target->EndDraw());
  if (!image.empty()) SavePng(wic, bitmap.Get(), image);
  std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
  Check(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
  return pixels;
}

void Benchmark(IDWriteFactory* factory) {
  auto format = Format(factory, 14);
  std::array<std::wstring, 32> labels;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    labels[i] = L"Application " + std::to_wstring(i) + L" — Open or focus window";
  }
  TextLayoutCache cache;
  const auto sample = [&](bool cached) {
    const auto begin = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 100; ++frame) {
      for (const auto& text : labels) {
        ComPtr<IDWriteTextLayout> layout;
        if (cached) layout = cache.Get(factory, text, format.Get(), 480, 40);
        else Check(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                             format.Get(), 480, 40, &layout));
        DWRITE_TEXT_METRICS metrics{};
        Check(layout->GetMetrics(&metrics));
        assert(metrics.width > 0);
      }
    }
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
  };
  (void)sample(false);
  (void)sample(true);
  std::vector<double> original, optimized;
  for (int i = 0; i < 12; ++i) {
    original.push_back(sample(false));
    optimized.push_back(sample(true));
  }
  std::sort(original.begin(), original.end());
  std::sort(optimized.begin(), optimized.end());
  std::printf("32 labels x 100 frames, 12 warm samples: uncached p95 %.3f ms; "
              "cached p95 %.3f ms; cached layouts %zu\n",
              original.back(), optimized.back(), cache.Size());
  // The benchmark reports comparisons; only the optimized path has a budget.
  assert(optimized.back() < 25 * feathercast::test::TimingBudgetScale());
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  ComPtr<IDWriteFactory> factory;
  Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
  TestCache(factory.Get());
  ComPtr<IWICImagingFactory> wic;
  Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&wic)));
  ComPtr<ID2D1Factory> d2d;
  Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()));
  for (const float dpi : {96.0f, 144.0f, 192.0f}) {
    for (const float textScale : {1.0f, 2.0f}) {
      const auto evidence = [&](const wchar_t* name) -> std::filesystem::path {
        if (argc < 2) return {};
        return std::filesystem::path(argv[1]) /
            (std::wstring(name) + L"-" + std::to_wstring(static_cast<int>(dpi)) +
             L"-" + std::to_wstring(static_cast<int>(textScale * 100)) + L".png");
      };
      assert(Render(factory.Get(), wic.Get(), d2d.Get(), false, dpi, textScale, evidence(L"before")) ==
             Render(factory.Get(), wic.Get(), d2d.Get(), true, dpi, textScale, evidence(L"after")));
    }
  }
  Benchmark(factory.Get());
  d2d.Reset();
  wic.Reset();
  factory.Reset();
  CoUninitialize();
  std::puts("Rendering cache, bounds, Unicode and DPI/text-size pixel parity passed");
}
