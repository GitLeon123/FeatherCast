#include "phone_screen_ui.hpp"
#include "phone_screen_playback.hpp"
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <array>
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
  theme::Theme theme;
  ScreenCallbacks callbacks;
  bool connected = false, available = false, sound = true, fullscreen = false;
  bool controlReady = false, keyboardReady = false, audioReady = false, mouseHeld = false;
  int generation = 0, lastX = 0, lastY = 0;
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
  std::shared_ptr<const phone::ScreenFrame> shownFrame;
  // Only the newest decoded frame and status are marshalled to the UI.
  std::mutex mutex;
  HWND notifyWindow = nullptr;
  bool notified = false;
  std::string mailboxSession;
  std::shared_ptr<const phone::ScreenFrame> latestFrame;
  std::optional<phone::ScreenPacket> latestStatus;
  std::shared_ptr<phone::ScreenPlayback> playback;

  ~Impl() {
    Close();
    { std::lock_guard lock(mutex); notifyWindow = nullptr; }
    playback.reset();
    if (hwnd) DestroyWindow(hwnd);
    if (font) DeleteObject(font);
    if (titleFont) DeleteObject(titleFont);
    if (background) DeleteObject(background);
    if (canvasBackground) DeleteObject(canvasBackground);
  }
  void NotifyLocked() {
    if (!notified && notifyWindow) { notified = true; PostMessageW(notifyWindow, kUpdated, 0, 0); }
  }
  void EnsurePlayback() {
    std::lock_guard lock(mutex);
    if (playback) return;
    phone::ScreenPlaybackCallbacks cb;
    cb.frame = [this](std::shared_ptr<const phone::ScreenFrame> frame) {
      std::lock_guard guard(mutex);
      if (frame->sessionId == mailboxSession) { latestFrame = std::move(frame); NotifyLocked(); }
    };
    cb.status = [this](phone::ScreenPacket packet) {
      std::lock_guard guard(mutex);
      if (packet.sessionId == mailboxSession) { latestStatus = std::move(packet); NotifyLocked(); }
    };
    cb.input = [this](phone::ScreenInput input) { if (callbacks.input) callbacks.input(std::move(input)); };
    playback = std::make_shared<phone::ScreenPlayback>(std::move(cb));
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
    { std::lock_guard lock(mutex); mailboxSession = id; latestFrame.reset(); latestStatus.reset(); }
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
    { std::lock_guard lock(mutex); mailboxSession.clear(); latestFrame.reset(); latestStatus.reset(); }
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
    Caption(controls[Fullscreen], fullscreen ? L"Exit full" : L"Full screen");
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
    accessibleText(Status, status); accessibleText(EmptyTitle, phase);
    const std::wstring help = !connected ? L"Connect your Android phone in Phone Connect to share its screen."
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
    std::optional<phone::ScreenPacket> state;
    std::shared_ptr<const phone::ScreenFrame> frame;
    { std::lock_guard lock(mutex); state = std::exchange(latestStatus, {}); frame = std::move(latestFrame); notified = false; }
    if (state && state->sessionId == sessionId) {
      if (state->state == "audio-error") audioDetail = Widen(state->detail);
      else if (state->state == "streaming") {
        if (state->generation < generation) { Update(); return; }
        if (state->generation != generation) { CancelInput(); shownFrame.reset(); bitmap.Reset(); imageRect = {}; }
        generation = state->generation; controlReady = state->control; keyboardReady = state->keyboard;
        audioReady = state->audio; phase = L"Screen shared"; detail = Widen(state->detail);
      } else if (state->state == "stopped" || state->state == "error") {
        detail = Widen(state->detail); phase = state->state == "error" ? L"Screen sharing unavailable" : L"Screen sharing stopped";
        EndSession(state->state == "error");
      } else if (state->state == "pending") detail = Widen(state->detail);
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
    place(Volume, x, 62, std::max(72.0f, width - x - 132), 30); place(Fullscreen, width - 116, 62, 92, 30);
    const float height = rc.bottom / scale;
    place(Status, 24, height - 92, width - 48, 56); place(Hint, 24, height - 32, width - 48, 20);
    const float center = (112 + height - 108) * .5f - 48;
    place(EmptyTitle, 56, center, width - 112, 32); place(EmptyHelp, 56, center + 40, width - 112, 90);
    if (target) { target->SetDpi(96 * scale, 96 * scale); target->Resize(D2D1::SizeU(rc.right, rc.bottom)); }
    InvalidateRect(hwnd, nullptr, FALSE);
  }
  void Style() {
    if (!hwnd) return;
    if (font) DeleteObject(font);
    if (titleFont) DeleteObject(titleFont);
    font = CreateFontW(-static_cast<int>(MulDiv(14, GetDpiForWindow(hwnd), 96) * textScale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme.fontFamily.c_str());
    if (background) DeleteObject(background);
    background = CreateSolidBrush(GdiColor(theme.surface));
    titleFont = CreateFontW(-static_cast<int>(MulDiv(20, GetDpiForWindow(hwnd), 96) * textScale), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme.fontFamily.c_str());
    if (canvasBackground) DeleteObject(canvasBackground);
    canvasBackground = CreateSolidBrush(GdiColor(theme::CompositeOver(theme.overlayBackground, theme.surface)));
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    const bool highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    for (auto control : controls) if (control) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); SetWindowTheme(control, highContrast ? L"" : L"DarkMode_Explorer", nullptr); }
    SendMessageW(controls[EmptyTitle], WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
    if (dwrite) {
      body.Reset(); heading.Reset();
      dwrite->CreateTextFormat(theme.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"en-us", &body);
      dwrite->CreateTextFormat(theme.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"en-us", &heading);
    }
    BOOL dark = !highContrast; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)); Update();
  }
  bool Resources() {
    if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (!dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) return false;
    if (!body) Style();
    if (!target) {
      RECT rc{}; GetClientRect(hwnd, &rc);
      if (FAILED(d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &target))) return false;
      target->SetDpi(96 * scale, 96 * scale);
      if (FAILED(target->CreateSolidColorBrush(Color(theme.textPrimary), &brush))) return false;
    }
    return body && heading;
  }
  void Text(const std::wstring& text, D2D1_RECT_F rc, IDWriteTextFormat* format, theme::Color color) {
    brush->SetColor(Color(color)); target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rc, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  void Paint() {
    PAINTSTRUCT ps{}; BeginPaint(hwnd, &ps);
    if (Resources()) {
      const auto size = target->GetSize(); target->BeginDraw(); target->Clear(Color(theme.surface));
      const phone::ScreenRect viewport{24, 112, size.width - 24, size.height - 108};
      brush->SetColor(Color(theme::CompositeOver(theme.overlayBackground, theme.surface))); target->FillRectangle({viewport.left, viewport.top, viewport.right, viewport.bottom}, brush.Get());
      imageRect = {};
      if (shownFrame) {
        const auto& f = *shownFrame; imageRect = phone::FitScreenRect(viewport, f.width, f.height);
        if (!bitmap || bitmap->GetPixelSize().width != static_cast<UINT32>(f.width) || bitmap->GetPixelSize().height != static_cast<UINT32>(f.height)) {
          bitmap.Reset(); target->CreateBitmap(D2D1::SizeU(f.width, f.height), f.bgra.data(), f.width * 4,
              D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &bitmap);
        } else bitmap->CopyFromMemory(nullptr, f.bgra.data(), f.width * 4);
        if (bitmap) target->DrawBitmap(bitmap.Get(), {imageRect.left, imageRect.top, imageRect.right, imageRect.bottom});
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
    if (text) { const auto count = wcsnlen(text, 16385); if (count <= 16384) utf8 = Utf8(std::wstring_view(text, count)); GlobalUnlock(handle); }
    CloseClipboard();
    if (!utf8.empty() && utf8.size() <= 16384) Input("text", 0, std::move(utf8));
    else { detail = L"Paste supports up to 16 KB of text at a time."; Update(); }
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
      case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {static_cast<LONG>(624 * scale), static_cast<LONG>(520 * scale)}; return 0;
      case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlType != ODT_BUTTON) break;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const auto fill = theme::CompositeOver(pressed ? theme.surfaceHover : theme.selectedBase, theme.surface);
        HBRUSH bg = CreateSolidBrush(GdiColor(fill)); FillRect(item->hDC, &item->rcItem, bg); DeleteObject(bg);
        HBRUSH edge = CreateSolidBrush(GdiColor(theme::CompositeOver(theme.surfaceHover, theme.surface)));
        FrameRect(item->hDC, &item->rcItem, edge); DeleteObject(edge);
        SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, GdiColor(disabled ? theme.textDim : theme.textPrimary));
        const auto oldFont = SelectObject(item->hDC, font); wchar_t label[64]{}; GetWindowTextW(item->hwndItem, label, 64);
        RECT textRect = item->rcItem; DrawTextW(item->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, oldFont);
        if (item->itemState & ODS_FOCUS) { RECT focus = item->rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item->hDC, &focus); }
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
        if (controlReady && mapped) { lastX = mapped->first; lastY = mapped->second; Input("scroll", std::clamp(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, -10, 10)); } return 0;
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
      { std::lock_guard lock(mutex); notifyWindow = hwnd; }
      INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
      const auto button = [&](int id, const wchar_t* label) {
        controls[id] = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
        SetWindowLongPtrW(controls[id], GWL_STYLE, (GetWindowLongPtrW(controls[id], GWL_STYLE) & ~BS_TYPEMASK) | BS_OWNERDRAW);
      };
      button(Start, L"Start"); button(Stop, L"Stop"); button(Back, L"Back"); button(Home, L"Home");
      button(Apps, L"Recent apps"); button(Sound, L"Mute"); button(Fullscreen, L"Full screen");
      controls[Volume] = CreateWindowExW(0, TRACKBAR_CLASSW, L"Device audio volume", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
          0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(Volume), cls.hInstance, nullptr);
      SendMessageW(controls[Volume], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(controls[Volume], TBM_SETPOS, TRUE, 100);
      controls[Title] = CreateWindowExW(0, L"STATIC", L"Phone Screen", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
          0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(Title), cls.hInstance, nullptr);
      for (int id : {Status, Hint, EmptyTitle, EmptyHelp}) {
        controls[id] = CreateWindowExW(0, L"STATIC", id == Hint ? L"F11 full screen · Ctrl+Tab toolbar · Stop ends sharing" : L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
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
    ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); SetFocus(hwnd);
    if (connected && available && sessionId.empty()) StartSession();
  }
};

PhoneScreenWindow::PhoneScreenWindow() : impl_(std::make_unique<Impl>()) {}
PhoneScreenWindow::~PhoneScreenWindow() = default;
void PhoneScreenWindow::SetCallbacks(ScreenCallbacks cb) { impl_->callbacks = std::move(cb); }
void PhoneScreenWindow::SetTheme(const theme::Theme& theme, theme::Color) { impl_->theme = theme; impl_->Style(); }
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
  { std::lock_guard lock(impl_->mutex); playback = impl_->playback; }
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
