#include "phone_ui.hpp"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>

#include "motion.hpp"
#include "qrcodegen.hpp"

namespace feathercast::phone_ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"FeatherCastPhoneWindow";
constexpr int kAppIconId = 101;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
constexpr float kHeaderHeight = 84.0f;
constexpr float kSidebarWidth = 212.0f;
constexpr float kPad = 24.0f;
constexpr size_t kMaxNotifications = 100;
// Only the newest notifications keep their app icon bytes; older rows show a letter badge.
constexpr size_t kMaxNotificationIcons = 30;
constexpr size_t kMaxClips = 50;
constexpr long long kQrRefreshMs = 4 * 60 * 1000 + 30 * 1000;

// Segoe MDL2 Assets glyphs.
constexpr wchar_t kGlyphPhone[] = L"";
constexpr wchar_t kGlyphHome[] = L"";
constexpr wchar_t kGlyphMessage[] = L"";
constexpr wchar_t kGlyphPhoto[] = L"";
constexpr wchar_t kGlyphPaste[] = L"";
constexpr wchar_t kGlyphDevices[] = L"";
constexpr wchar_t kGlyphCopy[] = L"";
constexpr wchar_t kGlyphClose[] = L"";
constexpr wchar_t kGlyphRefresh[] = L"";
constexpr wchar_t kGlyphSend[] = L"";

constexpr wchar_t kGlyphPrevious[] = L"\uE892";
constexpr wchar_t kGlyphPlay[] = L"\uE768";
constexpr wchar_t kGlyphPause[] = L"\uE769";
constexpr wchar_t kGlyphNext[] = L"\uE893";
constexpr wchar_t kGlyphRing[] = L"\uEA8F";
constexpr wchar_t kGlyphUpload[] = L"\uE898";
constexpr wchar_t kGlyphMusic[] = L"\uE8D6";
constexpr wchar_t kGlyphSms[] = L"\uE8BD";

// Sidebar order; SelectTab hits store the enum value.
enum class Tab { Overview, Notifications, Photos, Clipboard, Messages, Devices };

enum class Action {
  None,
  SelectTab,
  TurnOn,
  TurnOff,
  PairNew,
  CancelPairing,
  CopyNotification,
  DismissNotification,
  OpenPhoto,
  RefreshPhotos,
  CopyClip,
  SendPcClipboard,
  ToggleClipboardSync,
  ToggleToasts,
  ForgetDevice,
  ToggleLowBattery,
  FindPhone,
  SendFiles,
  Media,
  OpenSmsThread,
  RefreshSms,
};

struct Hit {
  D2D1_RECT_F rect{};
  Action action = Action::None;
  int index = 0;
  std::string id;
};

struct NotificationItem {
  phone::NotificationInfo info;
  ComPtr<ID2D1Bitmap> icon;
  bool iconFailed = false;
};

struct PhotoItem {
  phone::PhotoInfo info;
  ComPtr<ID2D1Bitmap> thumb;
  bool thumbFailed = false;
  bool downloading = false;
};

struct ClipItem {
  std::string text;
  long long time = 0;
};

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::wstring Widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

std::wstring FormatTime(long long ms) {
  if (ms <= 0) return {};
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  const std::time_t now = std::time(nullptr);
  std::tm value{};
  std::tm today{};
  localtime_s(&value, &seconds);
  localtime_s(&today, &now);
  wchar_t buffer[64]{};
  if (value.tm_year == today.tm_year && value.tm_yday == today.tm_yday) {
    wcsftime(buffer, std::size(buffer), L"%H:%M", &value);
  } else if (value.tm_year == today.tm_year) {
    wcsftime(buffer, std::size(buffer), L"%d %b, %H:%M", &value);
  } else {
    wcsftime(buffer, std::size(buffer), L"%d %b %Y", &value);
  }
  return buffer;
}

std::wstring SingleLine(std::wstring text) {
  for (auto& ch : text) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  return text;
}

D2D1_COLOR_F ToD2D(const theme::Color& color, float alphaScale = 1.0f) {
  return D2D1::ColorF(color.r, color.g, color.b, color.a * alphaScale);
}

D2D1_COLOR_F Opaque(const theme::Color& color) {
  return D2D1::ColorF(color.r * color.a, color.g * color.a, color.b * color.a, 1.0f);
}

D2D1_COLOR_F MixColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool Contains(const D2D1_RECT_F& rect, D2D1_POINT_2F point) {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
         point.y < rect.bottom;
}

UINT WindowDpi(HWND hwnd) {
  const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
  return dpi ? dpi : 96;
}

}  // namespace

struct PhoneWindow::Impl {
  HWND hwnd = nullptr;
  Callbacks callbacks;
  theme::Theme theme;
  theme::Color accent{0.36f, 0.42f, 1.0f, 1.0f};
  State state;

  // Model
  bool connected = false;
  std::string deviceName;
  int battery = -1;
  bool charging = false;
  std::deque<NotificationItem> notifications;
  std::vector<PhotoItem> photos;
  std::deque<ClipItem> clips;
  bool photosRequested = false;
  std::string dataDeviceId;  // Phone the lists above came from
  std::vector<std::string> features;
  bool ringing = false;
  phone::MediaInfo media;  // artJpeg keeps the current cover
  ComPtr<ID2D1Bitmap> mediaArt;
  bool mediaArtFailed = false;
  std::vector<phone::SmsThread> smsThreads;
  bool smsRequested = false;

  // View state
  Tab tab = Tab::Overview;
  bool pairing = false;
  std::string pairingUri;
  long long pairingCreatedAt = 0;
  std::map<Tab, float> scroll;
  float contentHeight = 0.0f;
  float viewportHeight = 0.0f;
  std::vector<Hit> hits;
  D2D1_POINT_2F mouse{-1.0f, -1.0f};
  std::wstring toast;
  long long toastUntil = 0;

  // Short-lived presentation state. The model and hit targets change immediately.
  bool fadeMotion = true;
  bool spatialMotion = true;
  bool controlMotion = true;
  bool closing = false;
  bool movingForAnimation = false;
  bool sidebarPositionReady = false;
  int naturalWindowY = 0;
  std::chrono::steady_clock::time_point lastFrame{};
  motion::ScalarAnimation windowOpacity;
  motion::Spring windowOffset;
  motion::Spring sidebarY;
  motion::ScalarAnimation contentOpacity;
  motion::ScalarAnimation contentOffset;
  motion::ScalarAnimation toastOpacity;
  motion::ScalarAnimation toastOffset;
  std::map<Tab, motion::Spring> visualScroll;
  std::map<Action, motion::Spring> toggleMotion;

  // Graphics
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<ID2D1HwndRenderTarget> target;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<IDWriteTextFormat> titleFormat;
  ComPtr<IDWriteTextFormat> headingFormat;
  ComPtr<IDWriteTextFormat> bodyFormat;
  ComPtr<IDWriteTextFormat> bodyWrapFormat;
  ComPtr<IDWriteTextFormat> smallFormat;
  ComPtr<IDWriteTextFormat> smallCenterFormat;
  ComPtr<IDWriteTextFormat> iconFormat;
  ComPtr<IDWriteTextFormat> iconLargeFormat;
  ComPtr<IDWriteTextFormat> stepFormat;
  ComPtr<IDWriteTextFormat> statFormat;

  Impl() {
    windowOpacity.Snap(1.0);
    windowOffset.Configure(0.30, 0.82);
    sidebarY.Configure(0.30, 0.76);
    contentOpacity.Snap(1.0);
    toastOpacity.Snap(0.0);
  }

  // ---------------------------------------------------------------- setup

