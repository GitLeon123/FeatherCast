#include "phone_ui.hpp"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>

#include "accessibility.hpp"
#include "background_services.hpp"
#include "motion.hpp"
#include "phone_presentation.hpp"
#include "text_layout_cache.hpp"
#include "qrcodegen.hpp"
#include "ui_renderer.hpp"

namespace feathercast::phone_ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"FeatherCastPhoneWindow";
constexpr int kAppIconId = 101;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
constexpr float kHeaderHeight = 84.0f;
// Longest decoded edge, in pixels, for images from the phone: enough for the
// drawn size at high DPI scales without keeping full-resolution bitmaps.
constexpr std::uint32_t kIconDecodeEdge = 128;
constexpr std::uint32_t kArtDecodeEdge = 256;
constexpr std::uint32_t kPhotoDecodeEdge = 512;
constexpr float kSidebarWidth = 212.0f;
constexpr float kPad = 24.0f;
constexpr size_t kMaxNotifications = 100;
// Only the newest notifications keep their app icon bytes; older rows show a letter badge.
constexpr size_t kMaxNotificationIcons = 30;
constexpr size_t kMaxClips = 50;
constexpr long long kQrRefreshMs = 4 * 60 * 1000 + 30 * 1000;
// How often the pairing screen checks that the PC's addresses are unchanged, and
// how soon it looks again after finding no network.
constexpr long long kNetworkRecheckMs = 3 * 1000;
// The service reports an unanswered photo after two minutes; this only covers a
// request that never reached it.
constexpr long long kPhotoTimeoutMs = 150 * 1000;
// A photo list request that is still unanswered after this long gets a hint.
constexpr long long kPhotoListSlowMs = 15 * 1000;
constexpr long long kToastMs = 2200;
constexpr long long kToastActionMs = 8000;
// The pairing screen scrolls independently of the dashboard tabs.
constexpr int kPairingScrollKey = 100;
// Accessible children before the actionable controls: header status, toast.
constexpr int kFixedAccessibleChildren = 2;
constexpr float kTransferRowHeight = 36.0f;
constexpr size_t kMaxTransferRows = 3;

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
  OpenScreen,
  RetryService,
  CancelTransfers,
  ToastAction,
};

// Where a control lives; focus keys include it because the sidebar tabs and the
// overview cards trigger the same action.
enum class Zone { Header, Sidebar, Content, Overlay };

enum class Role { Button, Tab, Row, Toggle };

struct Hit {
  D2D1_RECT_F rect{};  // clickable area, clipped to the visible content
  D2D1_RECT_F full{};  // where the control is drawn, even when scrolled out of view
  Action action = Action::None;
  int index = 0;
  std::string id;
  Zone zone = Zone::Content;
  Role role = Role::Button;
  std::wstring label;        // accessible name
  std::wstring description;  // accessible description
  std::wstring tip;          // hover/focus label for icon-only buttons
  bool focusable = true;     // false: mouse-only duplicate of a nearby control
  bool clickable = true;     // false while scrolled out of view
  bool checked = false;
  bool selected = false;
};

struct FocusKey {
  Action action = Action::None;
  int index = 0;
  std::string id;
  Zone zone = Zone::Content;
  bool operator==(const FocusKey&) const = default;
};

