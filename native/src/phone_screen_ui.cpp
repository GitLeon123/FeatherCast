#include "phone_screen_ui.hpp"
#include "phone_presentation.hpp"
#include "phone_screen_playback.hpp"
#include "ui_renderer.hpp"
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>

namespace feathercast::phone_ui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kUpdated = WM_APP + 177;
constexpr wchar_t kClass[] = L"FeatherCastPhoneScreenWindow";
enum Control { Start = 1, Stop, Back, Home, Apps, Sound, Volume, Fullscreen, Title, Status, Hint, EmptyTitle, EmptyHelp };
std::wstring Widen(std::string_view text) {
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(count), 0);
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
  return out;
}
std::string Utf8(std::wstring_view text) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(count), 0);
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
  return out;
}
D2D1_COLOR_F Color(theme::Color c) { return D2D1::ColorF(c.r, c.g, c.b, 1); }
COLORREF GdiColor(theme::Color c) { return RGB(static_cast<BYTE>(c.r * 255), static_cast<BYTE>(c.g * 255), static_cast<BYTE>(c.b * 255)); }
D2D1_COLOR_F FromGdi(COLORREF c) { return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1); }
// State shared with the playback worker and the service thread. Their
// callbacks hold it by shared_ptr, never the window, so a late frame, status or
// keyframe request after the window is gone only lands here.
struct ScreenMailbox {
  std::mutex mutex;
  HWND notifyWindow = nullptr;
  bool notified = false;
  std::string session;
  std::shared_ptr<const phone::ScreenFrame> frame;
  std::deque<phone::ScreenPacket> statuses;  // in order; a "stopped" never hides behind "streaming"
  std::function<bool(phone::ScreenInput)> input;
  void NotifyLocked() {
    if (!notified && notifyWindow) { notified = true; PostMessageW(notifyWindow, kUpdated, 0, 0); }
  }
};
// Tracks which owner-drawn button the pointer is over so it can draw a hover state.
LRESULT CALLBACK HoverProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* hover = reinterpret_cast<HWND*>(ref);
  switch (msg) {
    case WM_MOUSEMOVE:
      if (*hover != window) {
        *hover = window;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      if (*hover == window) { *hover = nullptr; InvalidateRect(window, nullptr, FALSE); }
      break;
    case WM_NCDESTROY:
      if (*hover == window) *hover = nullptr;
      RemoveWindowSubclass(window, HoverProc, 1);
      break;
  }
  return DefSubclassProc(window, msg, wp, lp);
}
bool Caption(HWND window, const std::wstring& text) {
  const int length = GetWindowTextLengthW(window);
  std::wstring old(static_cast<std::size_t>(length) + 1, 0);
  GetWindowTextW(window, old.data(), length + 1); old.resize(static_cast<std::size_t>(length));
  if (old == text) return false;
  SetWindowTextW(window, text.c_str()); return true;
}
}

struct PhoneScreenWindow::Impl {
  HWND hwnd = nullptr;
  std::array<HWND, 14> controls{};
  HFONT font = nullptr, titleFont = nullptr;
  HBRUSH background = nullptr, canvasBackground = nullptr;
  HWND hoverButton = nullptr;
  bool highContrast = false;
  theme::Theme baseTheme;
  theme::Theme theme;  // baseTheme, or system colors in Windows high contrast
  ScreenCallbacks callbacks;
  bool connected = false, available = false, sound = true, fullscreen = false;
  bool controlReady = false, keyboardReady = false, audioReady = false, mouseHeld = false;
  int generation = 0, lastX = 0, lastY = 0, wheelAccum = 0;
  std::string deviceName, sessionId;
  std::wstring detail, phase = L"Ready to share", audioDetail;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  DWORD savedStyle = WS_OVERLAPPEDWINDOW;
  wchar_t highSurrogate = 0;
  float scale = 1, textScale = 1;
  phone::ScreenRect imageRect;
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IDWriteTextFormat> body, heading;
  ComPtr<ID2D1HwndRenderTarget> target;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1Bitmap> bitmap;
  ComPtr<ID2D1DCRenderTarget> buttonTarget;  // antialiased rounded owner-drawn buttons
  std::shared_ptr<const phone::ScreenFrame> shownFrame;
  // Only the newest decoded frame and every status are marshalled to the UI.
  std::shared_ptr<ScreenMailbox> mailbox = std::make_shared<ScreenMailbox>();
  std::shared_ptr<phone::ScreenPlayback> playback;  // written on the UI thread under mailbox->mutex