  bool EnsureFactories() {
    if (!d2d &&
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
      return false;
    }
    if (!dwrite &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
      return false;
    }
    if (!wic) {
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(wic.GetAddressOf()));
    }
    if (!titleFormat) {
      const auto make = [&](ComPtr<IDWriteTextFormat>& format, const wchar_t* family,
                            float size, DWRITE_FONT_WEIGHT weight, bool wrap,
                            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                 format.GetAddressOf());
        if (!format) return;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!wrap) {
          format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
          DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
          ComPtr<IDWriteInlineObject> ellipsis;
          dwrite->CreateEllipsisTrimmingSign(format.Get(), ellipsis.GetAddressOf());
          format->SetTrimming(&trimming, ellipsis.Get());
        } else {
          format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
      };
      make(titleFormat, L"Segoe UI Variable Display", 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      make(headingFormat, L"Segoe UI Variable Text", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      make(bodyFormat, L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
      make(bodyWrapFormat, L"Segoe UI Variable Text", 13.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
      make(smallFormat, L"Segoe UI Variable Text", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false);
      make(smallCenterFormat, L"Segoe UI Variable Text", 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
           true, DWRITE_TEXT_ALIGNMENT_CENTER);
      make(iconFormat, L"Segoe MDL2 Assets", 15.0f, DWRITE_FONT_WEIGHT_NORMAL, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(iconLargeFormat, L"Segoe MDL2 Assets", 22.0f, DWRITE_FONT_WEIGHT_NORMAL, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(stepFormat, L"Segoe UI Variable Text", 14.0f, DWRITE_FONT_WEIGHT_BOLD, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(statFormat, L"Segoe UI Variable Display", 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      if (smallCenterFormat) {
        smallCenterFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      }
    }
    return titleFormat != nullptr;
  }

  void DiscardDeviceResources() {
    target.Reset();
    brush.Reset();
    for (auto& item : notifications) item.icon.Reset();
    for (auto& item : photos) item.thumb.Reset();
    mediaArt.Reset();
    mediaArtFailed = false;
  }

  bool HasFeature(std::string_view feature) const {
    return std::find(features.begin(), features.end(), feature) != features.end();
  }

  // Frees everything the window only needs while it is open. Photos are requested again the
  // next time the window opens; notification icons are decoded again from their PNG bytes.
  void ReleaseWindowResources() {
    DiscardDeviceResources();
    ComPtr<IDWriteTextFormat>* formats[] = {
        &titleFormat, &headingFormat, &bodyFormat, &bodyWrapFormat, &smallFormat,
        &smallCenterFormat, &iconFormat, &iconLargeFormat, &stepFormat, &statFormat};
    for (auto* format : formats) format->Reset();
    wic.Reset();
    dwrite.Reset();
    d2d.Reset();
    photos.clear();
    photos.shrink_to_fit();
    photosRequested = false;
    hits.clear();
    hits.shrink_to_fit();
    pairingUri.clear();
    toast.clear();
  }

  bool EnsureTarget() {
    if (target) return true;
    if (!EnsureFactories()) return false;
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT dpi = WindowDpi(hwnd);
    // A software target avoids a second Direct3D device (and its driver heaps) next to the
    // main overlay's device. This window is a static dashboard that repaints rarely.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi), static_cast<float>(dpi));
    if (FAILED(d2d->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(
                hwnd, D2D1::SizeU(static_cast<UINT32>(client.right),
                                  static_cast<UINT32>(client.bottom))),
            target.GetAddressOf()))) {
      return false;
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush.GetAddressOf());
    return brush != nullptr;
  }

  ComPtr<ID2D1Bitmap> DecodeImage(const phone::Bytes& data) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (data.empty() || !wic || !target) return bitmap;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    if (!stream) return bitmap;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                            WICDecodeMetadataCacheOnDemand,
                                            decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
      return bitmap;
    }
    target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf());
    return bitmap;
  }

  // -------------------------------------------------------------- drawing

  D2D1_COLOR_F Background() const { return Opaque(theme.settingsBackground); }
  D2D1_COLOR_F Surface() const { return Opaque(theme.surface); }
  D2D1_COLOR_F Accent() const { return ToD2D(accent); }
  D2D1_COLOR_F TextPrimary() const { return ToD2D(theme.textPrimary); }
  D2D1_COLOR_F TextMuted() const { return ToD2D(theme.textMuted); }

  void Fill(const D2D1_RECT_F& rect, D2D1_COLOR_F color, float radius = 0.0f) {
    brush->SetColor(color);
    if (radius > 0.0f) {
      target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    } else {
      target->FillRectangle(rect, brush.Get());
    }
  }

  void Stroke(const D2D1_RECT_F& rect, D2D1_COLOR_F color, float radius, float width = 1.0f) {
    brush->SetColor(color);
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                                      rect.right - 0.5f, rect.bottom - 0.5f),
                          radius, radius),
        brush.Get(), width);
  }

  void Text(const std::wstring& text, const D2D1_RECT_F& rect, IDWriteTextFormat* format,
            D2D1_COLOR_F color) {
    if (text.empty() || !format || rect.right <= rect.left) return;
    brush->SetColor(color);
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  float MeasureWidth(const std::wstring& text, IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        format, 4000.0f, 100.0f, layout.GetAddressOf()))) {
      return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
  }

  bool Hovered(const D2D1_RECT_F& rect) const { return Contains(rect, mouse); }

  void AddHit(const D2D1_RECT_F& rect, Action action, int index = 0, std::string id = {}) {
    hits.push_back(Hit{rect, action, index, std::move(id)});
  }

  // Returns the button width so callers can lay out rows.
  float Button(float x, float y, const std::wstring& label, Action action, bool primary,
               const wchar_t* glyph = nullptr, std::string id = {}, bool danger = false) {
    const float textWidth = MeasureWidth(label, bodyFormat.Get());
    const float width = textWidth + (glyph ? 52.0f : 32.0f);
    const D2D1_RECT_F rect{x, y, x + width, y + 36.0f};
    const bool hover = Hovered(rect);
    D2D1_COLOR_F fill = primary ? Accent() : ToD2D(theme.surfaceHover, hover ? 1.0f : 0.6f);
    if (primary && hover) fill = MixColor(fill, D2D1::ColorF(1, 1, 1), 0.12f);
    Fill(rect, fill, theme.controlRadius);
    const D2D1_COLOR_F textColor =
        primary ? D2D1::ColorF(1, 1, 1) : (danger ? ToD2D(theme.dangerText) : TextPrimary());
    float textLeft = x + 16.0f;
    if (glyph) {
      Text(glyph, {x + 10.0f, y, x + 34.0f, y + 36.0f}, iconFormat.Get(), textColor);
      textLeft = x + 36.0f;
    }
    Text(label, {textLeft, y, x + width - 12.0f, y + 36.0f}, bodyFormat.Get(), textColor);
    AddHit(rect, action, 0, std::move(id));
    return width;
  }

  void IconButton(const D2D1_RECT_F& rect, const wchar_t* glyph, Action action,
                  std::string id, int index = 0) {
    if (Hovered(rect)) Fill(rect, ToD2D(theme.surfaceHover), theme.controlRadius);
    Text(glyph, rect, iconFormat.Get(), TextMuted());
    AddHit(rect, action, index, std::move(id));
  }

  float Toggle(float left, float right, float y, const std::wstring& label,
               const std::wstring& description, bool on, Action action) {
    const D2D1_RECT_F row{left, y, right, y + 64.0f};
    if (Hovered(row)) Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
    Text(label, {left + 14.0f, y + 10.0f, right - 80.0f, y + 32.0f}, bodyFormat.Get(),
         TextPrimary());
    Text(description, {left + 14.0f, y + 32.0f, right - 80.0f, y + 54.0f},
         smallFormat.Get(), TextMuted());
    const D2D1_RECT_F track{right - 62.0f, y + 21.0f, right - 18.0f, y + 43.0f};
    auto [it, inserted] = toggleMotion.try_emplace(action);
    if (inserted) {
      it->second.Configure(0.24, 0.78);
      it->second.Snap(on ? 1.0 : 0.0);
    }
    it->second.Retarget(on ? 1.0 : 0.0, controlMotion);
    const float value = static_cast<float>(std::clamp(it->second.Value(), 0.0, 1.0));
    Fill(track, MixColor(ToD2D(theme.surfaceHover), Accent(), value), 11.0f);
    if (value < 0.99f) Stroke(track, ToD2D(theme.textDim, 0.6f * (1.0f - value)), 11.0f);
    const float knobX = track.left + 11.0f + value * (track.right - track.left - 22.0f);
    brush->SetColor(MixColor(TextMuted(), D2D1::ColorF(1, 1, 1), value));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, track.top + 11.0f), 7.0f, 7.0f),
                        brush.Get());
    AddHit(row, action);
    return 64.0f;
  }

  void SectionLabel(const std::wstring& label, float left, float right, float y) {
    Text(label, {left, y, right, y + 20.0f}, smallFormat.Get(), ToD2D(theme.sectionText));
  }

  void Card(const D2D1_RECT_F& rect) {
    Fill(rect, Surface(), theme.settingsRadius + 2.0f);
    Stroke(rect, ToD2D(theme.border), theme.settingsRadius + 2.0f);
  }

  void DrawQr(const std::string& payload, const D2D1_RECT_F& box) {
    try {
      const auto qr = qrcodegen::QrCode::encodeText(payload.c_str(),
                                                    qrcodegen::QrCode::Ecc::MEDIUM);
      const int size = qr.getSize();
      const int quiet = 3;
      const float side = std::min(box.right - box.left, box.bottom - box.top);
      const float module = std::floor(side / static_cast<float>(size + 2 * quiet));
      const float drawn = module * static_cast<float>(size + 2 * quiet);
      const float x0 = box.left + std::floor((box.right - box.left - drawn) / 2.0f);
      const float y0 = box.top + std::floor((box.bottom - box.top - drawn) / 2.0f);
      Fill({x0, y0, x0 + drawn, y0 + drawn}, D2D1::ColorF(1, 1, 1), 10.0f);
      brush->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.09f));
      for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
          if (!qr.getModule(x, y)) continue;
          const float px = x0 + module * static_cast<float>(x + quiet);
          const float py = y0 + module * static_cast<float>(y + quiet);
          target->FillRectangle({px, py, px + module, py + module}, brush.Get());
        }
      }
    } catch (...) {
      Text(L"QR code unavailable", box, smallCenterFormat.Get(), TextMuted());
    }
  }

  void StepBadge(int number, float x, float y) {
    brush->SetColor(Accent());
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 14.0f, y + 14.0f), 14.0f, 14.0f),
                        brush.Get());
    Text(std::to_wstring(number), {x, y, x + 28.0f, y + 28.0f}, stepFormat.Get(),
         D2D1::ColorF(1, 1, 1));
  }

  void DrawHeader(float width) {
    Fill({0, 0, width, kHeaderHeight}, Surface());
    brush->SetColor(ToD2D(theme.divider));
    target->DrawLine({0, kHeaderHeight - 0.5f}, {width, kHeaderHeight - 0.5f}, brush.Get());

    const float cx = kPad + 24.0f;
    const float cy = kHeaderHeight / 2.0f;
    brush->SetColor(ToD2D(accent, 0.18f));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 24.0f, 24.0f), brush.Get());
    Text(kGlyphPhone, {cx - 24.0f, cy - 24.0f, cx + 24.0f, cy + 24.0f}, iconLargeFormat.Get(),
         Accent());

    const bool paired = !state.devices.empty();
    std::wstring title = L"FeatherCast Phone";
    if (connected && !deviceName.empty()) {
      title = Widen(deviceName);
    } else if (paired) {
      title = Widen(state.devices.back().name);
    }
    const float textLeft = cx + 38.0f;
    Text(title, {textLeft, 16.0f, width - 260.0f, 46.0f}, titleFormat.Get(), TextPrimary());

    std::wstring status;
    D2D1_COLOR_F dot = ToD2D(theme.textDim);
    if (!state.enabled) {
      status = L"Phone connection is off";
    } else if (!state.running) {
      status = L"Not available";
      dot = ToD2D(theme.danger);
    } else if (connected) {
      status = L"Connected";
      if (battery >= 0) {
        status += L"  ·  Battery " + std::to_wstring(battery) + L"%";
        if (charging) status += L" (charging)";
      }
      dot = ToD2D(theme.success);
    } else if (paired) {
      status = L"Waiting for your phone – open FeatherCast Phone on it";
    } else {
      status = L"No phone connected yet";
    }
    brush->SetColor(dot);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(textLeft + 5.0f, 60.0f), 4.5f, 4.5f),
                        brush.Get());
    Text(status, {textLeft + 16.0f, 50.0f, width - 260.0f, 70.0f}, smallFormat.Get(),
         TextMuted());

    if (state.enabled && state.running && paired && !pairing) {
      const float w = MeasureWidth(L"Pair new phone", bodyFormat.Get()) + 52.0f;
      Button(width - kPad - w, cy - 18.0f, L"Pair new phone", Action::PairNew, false,
             kGlyphDevices);
    }
  }

  void DrawOff(float width, float height) {
    const float cardWidth = std::min(520.0f, width - 2 * kPad);
    const float left = (width - cardWidth) / 2.0f;
    const float top = kHeaderHeight + std::max(32.0f, (height - kHeaderHeight - 260.0f) / 2.0f);
    const D2D1_RECT_F card{left, top, left + cardWidth, top + 240.0f};
    Card(card);
    Text(L"Connect your Android phone", {left + 28, top + 24, card.right - 28, top + 54},
         titleFormat.Get(), TextPrimary());
    Text(L"See your phone's notifications, photos and clipboard on this PC, and send "
         L"your PC clipboard to the phone. Everything stays on your local network and "
         L"is end-to-end encrypted.",
         {left + 28, top + 64, card.right - 28, top + 140}, bodyWrapFormat.Get(), TextMuted());
    if (!state.error.empty()) {
      Text(Widen(state.error), {left + 28, top + 140, card.right - 28, top + 164},
           smallFormat.Get(), ToD2D(theme.dangerText));
    }
    Button(left + 28, top + 176, L"Turn on phone connection", Action::TurnOn, true,
           kGlyphPhone);
  }

  void DrawPairing(float width, float height) {
    const float top = kHeaderHeight + 28.0f;
    Text(L"Connect your phone in three steps", {kPad, top, width - kPad, top + 30},
         titleFormat.Get(), TextPrimary());
    Text(L"Your phone and this PC need to be on the same Wi-Fi network.",
         {kPad, top + 32, width - kPad, top + 54}, bodyFormat.Get(), TextMuted());

    const bool stacked = width < 760.0f;
    const float gap = 20.0f;
    const float cardTop = top + 72.0f;
    const float available = height - cardTop - 110.0f;
    const float cardWidth = stacked ? width - 2 * kPad : (width - 2 * kPad - gap) / 2.0f;
    const float qrSide = std::clamp(std::min(cardWidth - 80.0f, available - 120.0f), 150.0f, 260.0f);
    const float cardHeight = qrSide + 132.0f;

    const auto drawStep = [&](int number, float x, float y, const std::wstring& heading,
                              const std::wstring& caption, const std::string& qrPayload) {
      const D2D1_RECT_F card{x, y, x + cardWidth, y + cardHeight};
      Card(card);
      StepBadge(number, x + 20.0f, y + 18.0f);
      Text(heading, {x + 58.0f, y + 18.0f, card.right - 20.0f, y + 46.0f},
           headingFormat.Get(), TextPrimary());
      const float qrLeft = x + (cardWidth - qrSide) / 2.0f;
      const D2D1_RECT_F box{qrLeft, y + 58.0f, qrLeft + qrSide, y + 58.0f + qrSide};
      if (!qrPayload.empty()) {
        DrawQr(qrPayload, box);
      } else {
        Fill(box, ToD2D(theme.surfaceHover, 0.5f), 10.0f);
      }
      Text(caption, {x + 20.0f, box.bottom + 12.0f, card.right - 20.0f, card.bottom - 8.0f},
           smallCenterFormat.Get(), TextMuted());
    };

    const std::string apkUrl = callbacks.apkUrl ? callbacks.apkUrl() : std::string{};
    const std::wstring installCaption =
        state.apkAvailable
            ? L"Scan with the phone camera, download and install FeatherCast Phone.\n" +
                  Widen(apkUrl)
            : L"The app file (FeatherCast-Phone.apk) was not found next to FeatherCast.exe. "
              L"Copy it to your phone and install it.";
    const float x1 = kPad;
    const float y1 = cardTop;
    const float x2 = stacked ? kPad : kPad + cardWidth + gap;
    const float y2 = stacked ? cardTop + cardHeight + gap : cardTop;
    drawStep(1, x1, y1, L"Install the app", installCaption,
             state.apkAvailable ? apkUrl : std::string{});
    drawStep(2, x2, y2, L"Scan this pairing code",
             L"Open FeatherCast Phone and tap “Scan QR code”. "
             L"The code renews itself every few minutes.",
             pairingUri);

    const float footer = (stacked ? y2 : y1) + cardHeight + 18.0f;
    StepBadge(3, kPad, footer);
    Text(L"Done – your phone shows up here automatically. If Windows asks, allow "
         L"FeatherCast on private networks.",
         {kPad + 40.0f, footer, width - kPad, footer + 28.0f}, bodyFormat.Get(), TextMuted());
    if (!state.devices.empty()) {
      Button(kPad, footer + 42.0f, L"Back to my phone", Action::CancelPairing, false);
    }
    contentHeight = footer + 90.0f - kHeaderHeight;
  }

  void DrawSidebar(float height) {
    Fill({0, kHeaderHeight, kSidebarWidth, height}, Opaque(theme.settingsBackground));
    brush->SetColor(ToD2D(theme.divider));
    target->DrawLine({kSidebarWidth - 0.5f, kHeaderHeight}, {kSidebarWidth - 0.5f, height},
                     brush.Get());
    struct Entry {
      Tab tab;
      const wchar_t* glyph;
      const wchar_t* label;
      size_t count;
    };
    const Entry entries[] = {
        {Tab::Overview, kGlyphHome, L"Overview", 0},
        {Tab::Notifications, kGlyphMessage, L"Notifications", notifications.size()},
        {Tab::Photos, kGlyphPhoto, L"Photos", photos.size()},
        {Tab::Clipboard, kGlyphPaste, L"Clipboard", clips.size()},
        {Tab::Messages, kGlyphSms, L"Messages", smsThreads.size()},
        {Tab::Devices, kGlyphDevices, L"Devices", 0},
    };
    const float selectedY = kHeaderHeight + 16.0f + static_cast<int>(tab) * 46.0f;
    if (!sidebarPositionReady) {
      sidebarY.Snap(selectedY);
      sidebarPositionReady = true;
    }
    const float pillY = static_cast<float>(sidebarY.Value());
    const D2D1_RECT_F pill{12.0f, pillY, kSidebarWidth - 12.0f, pillY + 42.0f};
    Fill(pill, ToD2D(accent, 0.18f), theme.rowRadius);
    Fill({pill.left, pill.top + 11.0f, pill.left + 3.0f, pill.bottom - 11.0f},
         Accent(), 1.5f);
    float y = kHeaderHeight + 16.0f;
    int index = 0;
    for (const auto& entry : entries) {
      const D2D1_RECT_F row{12.0f, y, kSidebarWidth - 12.0f, y + 42.0f};
      const bool selected = tab == entry.tab;
      if (!selected && Hovered(row)) {
        Fill(row, ToD2D(theme.surfaceHover, 0.6f), theme.rowRadius);
      }
      Text(entry.glyph, {row.left + 8.0f, row.top, row.left + 36.0f, row.bottom},
           iconFormat.Get(), selected ? Accent() : TextMuted());
      Text(entry.label, {row.left + 42.0f, row.top, row.right - 40.0f, row.bottom},
           bodyFormat.Get(), TextPrimary());
      if (entry.count > 0) {
        const std::wstring count =
            entry.count > 99 ? std::wstring(L"99+") : std::to_wstring(entry.count);
        Text(count, {row.right - 40.0f, row.top, row.right - 10.0f, row.bottom},
             smallFormat.Get(), TextMuted());
      }
      AddHit(row, Action::SelectTab, index++);
      y += 46.0f;
    }
  }

  float DrawNotificationRow(const D2D1_RECT_F& row, NotificationItem& item, int index) {
    const bool hover = Hovered(row);
    if (hover) Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
    AddHit(row, Action::CopyNotification, index, item.info.key);
    const D2D1_RECT_F iconRect{row.left + 14.0f, row.top + 16.0f, row.left + 50.0f,
                               row.top + 52.0f};
    if (!item.icon && !item.iconFailed && !item.info.iconPng.empty()) {
      item.icon = DecodeImage(item.info.iconPng);
      item.iconFailed = !item.icon;
    }
    if (item.icon) {
      target->DrawBitmap(item.icon.Get(), iconRect);
    } else {
      Fill(iconRect, ToD2D(accent, 0.22f), 18.0f);
      const std::wstring appName = Widen(item.info.appName);
      const std::wstring letter = appName.empty() ? L"?" : appName.substr(0, 1);
      Text(letter, iconRect, stepFormat.Get(), TextPrimary());
    }
    const float textLeft = row.left + 64.0f;
    const float textRight = row.right - 90.0f;
    std::wstring meta = Widen(item.info.appName);
    const std::wstring time = FormatTime(item.info.time);
    if (!time.empty()) meta += (meta.empty() ? L"" : L"  ·  ") + time;
    Text(meta, {textLeft, row.top + 8.0f, textRight, row.top + 26.0f}, smallFormat.Get(),
         TextMuted());
    Text(SingleLine(Widen(item.info.title)), {textLeft, row.top + 26.0f, textRight, row.top + 46.0f},
         headingFormat.Get(), TextPrimary());
    Text(SingleLine(Widen(item.info.text)), {textLeft, row.top + 46.0f, textRight, row.top + 66.0f},
         bodyFormat.Get(), TextMuted());
    const float by = row.top + 20.0f;
    IconButton({row.right - 82.0f, by, row.right - 50.0f, by + 32.0f}, kGlyphCopy,
               Action::CopyNotification, item.info.key, index);
    IconButton({row.right - 44.0f, by, row.right - 12.0f, by + 32.0f}, kGlyphClose,
               Action::DismissNotification, item.info.key, index);
    return row.bottom - row.top;
  }

  void EmptyMessage(const std::wstring& title, const std::wstring& body, float left,
                    float right, float y) {
    Text(title, {left, y, right, y + 24.0f}, headingFormat.Get(), TextPrimary());
    Text(body, {left, y + 28.0f, right, y + 90.0f}, bodyWrapFormat.Get(), TextMuted());
  }

  float DrawOverview(float left, float right, float y) {
    const float start = y;
    // Stat cards
    struct Stat {
      const wchar_t* label;
      size_t value;
      Tab tab;
    };
    const Stat stats[] = {{L"Notifications", notifications.size(), Tab::Notifications},
                          {L"Photos", photos.size(), Tab::Photos},
                          {L"Clipboard items", clips.size(), Tab::Clipboard}};
    const float gap = 14.0f;
    const float cardWidth = (right - left - 2 * gap) / 3.0f;
    for (int i = 0; i < 3; ++i) {
      const float x = left + static_cast<float>(i) * (cardWidth + gap);
      const D2D1_RECT_F card{x, y, x + cardWidth, y + 88.0f};
      Card(card);
      if (Hovered(card)) Stroke(card, ToD2D(accent, 0.7f), theme.settingsRadius + 2.0f);
      Text(std::to_wstring(stats[i].value), {x + 18, y + 12, card.right - 12, y + 50},
           statFormat.Get(), TextPrimary());
      Text(stats[i].label, {x + 18, y + 52, card.right - 12, y + 74}, smallFormat.Get(),
           TextMuted());
      AddHit(card, Action::SelectTab, static_cast<int>(stats[i].tab));
    }
    y += 112.0f;

    if (!connected) {
      Card({left, y, right, y + 84.0f});
      Text(L"Your phone is not connected right now.", {left + 20, y + 14, right - 20, y + 38},
           headingFormat.Get(), TextPrimary());
      Text(L"Open FeatherCast Phone on your phone. It reconnects automatically when both "
           L"devices are on the same Wi-Fi.",
           {left + 20, y + 40, right - 20, y + 78}, bodyWrapFormat.Get(), TextMuted());
      y += 104.0f;
    }

    if (connected && media.active) y += DrawMediaCard(left, right, y);

    SectionLabel(L"QUICK ACTIONS", left, right, y);
    y += 26.0f;
    float x = left;
    x += Button(x, y, L"Send PC clipboard to phone", Action::SendPcClipboard, true, kGlyphSend) + 10;
    x += Button(x, y, L"Send files\u2026", Action::SendFiles, false, kGlyphUpload) + 10;
    x += Button(x, y, ringing ? L"Stop ringing" : L"Find my phone", Action::FindPhone, false,
                kGlyphRing) + 10;
    if (x + 180.0f < right) {
      Button(x, y, L"Load latest photos", Action::RefreshPhotos, false, kGlyphRefresh);
    }
    y += 44.0f;
    Text(L"Tip: drop files on this window to send them to your phone.",
         {left, y, right, y + 20.0f}, smallFormat.Get(), TextMuted());
    y += 34.0f;

    SectionLabel(L"LATEST NOTIFICATIONS", left, right, y);
    y += 26.0f;
    if (notifications.empty()) {
      EmptyMessage(L"No notifications yet",
                   L"Allow “Notifications to PC” in the phone app. New messages "
                   L"from WhatsApp, SMS, e-mail and other apps will appear here.",
                   left, right, y);
      y += 96.0f;
    } else {
      const size_t count = std::min<size_t>(3, notifications.size());
      for (size_t i = 0; i < count; ++i) {
        y += DrawNotificationRow({left, y, right, y + 72.0f}, notifications[i],
                                 static_cast<int>(i)) + 4.0f;
      }
      if (notifications.size() > count) {
        Button(left, y + 4.0f, L"Show all notifications", Action::SelectTab, false);
        hits.back().index = static_cast<int>(Tab::Notifications);
        y += 48.0f;
      }
      y += 16.0f;
    }

    SectionLabel(L"PHONE CLIPBOARD", left, right, y);
    y += 26.0f;
    if (clips.empty()) {
      EmptyMessage(L"Nothing copied on the phone yet",
                   L"Text you copy on the phone shows up here when the app is open, or "
                   L"when you use “Send clipboard to PC”.",
                   left, right, y);
      y += 96.0f;
    } else {
      y += DrawClipRow(left, right, y, clips.front(), 0);
    }
    return y - start;
  }

  float DrawMediaCard(float left, float right, float y) {
    const D2D1_RECT_F card{left, y, right, y + 96.0f};
    Card(card);
    const D2D1_RECT_F art{left + 14.0f, y + 14.0f, left + 82.0f, y + 82.0f};
    if (!mediaArt && !mediaArtFailed && !media.artJpeg.empty()) {
      mediaArt = DecodeImage(media.artJpeg);
      mediaArtFailed = !mediaArt;
    }
    if (mediaArt) {
      target->DrawBitmap(mediaArt.Get(), art);
    } else {
      Fill(art, ToD2D(accent, 0.22f), 8.0f);
      Text(kGlyphMusic, art, iconLargeFormat.Get(), TextPrimary());
    }
    const float textLeft = art.right + 16.0f;
    const float buttons = right - 150.0f;
    std::wstring meta = Widen(media.appName.empty() ? media.app : media.appName);
    meta += (meta.empty() ? L"" : L"  \u00b7  ") + std::wstring(media.playing ? L"Playing" : L"Paused");
    Text(meta, {textLeft, y + 16.0f, buttons, y + 34.0f}, smallFormat.Get(), TextMuted());
    Text(SingleLine(Widen(media.title)), {textLeft, y + 34.0f, buttons, y + 58.0f},
         headingFormat.Get(), TextPrimary());
    Text(SingleLine(Widen(media.artist)), {textLeft, y + 58.0f, buttons, y + 78.0f},
         bodyFormat.Get(), TextMuted());
    const float by = y + 30.0f;
    IconButton({right - 144.0f, by, right - 104.0f, by + 36.0f}, kGlyphPrevious, Action::Media,
               "prev");
    IconButton({right - 100.0f, by, right - 60.0f, by + 36.0f},
               media.playing ? kGlyphPause : kGlyphPlay, Action::Media, "toggle");
    IconButton({right - 56.0f, by, right - 16.0f, by + 36.0f}, kGlyphNext, Action::Media,
               "next");
    return 116.0f;
  }

  float DrawMessages(float left, float right, float y) {
    const float start = y;
    float x = left;
    x += Button(x, y, L"Refresh", Action::RefreshSms, false, kGlyphRefresh) + 14.0f;
    Text(L"Click a conversation to read and reply in the launcher.", {x, y, right, y + 36.0f},
         smallFormat.Get(), TextMuted());
    y += 54.0f;
    if (smsThreads.empty()) {
      const bool allowed = HasFeature("sms");
      EmptyMessage(!connected ? L"Phone not connected"
                   : allowed  ? L"No conversations loaded"
                              : L"Text messages are off",
                   !connected ? L"Open FeatherCast Phone on your phone to see your text messages."
                   : allowed  ? L"Click Refresh to load your text messages."
                              : L"On the phone, open FeatherCast Phone and turn on "
                                L"\u201cText messages\u201d under Optional.",
                   left, right, y);
      return y - start + 100.0f;
    }
    for (size_t i = 0; i < smsThreads.size(); ++i) {
      if (y - start > 20000.0f) break;
      const auto& thread = smsThreads[i];
      const D2D1_RECT_F row{left, y, right, y + 64.0f};
      if (Hovered(row)) Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
      const std::wstring name = Widen(thread.name.empty() ? thread.address : thread.name);
      const D2D1_RECT_F badge{left + 14.0f, y + 14.0f, left + 50.0f, y + 50.0f};
      Fill(badge, ToD2D(accent, thread.unread ? 0.5f : 0.22f), 18.0f);
      Text(name.empty() ? L"?" : name.substr(0, 1), badge, stepFormat.Get(), TextPrimary());
      Text(name, {left + 64.0f, y + 10.0f, right - 120.0f, y + 32.0f}, headingFormat.Get(),
           TextPrimary());
      Text(FormatTime(thread.time), {right - 116.0f, y + 10.0f, right - 12.0f, y + 32.0f},
           smallFormat.Get(), thread.unread ? Accent() : TextMuted());
      Text(SingleLine(Widen(thread.snippet)), {left + 64.0f, y + 32.0f, right - 12.0f, y + 54.0f},
           bodyFormat.Get(), TextMuted());
      AddHit(row, Action::OpenSmsThread, static_cast<int>(i), thread.thread);
      y += 68.0f;
    }
    return y - start;
  }

  float DrawClipRow(float left, float right, float y, const ClipItem& clip, int index) {
    const D2D1_RECT_F row{left, y, right, y + 76.0f};
    Card(row);
    if (Hovered(row)) Fill(row, ToD2D(theme.surfaceHover, 0.4f), theme.settingsRadius + 2.0f);
    Text(FormatTime(clip.time), {left + 16, y + 8, right - 60, y + 26}, smallFormat.Get(),
         TextMuted());
    brush->SetColor(TextPrimary());
    const std::wstring text = Widen(clip.text);
    target->PushAxisAlignedClip({left + 16, y + 26, right - 60, y + 70},
                                D2D1_ANTIALIAS_MODE_ALIASED);
    Text(text.substr(0, 600), {left + 16, y + 28, right - 60, y + 120}, bodyWrapFormat.Get(),
         TextPrimary());
    target->PopAxisAlignedClip();
    AddHit(row, Action::CopyClip, index);
    IconButton({right - 48, y + 22, right - 14, y + 54}, kGlyphCopy, Action::CopyClip, {},
               index);
    return 84.0f;
  }

  float DrawNotifications(float left, float right, float y) {
    const float start = y;
    SectionLabel(L"NOTIFICATIONS FROM YOUR PHONE", left, right, y);
    y += 28.0f;
    if (notifications.empty()) {
      EmptyMessage(L"No notifications yet",
                   L"On the phone, open FeatherCast Phone and turn on “Notifications "
                   L"to PC”. Android will ask you to allow notification access.",
                   left, right, y);
      return 120.0f;
    }
    Text(L"Click a notification to copy its text. ✕ dismisses it on the phone.",
         {left, y, right, y + 20.0f}, smallFormat.Get(), TextMuted());
    y += 30.0f;
    for (size_t i = 0; i < notifications.size(); ++i) {
      if (y - start > 20000.0f) break;
      y += DrawNotificationRow({left, y, right, y + 72.0f}, notifications[i],
                               static_cast<int>(i)) + 4.0f;
    }
    return y - start;
  }

  float DrawPhotos(float left, float right, float y) {
    const float start = y;
    float x = left;
    x += Button(x, y, L"Refresh", Action::RefreshPhotos, false, kGlyphRefresh) + 14.0f;
    Text(L"Click a photo to save it to Downloads\\FeatherCast and open it.",
         {x, y, right, y + 36.0f}, smallFormat.Get(), TextMuted());
    y += 54.0f;
    if (photos.empty()) {
      EmptyMessage(connected ? L"No photos loaded" : L"Phone not connected",
                   connected ? L"Click Refresh. On the phone, allow “Photos to PC” "
                               L"if Android asks for access to your photos."
                             : L"Open FeatherCast Phone on your phone to see its latest photos.",
                   left, right, y);
      return y - start + 100.0f;
    }
    const float tile = 150.0f;
    const float gap = 12.0f;
    const int columns = std::max(1, static_cast<int>((right - left + gap) / (tile + gap)));
    const float tileSize = (right - left - gap * static_cast<float>(columns - 1)) /
                           static_cast<float>(columns);
    for (size_t i = 0; i < photos.size(); ++i) {
      auto& photo = photos[i];
      const int column = static_cast<int>(i) % columns;
      const int rowIndex = static_cast<int>(i) / columns;
      const float px = left + static_cast<float>(column) * (tileSize + gap);
      const float py = y + static_cast<float>(rowIndex) * (tileSize + gap);
      const D2D1_RECT_F rect{px, py, px + tileSize, py + tileSize};
      Fill(rect, ToD2D(theme.surfaceHover, 0.5f), 8.0f);
      if (!photo.thumb && !photo.thumbFailed && !photo.info.thumbJpeg.empty()) {
        photo.thumb = DecodeImage(photo.info.thumbJpeg);
        photo.thumbFailed = !photo.thumb;
      }
      if (photo.thumb) {
        const D2D1_SIZE_F size = photo.thumb->GetSize();
        const float side = std::min(size.width, size.height);
        const D2D1_RECT_F source{(size.width - side) / 2.0f, (size.height - side) / 2.0f,
                                 (size.width + side) / 2.0f, (size.height + side) / 2.0f};
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, 8.0f, 8.0f),
                                            clip.GetAddressOf());
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
        target->DrawBitmap(photo.thumb.Get(), rect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
        target->PopLayer();
      } else {
        Text(kGlyphPhoto, rect, iconLargeFormat.Get(), TextMuted());
      }
      if (Hovered(rect)) Stroke(rect, Accent(), 8.0f, 2.0f);
      if (photo.downloading) {
        Fill(rect, D2D1::ColorF(0, 0, 0, 0.55f), 8.0f);
        Text(L"Downloading…", rect, smallCenterFormat.Get(), D2D1::ColorF(1, 1, 1));
      }
      AddHit(rect, Action::OpenPhoto, static_cast<int>(i), photo.info.id);
    }
    const int rows = (static_cast<int>(photos.size()) + columns - 1) / columns;
    y += static_cast<float>(rows) * (tileSize + gap);
    return y - start;
  }

  float DrawClipboard(float left, float right, float y) {
    const float start = y;
    Button(left, y, L"Send PC clipboard to phone", Action::SendPcClipboard, true, kGlyphSend);
    y += 52.0f;
    y += Toggle(left, right, y, L"Sync clipboard automatically",
                L"Text you copy on this PC is sent to the phone right away.",
                state.clipboardSync, Action::ToggleClipboardSync);
    y += 16.0f;
    SectionLabel(L"COPIED ON YOUR PHONE", left, right, y);
    y += 28.0f;
    if (clips.empty()) {
      EmptyMessage(L"Nothing here yet",
                   L"Copy text on the phone and open FeatherCast Phone, or use the "
                   L"“Send clipboard to PC” tile or share menu.",
                   left, right, y);
      return y - start + 100.0f;
    }
    for (size_t i = 0; i < clips.size(); ++i) {
      y += DrawClipRow(left, right, y, clips[i], static_cast<int>(i));
    }
    return y - start;
  }

  float DrawDevices(float left, float right, float y) {
    const float start = y;
    SectionLabel(L"PAIRED PHONES", left, right, y);
    y += 28.0f;
    for (const auto& device : state.devices) {
      const D2D1_RECT_F row{left, y, right, y + 64.0f};
      Card(row);
      Text(kGlyphPhone, {left + 12, y, left + 44, y + 64}, iconLargeFormat.Get(), Accent());
      Text(Widen(device.name), {left + 56, y + 12, right - 140, y + 34}, headingFormat.Get(),
           TextPrimary());
      const bool online = connected && ConnectedId() == device.id;
      Text(online ? L"Connected now" : L"Paired " + FormatTime(device.pairedAt),
           {left + 56, y + 34, right - 140, y + 54}, smallFormat.Get(),
           online ? ToD2D(theme.success) : TextMuted());
      const float w = MeasureWidth(L"Forget", bodyFormat.Get()) + 32.0f;
      Button(right - 14 - w, y + 14, L"Forget", Action::ForgetDevice, false, nullptr,
             device.id, true);
      y += 74.0f;
    }
    Button(left, y + 4, L"Pair new phone", Action::PairNew, false, kGlyphDevices);
    y += 60.0f;
    SectionLabel(L"SETTINGS", left, right, y);
    y += 26.0f;
    y += Toggle(left, right, y, L"Sync clipboard automatically",
                L"Send text copied on this PC to the phone.", state.clipboardSync,
                Action::ToggleClipboardSync);
    y += Toggle(left, right, y, L"Show phone notifications on this PC",
                L"Pop up a Windows alert when a new phone notification arrives.",
                state.notificationToasts, Action::ToggleToasts);
    y += Toggle(left, right, y, L"Warn when the phone battery is low",
                L"Pop up a Windows alert when the battery drops to 20% while not charging.",
                state.lowBatteryAlert, Action::ToggleLowBattery);
    y += Toggle(left, right, y, L"Phone connection",
                L"Turn off to stop listening for your phone on the network.", state.enabled,
                Action::TurnOff);
    y += 12.0f;
    Text(L"Phone and PC talk directly over your local network. Data is encrypted with a key "
         L"created when you paired, and nothing is sent to the internet.",
         {left, y, right, y + 44.0f}, bodyWrapFormat.Get(), TextMuted());
    y += 52.0f;
    return y - start;
  }

  std::string connectedId;
  const std::string& ConnectedId() const { return connectedId; }

  void DrawDashboard(float width, float height) {
    DrawSidebar(height);
    const float left = kSidebarWidth + kPad;
    const float right = width - kPad;
    const float top = kHeaderHeight;
    viewportHeight = height - top;
    float& offset = scroll[tab];
    auto [visual, inserted] = visualScroll.try_emplace(tab);
    if (inserted) {
      visual->second.Configure(0.30, 1.0);
      visual->second.Snap(offset);
    }
    target->PushAxisAlignedClip({kSidebarWidth, top, width, height},
                                D2D1_ANTIALIAS_MODE_ALIASED);
    const bool fading = contentOpacity.Value() < 0.999;
    if (fading) {
      target->PushLayer(D2D1::LayerParameters(
          D2D1::RectF(kSidebarWidth, top, width, height), nullptr,
          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
          static_cast<float>(contentOpacity.Value())), nullptr);
    }
    const float y = top + 22.0f - static_cast<float>(visual->second.Value()) +
                    static_cast<float>(contentOffset.Value());
    const size_t hitsBefore = hits.size();
    float used = 0.0f;
    switch (tab) {
      case Tab::Overview: used = DrawOverview(left, right, y); break;
      case Tab::Notifications: used = DrawNotifications(left, right, y); break;
      case Tab::Photos: used = DrawPhotos(left, right, y); break;
      case Tab::Clipboard: used = DrawClipboard(left, right, y); break;
      case Tab::Messages: used = DrawMessages(left, right, y); break;
      case Tab::Devices: used = DrawDevices(left, right, y); break;
    }
    if (fading) target->PopLayer();
    target->PopAxisAlignedClip();
    // Content scrolled under the header must not stay clickable.
    for (size_t i = hitsBefore; i < hits.size(); ++i) {
      if (hits[i].rect.bottom <= top) hits[i].action = Action::None;
      hits[i].rect.top = std::max(hits[i].rect.top, top);
    }
    contentHeight = used + 44.0f;
    const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
    if (offset > maxScroll) offset = maxScroll;
    if (visual->second.Target() > maxScroll) {
      visual->second.Retarget(maxScroll, spatialMotion);
    }
  }

  void DrawToast(float width, float height) {
    if (toast.empty() || toastOpacity.Value() <= 0.001) return;
    const float w = MeasureWidth(toast, bodyFormat.Get()) + 40.0f;
    const float lift = static_cast<float>(toastOffset.Value());
    const D2D1_RECT_F rect{(width - w) / 2.0f, height - 64.0f + lift,
                           (width + w) / 2.0f, height - 24.0f + lift};
    if (toastOpacity.Value() < 0.999) {
      target->PushLayer(D2D1::LayerParameters(
          rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
          D2D1::IdentityMatrix(), static_cast<float>(toastOpacity.Value())), nullptr);
    }
    Fill(rect, MixColor(Surface(), D2D1::ColorF(1, 1, 1), 0.08f), 20.0f);
    Stroke(rect, ToD2D(theme.border), 20.0f);
    Text(toast, {rect.left + 20.0f, rect.top, rect.right - 12.0f, rect.bottom},
         bodyFormat.Get(), TextPrimary());
    if (toastOpacity.Value() < 0.999) target->PopLayer();
  }

  void Paint() {
    if (!EnsureTarget()) return;
    const D2D1_SIZE_F size = target->GetSize();
    hits.clear();
    target->BeginDraw();
    target->SetTransform(D2D1::IdentityMatrix());
    target->Clear(Background());
    DrawHeader(size.width);
    const bool showPairing = state.enabled && state.running &&
                             (pairing || state.devices.empty());
    if (!state.enabled || !state.running) {
      DrawOff(size.width, size.height);
    } else if (showPairing) {
      EnsurePairingUri();
      DrawPairing(size.width, size.height);
    } else {
      DrawDashboard(size.width, size.height);
    }
    DrawToast(size.width, size.height);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
    RequestFrame();
  }

  void EnsurePairingUri() {
    if (!pairingUri.empty() && NowMs() - pairingCreatedAt < kQrRefreshMs) return;
    pairingUri = callbacks.createPairingUri ? callbacks.createPairingUri() : std::string{};
    pairingCreatedAt = NowMs();
  }

  void Invalidate() {
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
  }

  bool HasActiveMotion() const {
    if (windowOpacity.Active() || windowOffset.Active() || sidebarY.Active() ||
        contentOpacity.Active() || contentOffset.Active() || toastOpacity.Active() ||
        toastOffset.Active()) return true;
    for (const auto& [_, value] : visualScroll) if (value.Active()) return true;
    for (const auto& [_, value] : toggleMotion) if (value.Active()) return true;
    return false;
  }

  void RequestFrame() {
    if (!hwnd || !IsWindowVisible(hwnd) || !HasActiveMotion()) return;
    SetTimer(hwnd, kFrameTimer, 16, nullptr);
  }

  void ApplyWindowPresentation() {
    if (!hwnd) return;
    const BYTE alpha = static_cast<BYTE>(std::lround(
        std::clamp(windowOpacity.Value(), 0.0, 1.0) * 255.0));
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    const int y = naturalWindowY + static_cast<int>(std::lround(windowOffset.Value()));
    if (bounds.top != y) {
      movingForAnimation = true;
      SetWindowPos(hwnd, nullptr, bounds.left, y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      movingForAnimation = false;
    }
  }

  void Frame() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = lastFrame.time_since_epoch().count() == 0
                               ? 1.0 / 60.0
                               : std::chrono::duration<double>(now - lastFrame).count();
    lastFrame = now;
    windowOpacity.Update(elapsed);
    windowOffset.Update(elapsed);
    sidebarY.Update(elapsed);
    contentOpacity.Update(elapsed);
    contentOffset.Update(elapsed);
    toastOpacity.Update(elapsed);
    toastOffset.Update(elapsed);
    for (auto& [_, value] : visualScroll) value.Update(elapsed);
    for (auto& [_, value] : toggleMotion) value.Update(elapsed);
    ApplyWindowPresentation();
    if (!toast.empty() && NowMs() >= toastUntil && !toastOpacity.Active() &&
        toastOpacity.Value() <= 0.001) toast.clear();
    if (closing && !windowOpacity.Active() && !windowOffset.Active()) {
      KillTimer(hwnd, kFrameTimer);
      DestroyWindow(hwnd);
      return;
    }
    Invalidate();
    if (!HasActiveMotion()) {
      KillTimer(hwnd, kFrameTimer);
      lastFrame = {};
    }
  }

  void BeginClose() {
    if (!hwnd || closing) return;
    if (!IsWindowVisible(hwnd) || (!fadeMotion && !spatialMotion)) {
      DestroyWindow(hwnd);
      return;
    }
    closing = true;
    windowOpacity.Retarget(0.0, 0.16, fadeMotion, motion::Easing::InCubic);
    windowOffset.Retarget(10.0, spatialMotion);
    ApplyWindowPresentation();
    RequestFrame();
  }

  void ShowAnimated() {
    if (!hwnd) return;
    const bool wasVisible = IsWindowVisible(hwnd) != FALSE;
    if (!wasVisible) {
      RECT bounds{};
      GetWindowRect(hwnd, &bounds);
      naturalWindowY = bounds.top;
      windowOpacity.Snap(fadeMotion ? 0.0 : 1.0);
      windowOffset.Snap(spatialMotion ? 12.0 : 0.0);
      ApplyWindowPresentation();
    }
    closing = false;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
    windowOpacity.Retarget(1.0, 0.23, fadeMotion);
    windowOffset.Retarget(0.0, spatialMotion);
    ApplyWindowPresentation();
    RequestFrame();
    Invalidate();
  }

  void SetMotionPolicy(bool fade, bool spatial, bool controls) {
    fadeMotion = fade;
    spatialMotion = spatial;
    controlMotion = controls;
    if (!fade) {
      windowOpacity.Snap(closing ? 0.0 : 1.0);
      contentOpacity.Snap(1.0);
      toastOpacity.Snap(toast.empty() ? 0.0 : 1.0);
    }
    if (!spatial) {
      windowOffset.Snap(0.0);
      sidebarY.Snap(kHeaderHeight + 16.0f + static_cast<int>(tab) * 46.0f);
      contentOffset.Snap(0.0);
      toastOffset.Snap(0.0);
      for (auto& [page, value] : visualScroll) value.Snap(scroll[page]);
    }
    if (!controls) {
      for (auto& [action, value] : toggleMotion) {
        if (action == Action::ToggleClipboardSync) value.Snap(state.clipboardSync ? 1.0 : 0.0);
        if (action == Action::ToggleToasts) value.Snap(state.notificationToasts ? 1.0 : 0.0);
        if (action == Action::ToggleLowBattery) value.Snap(state.lowBatteryAlert ? 1.0 : 0.0);
      }
    }
    ApplyWindowPresentation();
    if (closing && !fade && !spatial && hwnd) {
      DestroyWindow(hwnd);
    } else {
      if (HasActiveMotion()) RequestFrame();
      Invalidate();
    }
  }

  void ShowToast(std::wstring text) {
    const bool replacing = !toast.empty();
    toast = std::move(text);
    toastUntil = NowMs() + 2200;
    if (!replacing) toastOpacity.Snap(fadeMotion ? 0.0 : 1.0);
    toastOpacity.Retarget(1.0, 0.17, fadeMotion);
    if (!replacing) toastOffset.Snap(spatialMotion ? 8.0 : 0.0);
    toastOffset.Retarget(0.0, 0.20, spatialMotion);
    RequestFrame();
    Invalidate();
  }

  // ------------------------------------------------------------- actions

  void AnimateContentChange() {
    if (!contentOpacity.Active()) contentOpacity.Snap(fadeMotion ? 0.45 : 1.0);
    contentOpacity.Retarget(1.0, 0.20, fadeMotion);
    if (!contentOffset.Active()) contentOffset.Snap(spatialMotion ? 14.0 : 0.0);
    contentOffset.Retarget(0.0, 0.22, spatialMotion);
    RequestFrame();
  }

  void Activate(const Hit& hit) {
    switch (hit.action) {
      case Action::None:
        break;
      case Action::SelectTab:
        if (tab != static_cast<Tab>(hit.index)) {
          tab = static_cast<Tab>(hit.index);
          sidebarY.Retarget(kHeaderHeight + 16.0f + hit.index * 46.0f,
                            spatialMotion);
          AnimateContentChange();
        }
        if (tab == Tab::Photos && photos.empty() && connected && !photosRequested &&
            callbacks.requestPhotos) {
          photosRequested = true;
          callbacks.requestPhotos();
        }
        if (tab == Tab::Messages && connected && !smsRequested && callbacks.requestSmsThreads) {
          smsRequested = true;
          callbacks.requestSmsThreads();
        }
        break;
      case Action::TurnOn:
        if (callbacks.setEnabled) callbacks.setEnabled(true);
        break;
      case Action::TurnOff:
        if (callbacks.setEnabled) callbacks.setEnabled(!state.enabled);
        break;
      case Action::PairNew:
        pairing = true;
        pairingUri.clear();
        AnimateContentChange();
        break;
      case Action::CancelPairing:
        pairing = false;
        AnimateContentChange();
        break;
      case Action::CopyNotification:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < notifications.size()) {
          const auto& info = notifications[static_cast<size_t>(hit.index)].info;
          std::string text = info.title;
          if (!info.text.empty()) text += (text.empty() ? "" : "\n") + info.text;
          if (callbacks.copyToPc) callbacks.copyToPc(text);
          ShowToast(L"Copied to the PC clipboard");
        }
        break;
      case Action::DismissNotification:
        if (callbacks.dismissNotification) callbacks.dismissNotification(hit.id);
        RemoveNotification(hit.id);
        break;
      case Action::OpenPhoto:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < photos.size()) {
          auto& photo = photos[static_cast<size_t>(hit.index)];
          if (!connected) {
            ShowToast(L"Connect your phone to download photos");
          } else if (!photo.downloading && callbacks.requestPhoto) {
            photo.downloading = true;
            callbacks.requestPhoto(photo.info.id);
          }
        }
        break;
      case Action::RefreshPhotos:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (callbacks.requestPhotos) {
          photosRequested = true;
          callbacks.requestPhotos();
          ShowToast(L"Loading photos from your phone…");
        }
        break;
      case Action::CopyClip:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < clips.size()) {
          if (callbacks.copyToPc) callbacks.copyToPc(clips[static_cast<size_t>(hit.index)].text);
          ShowToast(L"Copied to the PC clipboard");
        }
        break;
      case Action::SendPcClipboard:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (callbacks.sendPcClipboard) {
          callbacks.sendPcClipboard();
          ShowToast(L"Sent to your phone");
        }
        break;
      case Action::ToggleClipboardSync:
        if (callbacks.setClipboardSync) callbacks.setClipboardSync(!state.clipboardSync);
        break;
      case Action::ToggleToasts:
        if (callbacks.setNotificationToasts) {
          callbacks.setNotificationToasts(!state.notificationToasts);
        }
        break;
      case Action::ForgetDevice:
        if (callbacks.forgetDevice) callbacks.forgetDevice(hit.id);
        if (hit.id == dataDeviceId) ClearPhoneData();
        ShowToast(L"Phone removed");
        break;
      case Action::ToggleLowBattery:
        if (callbacks.setLowBatteryAlert) callbacks.setLowBatteryAlert(!state.lowBatteryAlert);
        break;
      case Action::FindPhone:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (!HasFeature("ring")) {
          ShowToast(L"Turn on \u201cFind my phone\u201d in the phone app");
        } else if (callbacks.findPhone) {
          callbacks.findPhone();
          ringing = !ringing;
        }
        break;
      case Action::SendFiles:
        if (callbacks.pickFilesToSend) callbacks.pickFilesToSend();
        break;
      case Action::Media:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (callbacks.mediaCommand) {
          callbacks.mediaCommand(hit.id);
        }
        break;
      case Action::RefreshSms:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (callbacks.requestSmsThreads) {
          smsRequested = true;
          callbacks.requestSmsThreads();
          ShowToast(L"Loading messages\u2026");
        }
        break;
      case Action::OpenSmsThread:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < smsThreads.size() &&
            callbacks.openSmsThread) {
          auto& thread = smsThreads[static_cast<size_t>(hit.index)];
          thread.unread = false;
          callbacks.openSmsThread(thread);
        }
        break;
    }
    Invalidate();
  }

  void ClearPhoneData() {
    notifications.clear();
    photos.clear();
    clips.clear();
    battery = -1;
    deviceName.clear();
    dataDeviceId.clear();
    photosRequested = false;
    features.clear();
    ringing = false;
    media = {};
    mediaArt.Reset();
    smsThreads.clear();
    smsRequested = false;
  }

  void RemoveNotification(const std::string& key) {
    std::erase_if(notifications, [&](const auto& item) { return item.info.key == key; });
  }

  // --------------------------------------------------------------- window

  D2D1_POINT_2F ToDip(LPARAM lParam) const {
    const float scale = 96.0f / static_cast<float>(WindowDpi(hwnd));
    return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) * scale,
                         static_cast<float>(GET_Y_LPARAM(lParam)) * scale);
  }

  const Hit* HitAt(D2D1_POINT_2F point) const {
    // Later hits are drawn on top (e.g. icon buttons inside rows).
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
      if (it->action != Action::None && Contains(it->rect, point)) return &*it;
    }
    return nullptr;
  }

  LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_MOVE:
        if (!movingForAnimation) {
          RECT bounds{};
          GetWindowRect(hwnd, &bounds);
          naturalWindowY = bounds.top;
          if (windowOffset.Active()) windowOffset.Snap(0.0);
        }
        return 0;
      case WM_ENTERSIZEMOVE: {
        RECT bounds{};
        GetWindowRect(hwnd, &bounds);
        naturalWindowY = bounds.top;
        windowOffset.Snap(0.0);
        return 0;
      }
      case WM_SIZE:
        if (target) {
          target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        Invalidate();
        return 0;
      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        DiscardDeviceResources();
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const float scale = static_cast<float>(WindowDpi(hwnd)) / 96.0f;
        info->ptMinTrackSize.x = static_cast<LONG>(620 * scale);
        info->ptMinTrackSize.y = static_cast<LONG>(520 * scale);
        return 0;
      }
      case WM_MOUSEMOVE: {
        const D2D1_POINT_2F point = ToDip(lParam);
        const Hit* before = HitAt(mouse);
        mouse = point;
        const Hit* after = HitAt(mouse);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        SetCursor(LoadCursorW(nullptr, after ? IDC_HAND : IDC_ARROW));
        if (before != after) Invalidate();
        return 0;
      }
      case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
          SetCursor(LoadCursorW(nullptr, HitAt(mouse) ? IDC_HAND : IDC_ARROW));
          return TRUE;
        }
        break;
      case WM_MOUSELEAVE:
        mouse = {-1.0f, -1.0f};
        Invalidate();
        return 0;
      case WM_LBUTTONUP: {
        const D2D1_POINT_2F point = ToDip(lParam);
        if (const Hit* hit = HitAt(point)) {
          const Hit copy = *hit;
          Activate(copy);
        }
        return 0;
      }
      case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
          const UINT length = DragQueryFileW(drop, i, nullptr, 0);
          std::wstring path(length + 1, L'\0');
          DragQueryFileW(drop, i, path.data(), length + 1);
          path.resize(length);
          if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) continue;
          paths.push_back(std::move(path));
        }
        DragFinish(drop);
        if (paths.empty()) {
          ShowToast(L"Only files can be sent, not folders");
        } else if (callbacks.sendFiles) {
          callbacks.sendFiles(paths);
          ShowToast(paths.size() == 1 ? L"Sending 1 file\u2026"
                                      : L"Sending " + std::to_wstring(paths.size()) + L" files\u2026");
        }
        return 0;
      }
      case WM_MOUSEWHEEL: {
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        float& offset = scroll[tab];
        const float previous = offset;
        const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
        offset = std::clamp(offset - delta * 64.0f, 0.0f, maxScroll);
        auto [it, inserted] = visualScroll.try_emplace(tab);
        if (inserted) {
          it->second.Configure(0.30, 1.0);
          it->second.Snap(previous);
        }
        it->second.Retarget(offset, spatialMotion);
        RequestFrame();
        Invalidate();
        return 0;
      }
      case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
          BeginClose();
          return 0;
        }
        break;
      case WM_TIMER:
        if (wParam == kFrameTimer) {
          Frame();
        } else if (wParam == kTickTimer) {
          const bool pairingVisible = state.enabled && state.running &&
                                      (pairing || state.devices.empty());
          if (pairingVisible && NowMs() - pairingCreatedAt >= kQrRefreshMs) Invalidate();
          if (!toast.empty() && NowMs() >= toastUntil) {
            if (fadeMotion) {
              toastOpacity.Retarget(0.0, 0.16, true, motion::Easing::InCubic);
              toastOffset.Retarget(6.0, 0.16, spatialMotion,
                                   motion::Easing::InCubic);
              RequestFrame();
            } else {
              toast.clear();
              toastOpacity.Snap(0.0);
              Invalidate();
            }
          }
        }
        return 0;
      case WM_CLOSE:
        BeginClose();
        return 0;
      case WM_DESTROY:
        KillTimer(hwnd, kTickTimer);
        KillTimer(hwnd, kFrameTimer);
        closing = false;
        lastFrame = {};
        windowOpacity.Snap(1.0);
        windowOffset.Snap(0.0);
        ReleaseWindowResources();
        hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
      auto* self = static_cast<Impl*>(create->lpCreateParams);
      self->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(msg, wParam, lParam);
  }

  void ApplyChrome() {
    if (!hwnd) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const DWORD corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners,
                          sizeof(corners));
    const D2D1_COLOR_F bg = Surface();
    const COLORREF caption = RGB(static_cast<BYTE>(bg.r * 255), static_cast<BYTE>(bg.g * 255),
                                 static_cast<BYTE>(bg.b * 255));
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
  }

  bool Create(HWND owner) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                                             0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconId),
                                               IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    HMONITOR monitor = MonitorFromWindow(owner ? owner : GetForegroundWindow(),
                                         MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96;
    UINT dpiY = 96;
    using GetDpiForMonitorProc = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
      if (auto proc = reinterpret_cast<GetDpiForMonitorProc>(
              GetProcAddress(shcore, "GetDpiForMonitor"))) {
        proc(monitor, 0, &dpiX, &dpiY);
      }
      FreeLibrary(shcore);
    }
    const float scale = static_cast<float>(dpiX) / 96.0f;
    const RECT work = info.rcWork;
    const int width = std::min(static_cast<int>(1000 * scale),
                               static_cast<int>(work.right - work.left) - 40);
    const int height = std::min(static_cast<int>(760 * scale),
                                static_cast<int>(work.bottom - work.top) - 40);
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + (work.bottom - work.top - height) / 2;
    CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, kWindowClass, L"FeatherCast Phone",
                    WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance,
                    this);
    if (!hwnd) return false;
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    naturalWindowY = bounds.top;
    ApplyWindowPresentation();
    ApplyChrome();
    DragAcceptFiles(hwnd, TRUE);
    SetTimer(hwnd, kTickTimer, 1000, nullptr);
    return true;
  }

  // ---------------------------------------------------------------- model

  void OnEvent(const phone::Event& event) {
    using phone::EventKind;
    switch (event.kind) {
      case EventKind::Connected:
        connected = true;
        connectedId = event.deviceId;
        if (event.deviceId != dataDeviceId) ClearPhoneData();
        dataDeviceId = event.deviceId;
        deviceName = event.deviceName;
        photosRequested = false;
        break;
      case EventKind::Disconnected:
        connected = false;
        connectedId.clear();
        battery = -1;
        ringing = false;
        media = {};
        mediaArt.Reset();
        smsRequested = false;
        for (auto& photo : photos) photo.downloading = false;
        break;
      case EventKind::Paired:
        pairing = false;
        pairingUri.clear();
        tab = Tab::Overview;
        sidebarY.Retarget(kHeaderHeight + 16.0f, spatialMotion);
        AnimateContentChange();
        ShowToast(L"Paired with " + Widen(event.deviceName));
        break;
      case EventKind::Status:
        battery = event.battery;
        charging = event.charging;
        deviceName = event.deviceName;
        features = event.features;
        break;
      case EventKind::RingState:
        ringing = event.ok;
        break;
      case EventKind::MediaState: {
        const bool sameTrack = media.active && event.media.title == media.title &&
                               event.media.artist == media.artist;
        phone::Bytes art = event.media.artJpeg.empty() && sameTrack ? std::move(media.artJpeg)
                                                                    : event.media.artJpeg;
        if (!event.media.artJpeg.empty() || !sameTrack) {
          mediaArt.Reset();
          mediaArtFailed = false;
        }
        media = event.media;
        media.artJpeg = std::move(art);
        break;
      }
      case EventKind::SmsThreads:
        smsThreads = event.smsThreads;
        break;
      case EventKind::SmsReceived:
        if (!event.smsThreads.empty()) {
          const auto& incoming = event.smsThreads.front();
          std::erase_if(smsThreads, [&](const auto& t) { return t.thread == incoming.thread; });
          auto item = incoming;
          item.unread = true;
          smsThreads.insert(smsThreads.begin(), std::move(item));
        }
        break;
      case EventKind::FileDelivered:
        ShowToast(event.ok ? L"Sent " + Widen(event.photo.name) + L" to your phone"
                           : Widen(event.text.empty() ? "The phone could not save the file"
                                                      : event.text));
        break;
      case EventKind::NotificationPosted: {
        RemoveNotification(event.notification.key);
        NotificationItem item;
        item.info = event.notification;
        notifications.push_front(std::move(item));
        std::stable_sort(notifications.begin(), notifications.end(),
                         [](const auto& a, const auto& b) { return a.info.time > b.info.time; });
        while (notifications.size() > kMaxNotifications) notifications.pop_back();
        for (size_t i = kMaxNotificationIcons; i < notifications.size(); ++i) {
          auto& old = notifications[i];
          if (old.info.iconPng.empty() && !old.icon) continue;
          phone::Bytes().swap(old.info.iconPng);
          old.icon.Reset();
        }
        break;
      }
      case EventKind::NotificationRemoved:
        RemoveNotification(event.text);
        break;
      case EventKind::NotificationsReset:
        notifications.clear();
        break;
      case EventKind::Clipboard:
        if (!event.text.empty() && (clips.empty() || clips.front().text != event.text)) {
          clips.push_front(ClipItem{event.text, NowMs()});
          while (clips.size() > kMaxClips) clips.pop_back();
        }
        break;
      case EventKind::PhotoList: {
        if (!hwnd) break;  // Photos are only kept while the window is open.
        std::vector<PhotoItem> next;
        for (const auto& info : event.photos) {
          PhotoItem item;
          item.info = info;
          for (auto& existing : photos) {
            if (existing.info.id == info.id) {
              item.info.thumbJpeg = existing.info.thumbJpeg;
              item.thumb = existing.thumb;
            }
          }
          next.push_back(std::move(item));
        }
        photos = std::move(next);
        break;
      }
      case EventKind::PhotoThumb:
        if (!hwnd) break;
        for (auto& photo : photos) {
          if (photo.info.id == event.photo.id) {
            photo.info.thumbJpeg = event.photo.thumbJpeg;
            photo.thumb.Reset();
            photo.thumbFailed = false;
          }
        }
        break;
      case EventKind::PhotoSaved: {
        // Only open photos this window asked for; the launcher opens its own.
        bool requested = false;
        for (auto& photo : photos) {
          if (photo.info.id == event.photo.id) {
            requested = requested || photo.downloading;
            photo.downloading = false;
          }
        }
        if (requested) {
          ShellExecuteW(hwnd, L"open", event.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
      }
      case EventKind::FileSaved:
        ShowToast(L"Saved to Downloads\\FeatherCast");
        break;
      case EventKind::Error:
        for (auto& photo : photos) photo.downloading = false;
        if (!event.text.empty()) ShowToast(Widen(event.text));
        break;
      case EventKind::ClipboardHistoryRequested:
        break;
    }
    Invalidate();
  }
};

PhoneWindow::PhoneWindow() : impl_(std::make_unique<Impl>()) {}

PhoneWindow::~PhoneWindow() {
  if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

void PhoneWindow::SetCallbacks(Callbacks callbacks) { impl_->callbacks = std::move(callbacks); }

void PhoneWindow::SetTheme(const theme::Theme& theme, theme::Color accent) {
  impl_->theme = theme;
  impl_->accent = accent;
  impl_->ApplyChrome();
  impl_->Invalidate();
}

void PhoneWindow::SetMotionPolicy(bool fade, bool spatial, bool controls) {
  impl_->SetMotionPolicy(fade, spatial, controls);
}

void PhoneWindow::SetState(State state) {
  const bool wasEnabled = impl_->state.enabled && impl_->state.running;
  impl_->state = std::move(state);
  if (!impl_->state.enabled || !impl_->state.running) {
    impl_->connected = false;
    impl_->connectedId.clear();
    impl_->ClearPhoneData();
  }
  if (!wasEnabled) impl_->pairingUri.clear();
  impl_->Invalidate();
}

void PhoneWindow::OnEvent(const phone::Event& event) { impl_->OnEvent(event); }

void PhoneWindow::Show(HWND owner, bool pairing) {
  if (!impl_->hwnd && !impl_->Create(owner)) return;
  if (pairing) {
    impl_->pairing = true;
    impl_->pairingUri.clear();
  }
  if (impl_->connected && impl_->photos.empty() && !impl_->photosRequested &&
      impl_->callbacks.requestPhotos) {
    impl_->photosRequested = true;
    impl_->callbacks.requestPhotos();
  }
  impl_->ShowAnimated();
}

void PhoneWindow::Close() {
  impl_->BeginClose();
}

bool PhoneWindow::Visible() const {
  return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

HWND PhoneWindow::Hwnd() const { return impl_->hwnd; }

}  // namespace feathercast::phone_ui
