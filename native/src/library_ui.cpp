#include "library_ui.hpp"

#include "command_catalog.hpp"
#include "automation.hpp"
#include "ui_renderer.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <initguid.h>
#include <oleacc.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>

namespace feathercast::library_ui {
namespace {

constexpr wchar_t kManagerClass[] = L"FeatherCastLibraryManager";
constexpr wchar_t kEditorClass[] = L"FeatherCastLibraryEditor";

bool IsLaunchItem(library::ItemKind kind) {
  return kind == library::ItemKind::Quicklink || kind == library::ItemKind::Script || kind == library::ItemKind::Workspace;
}

bool IsCommandItem(library::ItemKind kind) {
  return kind == library::ItemKind::CommandAlias || kind == library::ItemKind::CommandShortcut;
}

enum ControlId : int {
  IdTabs = 100,
  IdList,
  IdAdd,
  IdEdit,
  IdDelete,
  IdReload,
  IdOpenFile,
  IdStatus,
  IdName,
  IdKeyword,
  IdValue,
};

// Posted by the manager to itself so the initial editor opens after window
// creation has finished instead of running a nested modal loop in WM_CREATE.
constexpr UINT kMsgOpenInitialEditor = WM_APP + 1;

constexpr UINT kBaseDpi = 96;
constexpr int kWorkAreaInset = 24;

COLORREF ColorRefFromTheme(const theme::Color& color) {
  const auto channel = [](float value) {
    return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f +
                             0.5f);
  };
  return RGB(channel(color.r), channel(color.g), channel(color.b));
}

void ApplyLibraryChrome(HWND window, const theme::Theme& theme,
                        bool highContrast) {
  if (!window) return;
  constexpr DWORD kUseImmersiveDarkMode = 20;
  constexpr DWORD kWindowCornerPreference = 33;
  constexpr DWORD kBorderColor = 34;
  constexpr DWORD kCaptionColor = 35;
  constexpr DWORD kTextColor = 36;
  const BOOL darkMode = highContrast ? FALSE : TRUE;
  DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &darkMode,
                        sizeof(darkMode));
  const DWORD corners = 2;
  DwmSetWindowAttribute(window, kWindowCornerPreference, &corners,
                        sizeof(corners));
  const COLORREF background =
      highContrast ? GetSysColor(COLOR_WINDOW)
                   : ColorRefFromTheme(theme.overlayBackground);
  const COLORREF border = highContrast
                              ? GetSysColor(COLOR_WINDOWTEXT)
                              : ColorRefFromTheme(theme.border);
  const COLORREF text = highContrast
                            ? GetSysColor(COLOR_WINDOWTEXT)
                            : ColorRefFromTheme(theme.textPrimary);
  DwmSetWindowAttribute(window, kBorderColor, &border, sizeof(border));
  DwmSetWindowAttribute(window, kCaptionColor, &background,
                        sizeof(background));
  DwmSetWindowAttribute(window, kTextColor, &text, sizeof(text));
}

int ScaleForDpi(int logicalPixels, UINT dpi) {
  return MulDiv(logicalPixels, static_cast<int>(std::max<UINT>(kBaseDpi, dpi)),
                static_cast<int>(kBaseDpi));
}

UINT SystemDpi() {
  using GetDpiForSystemProc = UINT(WINAPI*)();
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    if (auto proc = reinterpret_cast<GetDpiForSystemProc>(
            GetProcAddress(user32, "GetDpiForSystem"))) {
      if (const UINT dpi = proc()) return dpi;
    }
  }
  return kBaseDpi;
}

UINT DpiForWindow(HWND window) {
  using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
  if (window) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      if (auto proc = reinterpret_cast<GetDpiForWindowProc>(
              GetProcAddress(user32, "GetDpiForWindow"))) {
        if (const UINT dpi = proc(window)) return dpi;
      }
    }
  }
  return 0;
}

UINT DpiForMonitor(HMONITOR monitor) {
  if (!monitor) return SystemDpi();
  using GetDpiForMonitorProc = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  HMODULE shcore = GetModuleHandleW(L"shcore.dll");
  bool loaded = false;
  if (!shcore) {
    shcore = LoadLibraryW(L"shcore.dll");
    loaded = shcore != nullptr;
  }
  UINT dpiX = kBaseDpi;
  UINT dpiY = kBaseDpi;
  if (shcore) {
    if (auto proc = reinterpret_cast<GetDpiForMonitorProc>(
            GetProcAddress(shcore, "GetDpiForMonitor"))) {
      proc(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
  }
  if (loaded) FreeLibrary(shcore);
  return dpiX > 0 ? dpiX : SystemDpi();
}

HMONITOR ReferenceMonitor(HWND reference) {
  if (reference && IsWindow(reference) && IsWindowVisible(reference)) {
    return MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
  }
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  }
  if (reference && IsWindow(reference)) {
    return MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
  }
  return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
}

UINT DpiForReference(HWND reference) {
  if (reference && IsWindow(reference) && IsWindowVisible(reference)) {
    if (const UINT dpi = DpiForWindow(reference)) return dpi;
  }
  return DpiForMonitor(ReferenceMonitor(reference));
}

RECT WorkAreaFor(HWND reference) {
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfoW(ReferenceMonitor(reference), &info)) return info.rcWork;
  RECT work{};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return work;
  return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
              GetSystemMetrics(SM_CYSCREEN)};
}

SIZE FitWindowSize(HWND reference, int logicalWidth, int logicalHeight) {
  const UINT dpi = DpiForReference(reference);
  const RECT work = WorkAreaFor(reference);
  const int inset = ScaleForDpi(kWorkAreaInset, dpi);
  const int maxWidth = std::max(
      1, static_cast<int>(work.right - work.left) - inset * 2);
  const int maxHeight = std::max(
      1, static_cast<int>(work.bottom - work.top) - inset * 2);
  return SIZE{
      std::min(std::max(1, ScaleForDpi(logicalWidth, dpi)), maxWidth),
      std::min(std::max(1, ScaleForDpi(logicalHeight, dpi)), maxHeight)};
}

// Message font for the window's own DPI (not the process-wide system DPI), in
// the first installed family of the theme's font list.
HFONT CreateDialogFont(HWND window, const theme::Theme& theme) {
  const UINT targetDpi = std::max(kBaseDpi, DpiForWindow(window));
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  using SystemParametersInfoForDpiProc =
      BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
  bool haveMetrics = false;
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    if (auto proc = reinterpret_cast<SystemParametersInfoForDpiProc>(
            GetProcAddress(user32, "SystemParametersInfoForDpi"))) {
      haveMetrics = proc(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                         targetDpi) != FALSE;
    }
  }
  LOGFONTW font{};
  if (haveMetrics) {
    font = metrics.lfMessageFont;
  } else {
    metrics = NONCLIENTMETRICSW{sizeof(metrics)};
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                               &metrics, 0)) {
      return nullptr;
    }
    font = metrics.lfMessageFont;
    font.lfHeight = MulDiv(font.lfHeight, static_cast<int>(targetDpi),
                           static_cast<int>(std::max(kBaseDpi, SystemDpi())));
  }
  const std::wstring family =
      ui::ResolveInstalledFontFamily(theme.fontFamily);
  wcsncpy_s(font.lfFaceName, LF_FACESIZE, family.c_str(), _TRUNCATE);
  return CreateFontIndirectW(&font);
}

void UseDefaultFont(HWND control, HFONT font = nullptr) {
  SendMessageW(control, WM_SETFONT,
               reinterpret_cast<WPARAM>(font ? font
                                              : GetStockObject(DEFAULT_GUI_FONT)),
               TRUE);
}