  ~Impl() {
    Close();
    std::shared_ptr<phone::ScreenPlayback> ended;
    {
      std::lock_guard lock(mailbox->mutex);
      mailbox->notifyWindow = nullptr; mailbox->input = nullptr; mailbox->session.clear();
      ended = std::move(playback);
    }
    // Joins the decoder worker unless the service thread still holds it for a
    // moment; either way its callbacks only reach the shared mailbox.
    ended.reset();
    if (hwnd) DestroyWindow(hwnd);
    if (font) DeleteObject(font);
    if (titleFont) DeleteObject(titleFont);
    if (background) DeleteObject(background);
    if (canvasBackground) DeleteObject(canvasBackground);
  }
  void EnsurePlayback() {
    if (playback) return;
    phone::ScreenPlaybackCallbacks cb;
    cb.frame = [box = mailbox](std::shared_ptr<const phone::ScreenFrame> frame) {
      std::lock_guard guard(box->mutex);
      if (frame->sessionId == box->session) { box->frame = std::move(frame); box->NotifyLocked(); }
    };
    cb.status = [box = mailbox](phone::ScreenPacket packet) {
      std::lock_guard guard(box->mutex);
      if (packet.sessionId != box->session) return;
      if (box->statuses.size() >= 16) box->statuses.pop_front();
      box->statuses.push_back(std::move(packet));
      box->NotifyLocked();
    };
    cb.input = [box = mailbox](phone::ScreenInput input) {
      std::function<bool(phone::ScreenInput)> send;
      { std::lock_guard guard(box->mutex); if (input.sessionId == box->session) send = box->input; }
      if (send) send(std::move(input));
    };
    auto created = std::make_shared<phone::ScreenPlayback>(std::move(cb));
    std::lock_guard lock(mailbox->mutex);
    playback = std::move(created);
  }
  void Input(std::string action, int value = 0, std::string text = {}) {
    if (!sessionId.empty() && generation > 0 && callbacks.input)
      callbacks.input({sessionId, generation, std::move(action), lastX, lastY, value, std::move(text)});
  }
  void CancelInput() {
    if (mouseHeld) Input("cancel");
    mouseHeld = false; highSurrogate = 0;
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
  }
  void StartSession() {
    if (!connected || !available || !callbacks.start) return;
    EnsurePlayback(); CancelInput();
    const auto id = callbacks.start(true);
    if (id.empty()) { detail = L"Reconnect your phone and try again."; Update(); return; }
    sessionId = id; generation = 0; controlReady = keyboardReady = audioReady = false;
    audioDetail.clear(); shownFrame.reset(); bitmap.Reset();
    { std::lock_guard lock(mailbox->mutex); mailbox->session = id; mailbox->frame.reset(); mailbox->statuses.clear(); }
    playback->Begin(id);
    playback->SetAudio(sound, static_cast<float>(SendMessageW(controls[Volume], TBM_GETPOS, 0, 0)) / 100);
    phase = L"Waiting for your phone";
    detail = L"Open FeatherCast on your phone and tap Share screen. Approve Android's screen sharing prompt within 60 seconds.";
    SetFocus(hwnd); Update();
  }
  void EndSession(bool notify = true) {
    CancelInput();
    if (notify && !sessionId.empty() && callbacks.stop) callbacks.stop();
    sessionId.clear(); generation = 0; controlReady = keyboardReady = audioReady = false;
    { std::lock_guard lock(mailbox->mutex); mailbox->session.clear(); mailbox->frame.reset(); mailbox->statuses.clear(); }
    if (playback) playback->End();
    shownFrame.reset(); bitmap.Reset(); Update();
  }
  void Close() { EndSession(); if (fullscreen) ToggleFullscreen(); if (hwnd) ShowWindow(hwnd, SW_HIDE); }
  void Update() {
    if (!controls[Start]) return;
    EnableWindow(controls[Start], connected && available && sessionId.empty());
    EnableWindow(controls[Stop], !sessionId.empty());
    for (int id : {Back, Home, Apps}) EnableWindow(controls[id], controlReady && !sessionId.empty());
    Caption(controls[Sound], sound ? L"Mute" : L"Unmute");
    Caption(controls[Fullscreen], fullscreen ? L"Exit full screen" : L"Full screen");
    Caption(controls[Title], deviceName.empty() ? L"Phone Screen" : Widen(deviceName));
    Caption(hwnd, L"Phone Screen — " + (deviceName.empty() ? std::wstring(L"FeatherCast") : Widen(deviceName)));
    const auto accessibleText = [&](int id, const std::wstring& text) {
      if (Caption(controls[id], text)) {
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, controls[id], OBJID_CLIENT, CHILDID_SELF);
      }
    };
    std::wstring status = detail.empty() ? L"Click to tap · drag to swipe · scroll to move · type directly in phone apps" : detail;
    if (!audioDetail.empty()) status += L" " + audioDetail;
    else if (audioReady) status += sound ? L" Device audio on; some apps cannot share sound." : L" Device audio muted.";
    accessibleText(Status, status); accessibleText(EmptyTitle, ScreenEmptyTitle(connected, available, phase));
    const std::wstring help = !connected ? L"Connect your Android phone in Phone Connection to share its screen."
        : !available ? L"Enable Screen sharing in the updated FeatherCast app on your phone, then choose Start."
        : sessionId.empty() ? L"Choose Start, then approve screen sharing on your phone. Every session stays on your local network."
        : L"Waiting for your phone to approve the Android screen sharing prompt.";
    accessibleText(EmptyHelp, help);
    for (int id : {EmptyTitle, EmptyHelp}) {
      const bool visible = !shownFrame;
      if ((IsWindowVisible(controls[id]) != FALSE) != visible) ShowWindow(controls[id], visible ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  void Mailbox() {
    std::deque<phone::ScreenPacket> states;
    std::shared_ptr<const phone::ScreenFrame> frame;
    {
      std::lock_guard lock(mailbox->mutex);
      states.swap(mailbox->statuses); frame = std::move(mailbox->frame); mailbox->notified = false;
    }
    for (const auto& state : states) {
      if (state.sessionId != sessionId) continue;  // also after EndSession below
      if (state.state == "audio-error") audioDetail = Widen(state.detail);
      else if (state.state == "streaming") {
        if (state.generation < generation) continue;
        if (state.generation != generation) { CancelInput(); shownFrame.reset(); bitmap.Reset(); imageRect = {}; }
        generation = state.generation; controlReady = state.control; keyboardReady = state.keyboard;
        audioReady = state.audio; phase = L"Screen shared"; detail = Widen(state.detail);
      } else if (state.state == "stopped" || state.state == "error") {
        detail = Widen(state.detail); phase = state.state == "error" ? L"Screen sharing unavailable" : L"Screen sharing stopped";
        EndSession(state.state == "error");
      } else if (state.state == "pending") detail = Widen(state.detail);
    }
    if (frame && frame->sessionId == sessionId && frame->generation >= generation) {
      if (frame->generation != generation) { CancelInput(); imageRect = {}; }
      generation = frame->generation; shownFrame = std::move(frame);
    }
    Update();
  }
  void Layout() {
    if (!controls[Start]) return;
    RECT rc{}; GetClientRect(hwnd, &rc); scale = GetDpiForWindow(hwnd) / 96.0f * textScale;
    const float width = rc.right / scale;
    const auto place = [&](int id, float x, float y, float w, float h) {
      SetWindowPos(controls[id], nullptr, static_cast<int>(x * scale), static_cast<int>(y * scale),
          static_cast<int>(w * scale), static_cast<int>(h * scale), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(Title, 24, 18, std::max(150.0f, width - 270), 28);
    place(Start, width - 232, 16, 96, 32); place(Stop, width - 124, 16, 100, 32);
    float x = 24;
    for (const auto& [id, w] : {std::pair{Back, 64.0f}, {Home, 64.0f}, {Apps, 100.0f}, {Sound, 80.0f}}) { place(id, x, 62, w, 30); x += w + 8; }
    place(Volume, x, 62, std::max(72.0f, width - x - 164), 30); place(Fullscreen, width - 148, 62, 124, 30);
    const float height = rc.bottom / scale;
    place(Status, 24, height - 92, width - 48, 56); place(Hint, 24, height - 32, width - 48, 20);
    const float center = (112 + height - 108) * .5f - 48;
    place(EmptyTitle, 56, center, width - 112, 32); place(EmptyHelp, 56, center + 40, width - 112, 90);
    if (target) { target->SetDpi(96 * scale, 96 * scale); target->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  static theme::Color SystemColor(int index) {
    const COLORREF value = GetSysColor(index);
    return {GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f, 1.0f};
  }
  void Style() {
    if (!hwnd) return;
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    theme = baseTheme;
    if (highContrast) {
      const auto window = SystemColor(COLOR_WINDOW), text = SystemColor(COLOR_WINDOWTEXT);
      theme.surface = theme.overlayBackground = theme.selectedBase = window;
      theme.surfaceHover = SystemColor(COLOR_BTNFACE);
      theme.border = theme.textPrimary = theme.textMuted = text;
      theme.textDim = SystemColor(COLOR_GRAYTEXT);
    }
    if (font) DeleteObject(font);
    if (titleFont) DeleteObject(titleFont);
    // The theme stores a comma-separated fallback list; GDI and DirectWrite
    // both need a single installed family name.
    const std::wstring family = ui::ResolveInstalledFontFamily(theme.fontFamily, dwrite.Get());
    font = CreateFontW(-static_cast<int>(MulDiv(14, GetDpiForWindow(hwnd), 96) * textScale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family.c_str());
    if (background) DeleteObject(background);
    background = CreateSolidBrush(GdiColor(theme.surface));
    titleFont = CreateFontW(-static_cast<int>(MulDiv(20, GetDpiForWindow(hwnd), 96) * textScale), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family.c_str());
    if (canvasBackground) DeleteObject(canvasBackground);
    canvasBackground = CreateSolidBrush(GdiColor(theme::CompositeOver(theme.overlayBackground, theme.surface)));
    for (auto control : controls) if (control) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); SetWindowTheme(control, highContrast ? L"" : L"DarkMode_Explorer", nullptr); }
    SendMessageW(controls[EmptyTitle], WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
    if (dwrite) {
      body.Reset(); heading.Reset();
      dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"en-us", &body);
      dwrite->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"en-us", &heading);
    }
    BOOL dark = !highContrast; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // The title bar continues the window surface, like the Phone window.
    const COLORREF caption = GdiColor(theme.surface); DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    Update();
  }
  bool Resources() {
    if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (!dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) return false;
    if (!body) Style();
    if (!target) {
      RECT rc{}; GetClientRect(hwnd, &rc);
      if (FAILED(d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &target))) return false;
      target->SetDpi(96 * scale, 96 * scale);
      // Without a brush the next call would accept the target and draw with null.
      if (FAILED(target->CreateSolidColorBrush(Color(theme.textPrimary), &brush))) { target.Reset(); return false; }
    }
    return body && heading && brush;
  }
  // Rounded, antialiased button body (and focus ring) like the Phone window's
  // controls. Returns false when Direct2D is unavailable; the caller then
  // draws the plain GDI frame.
  bool DrawButtonFrame(HDC dc, const RECT& rc, COLORREF fill, COLORREF edge, bool focused, COLORREF ring) {
    if (!d2d) return false;
    if (!buttonTarget) {
      const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
      if (FAILED(d2d->CreateDCRenderTarget(&props, &buttonTarget))) return false;
    }
    if (FAILED(buttonTarget->BindDC(dc, &rc))) return false;
    ComPtr<ID2D1SolidColorBrush> paint;
    if (FAILED(buttonTarget->CreateSolidColorBrush(FromGdi(fill), &paint))) return false;
    // Device pixels: the radius follows the DPI, strokes sit on half-pixel centres.
    const float dpiScale = GetDpiForWindow(hwnd) / 96.0f;
    const float width = static_cast<float>(rc.right - rc.left), height = static_cast<float>(rc.bottom - rc.top);
    const float radius = std::min(theme.controlRadius * dpiScale, height / 2);
    const float hairline = std::max(1.0f, std::round(dpiScale));
    buttonTarget->BeginDraw();
    buttonTarget->Clear(Color(theme.surface));  // what sits behind the rounded corners
    const D2D1_RECT_F face{hairline / 2, hairline / 2, width - hairline / 2, height - hairline / 2};
    buttonTarget->FillRoundedRectangle(D2D1::RoundedRect(face, radius, radius), paint.Get());
    paint->SetColor(FromGdi(edge));
    buttonTarget->DrawRoundedRectangle(D2D1::RoundedRect(face, radius, radius), paint.Get(), hairline);
    if (focused) {
      // Concentric with the body: inner radius = outer radius - inset.
      const float ringWidth = std::max(2.0f, std::round(2 * dpiScale)), inset = hairline + ringWidth / 2;
      paint->SetColor(FromGdi(ring));
      buttonTarget->DrawRoundedRectangle(D2D1::RoundedRect({inset, inset, width - inset, height - inset},
          std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)), paint.Get(), ringWidth);
    }
    if (FAILED(buttonTarget->EndDraw())) { buttonTarget.Reset(); return false; }
    return true;
  }
  void Text(const std::wstring& text, D2D1_RECT_F rc, IDWriteTextFormat* format, theme::Color color) {
    brush->SetColor(Color(color)); target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rc, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  void Paint() {
    PAINTSTRUCT ps{}; BeginPaint(hwnd, &ps);
    if (Resources()) {
      const auto size = target->GetSize(); target->BeginDraw(); target->Clear(Color(theme.surface));
      const phone::ScreenRect viewport{24, 112, size.width - 24, size.height - 108};
      const float viewportRadius = theme.settingsRadius + 2.0f;  // the Phone window's card radius
      brush->SetColor(Color(theme::CompositeOver(theme.overlayBackground, theme.surface)));
      target->FillRoundedRectangle(D2D1::RoundedRect({viewport.left, viewport.top, viewport.right, viewport.bottom}, viewportRadius, viewportRadius), brush.Get());
      imageRect = {};
      if (shownFrame) {
        const auto& f = *shownFrame; imageRect = phone::FitScreenRect(viewport, f.width, f.height);
        if (!bitmap || bitmap->GetPixelSize().width != static_cast<UINT32>(f.width) || bitmap->GetPixelSize().height != static_cast<UINT32>(f.height)) {
          bitmap.Reset(); target->CreateBitmap(D2D1::SizeU(f.width, f.height), f.bgra.data(), f.width * 4,
              D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &bitmap);
        } else bitmap->CopyFromMemory(nullptr, f.bgra.data(), f.width * 4);
        if (bitmap) {
          // Painted through a bitmap brush so the screen gets rounded corners that
          // stay inside the rounded viewport instead of poking out of it.
          const D2D1_RECT_F image{imageRect.left, imageRect.top, imageRect.right, imageRect.bottom};
          ComPtr<ID2D1BitmapBrush> imageBrush;
          if (SUCCEEDED(target->CreateBitmapBrush(bitmap.Get(), &imageBrush))) {
            imageBrush->SetTransform(D2D1::Matrix3x2F::Scale((image.right - image.left) / static_cast<float>(f.width),
                                                             (image.bottom - image.top) / static_cast<float>(f.height)) *
                                     D2D1::Matrix3x2F::Translation(image.left, image.top));
            const float radius = std::min(theme.controlRadius, viewportRadius);
            target->FillRoundedRectangle(D2D1::RoundedRect(image, radius, radius), imageBrush.Get());
          } else {
            target->DrawBitmap(bitmap.Get(), image);
          }
        }
      }
      if (FAILED(target->EndDraw())) { bitmap.Reset(); brush.Reset(); target.Reset(); }
    }
    EndPaint(hwnd, &ps);
  }
  void ToggleFullscreen() {
    if (!hwnd) return;
    fullscreen = !fullscreen;
    if (fullscreen) {
      GetWindowPlacement(hwnd, &placement); savedStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
      MONITORINFO monitor{sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
      SetWindowLongPtrW(hwnd, GWL_STYLE, savedStyle & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
          std::max(static_cast<int>(monitor.rcMonitor.right - monitor.rcMonitor.left), static_cast<int>(624 * scale)),
          std::max(static_cast<int>(monitor.rcMonitor.bottom - monitor.rcMonitor.top), static_cast<int>(520 * scale)),
          SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
      SetWindowLongPtrW(hwnd, GWL_STYLE, savedStyle); SetWindowPlacement(hwnd, &placement);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    Update();
  }
  void Paste() {
    if (!keyboardReady || !OpenClipboard(hwnd)) return;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    std::string utf8;
    bool tooLong = false;
    if (text) {
      const auto count = wcsnlen(text, phone::kMaxScreenTextBytes + 1);
      if (count <= phone::kMaxScreenTextBytes) utf8 = Utf8(std::wstring_view(text, count));
      else tooLong = true;
      GlobalUnlock(handle);
    }
    CloseClipboard();
    // The limit applies to the UTF-8 text and to its escaped JSON form, which
    // grows with quotes, backslashes and control characters.
    if (!utf8.empty() && !sessionId.empty() && generation > 0 &&
        !phone::ValidScreenInput({sessionId, generation, "text", lastX, lastY, 0, utf8})) tooLong = true;
    if (tooLong) { detail = L"Paste supports up to 16 KB of plain text at a time."; Update(); return; }
    if (!utf8.empty()) Input("text", 0, std::move(utf8));
  }
  void Character(wchar_t ch) {
    if (!keyboardReady || ch < 32 || ch == 127) return;
    if (ch >= 0xD800 && ch <= 0xDBFF) { highSurrogate = ch; return; }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
      if (highSurrogate) { const wchar_t pair[]{highSurrogate, ch}; highSurrogate = 0; Input("text", 0, Utf8(std::wstring_view(pair, 2))); }
      return;
    }
    highSurrogate = 0; Input("text", 0, Utf8(std::wstring_view(&ch, 1)));
  }
  LRESULT Message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_PAINT: Paint(); return 0;
      case WM_ERASEBKGND: return 1;
      case WM_SIZE: Layout(); return 0;
      case WM_DPICHANGED: {
        const auto* r = reinterpret_cast<RECT*>(lp); SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Style(); Layout(); return 0;
      }
      case WM_SYSCOLORCHANGE: case WM_SETTINGCHANGE: case WM_THEMECHANGED: Style(); InvalidateRect(hwnd, nullptr, TRUE); break;
      case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {static_cast<LONG>(624 * scale), static_cast<LONG>(520 * scale)}; return 0;
      case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlType != ODT_BUTTON) break;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool hover = !disabled && !pressed && item->hwndItem == hoverButton;
        COLORREF fillColor, edgeColor, textColor;
        if (highContrast) {
          // Windows high contrast: system colors only, inverted while hot.
          fillColor = GetSysColor(pressed || hover ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
          edgeColor = GetSysColor(COLOR_WINDOWTEXT);
          textColor = GetSysColor(disabled ? COLOR_GRAYTEXT : (pressed || hover ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT));
        } else {
          const auto base = pressed ? theme.surfaceHover : (hover ? theme::CompositeOver(theme.surfaceHover, theme.selectedBase) : theme.selectedBase);
          fillColor = GdiColor(theme::CompositeOver(base, theme.surface));
          edgeColor = GdiColor(theme::CompositeOver(hover ? theme.border : theme.surfaceHover, theme.surface));
          textColor = GdiColor(disabled ? theme.textDim : theme.textPrimary);
        }
        const bool focused = (item->itemState & ODS_FOCUS) != 0;
        const COLORREF ringColor = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : GdiColor(theme.textPrimary);
        const bool rounded = DrawButtonFrame(item->hDC, item->rcItem, fillColor, edgeColor, focused, ringColor);
        if (!rounded) {
          HBRUSH bg = CreateSolidBrush(fillColor); FillRect(item->hDC, &item->rcItem, bg); DeleteObject(bg);
          HBRUSH edge = CreateSolidBrush(edgeColor);
          FrameRect(item->hDC, &item->rcItem, edge); DeleteObject(edge);
        }
        SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, textColor);
        const auto oldFont = SelectObject(item->hDC, font); wchar_t label[64]{}; GetWindowTextW(item->hwndItem, label, 64);
        RECT textRect = item->rcItem; DrawTextW(item->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, oldFont);
        if (focused && !rounded) { RECT focus = item->rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item->hDC, &focus); }
        return TRUE;
      }
      case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        const auto control = reinterpret_cast<HWND>(lp);
        const bool empty = control == controls[EmptyTitle] || control == controls[EmptyHelp];
        const auto color = empty ? theme::CompositeOver(theme.overlayBackground, theme.surface) : theme.surface;
        const bool muted = control == controls[Hint] || control == controls[EmptyHelp];
        SetTextColor(reinterpret_cast<HDC>(wp), GdiColor(muted ? theme.textMuted : theme.textPrimary));
        SetBkColor(reinterpret_cast<HDC>(wp), GdiColor(color)); return reinterpret_cast<LRESULT>(empty ? canvasBackground : background);
      }
      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case Start: StartSession(); break;
          case Stop: EndSession(); phase = L"Screen sharing stopped"; detail = L"Choose Start to share again. Your phone will ask for approval."; Update(); break;
          case Back: if (controlReady) Input("back"); SetFocus(hwnd); break;
          case Home: if (controlReady) Input("home"); SetFocus(hwnd); break;
          case Apps: if (controlReady) Input("recents"); SetFocus(hwnd); break;
          case Sound: sound = !sound; if (playback) playback->SetAudio(sound, static_cast<float>(SendMessageW(controls[Volume], TBM_GETPOS, 0, 0)) / 100); Update(); SetFocus(hwnd); break;
          case Fullscreen: ToggleFullscreen(); SetFocus(hwnd); break;
        }
        return 0;
      case WM_HSCROLL: if (playback) playback->SetAudio(sound, static_cast<float>(SendMessageW(controls[Volume], TBM_GETPOS, 0, 0)) / 100); return 0;
      case kUpdated: Mailbox(); return 0;
      case WM_LBUTTONDOWN: {
        SetFocus(hwnd); const auto p = phone::ScreenCoordinates(imageRect, GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
        if (controlReady && p) { lastX = p->first; lastY = p->second; mouseHeld = true; SetCapture(hwnd); Input("down"); } return 0;
      }
      case WM_MOUSEMOVE: case WM_LBUTTONUP: {
        if (!mouseHeld) break;
        const auto p = phone::ScreenCoordinates(imageRect, GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
        if (p) { lastX = p->first; lastY = p->second; }
        Input(msg == WM_LBUTTONUP ? "up" : "move"); if (msg == WM_LBUTTONUP) { mouseHeld = false; ReleaseCapture(); } return 0;
      }
      case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &p);
        const auto mapped = phone::ScreenCoordinates(imageRect, p.x / scale, p.y / scale);
        if (controlReady && mapped) {
          lastX = mapped->first; lastY = mapped->second;
          // Precision touchpads send fractions of a notch; accumulate until a whole step.
          wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
          const int steps = wheelAccum / WHEEL_DELTA;
          wheelAccum -= steps * WHEEL_DELTA;
          if (steps != 0) Input("scroll", std::clamp(steps, -10, 10));
        } return 0;
      }
      case WM_RBUTTONUP: if (controlReady) Input("back"); return 0;
      case WM_CAPTURECHANGED: case WM_KILLFOCUS: CancelInput(); return 0;
      case WM_KEYDOWN: {
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        if (wp == VK_ESCAPE) { if (fullscreen) ToggleFullscreen(); else if (controlReady) Input("back"); return 0; }
        if (!keyboardReady) break;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wp == 'V') { Paste(); return 0; } if (ctrl && wp == 'A') { Input("key", 0); return 0; }
        int key = -1;
        switch (wp) {
          case VK_RETURN: key = 66; break; case VK_BACK: key = 67; break; case VK_DELETE: key = 112; break;
          case VK_UP: key = 19; break; case VK_DOWN: key = 20; break; case VK_LEFT: key = 21; break; case VK_RIGHT: key = 22; break;
          case VK_HOME: key = 122; break; case VK_END: key = 123; break; case VK_TAB: key = 61; break;
        }
        if (key >= 0) { Input("key", key); return 0; } break;
      }
      case WM_CHAR: Character(static_cast<wchar_t>(wp)); return 0;
      case WM_UNICHAR:
        if (wp == UNICODE_NOCHAR) return TRUE;
        if (wp <= 0xffff) Character(static_cast<wchar_t>(wp));
        else if (wp <= 0x10ffff) { const auto c = wp - 0x10000; Character(static_cast<wchar_t>(0xD800 + (c >> 10))); Character(static_cast<wchar_t>(0xDC00 + (c & 1023))); } return 0;
      case WM_CLOSE: Close(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  static LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->Message(msg, wp, lp) : DefWindowProcW(window, msg, wp, lp);
  }
  void Show() {
    if (!hwnd) {
      WNDCLASSEXW cls{sizeof(WNDCLASSEXW)}; cls.lpfnWndProc = WndProc; cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpszClassName = kClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101)); RegisterClassExW(&cls);
      if (!CreateWindowExW(0, kClass, L"Phone Screen — FeatherCast", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
          CW_USEDEFAULT, CW_USEDEFAULT, 760, 960, nullptr, nullptr, cls.hInstance, this)) return;
      { std::lock_guard lock(mailbox->mutex); mailbox->notifyWindow = hwnd; }
      INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
      const auto button = [&](int id, const wchar_t* label) {
        controls[id] = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
        SetWindowLongPtrW(controls[id], GWL_STYLE, (GetWindowLongPtrW(controls[id], GWL_STYLE) & ~BS_TYPEMASK) | BS_OWNERDRAW);
        SetWindowSubclass(controls[id], HoverProc, 1, reinterpret_cast<DWORD_PTR>(&hoverButton));
      };
      button(Start, L"Start"); button(Stop, L"Stop"); button(Back, L"Back"); button(Home, L"Home");
      button(Apps, L"Recent apps"); button(Sound, L"Mute"); button(Fullscreen, L"Full screen");
      controls[Volume] = CreateWindowExW(0, TRACKBAR_CLASSW, L"Device audio volume", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
          0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(Volume), cls.hInstance, nullptr);
      SendMessageW(controls[Volume], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(controls[Volume], TBM_SETPOS, TRUE, 100);
      controls[Title] = CreateWindowExW(0, L"STATIC", L"Phone Screen", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
          0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(Title), cls.hInstance, nullptr);
      for (int id : {Status, Hint, EmptyTitle, EmptyHelp}) {
        controls[id] = CreateWindowExW(0, L"STATIC", id == Hint ? L"F11 full screen · Esc or right-click = Back · Ctrl+Tab toolbar · Closing ends sharing" : L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | (id == Hint ? SS_ENDELLIPSIS : 0), 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
      }
      Style(); Layout();
      MONITORINFO monitor{sizeof(MONITORINFO)};
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const auto& area = monitor.rcWork;
        const int width = std::clamp(static_cast<int>(area.right - area.left), static_cast<int>(624 * scale), static_cast<int>(760 * scale));
        const int height = std::clamp(static_cast<int>(area.bottom - area.top), static_cast<int>(520 * scale), static_cast<int>(960 * scale));
        SetWindowPos(hwnd, nullptr, area.left + (area.right - area.left - width) / 2,
            area.top + (area.bottom - area.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    // Cloaked until the window and its controls have painted, so the first
    // frame on screen is the finished one, never a blank or white flash.
    BOOL cloak = TRUE;
    const bool cloaked = !IsWindowVisible(hwnd) && SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
    ShowWindow(hwnd, SW_SHOW);
    if (cloaked) {
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
      cloak = FALSE; DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }
    SetForegroundWindow(hwnd); SetFocus(hwnd);
    if (connected && available && sessionId.empty()) StartSession();
  }
};

PhoneScreenWindow::PhoneScreenWindow() : impl_(std::make_unique<Impl>()) {}
PhoneScreenWindow::~PhoneScreenWindow() = default;
void PhoneScreenWindow::SetCallbacks(ScreenCallbacks cb) {
  impl_->callbacks = std::move(cb);
  std::lock_guard lock(impl_->mailbox->mutex);
  impl_->mailbox->input = impl_->callbacks.input;
}
void PhoneScreenWindow::SetTheme(const theme::Theme& theme, theme::Color) { impl_->baseTheme = theme; impl_->theme = theme; impl_->Style(); }
void PhoneScreenWindow::SetTextScale(float scale) {
  impl_->textScale = std::clamp(scale, 0.9f, 2.0f); impl_->Style(); impl_->Layout();
  if (impl_->hwnd && !impl_->fullscreen) {
    RECT rect{}; GetWindowRect(impl_->hwnd, &rect);
    const int width = std::max(static_cast<int>(rect.right - rect.left), static_cast<int>(624 * impl_->scale));
    const int height = std::max(static_cast<int>(rect.bottom - rect.top), static_cast<int>(520 * impl_->scale));
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}
void PhoneScreenWindow::SetConnection(bool connected, bool available, std::string name) {
  impl_->connected = connected; impl_->available = available; impl_->deviceName = std::move(name);
  if ((!connected || !available) && !impl_->sessionId.empty()) {
    impl_->EndSession(); impl_->phase = L"Screen sharing stopped";
    impl_->detail = connected ? L"Screen sharing was turned off on your phone." : L"Phone disconnected. Reconnect it, then start a new session.";
  }
  impl_->Update();
}
void PhoneScreenWindow::Show() { impl_->Show(); }
void PhoneScreenWindow::Close() { impl_->Close(); }
void PhoneScreenWindow::OnPacket(phone::ScreenPacket packet) {
  std::shared_ptr<phone::ScreenPlayback> playback;
  { std::lock_guard lock(impl_->mailbox->mutex); playback = impl_->playback; }
  if (playback) playback->OnPacket(std::move(packet));
}
bool PhoneScreenWindow::HandleMessage(MSG& msg) {
  if (!impl_->hwnd || (msg.hwnd != impl_->hwnd && !IsChild(impl_->hwnd, msg.hwnd))) return false;
  if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
    HWND next = GetNextDlgTabItem(impl_->hwnd, GetFocus() == impl_->hwnd ? nullptr : GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    if (next) SetFocus(next); return true;
  }
  if (msg.hwnd != impl_->hwnd) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SetFocus(impl_->hwnd); return true; }
    return IsDialogMessageW(impl_->hwnd, &msg) != FALSE;
  }
  return false;
}
HWND PhoneScreenWindow::Hwnd() const { return impl_->hwnd; }
}  // namespace feathercast::phone_ui