struct Transfer {
  std::string id;
  std::wstring name;
  long long done = 0;
  long long total = 0;
  long long startedAt = 0;
  bool showPercent = false;  // sticky once the transfer proved long
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
  long long downloadingSince = 0;
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

// Opens Explorer with the file selected (never runs it); falls back to its folder.
bool RevealInFolder(HWND owner, const std::wstring& path) {
  if (path.empty()) return false;
  if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str())) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT shown = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    if (SUCCEEDED(initialized)) CoUninitialize();
    ILFree(item);
    if (SUCCEEDED(shown)) return true;
  }
  const std::wstring folder = std::filesystem::path(path).parent_path().wstring();
  if (folder.empty()) return false;
  return reinterpret_cast<INT_PTR>(
             ShellExecuteW(owner, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

std::wstring FormatTime(long long ms) {
  if (ms <= 0) return {};
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  const std::time_t now = std::time(nullptr);
  std::tm value{};
  std::tm today{};
  // Phone timestamps are untrusted; out-of-range values get no time label.
  if (localtime_s(&value, &seconds) != 0 || localtime_s(&today, &now) != 0) return {};
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

struct PhoneWindow::Impl : public accessibility::Model {
  HWND hwnd = nullptr;
  Callbacks callbacks;
  theme::Theme baseTheme;
  theme::Theme theme;  // baseTheme, or system colors in high contrast
  theme::Color baseAccent{0.36f, 0.42f, 1.0f, 1.0f};
  theme::Color accent{0.36f, 0.42f, 1.0f, 1.0f};
  bool highContrast = false;
  State state;

  // Model
  bool connected = false;
  std::string deviceName;
  int battery = -1;
  bool charging = false;
  std::deque<NotificationItem> notifications;
  std::vector<Transfer> transfers;  // outgoing and incoming, in start order
  std::vector<PhotoItem> photos;
  std::deque<ClipItem> clips;
  bool photosRequested = false;
  long long photosRequestedAt = 0;
  bool photosLoaded = false;   // the phone answered the last photo list request
  std::string photosAccess;    // "full", "selected" or "denied" from the phone
  std::string photosError;
  std::string dataDeviceId;  // Phone the lists above came from
  std::vector<std::string> features;
  bool ringing = false;
  phone::MediaInfo media;  // artJpeg keeps the current cover
  ComPtr<ID2D1Bitmap> mediaArt;
  bool mediaArtFailed = false;
  std::vector<phone::SmsThread> smsThreads;
  bool smsRequested = false;
  bool smsLoaded = false;  // the phone answered the last conversation list request

  // View state
  Tab tab = Tab::Overview;
  bool pairing = false;
  std::string pairingUri;
  long long pairingCreatedAt = 0;
  bool pairingUnavailable = false;  // the last attempt found no network address
  std::string apkUrl;               // install URL, rebuilt when the addresses change
  std::string networkKey;           // install URL and every local address
  long long networkCheckedAt = 0;
  std::map<int, float> scroll;  // keyed by ScrollKey()
  float contentHeight = 0.0f;
  float viewportHeight = 0.0f;
  float transferStripHeight = 0.0f;
  std::vector<Hit> hits;
  D2D1_POINT_2F mouse{-1.0f, -1.0f};
  std::wstring toast;
  long long toastUntil = 0;
  std::function<void()> toastRun;  // optional action shown inside the toast
  std::wstring toastActionLabel;
  std::string lastError;  // newest link error, shown on the pairing screen
  long long lastErrorAt = 0;
  // When each phone last dropped its connection in this session, for an honest
  // "connection lost" / "last connected" status instead of a generic wait.
  std::map<std::string, long long> lastSeen;
  std::string lastSeenId;  // the phone that disconnected most recently

  // Keyboard focus lives over the hit list; the key survives repaints.
  FocusKey focusKey;
  int focusOrdinal = -1;
  bool focusVisible = false;  // only after keyboard use
  std::vector<size_t> focusOrder;
  Zone currentZone = Zone::Header;
  // Press feedback while the left button is held.
  bool pressing = false;
  D2D1_POINT_2F pressPoint{-1.0f, -1.0f};
  // Hover never beats the keyboard: after a key moves focus, the control under a
  // resting pointer stops looking hovered until the pointer actually moves.
  bool hoverSuppressed = false;
  POINT lastPointer{-1, -1};  // screen position of the last real pointer move

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
  std::map<int, motion::Spring> visualScroll;
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
  ComPtr<IDWriteTextFormat> smallWrapFormat;
  ComPtr<IDWriteTextFormat> smallCenterFormat;
  ComPtr<IDWriteTextFormat> iconFormat;
  ComPtr<IDWriteTextFormat> iconLargeFormat;
  ComPtr<IDWriteTextFormat> stepFormat;
  ComPtr<IDWriteTextFormat> statFormat;
  ComPtr<IDWriteTypography> tabularFigures;  // tnum for counts, times, sizes and percents
  ui::TextLayoutCache textLayouts{128, 16 * 1024};

  Impl() {
    windowOpacity.Snap(1.0);
    // Critically damped: UI springs settle without bounce.
    windowOffset.Configure(0.25, 1.0);
    sidebarY.Configure(0.16, 1.0);
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
      // The theme stores a fallback list; DirectWrite substitutes a default face
      // for an unknown name, so resolve it to one installed family first.
      const std::wstring family =
          ui::ResolveInstalledFontFamily(theme.fontFamily, dwrite.Get());
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
      make(titleFormat, family.c_str(), 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      make(headingFormat, family.c_str(), 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      make(bodyFormat, family.c_str(), 13.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
      make(bodyWrapFormat, family.c_str(), 13.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
      make(smallFormat, family.c_str(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, false);
      make(smallWrapFormat, family.c_str(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, true);
      make(smallCenterFormat, family.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
           true, DWRITE_TEXT_ALIGNMENT_CENTER);
      make(iconFormat, L"Segoe MDL2 Assets", 15.0f, DWRITE_FONT_WEIGHT_NORMAL, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(iconLargeFormat, L"Segoe MDL2 Assets", 22.0f, DWRITE_FONT_WEIGHT_NORMAL, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(stepFormat, family.c_str(), 14.0f, DWRITE_FONT_WEIGHT_BOLD, false,
           DWRITE_TEXT_ALIGNMENT_CENTER);
      make(statFormat, family.c_str(), 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
      if (smallCenterFormat) {
        smallCenterFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      }
    }
    return titleFormat != nullptr;
  }

  // Drops every text format (and the layouts built from them) so the next
  // paint recreates them, e.g. after the theme font changed.
  void ResetFormats() {
    textLayouts.Clear();
    ComPtr<IDWriteTextFormat>* formats[] = {
        &titleFormat, &headingFormat, &bodyFormat, &bodyWrapFormat, &smallFormat,
        &smallWrapFormat, &smallCenterFormat, &iconFormat, &iconLargeFormat,
        &stepFormat, &statFormat};
    for (auto* format : formats) format->Reset();
    tabularFigures.Reset();
  }

  static theme::Color SystemColor(int index) {
    const COLORREF value = GetSysColor(index);
    return {static_cast<float>(GetRValue(value)) / 255.0f,
            static_cast<float>(GetGValue(value)) / 255.0f,
            static_cast<float>(GetBValue(value)) / 255.0f, 1.0f};
  }

  // Rebuilds the effective theme: the app theme normally, system colors when
  // Windows high contrast is on.
  void RefreshContrast() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    theme = baseTheme;
    accent = baseAccent;
    if (!highContrast) return;
    const theme::Color window = SystemColor(COLOR_WINDOW);
    const theme::Color text = SystemColor(COLOR_WINDOWTEXT);
    theme.overlayBackground = window;
    theme.settingsBackground = window;
    theme.surface = window;
    theme.selectedBase = window;
    theme.surfaceHover = SystemColor(COLOR_BTNFACE);
    theme.border = text;
    theme.divider = text;
    theme.textPrimary = text;
    theme.textMuted = text;
    theme.textDim = SystemColor(COLOR_GRAYTEXT);
    theme.sectionText = text;
    theme.danger = text;
    theme.dangerText = text;
    theme.success = SystemColor(COLOR_HOTLIGHT);
    accent = SystemColor(COLOR_HIGHLIGHT);
  }

  // Text and glyphs drawn on an accent-filled control.
  D2D1_COLOR_F OnAccent() const {
    return highContrast ? ToD2D(SystemColor(COLOR_HIGHLIGHTTEXT)) : D2D1::ColorF(1, 1, 1);
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
    textLayouts.Clear();
    ResetFormats();
    wic.Reset();
    dwrite.Reset();
    d2d.Reset();
    photos.clear();
    photos.shrink_to_fit();
    photosRequested = false;
    photosLoaded = false;
    hits.clear();
    hits.shrink_to_fit();
    focusOrder.clear();
    focusKey = {};
    focusOrdinal = -1;
    focusVisible = false;
    pressing = false;
    ResetPairing();
    toast.clear();
    toastRun = nullptr;
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
    // Without a brush the next call would accept the target and draw with null.
    if (!brush) target.Reset();
    return brush != nullptr;
  }

  // Phone images are untrusted: decode them through the size-capped WIC path
  // and scale them down to what the window draws, so a huge or hostile image
  // cannot exhaust memory or exceed the render target's bitmap limit.
  ComPtr<ID2D1Bitmap> DecodeImage(const phone::Bytes& data, std::uint32_t maxEdge) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (data.empty() || !wic || !target) return bitmap;
    const auto decoded = runtime::DecodeImageBytes(wic.Get(), data, {}, maxEdge);
    if (!decoded) return bitmap;
    target->CreateBitmap(
        D2D1::SizeU(decoded->width, decoded->height), decoded->pixels.data(),
        decoded->stride,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 D2D1_ALPHA_MODE_PREMULTIPLIED)),
        bitmap.GetAddressOf());
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

  float DpiScale() const { return static_cast<float>(WindowDpi(hwnd)) / 96.0f; }

  // Strokes the inside of `rect` on whole device pixels, so hairlines stay crisp
  // at 125/150/175 % instead of smearing across two pixel rows.
  void Stroke(const D2D1_RECT_F& rect, D2D1_COLOR_F color, float radius, float width = 1.0f) {
    brush->SetColor(color);
    const SnappedStroke snapped =
        SnapStroke(rect.left, rect.top, rect.right, rect.bottom, width, DpiScale());
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(snapped.left, snapped.top, snapped.right, snapped.bottom),
                          radius, radius),
        brush.Get(), snapped.width);
  }

  // `tabular` turns on tabular figures so digits keep their width while they
  // change (counts, times, sizes, percentages).
  void Text(const std::wstring& text, const D2D1_RECT_F& rect, IDWriteTextFormat* format,
            D2D1_COLOR_F color, bool tabular = false) {
    if (text.empty() || !format || rect.right <= rect.left) return;
    brush->SetColor(color);
    auto layout = textLayouts.Get(dwrite.Get(), text, format,
                                  rect.right - rect.left, rect.bottom - rect.top);
    if (layout && tabular) {
      if (!tabularFigures && SUCCEEDED(dwrite->CreateTypography(tabularFigures.GetAddressOf()))) {
        tabularFigures->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
      }
      // Only digits change shape, so sharing the cached layout is harmless.
      if (tabularFigures) {
        layout->SetTypography(tabularFigures.Get(), {0, static_cast<UINT32>(text.size())});
      }
    }
    if (layout) {
      target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(),
                              brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  float MeasureWidth(const std::wstring& text, IDWriteTextFormat* format) {
    auto layout = textLayouts.Get(dwrite.Get(), text, format, 4000.0f, 100.0f,
                                  {ui::TextLayoutStyle::MeasureSingleLine});
    if (!layout) {
      return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
  }

  bool Hovered(const D2D1_RECT_F& rect) const { return !hoverSuppressed && Contains(rect, mouse); }
  bool Pressed(const D2D1_RECT_F& rect) const {
    return pressing && Contains(rect, pressPoint) && Contains(rect, mouse);
  }

  float MeasureHeight(const std::wstring& text, IDWriteTextFormat* format, float width) {
    auto layout = textLayouts.Get(dwrite.Get(), text, format, std::max(width, 1.0f), 4000.0f);
    if (!layout) return 20.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::ceil(metrics.height);
  }

  // Draws wrapped text and returns the height it needs.
  float WrappedText(const std::wstring& text, float left, float y, float right,
                    IDWriteTextFormat* format, D2D1_COLOR_F color) {
    const float height = MeasureHeight(text, format, right - left);
    Text(text, {left, y, right, y + height}, format, color);
    return height;
  }

  Hit& AddHit(const D2D1_RECT_F& rect, Action action, int index = 0, std::string id = {},
              std::wstring label = {}, Role role = Role::Button) {
    Hit hit;
    hit.rect = rect;
    hit.full = rect;
    hit.action = action;
    hit.index = index;
    hit.id = std::move(id);
    hit.zone = currentZone;
    hit.role = role;
    hit.label = std::move(label);
    hits.push_back(std::move(hit));
    return hits.back();
  }

  static FocusKey KeyOf(const Hit& hit) {
    return FocusKey{hit.action, hit.index, hit.id, hit.zone};
  }

  // Returns the button width so callers can lay out rows.
  float Button(float x, float y, const std::wstring& label, Action action, bool primary,
               const wchar_t* glyph = nullptr, std::string id = {}, bool danger = false,
               std::wstring accessibleName = {}) {
    const float textWidth = MeasureWidth(label, bodyFormat.Get());
    const float width = textWidth + (glyph ? 52.0f : 32.0f);
    const D2D1_RECT_F rect{x, y, x + width, y + 36.0f};
    const bool hover = Hovered(rect);
    D2D1_COLOR_F fill = primary ? Accent() : ToD2D(theme.surfaceHover, hover ? 1.0f : 0.6f);
    if (primary && hover) fill = MixColor(fill, D2D1::ColorF(1, 1, 1), 0.12f);
    if (Pressed(rect)) fill = MixColor(fill, D2D1::ColorF(0, 0, 0), 0.2f);
    Fill(rect, fill, theme.controlRadius);
    if (highContrast) Stroke(rect, ToD2D(theme.border), theme.controlRadius);
    const D2D1_COLOR_F textColor =
        primary ? OnAccent() : (danger ? ToD2D(theme.dangerText) : TextPrimary());
    float textLeft = x + 16.0f;
    if (glyph) {
      Text(glyph, {x + 10.0f, y, x + 34.0f, y + 36.0f}, iconFormat.Get(), textColor);
      textLeft = x + 36.0f;
    }
    Text(label, {textLeft, y, x + width - 12.0f, y + 36.0f}, bodyFormat.Get(), textColor);
    AddHit(rect, action, 0, std::move(id), accessibleName.empty() ? label : accessibleName);
    return width;
  }

  // Icon-only buttons always carry a label: it is the accessible name and the
  // hover/focus tooltip.
  void IconButton(const D2D1_RECT_F& rect, const wchar_t* glyph, Action action,
                  std::string id, int index, const std::wstring& label,
                  std::wstring description = {}) {
    if (Pressed(rect)) {
      Fill(rect, MixColor(ToD2D(theme.surfaceHover), D2D1::ColorF(0, 0, 0), 0.2f),
           theme.controlRadius);
    } else if (Hovered(rect)) {
      Fill(rect, ToD2D(theme.surfaceHover), theme.controlRadius);
    }
    Text(glyph, rect, iconFormat.Get(), Hovered(rect) ? TextPrimary() : TextMuted());
    Hit& hit = AddHit(rect, action, index, std::move(id), label);
    hit.tip = label;
    hit.description = std::move(description);
  }

  float Toggle(float left, float right, float y, const std::wstring& label,
               const std::wstring& description, bool on, Action action) {
    const D2D1_RECT_F row{left, y, right, y + 64.0f};
    if (Pressed(row)) {
      Fill(row, ToD2D(theme.surfaceHover, 0.85f), theme.rowRadius);
    } else if (Hovered(row)) {
      Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
    }
    Text(label, {left + 14.0f, y + 10.0f, right - 80.0f, y + 32.0f}, bodyFormat.Get(),
         TextPrimary());
    Text(description, {left + 14.0f, y + 32.0f, right - 80.0f, y + 54.0f},
         smallFormat.Get(), TextMuted());
    const D2D1_RECT_F track{right - 62.0f, y + 21.0f, right - 18.0f, y + 43.0f};
    auto [it, inserted] = toggleMotion.try_emplace(action);
    if (inserted) {
      it->second.Configure(0.20, 1.0);
      it->second.Snap(on ? 1.0 : 0.0);
    }
    it->second.Retarget(on ? 1.0 : 0.0, controlMotion);
    const float value = static_cast<float>(std::clamp(it->second.Value(), 0.0, 1.0));
    Fill(track, MixColor(ToD2D(theme.surfaceHover), Accent(), value), 11.0f);
    if (value < 0.99f || highContrast) {
      Stroke(track, ToD2D(highContrast ? theme.textPrimary : theme.textDim,
                          highContrast ? 1.0f : 0.6f * (1.0f - value)), 11.0f);
    }
    const float knobX = track.left + 11.0f + value * (track.right - track.left - 22.0f);
    brush->SetColor(MixColor(TextMuted(), OnAccent(), value));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, track.top + 11.0f), 7.0f, 7.0f),
                        brush.Get());
    Hit& hit = AddHit(row, action, 0, {}, label, Role::Toggle);
    hit.checked = on;
    hit.description = description;
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
         OnAccent());
  }

  // What the header says about the link right now; also polled by the tick
  // timer so "connection lost" turns into "last connected …" on its own.
  LinkStatus CurrentLinkStatus() const {
    LinkFacts facts;
    facts.enabled = state.enabled;
    facts.running = state.running;
    facts.pairedCount = state.devices.size();
    facts.connected = connected;
    facts.battery = battery;
    facts.charging = charging;
    facts.nowMs = NowMs();
    const auto seen = lastSeen.find(lastSeenId);
    if (seen != lastSeen.end()) {
      // Only phones that are still paired count; a forgotten one is no longer awaited.
      for (const auto& device : state.devices) {
        if (device.id != lastSeenId) continue;
        facts.lastSeenMs = seen->second;
        facts.lastSeenName = Widen(device.name);
      }
    }
    return DescribeLink(facts);
  }

  void DrawHeader(float width) {
    currentZone = Zone::Header;
    Fill({0, 0, width, kHeaderHeight}, Surface());
    brush->SetColor(ToD2D(theme.divider));
    const float scale = DpiScale();
    const float dividerY = SnapHairlineAbove(kHeaderHeight, 1.0f, scale);
    target->DrawLine({0, dividerY}, {width, dividerY}, brush.Get(),
                     std::max(1.0f, std::round(scale)) / scale);

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
    } else if (state.devices.size() == 1) {
      title = Widen(state.devices.front().name);
    } else if (state.devices.size() > 1) {
      // Several phones are paired and none is connected: do not guess which one.
      title = L"No phone connected";
    }
    const float textLeft = cx + 38.0f;
    Text(title, {textLeft, 16.0f, width - 260.0f, 46.0f}, titleFormat.Get(), TextPrimary());

    const LinkStatus link = CurrentLinkStatus();
    D2D1_COLOR_F dot = ToD2D(theme.textDim);
    switch (link.tone) {
      case LinkTone::Good: dot = ToD2D(theme.success); break;
      case LinkTone::Bad: dot = ToD2D(theme.danger); break;
      case LinkTone::Warning:
        // Amber reads as "degraded"; high contrast has no such color, so use text.
        dot = highContrast ? TextPrimary() : D2D1::ColorF(0.96f, 0.69f, 0.26f);
        break;
      case LinkTone::Neutral: break;
    }
    headerTitle = title;
    headerStatus = link.text;
    brush->SetColor(dot);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(textLeft + 5.0f, 60.0f), 4.5f, 4.5f),
                        brush.Get());
    Text(link.text, {textLeft + 16.0f, 50.0f, width - 260.0f, 70.0f}, smallFormat.Get(),
         TextMuted(), true);

    if (state.enabled && state.running && paired && !pairing) {
      const float w = MeasureWidth(L"Pair new phone", bodyFormat.Get()) + 52.0f;
      Button(width - kPad - w, cy - 18.0f, L"Pair new phone", Action::PairNew, false,
             kGlyphDevices);
    }
  }

  // The service is off (by the user) or could not start (port in use, ...).
  void DrawOff(float width, float height) {
    currentZone = Zone::Content;
    const bool failed = state.enabled && !state.running;
    const float cardWidth = std::min(520.0f, width - 2 * kPad);
    const float left = (width - cardWidth) / 2.0f;
    const float inner = cardWidth - 56.0f;
    const float top = kHeaderHeight + std::max(32.0f, (height - kHeaderHeight - 300.0f) / 2.0f);
    float y = top + 24.0f;
    if (failed) {
      // Measure first so the card fits the error text and hint.
      const std::wstring heading = L"Phone connection could not start";
      const std::wstring error = Widen(state.error.empty()
                                           ? std::string("The service did not start.")
                                           : state.error);
      const std::wstring hint =
          L"Another program may be using FeatherCast's phone port (47800 and the next few), "
          L"or Windows Firewall blocked FeatherCast. In Windows Security > Firewall & network "
          L"protection > Allow an app through firewall, allow FeatherCast on private networks, "
          L"then try again.";
      const float errorHeight = MeasureHeight(error, bodyWrapFormat.Get(), inner);
      const float hintHeight = MeasureHeight(hint, smallWrapFormat.Get(), inner);
      const float cardHeight = 24.0f + 34.0f + errorHeight + 12.0f + hintHeight + 20.0f + 36.0f + 24.0f;
      const D2D1_RECT_F card{left, top, left + cardWidth, top + cardHeight};
      Card(card);
      Text(heading, {left + 28, y, card.right - 28, y + 30}, titleFormat.Get(), TextPrimary());
      y += 34.0f;
      y += WrappedText(error, left + 28, y, card.right - 28, bodyWrapFormat.Get(),
                       ToD2D(theme.dangerText));
      y += 12.0f;
      y += WrappedText(hint, left + 28, y, card.right - 28, smallWrapFormat.Get(), TextMuted());
      y += 20.0f;
      Button(left + 28, y, L"Try again", Action::RetryService, true, kGlyphRefresh);
      return;
    }
    const D2D1_RECT_F card{left, top, left + cardWidth, top + 240.0f};
    Card(card);
    Text(L"Connect your Android phone", {left + 28, y, card.right - 28, y + 30},
         titleFormat.Get(), TextPrimary());
    y += 40.0f;
    Text(L"See your phone's notifications, photos and clipboard on this PC, and send "
         L"your PC clipboard to the phone. Everything stays on your local network and "
         L"is end-to-end encrypted.",
         {left + 28, y, card.right - 28, y + 76}, bodyWrapFormat.Get(), TextMuted());
    Button(left + 28, top + 176, L"Turn on phone connection", Action::TurnOn, true,
           kGlyphPhone);
  }

  // Common scroll state for a scrolling view; returns the animated offset.
  float BeginScrollView(int key) {
    float& offset = scroll[key];
    auto [visual, inserted] = visualScroll.try_emplace(key);
    if (inserted) {
      visual->second.Configure(0.30, 1.0);
      visual->second.Snap(offset);
    }
    return static_cast<float>(visual->second.Value());
  }

  // Keeps hits that scroll under the header or the transfer strip from being
  // clicked, but leaves them focusable so keyboard focus can scroll them back.
  void FinishContentHits(size_t hitsBefore, float top, float bottom) {
    for (size_t i = hitsBefore; i < hits.size(); ++i) {
      Hit& hit = hits[i];
      if (hit.rect.bottom <= top || hit.rect.top >= bottom) hit.clickable = false;
      hit.rect.top = std::max(hit.rect.top, top);
      hit.rect.bottom = std::min(hit.rect.bottom, bottom);
    }
  }

  void FinishScrollView(int key, float used) {
    contentHeight = used;
    const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
    float& offset = scroll[key];
    if (offset > maxScroll) offset = maxScroll;
    auto& visual = visualScroll[key];
    if (visual.Target() > maxScroll) visual.Retarget(maxScroll, spatialMotion);
  }

  // Thin indicator at the right edge when the content overflows its viewport.
  void DrawScrollbar(float width, float top, float viewport, float offsetValue) {
    if (contentHeight <= viewport + 1.0f || viewport < 40.0f) return;
    const float track = viewport - 8.0f;
    const float thumb = std::clamp(track * viewport / contentHeight, 28.0f, track);
    const float maxScroll = contentHeight - viewport;
    const float t = std::clamp(offsetValue / maxScroll, 0.0f, 1.0f);
    const float y = top + 4.0f + t * (track - thumb);
    Fill({width - 8.0f, y, width - 4.0f, y + thumb},
         ToD2D(highContrast ? theme.textPrimary : theme.textDim, 0.7f), 2.0f);
  }

  void DrawPairing(float width, float height) {
    currentZone = Zone::Content;
    transferStripHeight = 0.0f;
    viewportHeight = height - kHeaderHeight;
    const float scrolled = BeginScrollView(kPairingScrollKey);
    target->PushAxisAlignedClip({0, kHeaderHeight, width, height}, D2D1_ANTIALIAS_MODE_ALIASED);
    const size_t hitsBefore = hits.size();
    const float top = kHeaderHeight + 28.0f - scrolled;
    Text(L"Connect your phone in three steps", {kPad, top, width - kPad, top + 30},
         titleFormat.Get(), TextPrimary());
    Text(L"Your phone and this PC need to be on the same Wi-Fi network.",
         {kPad, top + 32, width - kPad, top + 54}, bodyFormat.Get(), TextMuted());

    // The newest link error stays visible here, not only as a toast.
    float errorHeight = 0.0f;
    if (!lastError.empty()) {
      const std::wstring error = L"Last problem: " + Widen(lastError);
      errorHeight = WrappedText(error, kPad, top + 60, width - kPad, bodyWrapFormat.Get(),
                                ToD2D(theme.dangerText)) + 8.0f;
    }

    const bool stacked = width < 760.0f;
    const float gap = 20.0f;
    const float cardTop = top + 72.0f + errorHeight;
    const float available = height - (kHeaderHeight + 28.0f + 72.0f) - 110.0f;
    const float cardWidth = stacked ? width - 2 * kPad : (width - 2 * kPad - gap) / 2.0f;
    const float qrSide = std::clamp(std::min(cardWidth - 80.0f, available - 120.0f), 150.0f, 260.0f);
    const float cardHeight = qrSide + 150.0f;

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

    const std::wstring noNetwork =
        L"No network connection found. Connect this PC to Wi-Fi or a LAN to pair your phone.";
    const bool installReady = state.apkAvailable && !apkUrl.empty();
    const std::wstring installCaption =
        installReady
            ? L"Scan with the phone camera, download and install FeatherCast Phone.\n" +
                  Widen(apkUrl)
        : state.apkAvailable
            ? noNetwork
            : L"The app file (FeatherCast-Phone.apk) was not found next to FeatherCast.exe. "
              L"Copy it to your phone and install it.";
    std::wstring pairCaption;
    if (pairingUri.empty() && pairingUnavailable) {
      pairCaption = noNetwork;
    } else {
      const long long remaining =
          std::max(0LL, kQrRefreshMs - (NowMs() - pairingCreatedAt)) / 1000;
      wchar_t countdown[48]{};
      swprintf_s(countdown, L"%lld:%02lld", remaining / 60, remaining % 60);
      pairCaption = L"Open FeatherCast Phone and tap “Scan QR code”.\n"
                    L"Code refreshes automatically in " + std::wstring(countdown) + L".";
    }
    const float x1 = kPad;
    const float y1 = cardTop;
    const float x2 = stacked ? kPad : kPad + cardWidth + gap;
    const float y2 = stacked ? cardTop + cardHeight + gap : cardTop;
    drawStep(1, x1, y1, L"Install the app", installCaption,
             installReady ? apkUrl : std::string{});
    drawStep(2, x2, y2, L"Scan this pairing code", pairCaption, pairingUri);

    float y = (stacked ? y2 : y1) + cardHeight + 18.0f;
    StepBadge(3, kPad, y);
    Text(L"Done – your phone shows up here automatically. If Windows asks, allow "
         L"FeatherCast on private networks.",
         {kPad + 40.0f, y, width - kPad, y + 28.0f}, bodyFormat.Get(), TextMuted());
    y += 42.0f;
    if (!state.devices.empty()) {
      Button(kPad, y, L"Back to my phone", Action::CancelPairing, false);
      y += 52.0f;
    }

    // Troubleshooting help, always visible so a failed pairing is easy to fix.
    y += 8.0f;
    const float helpInner = width - 2 * kPad - 40.0f;
    std::vector<std::wstring> tips = {
        L"Your phone and this PC must be on the same Wi-Fi network. Guest networks and "
        L"routers with client isolation block the connection.",
        L"Set this PC's network profile to Private: Settings > Network & internet > Wi-Fi > "
        L"your network > Network profile type.",
        L"Allow FeatherCast in Windows Firewall on private networks: Windows Security > "
        L"Firewall & network protection > Allow an app through firewall.",
    };
    if (installReady) {
      tips.push_back(L"Cannot scan the first code? Open " + Widen(apkUrl) +
                     L" in the phone's browser to download the app.");
    }
    tips.push_back(L"Pairing codes work once. If a scan fails, wait for the new code or "
                   L"choose Pair new phone again.");
    float tipsHeight = 0.0f;
    for (const auto& tip : tips) {
      tipsHeight += MeasureHeight(L"•  " + tip, bodyWrapFormat.Get(), helpInner) + 6.0f;
    }
    const D2D1_RECT_F help{kPad, y, width - kPad, y + 48.0f + tipsHeight + 8.0f};
    Card(help);
    Text(L"Having trouble?", {kPad + 20.0f, y + 16.0f, width - kPad - 20.0f, y + 40.0f},
         headingFormat.Get(), TextPrimary());
    float ty = y + 48.0f;
    for (const auto& tip : tips) {
      ty += WrappedText(L"•  " + tip, kPad + 20.0f, ty, width - kPad - 20.0f,
                        bodyWrapFormat.Get(), TextMuted()) + 6.0f;
    }
    y = help.bottom + 24.0f;

    DrawScrollbar(width, kHeaderHeight, viewportHeight, scrolled);
    target->PopAxisAlignedClip();
    FinishContentHits(hitsBefore, kHeaderHeight, height);
    FinishScrollView(kPairingScrollKey, y + scrolled - kHeaderHeight);
  }

  void DrawSidebar(float height) {
    currentZone = Zone::Sidebar;
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
    Fill(pill, ToD2D(accent, highContrast ? 0.5f : 0.18f), theme.rowRadius);
    Fill({pill.left, pill.top + 11.0f, pill.left + 3.0f, pill.bottom - 11.0f},
         Accent(), 1.5f);
    float y = kHeaderHeight + 16.0f;
    int index = 0;
    for (const auto& entry : entries) {
      const D2D1_RECT_F row{12.0f, y, kSidebarWidth - 12.0f, y + 42.0f};
      const bool selected = tab == entry.tab;
      if (Pressed(row)) {
        Fill(row, ToD2D(theme.surfaceHover, 0.9f), theme.rowRadius);
      } else if (!selected && Hovered(row)) {
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
             smallFormat.Get(), TextMuted(), true);
      }
      Hit& hit = AddHit(row, Action::SelectTab, index++, {}, entry.label, Role::Tab);
      hit.selected = selected;
      if (entry.count > 0) hit.description = std::to_wstring(entry.count) + L" items";
      y += 46.0f;
    }
  }

  static std::wstring NotificationSummary(const phone::NotificationInfo& info) {
    std::wstring summary = Widen(info.appName);
    const std::wstring title = SingleLine(Widen(info.title));
    const std::wstring text = SingleLine(Widen(info.text));
    if (!title.empty()) summary += (summary.empty() ? L"" : L": ") + title;
    if (!text.empty()) summary += (summary.empty() ? L"" : L" – ") + text;
    return summary;
  }

  float DrawNotificationRow(const D2D1_RECT_F& row, NotificationItem& item, int index) {
    const bool hover = Hovered(row);
    if (Pressed(row)) {
      Fill(row, ToD2D(theme.surfaceHover, 0.8f), theme.rowRadius);
    } else if (hover) {
      Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
    }
    // The whole row copies on click; the keyboard reaches the same action through the
    // Copy button, so the row itself is not a second tab stop.
    AddHit(row, Action::CopyNotification, index, item.info.key, {}, Role::Row).focusable = false;
    const D2D1_RECT_F iconRect{row.left + 14.0f, row.top + 16.0f, row.left + 50.0f,
                               row.top + 52.0f};
    if (!item.icon && !item.iconFailed && !item.info.iconPng.empty()) {
      item.icon = DecodeImage(item.info.iconPng, kIconDecodeEdge);
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
         TextMuted(), true);
    Text(SingleLine(Widen(item.info.title)), {textLeft, row.top + 26.0f, textRight, row.top + 46.0f},
         headingFormat.Get(), TextPrimary());
    Text(SingleLine(Widen(item.info.text)), {textLeft, row.top + 46.0f, textRight, row.top + 66.0f},
         bodyFormat.Get(), TextMuted());
    const float by = row.top + 20.0f;
    const std::wstring summary = NotificationSummary(item.info);
    IconButton({row.right - 82.0f, by, row.right - 50.0f, by + 32.0f}, kGlyphCopy,
               Action::CopyNotification, item.info.key, index, L"Copy notification text",
               summary);
    IconButton({row.right - 44.0f, by, row.right - 12.0f, by + 32.0f}, kGlyphClose,
               Action::DismissNotification, item.info.key, index, L"Dismiss notification",
               summary);
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
      if (Pressed(card)) {
        Fill(card, ToD2D(theme.surfaceHover, 0.6f), theme.settingsRadius + 2.0f);
      }
      if (Hovered(card)) Stroke(card, ToD2D(accent, 0.7f), theme.settingsRadius + 2.0f);
      Text(std::to_wstring(stats[i].value), {x + 18, y + 12, card.right - 12, y + 50},
           statFormat.Get(), TextPrimary(), true);
      Text(stats[i].label, {x + 18, y + 52, card.right - 12, y + 74}, smallFormat.Get(),
           TextMuted());
      AddHit(card, Action::SelectTab, static_cast<int>(stats[i].tab), {},
             std::wstring(stats[i].label) + L": " + std::to_wstring(stats[i].value));
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

    Button(left, y, L"Phone Screen", Action::OpenScreen, true, kGlyphPhone);
    Text(L"See and control your phone, with device audio",
         {left + 168.0f, y + 6, right, y + 28}, bodyFormat.Get(), TextMuted());
    y += 52.0f;

    SectionLabel(L"QUICK ACTIONS", left, right, y);
    y += 26.0f;
    float x = left;
    // Buttons wrap onto a second row instead of disappearing at narrow widths.
    const auto fits = [&](const std::wstring& label) {
      return x + MeasureWidth(label, bodyFormat.Get()) + 52.0f <= right;
    };
    const auto wrapIfNeeded = [&](const std::wstring& label) {
      if (x > left && !fits(label)) {
        x = left;
        y += 44.0f;
      }
    };
    wrapIfNeeded(L"Send PC clipboard to phone");
    x += Button(x, y, L"Send PC clipboard to phone", Action::SendPcClipboard, true, kGlyphSend) + 10;
    wrapIfNeeded(L"Send files…");
    x += Button(x, y, L"Send files…", Action::SendFiles, false, kGlyphUpload) + 10;
    const std::wstring ringLabel = ringing ? L"Stop ringing" : L"Find my phone";
    wrapIfNeeded(ringLabel);
    x += Button(x, y, ringLabel, Action::FindPhone, false, kGlyphRing) + 10;
    const std::wstring photoLabel = fits(L"Load latest photos") ? L"Load latest photos"
                                                                 : L"Load photos";
    wrapIfNeeded(photoLabel);
    Button(x, y, photoLabel, Action::RefreshPhotos, false, kGlyphRefresh, {}, false,
           L"Load latest photos");
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
      mediaArt = DecodeImage(media.artJpeg, kArtDecodeEdge);
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
    meta += (meta.empty() ? L"" : L"  ·  ") + std::wstring(media.playing ? L"Playing" : L"Paused");
    Text(meta, {textLeft, y + 16.0f, buttons, y + 34.0f}, smallFormat.Get(), TextMuted());
    Text(SingleLine(Widen(media.title)), {textLeft, y + 34.0f, buttons, y + 58.0f},
         headingFormat.Get(), TextPrimary());
    Text(SingleLine(Widen(media.artist)), {textLeft, y + 58.0f, buttons, y + 78.0f},
         bodyFormat.Get(), TextMuted());
    const float by = y + 30.0f;
    const std::wstring track = SingleLine(Widen(media.title));
    IconButton({right - 144.0f, by, right - 104.0f, by + 36.0f}, kGlyphPrevious, Action::Media,
               "prev", 0, L"Previous track", track);
    IconButton({right - 100.0f, by, right - 60.0f, by + 36.0f},
               media.playing ? kGlyphPause : kGlyphPlay, Action::Media, "toggle", 0,
               media.playing ? L"Pause" : L"Play", track);
    IconButton({right - 56.0f, by, right - 16.0f, by + 36.0f}, kGlyphNext, Action::Media,
               "next", 0, L"Next track", track);
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
      const bool loading = connected && allowed && smsRequested && !smsLoaded;
      EmptyMessage(!connected ? L"Phone not connected"
                   : !allowed ? L"Text messages are off"
                   : loading  ? L"Loading messages…"
                   : smsLoaded ? L"No conversations found"
                              : L"No conversations loaded",
                   !connected ? L"Open FeatherCast Phone on your phone to see your text messages."
                   : !allowed ? L"On the phone, open FeatherCast Phone and turn on "
                                L"“Text messages” under Optional. Android asks for "
                                L"SMS and contacts permission."
                   : loading  ? L"Asking your phone for its conversations."
                   : smsLoaded ? L"Your phone returned no conversations. Click Refresh to check "
                                 L"again."
                              : L"Click Refresh to load your text messages.",
                   left, right, y);
      return y - start + 100.0f;
    }
    for (size_t i = 0; i < smsThreads.size(); ++i) {
      if (y - start > 20000.0f) break;
      const auto& thread = smsThreads[i];
      const D2D1_RECT_F row{left, y, right, y + 64.0f};
      if (Pressed(row)) {
        Fill(row, ToD2D(theme.surfaceHover, 0.8f), theme.rowRadius);
      } else if (Hovered(row)) {
        Fill(row, ToD2D(theme.surfaceHover, 0.5f), theme.rowRadius);
      }
      const std::wstring name = Widen(thread.name.empty() ? thread.address : thread.name);
      const D2D1_RECT_F badge{left + 14.0f, y + 14.0f, left + 50.0f, y + 50.0f};
      Fill(badge, ToD2D(accent, thread.unread ? 0.5f : 0.22f), 18.0f);
      Text(name.empty() ? L"?" : name.substr(0, 1), badge, stepFormat.Get(), TextPrimary());
      Text(name, {left + 64.0f, y + 10.0f, right - 120.0f, y + 32.0f}, headingFormat.Get(),
           TextPrimary());
      Text(FormatTime(thread.time), {right - 116.0f, y + 10.0f, right - 12.0f, y + 32.0f},
           smallFormat.Get(), thread.unread ? Accent() : TextMuted(), true);
      Text(SingleLine(Widen(thread.snippet)), {left + 64.0f, y + 32.0f, right - 12.0f, y + 54.0f},
           bodyFormat.Get(), TextMuted());
      Hit& hit = AddHit(row, Action::OpenSmsThread, static_cast<int>(i), thread.thread,
                        (thread.unread ? L"Unread conversation with " : L"Conversation with ") +
                            (name.empty() ? std::wstring(L"unknown") : name),
                        Role::Row);
      hit.description = SingleLine(Widen(thread.snippet));
      y += 68.0f;
    }
    return y - start;
  }

  float DrawClipRow(float left, float right, float y, const ClipItem& clip, int index) {
    const D2D1_RECT_F row{left, y, right, y + 76.0f};
    Card(row);
    if (Pressed(row)) {
      Fill(row, ToD2D(theme.surfaceHover, 0.7f), theme.settingsRadius + 2.0f);
    } else if (Hovered(row)) {
      Fill(row, ToD2D(theme.surfaceHover, 0.4f), theme.settingsRadius + 2.0f);
    }
    Text(FormatTime(clip.time), {left + 16, y + 8, right - 60, y + 26}, smallFormat.Get(),
         TextMuted(), true);
    brush->SetColor(TextPrimary());
    const std::wstring text = Widen(clip.text);
    target->PushAxisAlignedClip({left + 16, y + 26, right - 60, y + 70},
                                D2D1_ANTIALIAS_MODE_ALIASED);
    Text(text.substr(0, 600), {left + 16, y + 28, right - 60, y + 120}, bodyWrapFormat.Get(),
         TextPrimary());
    target->PopAxisAlignedClip();
    AddHit(row, Action::CopyClip, index, {}, {}, Role::Row).focusable = false;
    std::wstring preview = SingleLine(text.substr(0, 120));
    IconButton({right - 48, y + 22, right - 14, y + 54}, kGlyphCopy, Action::CopyClip, {},
               index, L"Copy clipboard item", preview);
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
    Text(L"Click a notification to copy its text. The X button dismisses it on the phone.",
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
    Text(photosAccess == "selected" && !photos.empty()
             ? L"Android shares only the photos you selected. Click a photo to save it and open it."
             : L"Click a photo to save it to Downloads\\FeatherCast and open it.",
         {x, y, right, y + 36.0f}, smallFormat.Get(), TextMuted());
    y += 54.0f;
    if (photos.empty()) {
      const bool slow = NowMs() - photosRequestedAt >= kPhotoListSlowMs;
      std::wstring title;
      std::wstring body;
      if (!connected) {
        title = L"Phone not connected";
        body = L"Open FeatherCast Phone on your phone to see its latest photos.";
      } else if (photosLoaded && photosAccess == "denied") {
        title = L"Photo access is off on your phone";
        body = (photosError.empty() ? std::wstring(L"Allow photo access in FeatherCast on your phone.")
                                    : Widen(photosError)) +
               L" Then click Refresh.";
      } else if (photosLoaded) {
        title = L"No photos found";
        body = photosAccess == "selected"
                   ? L"Android shares only the photos you selected for FeatherCast. Open the "
                     L"app on the phone to choose more, then click Refresh."
                   : L"Your phone returned no pictures. If you expected some, check that "
                     L"“Photos to PC” is on in the phone app, then click Refresh.";
      } else if (photosRequested && !slow) {
        title = L"Loading photos…";
        body = L"Asking your phone for its latest photos.";
      } else if (photosRequested) {
        title = L"Your phone has not answered yet";
        body = L"Open FeatherCast Phone on the phone and allow photo access, then click "
               L"Refresh to try again.";
      } else {
        title = L"No photos loaded";
        body = L"Click Refresh to load the latest photos from your phone.";
      }
      EmptyMessage(title, body, left, right, y);
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
      // Decode lazily as tiles scroll into view instead of the whole grid at once.
      const bool visible = rect.bottom > kHeaderHeight &&
                           rect.top < kHeaderHeight + viewportHeight;
      if (visible && !photo.thumb && !photo.thumbFailed && !photo.info.thumbJpeg.empty()) {
        photo.thumb = DecodeImage(photo.info.thumbJpeg, kPhotoDecodeEdge);
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
      if (photo.downloading || Pressed(rect)) {
        Fill(rect, D2D1::ColorF(0, 0, 0, photo.downloading ? 0.55f : 0.25f), 8.0f);
      }
      if (photo.downloading) {
        Text(L"Downloading…", rect, smallCenterFormat.Get(), D2D1::ColorF(1, 1, 1));
      }
      std::wstring name = Widen(photo.info.name);
      Hit& hit = AddHit(rect, Action::OpenPhoto, static_cast<int>(i), photo.info.id,
                        name.empty() ? std::wstring(L"Photo") : L"Photo " + name, Role::Row);
      hit.description = FormatTime(photo.info.time);
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
    y += 24.0f;
    Text(L"Keeps the last " + std::to_wstring(kMaxClips) +
             L" items in memory only. They are gone when FeatherCast closes.",
         {left, y, right, y + 20.0f}, smallFormat.Get(), TextMuted());
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

  const phone::PairedDevice* FindDevice(const std::string& id) const {
    for (const auto& device : state.devices) {
      if (device.id == id) return &device;
    }
    return nullptr;
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
      const auto seen = lastSeen.find(device.id);
      const std::wstring presence =
          online ? std::wstring(L"Connected now")
          : seen != lastSeen.end() ? L"Last connected " + FormatTime(seen->second)
                                   : L"Paired " + FormatTime(device.pairedAt);
      Text(presence, {left + 56, y + 34, right - 140, y + 54}, smallFormat.Get(),
           online ? ToD2D(theme.success) : TextMuted(), true);
      const float w = MeasureWidth(L"Forget", bodyFormat.Get()) + 32.0f;
      Button(right - 14 - w, y + 14, L"Forget", Action::ForgetDevice, false, nullptr,
             device.id, true, L"Forget " + Widen(device.name));
      y += 74.0f;
    }
    Button(left, y + 4, L"Pair new phone", Action::PairNew, false, kGlyphDevices);
    y += 60.0f;
    SectionLabel(L"SETTINGS", left, right, y);
    y += 26.0f;
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
  std::wstring headerTitle;
  std::wstring headerStatus;
  long long paintNow = 0;  // one clock reading per paint keeps the layout consistent

  // Transfers that finish within a second never get a strip: the action's own
  // toast already acknowledged them, and a flash of progress reads as a glitch.
  bool TransfersShown() const {
    return std::any_of(transfers.begin(), transfers.end(), [&](const Transfer& t) {
      return TransferRevealed(t.startedAt, paintNow);
    });
  }

  float TransferStripHeight() const {
    if (!TransfersShown()) return 0.0f;
    const size_t rows = std::min(transfers.size(), kMaxTransferRows);
    return 52.0f + static_cast<float>(rows) * kTransferRowHeight +
           (transfers.size() > rows ? 24.0f : 0.0f) + 8.0f;
  }

  // Persistent strip above the toast area: one row with progress per file and a
  // Cancel button that stops them all. Sizes always show; a percentage joins
  // them only for transfers that take ten seconds or more.
  void DrawTransferStrip(float width, float height) {
    if (!TransfersShown()) return;
    currentZone = Zone::Overlay;
    const float left = kSidebarWidth + kPad;
    const float right = width - kPad;
    const float strip = TransferStripHeight();
    const D2D1_RECT_F card{left, height - 12.0f - strip, right, height - 12.0f};
    Card(card);
    const size_t n = transfers.size();
    Text(n == 1 ? std::wstring(L"Transferring 1 file")
                : L"Transferring " + std::to_wstring(n) + L" files",
         {card.left + 16.0f, card.top + 8.0f, card.right - 120.0f, card.top + 44.0f},
         headingFormat.Get(), TextPrimary(), true);
    const float cw = MeasureWidth(L"Cancel", bodyFormat.Get()) + 32.0f;
    Button(card.right - 12.0f - cw, card.top + 8.0f, L"Cancel", Action::CancelTransfers, false,
           nullptr, {}, false, L"Cancel file transfers");
    float y = card.top + 52.0f;
    const size_t rows = std::min(n, kMaxTransferRows);
    for (size_t i = 0; i < rows; ++i) {
      Transfer& transfer = transfers[i];
      if (!transfer.showPercent) {
        transfer.showPercent =
            TransferWantsPercent(transfer.startedAt, paintNow, transfer.done, transfer.total);
      }
      const int percent = TransferPercent(transfer.done, transfer.total);
      std::wstring amount = TransferAmount(transfer.done, transfer.total);
      if (transfer.showPercent && percent >= 0) {
        amount = std::to_wstring(percent) + L"%  \u00b7  " + amount;
      }
      // Slack for tabular digits, which can be wider than the measured ones.
      const float amountWidth = std::min(MeasureWidth(amount, smallFormat.Get()) + 8.0f,
                                         (card.right - card.left) / 2.0f);
      Text(transfer.name.empty() ? std::wstring(L"File") : transfer.name,
           {card.left + 16.0f, y, card.right - 24.0f - amountWidth, y + 18.0f}, smallFormat.Get(),
           TextPrimary());
      Text(amount, {card.right - 16.0f - amountWidth, y, card.right - 16.0f, y + 18.0f},
           smallFormat.Get(), TextMuted(), true);
      const D2D1_RECT_F bar{card.left + 16.0f, y + 22.0f, card.right - 16.0f, y + 26.0f};
      Fill(bar, ToD2D(theme.surfaceHover, 1.0f), 2.0f);
      // An unknown size has no honest fill; the byte count beside it still moves.
      if (percent > 0) {
        Fill({bar.left, bar.top,
              bar.left + (bar.right - bar.left) * static_cast<float>(percent) / 100.0f,
              bar.bottom},
             Accent(), 2.0f);
      }
      y += kTransferRowHeight;
    }
    if (n > rows) {
      Text(L"+" + std::to_wstring(n - rows) + L" more",
           {card.left + 16.0f, y, card.right - 16.0f, y + 18.0f}, smallFormat.Get(), TextMuted(),
           true);
    }
  }

  void DrawDashboard(float width, float height) {
    DrawSidebar(height);
    currentZone = Zone::Content;
    const float left = kSidebarWidth + kPad;
    const float right = width - kPad - 6.0f;
    const float top = kHeaderHeight;
    const float strip = TransferStripHeight();
    transferStripHeight = strip > 0.0f ? strip + 12.0f + 8.0f : 0.0f;
    const float bottom = height - transferStripHeight;
    viewportHeight = bottom - top;
    const int key = ScrollKey();
    const float scrolled = BeginScrollView(key);
    target->PushAxisAlignedClip({kSidebarWidth, top, width, bottom},
                                D2D1_ANTIALIAS_MODE_ALIASED);
    const bool fading = contentOpacity.Value() < 0.999;
    if (fading) {
      target->PushLayer(D2D1::LayerParameters(
          D2D1::RectF(kSidebarWidth, top, width, bottom), nullptr,
          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
          static_cast<float>(contentOpacity.Value())), nullptr);
    }
    const float y = top + 22.0f - scrolled + static_cast<float>(contentOffset.Value());
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
    FinishScrollView(key, used + 44.0f);
    DrawScrollbar(width, top, viewportHeight, scrolled);
    target->PopAxisAlignedClip();
    FinishContentHits(hitsBefore, top, bottom);
    DrawTransferStrip(width, height);
  }

  void DrawToast(float width, float height) {
    if (toast.empty() || toastOpacity.Value() <= 0.001) return;
    currentZone = Zone::Overlay;
    const float actionWidth =
        toastRun ? MeasureWidth(toastActionLabel, bodyFormat.Get()) + 28.0f : 0.0f;
    const float maxWidth = std::max(160.0f, width - 2 * kPad);
    const float w = std::min(MeasureWidth(toast, bodyFormat.Get()) + 40.0f + actionWidth, maxWidth);
    const float lift = static_cast<float>(toastOffset.Value());
    const float bottomEdge = height - 24.0f - transferStripHeight;
    const D2D1_RECT_F rect{(width - w) / 2.0f, bottomEdge - 40.0f + lift,
                           (width + w) / 2.0f, bottomEdge + lift};
    if (toastOpacity.Value() < 0.999) {
      target->PushLayer(D2D1::LayerParameters(
          rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
          D2D1::IdentityMatrix(), static_cast<float>(toastOpacity.Value())), nullptr);
    }
    Fill(rect, MixColor(Surface(), D2D1::ColorF(1, 1, 1), highContrast ? 0.0f : 0.08f), 20.0f);
    Stroke(rect, ToD2D(theme.border), 20.0f);
    Text(toast, {rect.left + 20.0f, rect.top, rect.right - 12.0f - actionWidth, rect.bottom},
         bodyFormat.Get(), TextPrimary());
    if (toastRun) {
      const D2D1_RECT_F action{rect.right - actionWidth - 8.0f, rect.top + 6.0f, rect.right - 8.0f,
                               rect.bottom - 6.0f};
      Fill(action, ToD2D(accent, Pressed(action) ? 0.55f : (Hovered(action) ? 0.4f : 0.25f)), 14.0f);
      Text(toastActionLabel, action, stepFormatBody(), Accent());
      if (toastOpacity.Value() > 0.5) AddHit(action, Action::ToastAction, 0, {}, toastActionLabel);
    }
    if (toastOpacity.Value() < 0.999) target->PopLayer();
  }

  // Centered body text for small pill buttons.
  IDWriteTextFormat* stepFormatBody() { return smallCenterFormat.Get(); }

  // Rebuilds the keyboard focus order from the hits of the last paint and
  // re-finds the focused control, or its neighbor when it went away.
  void RebuildFocusOrder() {
    focusOrder.clear();
    for (size_t i = 0; i < hits.size(); ++i) {
      if (hits[i].action != Action::None && hits[i].focusable) focusOrder.push_back(i);
    }
    if (focusOrder.empty()) {
      focusKey = {};
      focusOrdinal = -1;
      return;
    }
    if (focusKey.action == Action::None) {
      focusOrdinal = -1;
      return;
    }
    int found = -1;
    for (size_t k = 0; k < focusOrder.size(); ++k) {
      if (KeyOf(hits[focusOrder[k]]) == focusKey) found = static_cast<int>(k);
    }
    if (found < 0) {
      found = std::clamp(focusOrdinal, 0, static_cast<int>(focusOrder.size()) - 1);
      focusKey = KeyOf(hits[focusOrder[static_cast<size_t>(found)]]);
    }
    focusOrdinal = found;
  }

  const Hit* FocusedHit() const {
    if (focusOrdinal < 0 || static_cast<size_t>(focusOrdinal) >= focusOrder.size()) return nullptr;
    const size_t index = focusOrder[static_cast<size_t>(focusOrdinal)];
    return index < hits.size() ? &hits[index] : nullptr;
  }

  void DrawFocusRing(float width, float height) {
    if (!focusVisible) return;
    const Hit* hit = FocusedHit();
    if (!hit) return;
    const bool content = hit->zone == Zone::Content;
    if (content) {
      target->PushAxisAlignedClip({hit->zone == Zone::Content && !ShowingPairing() ? kSidebarWidth : 0.0f,
                                   kHeaderHeight, width, height - transferStripHeight},
                                  D2D1_ANTIALIAS_MODE_ALIASED);
    }
    const D2D1_RECT_F ring{hit->full.left - 2.0f, hit->full.top - 2.0f, hit->full.right + 2.0f,
                           hit->full.bottom + 2.0f};
    const float radius = std::min(10.0f, (ring.bottom - ring.top) / 2.0f);
    const D2D1_COLOR_F color = highContrast ? ToD2D(SystemColor(COLOR_HIGHLIGHT)) : Accent();
    Stroke(ring, color, radius, 2.0f);
    if (content) target->PopAxisAlignedClip();
  }

  // Name of icon-only buttons, shown under the pointer or the keyboard focus.
  void DrawTooltip(float width, float height) {
    const Hit* hit = nullptr;
    if (mouse.x >= 0.0f && !hoverSuppressed) hit = HitAt(mouse);
    if ((!hit || hit->tip.empty()) && focusVisible) hit = FocusedHit();
    if (!hit || hit->tip.empty()) return;
    const float w = MeasureWidth(hit->tip, smallFormat.Get()) + 20.0f;
    float x = (hit->full.left + hit->full.right - w) / 2.0f;
    x = std::clamp(x, 8.0f, std::max(8.0f, width - w - 8.0f));
    float y = hit->full.bottom + 6.0f;
    if (y + 26.0f > height - 4.0f) y = hit->full.top - 32.0f;
    const D2D1_RECT_F rect{x, y, x + w, y + 26.0f};
    Fill(rect, MixColor(Surface(), D2D1::ColorF(1, 1, 1), highContrast ? 0.0f : 0.12f), 6.0f);
    Stroke(rect, ToD2D(theme.border), 6.0f);
    Text(hit->tip, {rect.left + 10.0f, rect.top, rect.right - 6.0f, rect.bottom},
         smallFormat.Get(), TextPrimary());
  }

  bool ShowingPairing() const {
    return state.enabled && state.running && (pairing || state.devices.empty());
  }

  int ScrollKey() const {
    return ShowingPairing() ? kPairingScrollKey : static_cast<int>(tab);
  }

  void Paint() {
    if (!EnsureTarget()) return;
    paintNow = NowMs();
    const D2D1_SIZE_F size = target->GetSize();
    hits.clear();
    target->BeginDraw();
    target->SetTransform(D2D1::IdentityMatrix());
    target->Clear(Background());
    DrawHeader(size.width);
    transferStripHeight = 0.0f;
    if (!state.enabled || !state.running) {
      contentHeight = 0.0f;
      viewportHeight = size.height - kHeaderHeight;
      DrawOff(size.width, size.height);
    } else if (ShowingPairing()) {
      EnsurePairingUri();
      DrawPairing(size.width, size.height);
    } else {
      DrawDashboard(size.width, size.height);
    }
    DrawToast(size.width, size.height);
    RebuildFocusOrder();
    DrawFocusRing(size.width, size.height);
    DrawTooltip(size.width, size.height);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
    RequestFrame();
  }

  void ResetPairing() {
    pairingUri.clear();
    pairingUnavailable = false;
    apkUrl.clear();
    networkKey.clear();
    networkCheckedAt = 0;
  }

  // Reads the PC's addresses and port again. True when they changed, which makes
  // the cached install URL and pairing code point at an address that is gone.
  bool RefreshNetwork() {
    networkCheckedAt = NowMs();
    std::string url = callbacks.apkUrl ? callbacks.apkUrl() : std::string{};
    std::string key = url;
    for (const auto& address : phone::LocalNetworkAddresses()) key += "|" + address;
    if (key == networkKey) return false;
    networkKey = std::move(key);
    apkUrl = std::move(url);
    pairingUri.clear();
    pairingUnavailable = false;
    return true;
  }

  void EnsurePairingUri() {
    if (networkCheckedAt == 0) RefreshNetwork();
    const long long now = NowMs();
    if (!pairingUri.empty() && now - pairingCreatedAt < kQrRefreshMs) return;
    // Without a network address there is no code to make; look again shortly.
    if (pairingUri.empty() && pairingUnavailable && now - pairingCreatedAt < kNetworkRecheckMs) {
      return;
    }
    pairingUri = callbacks.createPairingUri ? callbacks.createPairingUri() : std::string{};
    pairingCreatedAt = now;
    pairingUnavailable = pairingUri.empty();
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
        toastOpacity.Value() <= 0.001) {
      toast.clear();
      toastRun = nullptr;
    }
    if (closing && !windowOpacity.Active() && (fadeMotion || !windowOffset.Active())) {
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
    BOOL cloak = TRUE;
    const bool cloaked = !wasVisible &&
        SUCCEEDED(DwmSetWindowAttribute(hwnd, 13 /* DWMWA_CLOAK */, &cloak, sizeof(cloak)));
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    if (cloaked) {
      // The first frame is drawn while DWM still hides the window.
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
      cloak = FALSE;
      DwmSetWindowAttribute(hwnd, 13 /* DWMWA_CLOAK */, &cloak, sizeof(cloak));
    }
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

  // A toast with an action stays longer so there is time to use it.
  void ShowToast(std::wstring text, std::wstring actionLabel = {},
                 std::function<void()> run = {}) {
    const bool replacing = !toast.empty();
    toast = std::move(text);
    toastRun = std::move(run);
    toastActionLabel = toastRun ? std::move(actionLabel) : std::wstring{};
    toastUntil = NowMs() + (toastRun ? kToastActionMs : kToastMs);
    // Screen readers announce the toast as a polite live region.
    if (hwnd) accessibility::NotifyStatusChanged(hwnd, 2);
    if (!replacing) toastOpacity.Snap(fadeMotion ? 0.0 : 1.0);
    toastOpacity.Retarget(1.0, 0.17, fadeMotion);
    if (!replacing) toastOffset.Snap(spatialMotion ? 8.0 : 0.0);
    toastOffset.Retarget(0.0, 0.20, spatialMotion);
    RequestFrame();
    Invalidate();
  }

  // ------------------------------------------------------------- actions

  void AnimateContentChange() {
    if (!contentOpacity.Active()) contentOpacity.Snap(fadeMotion ? 0.6 : 1.0);
    contentOpacity.Retarget(1.0, 0.12, fadeMotion);
    if (!contentOffset.Active()) contentOffset.Snap(spatialMotion ? 6.0 : 0.0);
    contentOffset.Retarget(0.0, 0.15, spatialMotion);
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
        if (tab == Tab::Photos && photos.empty() && connected && !photosRequested) {
          RequestPhotoList();
        }
        if (tab == Tab::Messages && connected && !smsRequested && callbacks.requestSmsThreads) {
          smsRequested = true;
          smsLoaded = false;
          callbacks.requestSmsThreads();
        }
        break;
      case Action::TurnOn:
        if (callbacks.setEnabled) callbacks.setEnabled(true);
        break;
      case Action::TurnOff: {
        const bool enable = !state.enabled;
        if (callbacks.setEnabled) callbacks.setEnabled(enable);
        if (!enable) ShowToast(L"Phone connection turned off");
        break;
      }
      case Action::RetryService:
        if (callbacks.setEnabled) {
          // There is no restart callback: turning the service off and on again
          // starts it afresh and picks up a changed port or firewall rule.
          callbacks.setEnabled(false);
          callbacks.setEnabled(true);
          ShowToast(state.running ? L"Phone connection started"
                                  : L"The phone connection still could not start");
        }
        break;
      case Action::PairNew:
        pairing = true;
        ResetPairing();
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
          if (!connected) {
            ShowToast(L"Connect your phone to download photos");
          } else {
            RequestPhoto(photos[static_cast<size_t>(hit.index)].info.id);
          }
        }
        break;
      case Action::RefreshPhotos:
        if (!connected) {
          ShowToast(L"Your phone is not connected");
        } else if (callbacks.requestPhotos) {
          RequestPhotoList();
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
      case Action::ForgetDevice: {
        const phone::PairedDevice* device = FindDevice(hit.id);
        const std::wstring name = device ? Widen(device->name) : std::wstring(L"this phone");
        const std::wstring question =
            L"Forget " + name + L"?\n\nYou'll need to pair again to reconnect it.";
        // No is the default so a stray Enter cannot unpair the phone.
        if (MessageBoxW(hwnd, question.c_str(), L"Forget phone",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) {
          break;
        }
        if (callbacks.forgetDevice) callbacks.forgetDevice(hit.id);
        if (hit.id == dataDeviceId) ClearPhoneData();
        ShowToast(name + L" removed");
        break;
      }
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
      case Action::CancelTransfers:
        // The service can only cancel every transfer at once.
        if (callbacks.cancelTransfers) callbacks.cancelTransfers();
        transfers.clear();
        ShowToast(L"File transfers cancelled");
        break;
      case Action::ToastAction: {
        const auto run = std::move(toastRun);
        toastRun = nullptr;
        toastUntil = 0;  // the toast fades out now
        if (run) run();
        break;
      }
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
          if (smsThreads.empty()) smsLoaded = false;
          callbacks.requestSmsThreads();
          ShowToast(L"Loading messages\u2026");
        }
        break;
      case Action::OpenScreen:
        if (callbacks.openScreen) callbacks.openScreen();
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
    textLayouts.Clear();
    notifications.clear();
    photos.clear();
    clips.clear();
    battery = -1;
    deviceName.clear();
    dataDeviceId.clear();
    photosRequested = false;
    photosRequestedAt = 0;
    photosLoaded = false;
    photosAccess.clear();
    photosError.clear();
    features.clear();
    ringing = false;
    media = {};
    mediaArt.Reset();
    smsThreads.clear();
    smsRequested = false;
    smsLoaded = false;
    transfers.clear();
  }

  void EndTransfer(const std::string& id) {
    std::erase_if(transfers, [&](const Transfer& t) { return t.id == id; });
  }

  void RequestPhotoList() {
    if (!callbacks.requestPhotos) return;
    photosRequested = true;
    photosRequestedAt = NowMs();
    if (photos.empty()) photosLoaded = false;
    callbacks.requestPhotos();
  }

  // Asks the phone for the full photo; it is opened when it arrives.
  void RequestPhoto(const std::string& id) {
    if (!callbacks.requestPhoto) return;
    for (auto& photo : photos) {
      if (photo.info.id != id || photo.downloading) continue;
      photo.downloading = true;
      photo.downloadingSince = NowMs();
      callbacks.requestPhoto(id);
    }
  }

  void ShowPhotoFailure(const std::wstring& message, const std::string& id) {
    if (id.empty() || !connected) {
      ShowToast(message);
      return;
    }
    ShowToast(message, L"Try again", [this, id] { RequestPhoto(id); });
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
      if (it->action != Action::None && it->clickable && Contains(it->rect, point)) {
        return &*it;
      }
    }
    return nullptr;
  }

  // ------------------------------------------------------------- keyboard

  // Moves the scroll target of the visible view, clamped to its content.
  void ScrollTo(float wanted) {
    const int key = ScrollKey();
    float& offset = scroll[key];
    const float previous = offset;
    const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
    offset = std::clamp(wanted, 0.0f, maxScroll);
    auto [it, inserted] = visualScroll.try_emplace(key);
    if (inserted) {
      it->second.Configure(0.30, 1.0);
      it->second.Snap(previous);
    }
    it->second.Retarget(offset, spatialMotion);
    RequestFrame();
    Invalidate();
  }

  // Scrolls so the focused control is inside the visible content area.
  void EnsureFocusVisible() {
    const Hit* hit = FocusedHit();
    if (!hit || hit->zone != Zone::Content) return;
    const float top = kHeaderHeight + 8.0f;
    const float bottom = kHeaderHeight + viewportHeight - 8.0f;
    const auto it = visualScroll.find(ScrollKey());
    const float current = it != visualScroll.end() ? static_cast<float>(it->second.Value())
                                                   : scroll[ScrollKey()];
    if (hit->full.top < top) {
      ScrollTo(current - (top - hit->full.top));
    } else if (hit->full.bottom > bottom) {
      // Items taller than the view line up at the top instead.
      ScrollTo(current + std::min(hit->full.bottom - bottom, hit->full.top - top));
    }
  }

  void NotifyFocusChanged() {
    if (hwnd) NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, AccessibleFocusedChild(hwnd));
  }

  void SetFocusOrdinal(int ordinal) {
    if (ordinal < 0 || static_cast<size_t>(ordinal) >= focusOrder.size()) return;
    focusOrdinal = ordinal;
    focusKey = KeyOf(hits[focusOrder[static_cast<size_t>(ordinal)]]);
    focusVisible = true;
    EnsureFocusVisible();
    NotifyFocusChanged();
    Invalidate();
  }

  void MoveFocus(int delta) {
    if (focusOrder.empty()) return;
    const int count = static_cast<int>(focusOrder.size());
    const int next = focusOrdinal < 0 ? (delta > 0 ? 0 : count - 1)
                                      : (focusOrdinal + delta + count) % count;
    SetFocusOrdinal(next);
  }

  // Arrow keys jump to the nearest control in that direction; with nothing
  // there, Up and Down scroll the view instead.
  void MoveFocusDirection(WPARAM key) {
    const Hit* current = FocusedHit();
    if (!current) {
      MoveFocus(key == VK_UP || key == VK_LEFT ? -1 : 1);
      return;
    }
    const float cx = (current->full.left + current->full.right) / 2.0f;
    const float cy = (current->full.top + current->full.bottom) / 2.0f;
    int best = -1;
    float bestScore = 0.0f;
    for (size_t k = 0; k < focusOrder.size(); ++k) {
      if (static_cast<int>(k) == focusOrdinal) continue;
      const Hit& other = hits[focusOrder[k]];
      const float dx = (other.full.left + other.full.right) / 2.0f - cx;
      const float dy = (other.full.top + other.full.bottom) / 2.0f - cy;
      float along = 0.0f;
      float across = 0.0f;
      switch (key) {
        case VK_UP: along = -dy; across = std::abs(dx); break;
        case VK_DOWN: along = dy; across = std::abs(dx); break;
        case VK_LEFT: along = -dx; across = std::abs(dy); break;
        default: along = dx; across = std::abs(dy); break;
      }
      if (along < 4.0f) continue;
      const float score = along + 2.0f * across;
      if (best < 0 || score < bestScore) {
        best = static_cast<int>(k);
        bestScore = score;
      }
    }
    if (best >= 0) {
      SetFocusOrdinal(best);
    } else if (key == VK_UP || key == VK_DOWN) {
      const auto it = visualScroll.find(ScrollKey());
      const float now = it != visualScroll.end() ? static_cast<float>(it->second.Value()) : 0.0f;
      ScrollTo(now + (key == VK_UP ? -80.0f : 80.0f));
    }
  }

  void ActivateFocused() {
    const Hit* hit = FocusedHit();
    if (!hit) return;
    const Hit copy = *hit;
    focusVisible = true;
    Activate(copy);
  }

  bool OnKeyDown(WPARAM key) {
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (key) {
      case VK_ESCAPE:
        BeginClose();
        return true;
      case VK_TAB:
        MoveFocus(shift ? -1 : 1);
        return true;
      case VK_LEFT:
      case VK_RIGHT:
      case VK_UP:
      case VK_DOWN:
        MoveFocusDirection(key);
        return true;
      case VK_HOME:
        SetFocusOrdinal(0);
        return true;
      case VK_END:
        SetFocusOrdinal(static_cast<int>(focusOrder.size()) - 1);
        return true;
      case VK_PRIOR:
      case VK_NEXT: {
        const auto it = visualScroll.find(ScrollKey());
        const float now = it != visualScroll.end() ? static_cast<float>(it->second.Value()) : 0.0f;
        const float page = std::max(80.0f, viewportHeight * 0.9f);
        ScrollTo(now + (key == VK_PRIOR ? -page : page));
        return true;
      }
      case VK_RETURN:
      case VK_SPACE:
        ActivateFocused();
        return true;
    }
    return false;
  }

  // -------------------------------------------------------- accessibility

  std::wstring AccessibleWindowName(HWND) const override { return L"FeatherCast Phone"; }

  std::vector<accessibility::Item> AccessibleItems(HWND window) const override {
    using accessibility::Item;
    std::vector<Item> items;
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    RECT client{};
    GetClientRect(window, &client);
    const float scale = static_cast<float>(WindowDpi(window)) / 96.0f;
    const auto screenRect = [&](const D2D1_RECT_F& rect) {
      return RECT{origin.x + static_cast<LONG>(std::lround(rect.left * scale)),
                  origin.y + static_cast<LONG>(std::lround(rect.top * scale)),
                  origin.x + static_cast<LONG>(std::lround(rect.right * scale)),
                  origin.y + static_cast<LONG>(std::lround(rect.bottom * scale))};
    };
    // Child 1: who is connected and how it is going.
    Item status;
    status.name = headerTitle.empty() ? std::wstring(L"FeatherCast Phone") : headerTitle;
    status.value = headerStatus;
    status.role = ROLE_SYSTEM_STATICTEXT;
    status.state = STATE_SYSTEM_READONLY;
    status.screenRect = screenRect({0.0f, 0.0f, static_cast<float>(client.right) / scale,
                                    kHeaderHeight});
    items.push_back(std::move(status));
    // Child 2: the toast, announced politely when it changes.
    Item message;
    message.name = toast;
    message.role = ROLE_SYSTEM_STATICTEXT;
    message.state = STATE_SYSTEM_READONLY | (toast.empty() ? STATE_SYSTEM_INVISIBLE : 0);
    message.liveSetting = LiveSetting::Polite;
    message.screenRect = screenRect({0.0f, static_cast<float>(client.bottom) / scale - 80.0f,
                                     static_cast<float>(client.right) / scale,
                                     static_cast<float>(client.bottom) / scale});
    items.push_back(std::move(message));
    const bool windowFocused = GetFocus() == window;
    for (size_t k = 0; k < focusOrder.size(); ++k) {
      const Hit& hit = hits[focusOrder[k]];
      Item item;
      item.name = hit.label.empty() ? hit.tip : hit.label;
      item.description = hit.description;
      item.screenRect = screenRect(hit.full);
      item.state = STATE_SYSTEM_FOCUSABLE;
      switch (hit.role) {
        case Role::Tab:
          item.role = ROLE_SYSTEM_PAGETAB;
          item.defaultAction = L"Switch";
          if (hit.selected) item.state |= STATE_SYSTEM_SELECTED;
          break;
        case Role::Toggle:
          item.role = ROLE_SYSTEM_CHECKBUTTON;
          item.defaultAction = L"Toggle";
          item.value = hit.checked ? L"On" : L"Off";
          if (hit.checked) item.state |= STATE_SYSTEM_CHECKED;
          break;
        case Role::Row:
          item.role = ROLE_SYSTEM_LISTITEM;
          item.defaultAction = L"Open";
          break;
        default:
          item.role = ROLE_SYSTEM_PUSHBUTTON;
          item.defaultAction = L"Press";
          break;
      }
      if (!hit.clickable) item.state |= STATE_SYSTEM_OFFSCREEN;
      if (windowFocused && static_cast<int>(k) == focusOrdinal) item.state |= STATE_SYSTEM_FOCUSED;
      items.push_back(std::move(item));
    }
    return items;
  }

  int AccessibleFocusedChild(HWND) const override {
    if (focusOrdinal < 0 || static_cast<size_t>(focusOrdinal) >= focusOrder.size()) return 0;
    return kFixedAccessibleChildren + 1 + focusOrdinal;
  }

  void AccessibleFocusChild(HWND, int child) override {
    if (child <= kFixedAccessibleChildren) return;
    SetFocusOrdinal(child - kFixedAccessibleChildren - 1);
  }

  void AccessibleInvokeChild(HWND, int child) override {
    if (child <= kFixedAccessibleChildren) return;
    const size_t ordinal = static_cast<size_t>(child - kFixedAccessibleChildren - 1);
    if (ordinal >= focusOrder.size()) return;
    const Hit copy = hits[focusOrder[ordinal]];
    Activate(copy);
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
        // Windows also sends WM_MOUSEMOVE when content moves under a resting
        // pointer; only a real move brings hover back after keyboard use.
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &screen);
        const bool moved = screen.x != lastPointer.x || screen.y != lastPointer.y;
        lastPointer = screen;
        if (hoverSuppressed && moved) {
          hoverSuppressed = false;
          Invalidate();
        }
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
      case WM_LBUTTONDOWN: {
        const D2D1_POINT_2F point = ToDip(lParam);
        mouse = point;
        hoverSuppressed = false;
        focusVisible = false;  // the ring is for keyboard use only
        if (HitAt(point)) {
          pressing = true;
          pressPoint = point;
          SetCapture(hwnd);
        }
        Invalidate();
        return 0;
      }
      case WM_LBUTTONUP: {
        const D2D1_POINT_2F point = ToDip(lParam);
        const bool wasPressing = pressing;
        const D2D1_POINT_2F start = pressPoint;
        pressing = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        mouse = point;
        // A click counts when the button goes down and up on the same control.
        const Hit* down = wasPressing ? HitAt(start) : nullptr;
        const Hit* up = HitAt(point);
        if (down && up && KeyOf(*down) == KeyOf(*up)) {
          const Hit copy = *up;
          if (copy.focusable) focusKey = KeyOf(copy);
          Activate(copy);
        }
        Invalidate();
        return 0;
      }
      case WM_CAPTURECHANGED:
        if (pressing) {
          pressing = false;
          Invalidate();
        }
        return 0;
      case WM_GETOBJECT:
        if (static_cast<LONG>(lParam) == OBJID_CLIENT) {
          return accessibility::HandleGetObject(this, hwnd, wParam, lParam);
        }
        break;
      case WM_KILLFOCUS:
        if (pressing) {
          pressing = false;
          Invalidate();
        }
        break;
      case WM_SYSCOLORCHANGE:
      case WM_THEMECHANGED:
      case WM_SETTINGCHANGE: {
        // High contrast switches the whole palette, so rebuild it and the chrome.
        RefreshContrast();
        ApplyChrome();
        Invalidate();
        break;
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
          const std::wstring noun =
              paths.size() == 1 ? std::wstring(L"1 file") : std::to_wstring(paths.size()) + L" files";
          // The launcher queues files while the phone is away and sends them on reconnect.
          ShowToast(connected ? L"Sending " + noun + L"\u2026"
                              : L"Phone is offline \u2013 " + noun +
                                    L" will be sent when it connects");
        }
        return 0;
      }
      case WM_MOUSEWHEEL: {
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        ScrollTo(scroll[ScrollKey()] - delta * 64.0f);
        return 0;
      }
      case WM_KEYDOWN:
        if (OnKeyDown(wParam)) {
          if (mouse.x >= 0.0f && !hoverSuppressed) {
            hoverSuppressed = true;
            Invalidate();
          }
          return 0;
        }
        break;
      case WM_TIMER:
        if (wParam == kFrameTimer) {
          Frame();
        } else if (wParam == kTickTimer) {
          if (ShowingPairing() && IsWindowVisible(hwnd)) {
            const long long now = NowMs();
            if (now - networkCheckedAt >= kNetworkRecheckMs) RefreshNetwork();
            // The code's countdown changes every second.
            Invalidate();
          }
          for (auto& photo : photos) {
            if (photo.downloading && NowMs() - photo.downloadingSince >= kPhotoTimeoutMs) {
              photo.downloading = false;
              ShowPhotoFailure(L"Your phone did not send the photo in time.", photo.info.id);
              Invalidate();
            }
          }
          if (IsWindowVisible(hwnd)) {
            // "Connection lost" ages into "last connected …"; a transfer that
            // has run a second gets its strip even before the next progress event.
            std::wstring status = CurrentLinkStatus().text;
            const bool statusChanged = status != headerStatus;
            if (statusChanged) headerStatus = std::move(status);
            const long long now = NowMs();
            const bool stripDue =
                !transfers.empty() && !TransfersShown() &&
                std::any_of(transfers.begin(), transfers.end(),
                            [&](const Transfer& t) { return TransferRevealed(t.startedAt, now); });
            if (statusChanged || stripDue) Invalidate();
            // Child 1 carries the status as its value.
            if (statusChanged) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, 1);
          }
          if (!toast.empty() && NowMs() >= toastUntil) {
            if (fadeMotion) {
              toastOpacity.Retarget(0.0, 0.16, true, motion::Easing::InCubic);
              toastOffset.Retarget(6.0, 0.16, spatialMotion,
                                   motion::Easing::InCubic);
              RequestFrame();
            } else {
              toast.clear();
              toastRun = nullptr;
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
        accessibility::Disconnect(hwnd);
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
    const BOOL dark = highContrast ? FALSE : TRUE;
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
    RefreshContrast();
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
    textLayouts.Clear();
    using phone::EventKind;
    switch (event.kind) {
      case EventKind::Connected:
        connected = true;
        connectedId = event.deviceId;
        if (event.deviceId != dataDeviceId) ClearPhoneData();
        dataDeviceId = event.deviceId;
        deviceName = event.deviceName;
        photosRequested = false;
        photosLoaded = false;
        lastError.clear();
        break;
      case EventKind::Disconnected:
        if (!event.deviceId.empty()) {
          lastSeen[event.deviceId] = NowMs();
          lastSeenId = event.deviceId;
        }
        transfers.clear();
        connected = false;
        connectedId.clear();
        battery = -1;
        ringing = false;
        media = {};
        mediaArt.Reset();
        smsRequested = false;
        smsLoaded = false;
        for (auto& photo : photos) photo.downloading = false;
        break;
      case EventKind::Paired:
        pairing = false;
        lastError.clear();
        ResetPairing();
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
        smsLoaded = true;
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
      case EventKind::TransferProgress: {
        // The persistent transfers strip shows progress; no toast per update.
        auto found = std::find_if(transfers.begin(), transfers.end(),
                                  [&](const Transfer& t) { return t.id == event.id; });
        if (found == transfers.end()) {
          transfers.push_back(Transfer{event.id, Widen(event.photo.name), 0, 0, NowMs()});
          found = std::prev(transfers.end());
        }
        found->done = event.transferredBytes;
        found->total = event.totalBytes;
        break;
      }
      case EventKind::FileDelivered:
        EndTransfer(event.id);
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
        photosLoaded = true;
        photosAccess = event.id;  // "full", "selected" or "denied"
        photosError = event.text;
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
        EndTransfer(event.id);
        // Only open photos this window asked for; the launcher opens its own.
        bool requested = false;
        for (auto& photo : photos) {
          if (photo.info.id == event.photo.id) {
            requested = requested || photo.downloading;
            photo.downloading = false;
          }
        }
        if (requested) OpenReceivedFileSafely(hwnd, event.path);
        break;
      }
      case EventKind::FileSaved:
      case EventKind::RemoteFileSaved:
        EndTransfer(event.id);
        if (event.path.empty()) {
          ShowToast(L"Saved to Downloads\\FeatherCast");
        } else {
          const std::wstring path = event.path;
          const HWND owner = hwnd;
          ShowToast(L"Saved " + std::filesystem::path(path).filename().wstring(),
                    L"Show in folder", [owner, path] { RevealInFolder(owner, path); });
        }
        break;
      case EventKind::Error:
        if (event.id == "*") transfers.clear();
        else EndTransfer(event.id);
        // Only the failed photo stops downloading; other requests are still pending.
        if (!event.photo.id.empty()) {
          for (auto& photo : photos) {
            if (photo.info.id == event.photo.id) photo.downloading = false;
          }
        }
        if (!event.text.empty()) {
          // Without a photo or transfer it is a link problem; the pairing screen shows it too.
          if (event.photo.id.empty() && event.id.empty()) {
            lastError = event.text;
            lastErrorAt = NowMs();
          }
          if (event.photo.id.empty()) ShowToast(Widen(event.text));
          else ShowPhotoFailure(Widen(event.text), event.photo.id);
        }
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
  impl_->baseTheme = theme;
  impl_->baseAccent = accent;
  impl_->RefreshContrast();
  impl_->ResetFormats();  // the theme may name another font
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
  if (!wasEnabled) impl_->ResetPairing();  // the service may listen on another port now
  impl_->Invalidate();
}

void PhoneWindow::OnEvent(const phone::Event& event) { impl_->OnEvent(event); }

void PhoneWindow::Show(HWND owner, bool pairing) {
  if (!impl_->hwnd && !impl_->Create(owner)) return;
  if (pairing) {
    impl_->pairing = true;
    impl_->ResetPairing();
  }
  impl_->networkCheckedAt = 0;  // look at the addresses again on the first paint
  if (impl_->connected && impl_->photos.empty() && !impl_->photosRequested) {
    impl_->RequestPhotoList();
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

void OpenReceivedFileSafely(HWND owner, const std::wstring& path) {
  if (path.empty()) return;
  const std::filesystem::path file(path);
  const std::wstring extension = file.extension().wstring();
  if (!extension.empty() && !AssocIsDangerous(extension.c_str()) &&
      reinterpret_cast<INT_PTR>(
          ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32) {
    return;
  }
  RevealInFolder(owner, path);
}

}  // namespace feathercast::phone_ui