void SetAccessibleName(HWND control, const wchar_t* name) {
  IAccPropServices* services = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&services)))) {
    services->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT),
                             CHILDID_SELF,
                             PROPID_ACC_NAME, name);
    services->Release();
  }
}

// Marks a static status control as a UIA live region (1 = polite, 2 =
// assertive) so assistive technology announces its text when it changes.
void SetLiveSetting(HWND control, int setting) {
  IAccPropServices* services = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&services)))) {
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = setting;
    services->SetHwndProp(control, static_cast<DWORD>(OBJID_CLIENT),
                          CHILDID_SELF, LiveSetting_Property_GUID, value);
    services->Release();
  }
}

// The status text is the control's accessible name, so a change is a name
// change plus a live-region change on the control's own window. (The old
// VALUECHANGE on the parent with the control id as child id matched no
// accessible object.)
void AnnounceStatus(HWND status, bool error) {
  if (!status) return;
  SetLiveSetting(status, error ? 2 : 1);
  NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, status, OBJID_CLIENT, CHILDID_SELF);
  NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, status, OBJID_CLIENT,
                 CHILDID_SELF);
}

std::wstring ControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
  if (length > 0) GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<std::size_t>(std::max(0, length)));
  return value;
}

void CenterOwnedWindow(HWND window, HWND owner) {
  RECT windowRect{};
  GetWindowRect(window, &windowRect);
  RECT centerRect{};
  if (!owner || !IsWindowVisible(owner) || !GetWindowRect(owner, &centerRect)) {
    centerRect = WorkAreaFor(owner);
  }
  const RECT work = WorkAreaFor(owner);
  const int width = windowRect.right - windowRect.left;
  const int height = windowRect.bottom - windowRect.top;
  const int centeredLeft =
      centerRect.left + (centerRect.right - centerRect.left - width) / 2;
  const int centeredTop =
      centerRect.top + (centerRect.bottom - centerRect.top - height) / 2;
  const int workLeft = static_cast<int>(work.left);
  const int workTop = static_cast<int>(work.top);
  const int workRight = static_cast<int>(work.right);
  const int workBottom = static_cast<int>(work.bottom);
  const int left = std::clamp(centeredLeft, workLeft,
                              std::max(workLeft, workRight - width));
  const int top = std::clamp(centeredTop, workTop,
                             std::max(workTop, workBottom - height));
  SetWindowPos(window, nullptr, left, top, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Pumps messages until `dialog` is destroyed. Dialog keyboard handling only
// applies to messages for the dialog or its children; other windows of the
// thread keep their normal dispatch. `consumeKey` may swallow a message that
// belongs to the dialog before IsDialogMessage sees it. When the thread gets
// WM_QUIT (or GetMessage fails) the dialog is closed through `closeDialog` and
// the quit is re-posted so outer loops terminate too.
template <typename ConsumeKey, typename CloseDialog>
void RunDialogLoop(HWND dialog, ConsumeKey&& consumeKey,
                   CloseDialog&& closeDialog) {
  MSG message{};
  while (IsWindow(dialog)) {
    const BOOL got = GetMessageW(&message, nullptr, 0, 0);
    if (got <= 0) {
      closeDialog();
      if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
      return;
    }
    if (message.hwnd == dialog || IsChild(dialog, message.hwnd)) {
      if (consumeKey(message) || IsDialogMessageW(dialog, &message)) continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

// Re-enables the owner before the dialog goes away so activation returns to
// it instead of an arbitrary window, then destroys the dialog.
void CloseModalDialog(HWND dialog, HWND owner) {
  if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
  if (dialog && IsWindow(dialog)) DestroyWindow(dialog);
}

class EditorWindow {
 public:
  EditorWindow(HWND owner, library::ItemKind kind,
               const std::vector<snippets::Snippet>& snippets,
               const std::vector<settings::Quicklink>& quicklinks,
               const std::vector<library::AppAlias>& aliases,
               const std::vector<library::CommandAlias>& commandAliases,
               const std::vector<library::AppChoice>& availableApps,
               const std::vector<library::WebSearch>& searches,
               std::optional<std::size_t> editingIndex,
               std::wstring preferredAppId = {},
               theme::Theme theme = {}, bool highContrast = false)
      : owner_(owner),
        kind_(kind),
        snippets_(snippets),
        quicklinks_(quicklinks),
        aliases_(aliases),
        commandAliases_(commandAliases),
        availableApps_(availableApps),
        searches_(searches),
        editingIndex_(editingIndex),
        preferredAppId_(std::move(preferredAppId)),
        theme_(std::move(theme)),
        highContrast_(highContrast) {
    if (editingIndex_) {
      if (kind_ == library::ItemKind::Snippet) {
        snippet_ = snippets_.at(*editingIndex_);
      } else if (IsLaunchItem(kind_)) {
        quicklink_ = quicklinks_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::AppAlias) {
        alias_ = aliases_.at(*editingIndex_);
      } else if (IsCommandItem(kind_)) {
        commandAlias_ = commandAliases_.at(*editingIndex_);
      } else {
        webSearch_ = searches_.at(*editingIndex_);
      }
    }
    for (const auto& descriptor : commands::Catalog()) {
      availableCommands_.push_back({descriptor.stableId, descriptor.label});
    }
  }

  ~EditorWindow() {
    if (font_) DeleteObject(font_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
  }

  bool Run() {
    Register();
    const wchar_t* title = L"Edit Library Item";
    if (kind_ == library::ItemKind::Snippet) {
      title = editingIndex_ ? L"Edit Snippet" : L"Add Snippet";
    } else if (IsLaunchItem(kind_)) {
      title = kind_ == library::ItemKind::Script ? (editingIndex_ ? L"Edit Script" : L"Add Script") :
          kind_ == library::ItemKind::Workspace ? (editingIndex_ ? L"Edit Workspace" : L"Add Workspace") :
          (editingIndex_ ? L"Edit Quicklink" : L"Add Quicklink");
    } else if (kind_ == library::ItemKind::AppAlias) {
      title = editingIndex_ ? L"Edit App Alias" : L"Add App Alias";
    } else if (IsCommandItem(kind_)) {
      title = kind_ == library::ItemKind::CommandShortcut ? L"Command Shortcut" :
          (editingIndex_ ? L"Edit Command Alias" : L"Add Command Alias");
    } else {
      title = editingIndex_ ? L"Edit Web Search" : L"Add Web Search";
    }
    const SIZE windowSize = FitWindowSize(
        owner_, 570, (kind_ == library::ItemKind::Snippet || kind_ == library::ItemKind::Workspace) ? 430 : 320);
    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kEditorClass, title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
        windowSize.cx, windowSize.cy, owner_, nullptr, GetModuleHandleW(nullptr),
        this);
    if (!hwnd_) return false;
    ApplyLibraryChrome(hwnd_, theme_, highContrast_);
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    RunDialogLoop(
        hwnd_,
        [this](const MSG& message) {
          if (message.message != WM_KEYDOWN) return false;
          if (message.wParam == VK_ESCAPE) {
            SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
            return true;
          }
          if (message.wParam == VK_RETURN &&
              ((kind_ != library::ItemKind::Snippet && kind_ != library::ItemKind::Workspace) || GetFocus() != value_ ||
               (GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
            SendMessageW(hwnd_, WM_COMMAND, IDOK, 0);
            return true;
          }
          return false;
        },
        [this] { Close(); });
    // The owner was re-enabled before the dialog was destroyed; this only
    // matters if the loop ended some other way.
    if (owner_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
    return accepted_;
  }

  const snippets::Snippet& Snippet() const { return snippet_; }
  const settings::Quicklink& Quicklink() const { return quicklink_; }
  const library::AppAlias& Alias() const { return alias_; }
  const library::CommandAlias& CommandAlias() const { return commandAlias_; }
  const library::WebSearch& WebSearch() const { return webSearch_; }

 private:
  static void Register() {
    static const bool registered = [] {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = &EditorWindow::WindowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = nullptr;
      wc.lpszClassName = kEditorClass;
      return RegisterClassExW(&wc) != 0 ||
             GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    auto* self = reinterpret_cast<EditorWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<EditorWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
      self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
                  int id) {
    HWND control = CreateWindowExW(
        wcscmp(className, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0, className, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    UseDefaultFont(control);
    return control;
  }

  void RefreshFont() {
    HFONT next = CreateDialogFont(hwnd_, theme_);
    if (!next) return;
    const HFONT previous = font_;
    font_ = next;
    for (HWND control : {nameLabel_, name_, keywordLabel_, keyword_, valueLabel_,
                         value_, save_, cancel_, status_}) {
      UseDefaultFont(control, font_);
    }
    if (previous) DeleteObject(previous);
  }

  void RefreshBrushes() {
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
    const COLORREF background =
        highContrast_ ? GetSysColor(COLOR_WINDOW)
                      : ColorRefFromTheme(theme_.overlayBackground);
    const COLORREF surface =
        highContrast_ ? GetSysColor(COLOR_BTNFACE)
                      : ColorRefFromTheme(theme_.surface);
    backgroundBrush_ = CreateSolidBrush(background);
    surfaceBrush_ = CreateSolidBrush(surface);
  }

  void Close() { CloseModalDialog(hwnd_, owner_); }

  void SetStatus(std::wstring text, bool error) {
    statusError_ = error;
    SetWindowTextW(status_, text.c_str());
    InvalidateRect(status_, nullptr, TRUE);
    AnnounceStatus(status_, error);
  }

  LRESULT ColorControl(UINT message, WPARAM wParam, LPARAM lParam) const {
    HDC dc = reinterpret_cast<HDC>(wParam);
    const HWND control = reinterpret_cast<HWND>(lParam);
    const bool staticControl = message == WM_CTLCOLORSTATIC;
    const bool statusControl = control == status_;
    const COLORREF text = statusControl && statusError_
                              ? (highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                                               : ColorRefFromTheme(theme_.dangerText))
                              : (highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                                               : ColorRefFromTheme(theme_.textPrimary));
    SetTextColor(dc, text);
    SetBkColor(dc, staticControl
                         ? (highContrast_ ? GetSysColor(COLOR_WINDOW)
                                          : ColorRefFromTheme(theme_.overlayBackground))
                         : (highContrast_ ? GetSysColor(COLOR_BTNFACE)
                                          : ColorRefFromTheme(theme_.surface)));
    SetBkMode(dc, staticControl ? TRANSPARENT : OPAQUE);
    return reinterpret_cast<LRESULT>(staticControl ? backgroundBrush_
                                                    : surfaceBrush_);
  }

  void PaintBackground() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, backgroundBrush_);
    EndPaint(hwnd_, &paint);
  }

  void LayoutCurrentClient() const {
    RECT client{};
    if (GetClientRect(hwnd_, &client)) Layout(client.right, client.bottom);
  }

  void CreateControls() {
    const bool appAlias = kind_ == library::ItemKind::AppAlias;
    const bool commandAlias = IsCommandItem(kind_);
    const bool alias = appAlias || commandAlias;
    const bool webSearch = kind_ == library::ItemKind::WebSearch;
    const wchar_t* nameLabel = appAlias
        ? L"App"
        : (commandAlias
               ? L"Command"
               : (kind_ == library::ItemKind::Snippet ? L"Name"
                                                       : L"Name (optional)"));
    nameLabel_ = AddControl(WC_STATICW, nameLabel, SS_LEFT, 0);
    name_ = AddControl(alias ? WC_COMBOBOXW : WC_EDITW, L"",
                       WS_TABSTOP | (alias ? CBS_DROPDOWNLIST | WS_VSCROLL
                                           : ES_AUTOHSCROLL),
                       IdName);
    keywordLabel_ = AddControl(WC_STATICW, kind_ == library::ItemKind::CommandShortcut ? L"Global shortcut (e.g. Ctrl+Alt+V)" : (alias ? L"Alias" : L"Keyword"),
                               SS_LEFT, 0);
    keyword_ =
        AddControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IdKeyword);
    valueLabel_ = AddControl(
        WC_STATICW,
        kind_ == library::ItemKind::Snippet ? L"Text" :
        (webSearch ? L"URL template (use %s for the query)" :
         kind_ == library::ItemKind::Script ? L"PowerShell file (.ps1); runs under your Windows policy" :
         kind_ == library::ItemKind::Workspace ? L"Apps, files, folders or URLs (one per line, up to 16)" : L"Target"),
        SS_LEFT, 0);
    DWORD valueStyle = WS_TABSTOP;
    if (kind_ == library::ItemKind::Snippet || kind_ == library::ItemKind::Workspace) {
      valueStyle |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
    } else {
      valueStyle |= ES_AUTOHSCROLL;
    }
    value_ = AddControl(WC_EDITW, L"", valueStyle, IdValue);
    save_ = AddControl(WC_BUTTONW, L"Save",
                       WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = AddControl(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
    // No fixed accessible name: the status text itself is the name, so
    // screen readers read the validation message.
    status_ = AddControl(WC_STATICW, L"", SS_LEFT, IdStatus);
    SetLiveSetting(status_, 2);
    const wchar_t* accessibleName = appAlias
        ? L"App"
        : (commandAlias
               ? L"Command"
               : (kind_ == library::ItemKind::Snippet
                      ? L"Snippet name"
                      : kind_ == library::ItemKind::Script ? L"Script name (optional)"
                      : kind_ == library::ItemKind::Workspace ? L"Workspace name (optional)"
                      : L"Quicklink name (optional)"));
    SetAccessibleName(name_, accessibleName);
    SetAccessibleName(keyword_, appAlias ? L"App alias" :
                                  (commandAlias ? (kind_ == library::ItemKind::CommandShortcut ? L"Global command shortcut" : L"Command alias") : L"Keyword"));
    SetAccessibleName(value_, webSearch ? L"Web search URL template" :
        (kind_ == library::ItemKind::Snippet ? L"Snippet text"
         : kind_ == library::ItemKind::Script ? L"PowerShell file path"
         : kind_ == library::ItemKind::Workspace ? L"Workspace targets, one per line"
                                             : L"Quicklink target"));

    if (kind_ == library::ItemKind::Snippet) {
      SetWindowTextW(name_, snippet_.name.c_str());
      SetWindowTextW(keyword_, snippet_.keyword.c_str());
      SetWindowTextW(value_, snippet_.text.c_str());
    } else if (IsLaunchItem(kind_)) {
      SetWindowTextW(name_, quicklink_.name.c_str());
      SetWindowTextW(keyword_, quicklink_.keyword.c_str());
      SetWindowTextW(value_, quicklink_.target.c_str());
    } else if (appAlias) {
      std::wstring selectedId = alias_.appId.empty() ? preferredAppId_
                                                     : alias_.appId;
      int selected = -1;
      for (std::size_t index = 0; index < availableApps_.size(); ++index) {
        const auto& app = availableApps_[index];
        const int item = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(app.name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, item,
                     static_cast<LPARAM>(index));
        if (app.id == selectedId) selected = item;
      }
      if (selected < 0 && !selectedId.empty()) {
        library::AppChoice missing{selectedId, L"Missing app - " + selectedId};
        availableApps_.push_back(std::move(missing));
        selected = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(availableApps_.back().name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, selected,
                     static_cast<LPARAM>(availableApps_.size() - 1));
      }
      if (selected < 0 && !availableApps_.empty()) selected = 0;
      SendMessageW(name_, CB_SETCURSEL, selected, 0);
      SetWindowTextW(keyword_, alias_.alias.c_str());
      ShowWindow(valueLabel_, SW_HIDE);
      ShowWindow(value_, SW_HIDE);
    } else if (commandAlias) {
      std::wstring selectedId = commandAlias_.stableId.empty()
          ? preferredAppId_
          : commandAlias_.stableId;
      int selected = -1;
      for (std::size_t index = 0; index < availableCommands_.size(); ++index) {
        const auto& command = availableCommands_[index];
        const int item = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(command.name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, item, static_cast<LPARAM>(index));
        if (command.stableId == selectedId) selected = item;
      }
      if (selected < 0 && !selectedId.empty()) {
        availableCommands_.push_back(
            {selectedId, L"Missing command - " + selectedId});
        selected = static_cast<int>(SendMessageW(
            name_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(availableCommands_.back().name.c_str())));
        SendMessageW(name_, CB_SETITEMDATA, selected,
                     static_cast<LPARAM>(availableCommands_.size() - 1));
      }
      if (selected < 0 && !availableCommands_.empty()) selected = 0;
      SendMessageW(name_, CB_SETCURSEL, selected, 0);
      SetWindowTextW(keyword_, commandAlias_.alias.c_str());
      ShowWindow(valueLabel_, SW_HIDE);
      ShowWindow(value_, SW_HIDE);
    } else {
      ShowWindow(nameLabel_, SW_HIDE);
      ShowWindow(name_, SW_HIDE);
      SetWindowTextW(keyword_, webSearch_.keyword.c_str());
      SetWindowTextW(value_, webSearch_.urlTemplate.c_str());
    }
    SetFocus(webSearch ? keyword_ : name_);
  }

  void Layout(int width, int height) const {
    const UINT dpi = std::max(kBaseDpi, DpiForWindow(hwnd_));
    const auto px = [dpi](int logicalPixels) {
      return ScaleForDpi(logicalPixels, dpi);
    };
    const int margin = px(18);
    const int labelHeight = px(20);
    const int editHeight = px(27);
    const int gap = px(12);
    const int statusHeight = px(24);
    const int contentWidth = std::max(1, width - 2 * margin);
    int y = margin;
    if (kind_ != library::ItemKind::WebSearch) {
      MoveWindow(nameLabel_, margin, y, contentWidth, labelHeight, TRUE);
      y += labelHeight;
      MoveWindow(name_, margin, y, contentWidth,
                 (kind_ == library::ItemKind::AppAlias ||
                  IsCommandItem(kind_))
                     ? px(240)
                     : editHeight,
                 TRUE);
      y += editHeight + gap;
    }
    MoveWindow(keywordLabel_, margin, y, contentWidth, labelHeight, TRUE);
    y += labelHeight;
    MoveWindow(keyword_, margin, y, contentWidth, editHeight, TRUE);
    y += editHeight + gap;
    if (kind_ == library::ItemKind::AppAlias ||
        IsCommandItem(kind_)) {
      const int buttonWidth = px(90);
      const int buttonHeight = px(30);
      const int buttonsTop = std::max(y, height - margin - buttonHeight);
      const int statusTop = std::max(y, buttonsTop - gap - statusHeight);
      MoveWindow(status_, margin, statusTop, contentWidth, statusHeight, TRUE);
      MoveWindow(cancel_, std::max(margin, width - margin - buttonWidth),
                 buttonsTop, buttonWidth, buttonHeight, TRUE);
      MoveWindow(save_, std::max(margin, width - margin - buttonWidth - gap -
                                         buttonWidth),
                 buttonsTop, buttonWidth, buttonHeight, TRUE);
      return;
    }
    MoveWindow(valueLabel_, margin, y, contentWidth, labelHeight, TRUE);
    y += labelHeight;
    const int buttonWidth = px(90);
    const int buttonHeight = px(30);
    const int buttonsTop = std::max(y + editHeight,
                                   height - margin - buttonHeight);
    const int statusTop = std::max(y + editHeight,
                                   buttonsTop - gap - statusHeight);
    MoveWindow(value_, margin, y, contentWidth,
               std::max(editHeight, statusTop - y - gap), TRUE);
    MoveWindow(status_, margin, statusTop, contentWidth, statusHeight, TRUE);
    MoveWindow(cancel_, std::max(margin, width - margin - buttonWidth),
               buttonsTop, buttonWidth, buttonHeight, TRUE);
    MoveWindow(save_, std::max(margin, width - margin - buttonWidth - gap -
                                      buttonWidth),
               buttonsTop, buttonWidth, buttonHeight, TRUE);
  }

  void Accept() {
    if (kind_ == library::ItemKind::Snippet) {
      snippet_.name = snippets::Trim(ControlText(name_));
      snippet_.keyword = snippets::Trim(ControlText(keyword_));
      snippet_.text = ControlText(value_);
      if (const auto error = library::ValidateSnippet(
              snippet_, snippets_, editingIndex_)) {
        SetStatus(*error, true);
        SetFocus(name_);
        return;
      }
    } else if (IsLaunchItem(kind_)) {
      quicklink_.name = snippets::Trim(ControlText(name_));
      quicklink_.keyword = snippets::Trim(ControlText(keyword_));
      quicklink_.target = snippets::Trim(ControlText(value_));
      const auto validation = kind_ == library::ItemKind::Quicklink
          ? library::ValidateQuicklink(quicklink_, quicklinks_, editingIndex_)
          : automation::Validate(kind_ == library::ItemKind::Script ? automation::Kind::Script : automation::Kind::Workspace, quicklink_);
      if (validation) {
        SetStatus(*validation, true);
        SetFocus(value_);
        return;
      }
      for (std::size_t index = 0; index < quicklinks_.size(); ++index) {
        if (editingIndex_ == index) continue;
        if (library::NormalizeKeyword(quicklinks_[index].keyword) != library::NormalizeKeyword(quicklink_.keyword)) continue;
        const auto error = std::optional<std::wstring>(L"That keyword is already used.");
        SetStatus(*error, true);
        SetFocus(name_);
        return;
      }
    } else if (kind_ == library::ItemKind::AppAlias) {
      const int selected = static_cast<int>(SendMessageW(name_, CB_GETCURSEL, 0, 0));
      if (selected >= 0) {
        const auto index = static_cast<std::size_t>(SendMessageW(
            name_, CB_GETITEMDATA, selected, 0));
        if (index < availableApps_.size()) {
          alias_.appId = availableApps_[index].id;
          alias_.appName = availableApps_[index].name;
        }
      }
      alias_.alias = snippets::Trim(ControlText(keyword_));
      if (const auto error = library::ValidateAppAlias(
              alias_, aliases_, editingIndex_)) {
        SetStatus(*error, true);
        SetFocus(keyword_);
        return;
      }
    } else if (IsCommandItem(kind_)) {
      const int selected =
          static_cast<int>(SendMessageW(name_, CB_GETCURSEL, 0, 0));
      if (selected >= 0) {
        const auto index = static_cast<std::size_t>(
            SendMessageW(name_, CB_GETITEMDATA, selected, 0));
        if (index < availableCommands_.size()) {
          commandAlias_.stableId = availableCommands_[index].stableId;
          commandAlias_.commandName = availableCommands_[index].name;
        }
      }
      commandAlias_.alias = snippets::Trim(ControlText(keyword_));
      const auto validation = kind_ == library::ItemKind::CommandShortcut
          ? (commandAlias_.stableId.empty() ? std::optional<std::wstring>(L"Choose a command.") : automation::ValidateShortcut(commandAlias_.alias))
          : library::ValidateCommandAlias(commandAlias_, commandAliases_, aliases_, snippets_, quicklinks_, editingIndex_);
      if (const auto error = validation) {
        SetStatus(*error, true);
        SetFocus(keyword_);
        return;
      }
    } else {
      webSearch_.keyword = library::NormalizeKeyword(ControlText(keyword_));
      webSearch_.urlTemplate = snippets::Trim(ControlText(value_));
      if (const auto error = library::ValidateWebSearch(
              webSearch_, searches_, editingIndex_)) {
        SetStatus(*error, true);
        SetFocus(keyword_);
        return;
      }
    }
    accepted_ = true;
    Close();
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        RefreshBrushes();
        CreateControls();
        RefreshFont();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        PaintBackground();
        return 0;
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORBTN:
        return ColorControl(message, wParam, lParam);
      case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
          SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ApplyLibraryChrome(hwnd_, theme_, highContrast_);
        RefreshFont();
        RefreshBrushes();
        LayoutCurrentClient();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
      }
      case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
          Accept();
          return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
          Close();
          return 0;
        }
        break;
      case WM_CLOSE:
        Close();
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
  }

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  library::ItemKind kind_ = library::ItemKind::Snippet;
  std::vector<snippets::Snippet> snippets_;
  std::vector<settings::Quicklink> quicklinks_;
  std::vector<library::AppAlias> aliases_;
  std::vector<library::CommandAlias> commandAliases_;
  std::vector<library::AppChoice> availableApps_;
  std::vector<library::CommandChoice> availableCommands_;
  std::vector<library::WebSearch> searches_;
  std::optional<std::size_t> editingIndex_;
  snippets::Snippet snippet_;
  settings::Quicklink quicklink_;
  library::AppAlias alias_;
  library::CommandAlias commandAlias_;
  library::WebSearch webSearch_;
  std::wstring preferredAppId_;
  theme::Theme theme_;
  bool highContrast_ = false;
  bool accepted_ = false;
  HWND nameLabel_ = nullptr;
  HWND name_ = nullptr;
  HWND keywordLabel_ = nullptr;
  HWND keyword_ = nullptr;
  HWND valueLabel_ = nullptr;
  HWND value_ = nullptr;
  HWND save_ = nullptr;
  HWND cancel_ = nullptr;
  HWND status_ = nullptr;
  bool statusError_ = false;
  HBRUSH backgroundBrush_ = nullptr;
  HBRUSH surfaceBrush_ = nullptr;
};

class ManagerWindow {
 public:
  ManagerWindow(HWND owner, ManagerData data, ManagerCallbacks callbacks,
                library::ItemKind initialKind, std::wstring initialAppId,
                theme::Theme theme, bool highContrast)
      : owner_(owner),
        data_(std::move(data)),
        callbacks_(std::move(callbacks)),
        kind_(initialKind),
        initialAppId_(std::move(initialAppId)),
        theme_(std::move(theme)),
        highContrast_(highContrast) {}

  ~ManagerWindow() {
    if (font_) DeleteObject(font_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
    if (selectedBrush_) DeleteObject(selectedBrush_);
  }

  void Run() {
    Register();
    INITCOMMONCONTROLSEX controls{sizeof(controls),
                                  ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    const SIZE windowSize = FitWindowSize(owner_, 760, 530);
    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kManagerClass,
        L"FeatherCast Library",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, windowSize.cx, windowSize.cy, owner_,
        nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;
    ApplyLibraryChrome(hwnd_, theme_, highContrast_);
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    RunDialogLoop(
        hwnd_,
        [this](const MSG& message) {
          if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
            return true;
          }
          return false;
        },
        [this] { Close(); });
    if (owner_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
  }

 private:
  static void Register() {
    static const bool registered = [] {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = &ManagerWindow::WindowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = nullptr;
      wc.lpszClassName = kManagerClass;
      return RegisterClassExW(&wc) != 0 ||
             GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    auto* self = reinterpret_cast<ManagerWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<ManagerWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
      self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
  }

  HWND AddControl(const wchar_t* className, const wchar_t* text, DWORD style,
                  int id) {
    HWND control = CreateWindowExW(
        wcscmp(className, WC_LISTVIEWW) == 0 ? WS_EX_CLIENTEDGE : 0, className, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    UseDefaultFont(control);
    return control;
  }

  void RefreshFont() {
    HFONT next = CreateDialogFont(hwnd_, theme_);
    if (!next) return;
    const HFONT previous = font_;
    font_ = next;
    for (HWND control : {tabs_, list_, add_, edit_, remove_, reload_, openFile_,
                         close_, status_}) {
      UseDefaultFont(control, font_);
    }
    if (previous) DeleteObject(previous);
  }

  void RefreshBrushes() {
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
    if (selectedBrush_) DeleteObject(selectedBrush_);
    const COLORREF background =
        highContrast_ ? GetSysColor(COLOR_WINDOW)
                      : ColorRefFromTheme(theme_.overlayBackground);
    const COLORREF surface =
        highContrast_ ? GetSysColor(COLOR_BTNFACE)
                      : ColorRefFromTheme(theme_.surface);
    const COLORREF selected =
        highContrast_ ? GetSysColor(COLOR_HIGHLIGHT)
                      : ColorRefFromTheme(theme_.selectedBase);
    backgroundBrush_ = CreateSolidBrush(background);
    surfaceBrush_ = CreateSolidBrush(surface);
    selectedBrush_ = CreateSolidBrush(selected);
    if (list_) {
      ListView_SetBkColor(list_, surface);
      ListView_SetTextBkColor(list_, surface);
      ListView_SetTextColor(
          list_, highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                               : ColorRefFromTheme(theme_.textPrimary));
      ListView_SetOutlineColor(list_, selected);
    }
  }

  void Close() { CloseModalDialog(hwnd_, owner_); }

  void SetStatus(std::wstring text, bool error) {
    operationStatus_ = std::move(text);
    operationStatusError_ = error;
    if (status_) SetWindowTextW(status_, operationStatus_.c_str());
    if (status_) InvalidateRect(status_, nullptr, TRUE);
    AnnounceStatus(status_, error);
  }

  LRESULT ColorControl(UINT message, WPARAM wParam, LPARAM lParam) const {
    HDC dc = reinterpret_cast<HDC>(wParam);
    const HWND control = reinterpret_cast<HWND>(lParam);
    const bool staticControl = message == WM_CTLCOLORSTATIC;
    const bool statusControl = control == status_;
    const COLORREF text = statusControl && operationStatusError_
                              ? (highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                                               : ColorRefFromTheme(theme_.dangerText))
                              : (highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                                               : ColorRefFromTheme(theme_.textPrimary));
    SetTextColor(dc, text);
    SetBkColor(dc, staticControl
                         ? (highContrast_ ? GetSysColor(COLOR_WINDOW)
                                          : ColorRefFromTheme(theme_.overlayBackground))
                         : (highContrast_ ? GetSysColor(COLOR_BTNFACE)
                                          : ColorRefFromTheme(theme_.surface)));
    SetBkMode(dc, staticControl ? TRANSPARENT : OPAQUE);
    return reinterpret_cast<LRESULT>(staticControl ? backgroundBrush_
                                                    : surfaceBrush_);
  }

  void PaintBackground() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, backgroundBrush_);
    EndPaint(hwnd_, &paint);
  }

  void LayoutCurrentClient() const {
    RECT client{};
    if (GetClientRect(hwnd_, &client)) Layout(client.right, client.bottom);
  }

  void CreateControls() {
    tabs_ = AddControl(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_MULTILINE, IdTabs);
    TCITEMW tab{TCIF_TEXT};
    tab.pszText = const_cast<wchar_t*>(L"Snippets");
    TabCtrl_InsertItem(tabs_, 0, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Quicklinks");
    TabCtrl_InsertItem(tabs_, 1, &tab);
    tab.pszText = const_cast<wchar_t*>(L"App Aliases");
    TabCtrl_InsertItem(tabs_, 2, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Command Aliases");
    TabCtrl_InsertItem(tabs_, 3, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Web Searches");
    TabCtrl_InsertItem(tabs_, 4, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Scripts");
    TabCtrl_InsertItem(tabs_, 5, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Workspaces");
    TabCtrl_InsertItem(tabs_, 6, &tab);
    tab.pszText = const_cast<wchar_t*>(L"Command Shortcuts");
    TabCtrl_InsertItem(tabs_, 7, &tab);
    TabCtrl_SetCurSel(tabs_, static_cast<int>(kind_));

    list_ = AddControl(WC_LISTVIEWW, L"",
                       WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                           LVS_SHOWSELALWAYS,
                       IdList);
    SetAccessibleName(list_, L"Library items");
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    RefreshBrushes();
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.pszText = const_cast<wchar_t*>(L"Name");
    const UINT dpi = std::max(kBaseDpi, DpiForWindow(hwnd_));
    column.cx = ScaleForDpi(190, dpi);
    ListView_InsertColumn(list_, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"Keyword");
    column.cx = ScaleForDpi(130, dpi);
    ListView_InsertColumn(list_, 1, &column);
    column.pszText = const_cast<wchar_t*>(L"Text / Target");
    column.cx = ScaleForDpi(360, dpi);
    ListView_InsertColumn(list_, 2, &column);

    add_ = AddControl(WC_BUTTONW, L"Add", WS_TABSTOP, IdAdd);
    edit_ = AddControl(WC_BUTTONW, L"Edit", WS_TABSTOP, IdEdit);
    remove_ = AddControl(WC_BUTTONW, L"Delete", WS_TABSTOP, IdDelete);
    reload_ = AddControl(WC_BUTTONW, L"Reload", WS_TABSTOP, IdReload);
    openFile_ =
        AddControl(WC_BUTTONW, L"Open File", WS_TABSTOP, IdOpenFile);
    close_ = AddControl(WC_BUTTONW, L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON,
                        IDCANCEL);
    status_ = AddControl(WC_STATICW, L"", SS_LEFT, IdStatus);
    SetLiveSetting(status_, 1);
    RefreshFont();
    Refresh();
    // Opening the editor runs a nested modal loop, which does not belong in
    // WM_CREATE; do it once creation has completed.
    if ((kind_ == library::ItemKind::AppAlias ||
         IsCommandItem(kind_) || IsLaunchItem(kind_)) &&
        !initialAppId_.empty()) {
      PostMessageW(hwnd_, kMsgOpenInitialEditor, 0, 0);
    }
  }

  void OpenInitialEditor() {
    if ((kind_ == library::ItemKind::AppAlias ||
         IsCommandItem(kind_) || IsLaunchItem(kind_)) &&
        !initialAppId_.empty()) {
      std::optional<std::size_t> sourceIndex;
      if (kind_ == library::ItemKind::AppAlias) {
        const auto alias = std::find_if(
            data_.appAliases.begin(), data_.appAliases.end(),
            [&](const auto& item) { return item.appId == initialAppId_; });
        if (alias != data_.appAliases.end()) {
          sourceIndex = static_cast<std::size_t>(
              std::distance(data_.appAliases.begin(), alias));
        }
      } else if (IsLaunchItem(kind_)) {
        const auto item = std::find_if(LaunchItems().begin(), LaunchItems().end(),
            [&](const auto& entry) { return entry.keyword == initialAppId_; });
        if (item != LaunchItems().end()) sourceIndex = static_cast<std::size_t>(std::distance(LaunchItems().begin(), item));
      } else {
        const auto alias = std::find_if(
            CommandItems().begin(), CommandItems().end(),
            [&](const auto& item) { return item.stableId == initialAppId_; });
        if (alias != CommandItems().end()) {
          sourceIndex = static_cast<std::size_t>(
              std::distance(CommandItems().begin(), alias));
        }
      }
      if (!sourceIndex) {
        AddItem();
      } else {
        if (SelectSourceIndex(*sourceIndex)) EditItem();
      }
      initialAppId_.clear();
    }
  }

  void Layout(int width, int height) const {
    const UINT dpi = std::max(kBaseDpi, DpiForWindow(hwnd_));
    const auto px = [dpi](int logicalPixels) {
      return ScaleForDpi(logicalPixels, dpi);
    };
    const int margin = px(14);
    const int tabHeight = px(58);
    const int buttonHeight = px(30);
    const int gap = px(8);
    const int contentWidth = std::max(1, width - 2 * margin);
    const int listTop = margin + tabHeight + px(6);
    const int closeWidth = std::min(px(90), std::max(px(72), contentWidth / 6));
    const int specialWidth = std::min(px(118), std::max(px(96), contentWidth / 5));
    const int closeX = std::max(margin, width - margin - closeWidth);
    const int specialX = std::max(margin, closeX - gap - specialWidth);
    const int actionSpace = std::max(1, specialX - margin - gap * 4);
    const int actionWidth = std::max(1, std::min(px(90), actionSpace / 4));
    const int buttonsTop = std::max(listTop, height - margin - buttonHeight);
    const int statusTop = std::max(listTop, buttonsTop - px(28));
    const int listHeight = std::max(1, statusTop - listTop - px(12));
    MoveWindow(tabs_, margin, margin, contentWidth, tabHeight, TRUE);
    MoveWindow(list_, margin, listTop, contentWidth, listHeight, TRUE);
    MoveWindow(status_, margin, statusTop, contentWidth, px(22), TRUE);
    const int listColumnWidth = std::max(1, contentWidth - px(4));
    const int nameColumn = listColumnWidth * 28 / 100;
    const int keywordColumn = listColumnWidth * 20 / 100;
    ListView_SetColumnWidth(list_, 0, std::max(1, nameColumn));
    ListView_SetColumnWidth(list_, 1, std::max(1, keywordColumn));
    ListView_SetColumnWidth(
        list_, 2, std::max(1, listColumnWidth - nameColumn - keywordColumn));
    int x = margin;
    for (HWND button : {add_, edit_, remove_, reload_}) {
      MoveWindow(button, x, buttonsTop, actionWidth, buttonHeight, TRUE);
      x += actionWidth + gap;
    }
    MoveWindow(openFile_, specialX, buttonsTop, specialWidth, buttonHeight,
               TRUE);
    MoveWindow(close_, closeX, buttonsTop, closeWidth, buttonHeight, TRUE);
  }

  std::vector<settings::Quicklink>& LaunchItems() {
    return kind_ == library::ItemKind::Script ? data_.scripts : kind_ == library::ItemKind::Workspace ? data_.workspaces : data_.quicklinks;
  }
  std::vector<library::CommandAlias>& CommandItems() {
    return kind_ == library::ItemKind::CommandShortcut ? data_.commandShortcuts : data_.commandAliases;
  }
  const auto& SaveLaunchItems() const {
    return kind_ == library::ItemKind::Script ? callbacks_.saveScripts : kind_ == library::ItemKind::Workspace ? callbacks_.saveWorkspaces : callbacks_.saveQuicklinks;
  }
  const auto& SaveCommandItems() const {
    return kind_ == library::ItemKind::CommandShortcut ? callbacks_.saveCommandShortcuts : callbacks_.saveCommandAliases;
  }

  bool Writable() const {
    if (kind_ == library::ItemKind::Snippet) return data_.snippetsWritable;
    if (IsLaunchItem(kind_)) return kind_ == library::ItemKind::Quicklink ? data_.quicklinksWritable : data_.settingsWritable;
    if (IsCommandItem(kind_)) {
      return data_.settingsWritable &&
             static_cast<bool>(SaveCommandItems());
    }
    return data_.settingsWritable;
  }

  std::optional<std::size_t> SelectedIndex() const {
    const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (selected < 0) return std::nullopt;
    LVITEMW item{LVIF_PARAM};
    item.iItem = selected;
    if (!ListView_GetItem(list_, &item) || item.lParam < 0) return std::nullopt;
    return static_cast<std::size_t>(item.lParam);
  }

  void InsertRow(int row, std::size_t sourceIndex, const std::wstring& name,
                 const std::wstring& keyword, const std::wstring& detail) {
    LVITEMW item{LVIF_TEXT | LVIF_PARAM};
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(name.c_str());
    item.lParam = static_cast<LPARAM>(sourceIndex);
    const int inserted = ListView_InsertItem(list_, &item);
    ListView_SetItemText(list_, inserted, 1,
                         const_cast<wchar_t*>(keyword.c_str()));
    ListView_SetItemText(list_, inserted, 2,
                         const_cast<wchar_t*>(detail.c_str()));
  }

  // Selects the row showing `sourceIndex` (an index into the underlying
  // vector, not a row: the list is sorted, so rows move after an edit).
  bool SelectSourceIndex(std::size_t sourceIndex) {
    const int count = ListView_GetItemCount(list_);
    for (int row = 0; row < count; ++row) {
      LVITEMW item{LVIF_PARAM};
      item.iItem = row;
      if (ListView_GetItem(list_, &item) &&
          item.lParam == static_cast<LPARAM>(sourceIndex)) {
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, row, FALSE);
        return true;
      }
    }
    return false;
  }

  // Rebuilds the list. `select`, when set, is the source index of the item to
  // leave selected afterwards (the edited or newly added one).
  void Refresh(std::optional<std::size_t> select = std::nullopt) {
    ListView_DeleteAllItems(list_);
    if (kind_ == library::ItemKind::Snippet) {
      int row = 0;
      for (const auto index : library::SortedSnippetIndices(data_.snippets)) {
        auto preview = data_.snippets[index].text;
        std::replace(preview.begin(), preview.end(), L'\n', L' ');
        std::replace(preview.begin(), preview.end(), L'\r', L' ');
        const auto keyword =
            library::NormalizeKeyword(data_.snippets[index].keyword);
        const auto duplicates = std::count_if(
            data_.snippets.begin(), data_.snippets.end(), [&](const auto& item) {
              return library::NormalizeKeyword(item.keyword) == keyword;
            });
        if (duplicates > 1) preview = L"[Duplicate keyword] " + preview;
        InsertRow(row++, index, data_.snippets[index].name,
                  data_.snippets[index].keyword, preview);
      }
    } else if (IsLaunchItem(kind_)) {
      int row = 0;
      for (const auto index :
           library::SortedQuicklinkIndices(LaunchItems())) {
        const auto& link = LaunchItems()[index];
        std::wstring detail = link.target;
        std::replace(detail.begin(), detail.end(), L'\n', L' ');
        std::replace(detail.begin(), detail.end(), L'\r', L' ');
        const auto keyword = library::NormalizeKeyword(link.keyword);
        const auto duplicates = std::count_if(
            LaunchItems().begin(), LaunchItems().end(),
            [&](const auto& item) {
              return library::NormalizeKeyword(item.keyword) == keyword;
            });
        if (duplicates > 1) detail = L"[Duplicate keyword] " + detail;
        InsertRow(row++, index, link.name.empty() ? link.keyword : link.name,
                  link.keyword, detail);
      }
    } else if (kind_ == library::ItemKind::AppAlias) {
      int row = 0;
      for (const auto index :
           library::SortedAppAliasIndices(data_.appAliases)) {
        const auto& alias = data_.appAliases[index];
        InsertRow(row++, index,
                  alias.appName.empty() ? L"Missing app" : alias.appName,
                  alias.alias, alias.appId);
      }
    } else if (IsCommandItem(kind_)) {
      int row = 0;
      for (const auto index :
           library::SortedCommandAliasIndices(CommandItems())) {
        const auto& alias = CommandItems()[index];
        InsertRow(row++, index,
                  alias.commandName.empty() ? L"Missing command"
                                            : alias.commandName,
                  alias.alias, alias.stableId);
      }
    } else {
      int row = 0;
      for (const auto index :
           library::SortedWebSearchIndices(data_.webSearches)) {
        const auto& search = data_.webSearches[index];
        InsertRow(row++, index, search.keyword, search.keyword,
                  search.urlTemplate);
      }
    }
    if (select) SelectSourceIndex(*select);
    const auto selected = SelectedIndex();
    const bool canAdd = Writable() &&
                        (!IsCommandItem(kind_) ||
                         CommandItems().size() < commands::Catalog().size());
    EnableWindow(add_, canAdd);
    EnableWindow(edit_, Writable() && selected.has_value());
    EnableWindow(remove_, Writable() && selected.has_value());
    EnableWindow(openFile_, kind_ == library::ItemKind::Snippet ||
                                (kind_ == library::ItemKind::WebSearch &&
                                 Writable()));
    SetWindowTextW(openFile_, kind_ == library::ItemKind::WebSearch
                                  ? L"Restore Defaults" : L"Open File");
    const std::wstring message = kind_ == library::ItemKind::Snippet
        ? data_.snippetsMessage
        : (IsLaunchItem(kind_) ? data_.quicklinksMessage
                                                  : data_.settingsMessage);
    const bool integrationMissing =
        IsCommandItem(kind_) &&
        !SaveCommandItems();
    if (operationStatus_.empty()) {
      operationStatus_ =
          integrationMissing
              ? L"Command alias persistence is not connected."
              : message.empty()
                    ? (Writable() ? L"Changes are saved immediately."
                                  : L"Editing is unavailable.")
                    : message;
      operationStatusError_ = !integrationMissing && !message.empty() &&
                              !Writable();
    }
    SetWindowTextW(status_, operationStatus_.c_str());
  }

  void ShowResult(const library::OperationResult& result) {
    SetStatus(result.succeeded
                  ? L"Changes saved."
                  : (result.message.empty() ? L"Could not save changes."
                                             : result.message),
              !result.succeeded);
  }

  // Saves `candidate`. On success it becomes the stored list; on failure the
  // stored data is reloaded so the view matches what is on disk.
  template <typename Item, typename Save>
  bool Commit(std::vector<Item>& stored, std::vector<Item> candidate,
              const Save& save) {
    const auto result = save(candidate);
    ShowResult(result);
    if (result.succeeded) {
      stored = std::move(candidate);
    } else if (callbacks_.reload) {
      data_ = callbacks_.reload();
    }
    return result.succeeded;
  }

  void AddItem() {
    if (!Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, LaunchItems(),
                        data_.appAliases, CommandItems(),
                        data_.availableApps,
                        data_.webSearches, std::nullopt, initialAppId_, theme_,
                        highContrast_);
    if (!editor.Run()) return;
    bool saved = false;
    std::size_t added = 0;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.push_back(editor.Snippet());
      added = candidate.size() - 1;
      saved = Commit(data_.snippets, std::move(candidate),
                     callbacks_.saveSnippets);
    } else if (IsLaunchItem(kind_)) {
      auto candidate = LaunchItems();
      candidate.push_back(editor.Quicklink());
      added = candidate.size() - 1;
      saved = Commit(LaunchItems(), std::move(candidate),
                     SaveLaunchItems());
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.push_back(editor.Alias());
      added = candidate.size() - 1;
      saved = Commit(data_.appAliases, std::move(candidate),
                     callbacks_.saveAppAliases);
    } else if (IsCommandItem(kind_)) {
      auto candidate = CommandItems();
      candidate.push_back(editor.CommandAlias());
      added = candidate.size() - 1;
      saved = Commit(CommandItems(), std::move(candidate),
                     SaveCommandItems());
    } else {
      auto candidate = data_.webSearches;
      candidate.push_back(editor.WebSearch());
      added = candidate.size() - 1;
      saved = Commit(data_.webSearches, std::move(candidate),
                     callbacks_.saveWebSearches);
    }
    Refresh(saved ? std::optional<std::size_t>(added) : std::nullopt);
  }

  void EditItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, LaunchItems(),
                        data_.appAliases, CommandItems(),
                        data_.availableApps,
                        data_.webSearches, selected, {}, theme_, highContrast_);
    if (!editor.Run()) return;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate[*selected] = editor.Snippet();
      Commit(data_.snippets, std::move(candidate), callbacks_.saveSnippets);
    } else if (IsLaunchItem(kind_)) {
      auto candidate = LaunchItems();
      candidate[*selected] = editor.Quicklink();
      Commit(LaunchItems(), std::move(candidate),
             SaveLaunchItems());
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate[*selected] = editor.Alias();
      Commit(data_.appAliases, std::move(candidate),
             callbacks_.saveAppAliases);
    } else if (IsCommandItem(kind_)) {
      auto candidate = CommandItems();
      candidate[*selected] = editor.CommandAlias();
      Commit(CommandItems(), std::move(candidate),
             SaveCommandItems());
    } else {
      auto candidate = data_.webSearches;
      candidate[*selected] = editor.WebSearch();
      Commit(data_.webSearches, std::move(candidate),
             callbacks_.saveWebSearches);
    }
    // The list is sorted, so the edited row can move; keep the selection on
    // the edited item itself.
    Refresh(*selected);
  }

  void DeleteItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    if (MessageBoxW(hwnd_, L"Delete the selected library item?",
                    L"FeatherCast Library",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
      return;
    }
    const auto position = static_cast<std::ptrdiff_t>(*selected);
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.erase(candidate.begin() + position);
      Commit(data_.snippets, std::move(candidate), callbacks_.saveSnippets);
    } else if (IsLaunchItem(kind_)) {
      auto candidate = LaunchItems();
      candidate.erase(candidate.begin() + position);
      Commit(LaunchItems(), std::move(candidate),
             SaveLaunchItems());
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.erase(candidate.begin() + position);
      Commit(data_.appAliases, std::move(candidate),
             callbacks_.saveAppAliases);
    } else if (IsCommandItem(kind_)) {
      auto candidate = CommandItems();
      candidate.erase(candidate.begin() + position);
      Commit(CommandItems(), std::move(candidate),
             SaveCommandItems());
    } else {
      auto candidate = data_.webSearches;
      candidate.erase(candidate.begin() + position);
      Commit(data_.webSearches, std::move(candidate),
             callbacks_.saveWebSearches);
    }
    Refresh();
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        RefreshBrushes();
        CreateControls();
        return 0;
      case kMsgOpenInitialEditor:
        OpenInitialEditor();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        PaintBackground();
        return 0;
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORBTN:
        return ColorControl(message, wParam, lParam);
      case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
      case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits) {
          const UINT dpi = std::max(kBaseDpi, DpiForWindow(hwnd_));
          const RECT work = WorkAreaFor(hwnd_);
          const int inset = ScaleForDpi(kWorkAreaInset, dpi);
          const int maxWidth = std::max(
              1, static_cast<int>(work.right - work.left) - inset * 2);
          const int maxHeight = std::max(
              1, static_cast<int>(work.bottom - work.top) - inset * 2);
          limits->ptMinTrackSize.x = std::min(ScaleForDpi(520, dpi), maxWidth);
          limits->ptMinTrackSize.y = std::min(ScaleForDpi(360, dpi), maxHeight);
        }
        return 0;
      }
      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
          SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ApplyLibraryChrome(hwnd_, theme_, highContrast_);
        RefreshFont();
        RefreshBrushes();
        LayoutCurrentClient();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
      }
      case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IdList && header->code == NM_CUSTOMDRAW) {
          auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
          if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
          }
          if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
            draw->clrText = highContrast_ ? GetSysColor(COLOR_WINDOWTEXT)
                                          : ColorRefFromTheme(theme_.textPrimary);
            draw->clrTextBk = selected
                                  ? (highContrast_ ? GetSysColor(COLOR_HIGHLIGHT)
                                                   : ColorRefFromTheme(theme_.selectedBase))
                                  : (highContrast_ ? GetSysColor(COLOR_BTNFACE)
                                                   : ColorRefFromTheme(theme_.surface));
            return CDRF_NEWFONT;
          }
        }
        if (header->idFrom == IdTabs && header->code == TCN_SELCHANGE) {
          kind_ = static_cast<library::ItemKind>(TabCtrl_GetCurSel(tabs_));
          initialAppId_.clear();
          operationStatus_.clear();
          operationStatusError_ = false;
          Refresh();
          return 0;
        }
        if (header->idFrom == IdList && header->code == NM_DBLCLK) {
          EditItem();
          return 0;
        }
        if (header->idFrom == IdList && header->code == LVN_ITEMCHANGED) {
          const bool selected = SelectedIndex().has_value();
          EnableWindow(edit_, Writable() && selected);
          EnableWindow(remove_, Writable() && selected);
        }
        break;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IdAdd: AddItem(); return 0;
          case IdEdit: EditItem(); return 0;
          case IdDelete: DeleteItem(); return 0;
          case IdReload:
            if (callbacks_.reload) data_ = callbacks_.reload();
            operationStatus_.clear();
            operationStatusError_ = false;
            Refresh();
            return 0;
          case IdOpenFile:
            if (kind_ == library::ItemKind::WebSearch &&
                callbacks_.restoreDefaultWebSearches) {
              // Replaces every web search, including custom ones.
              if (MessageBoxW(
                      hwnd_,
                      L"Replace all web searches with the defaults? Custom "
                      L"web searches will be lost.",
                      L"FeatherCast Library",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
              }
              const auto result = callbacks_.restoreDefaultWebSearches();
              ShowResult(result);
              if (callbacks_.reload) data_ = callbacks_.reload();
              Refresh();
            } else if (callbacks_.openSnippetsFile) {
              callbacks_.openSnippetsFile();
            }
            return 0;
          case IDCANCEL:
            Close();
            return 0;
        }
        break;
      case WM_CLOSE:
        Close();
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
  }

  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  ManagerData data_;
  ManagerCallbacks callbacks_;
  library::ItemKind kind_ = library::ItemKind::Snippet;
  std::wstring initialAppId_;
  theme::Theme theme_;
  bool highContrast_ = false;
  std::wstring operationStatus_;
  bool operationStatusError_ = false;
  HWND tabs_ = nullptr;
  HWND list_ = nullptr;
  HWND add_ = nullptr;
  HWND edit_ = nullptr;
  HWND remove_ = nullptr;
  HWND reload_ = nullptr;
  HWND openFile_ = nullptr;
  HWND close_ = nullptr;
  HWND status_ = nullptr;
  HBRUSH backgroundBrush_ = nullptr;
  HBRUSH surfaceBrush_ = nullptr;
  HBRUSH selectedBrush_ = nullptr;
};

}  // namespace

void ShowLibraryManager(HWND owner, ManagerData data,
                        ManagerCallbacks callbacks,
                        library::ItemKind initialKind,
                        std::wstring initialAppId, theme::Theme theme,
                        bool highContrast) {
  ManagerWindow(owner, std::move(data), std::move(callbacks), initialKind,
                std::move(initialAppId), std::move(theme), highContrast)
      .Run();
}

}  // namespace feathercast::library_ui
