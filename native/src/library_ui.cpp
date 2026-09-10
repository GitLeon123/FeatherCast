#include "library_ui.hpp"

#include "command_catalog.hpp"

#include <commctrl.h>
#include <initguid.h>
#include <oleacc.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <optional>
#include <string>

namespace feathercast::library_ui {
namespace {

constexpr wchar_t kManagerClass[] = L"FeatherCastLibraryManager";
constexpr wchar_t kEditorClass[] = L"FeatherCastLibraryEditor";

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

constexpr UINT kBaseDpi = 96;
constexpr int kWorkAreaInset = 24;

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

HFONT CreateDialogFont(HWND window) {
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                             &metrics, 0)) {
    return nullptr;
  }
  LOGFONTW font = metrics.lfMessageFont;
  const UINT systemDpi = SystemDpi();
  const UINT targetDpi = std::max(kBaseDpi, DpiForWindow(window));
  font.lfHeight = MulDiv(font.lfHeight, static_cast<int>(targetDpi),
                         static_cast<int>(std::max(kBaseDpi, systemDpi)));
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
               std::wstring preferredAppId = {})
      : owner_(owner),
        kind_(kind),
        snippets_(snippets),
        quicklinks_(quicklinks),
        aliases_(aliases),
        commandAliases_(commandAliases),
        availableApps_(availableApps),
        searches_(searches),
        editingIndex_(editingIndex),
        preferredAppId_(std::move(preferredAppId)) {
    if (editingIndex_) {
      if (kind_ == library::ItemKind::Snippet) {
        snippet_ = snippets_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::Quicklink) {
        quicklink_ = quicklinks_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::AppAlias) {
        alias_ = aliases_.at(*editingIndex_);
      } else if (kind_ == library::ItemKind::CommandAlias) {
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
  }

  bool Run() {
    Register();
    const wchar_t* title = L"Edit Library Item";
    if (kind_ == library::ItemKind::Snippet) {
      title = editingIndex_ ? L"Edit Snippet" : L"Add Snippet";
    } else if (kind_ == library::ItemKind::Quicklink) {
      title = editingIndex_ ? L"Edit Quicklink" : L"Add Quicklink";
    } else if (kind_ == library::ItemKind::AppAlias) {
      title = editingIndex_ ? L"Edit App Alias" : L"Add App Alias";
    } else if (kind_ == library::ItemKind::CommandAlias) {
      title = editingIndex_ ? L"Edit Command Alias" : L"Add Command Alias";
    } else {
      title = editingIndex_ ? L"Edit Web Search" : L"Add Web Search";
    }
    const SIZE windowSize = FitWindowSize(
        owner_, 570, kind_ == library::ItemKind::Snippet ? 430 : 285);
    hwnd_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kEditorClass, title,
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
        windowSize.cx, windowSize.cy, owner_, nullptr, GetModuleHandleW(nullptr),
        this);
    if (!hwnd_) return false;
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG message{};
    while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
        continue;
      }
      if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
          (kind_ != library::ItemKind::Snippet || GetFocus() != value_ ||
           (GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
        SendMessageW(hwnd_, WM_COMMAND, IDOK, 0);
        continue;
      }
      if (!IsDialogMessageW(hwnd_, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
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
      wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
    HFONT next = CreateDialogFont(hwnd_);
    if (!next) return;
    const HFONT previous = font_;
    font_ = next;
    for (HWND control : {nameLabel_, name_, keywordLabel_, keyword_, valueLabel_,
                         value_, save_, cancel_}) {
      UseDefaultFont(control, font_);
    }
    if (previous) DeleteObject(previous);
  }

  void LayoutCurrentClient() const {
    RECT client{};
    if (GetClientRect(hwnd_, &client)) Layout(client.right, client.bottom);
  }

  void CreateControls() {
    const bool appAlias = kind_ == library::ItemKind::AppAlias;
    const bool commandAlias = kind_ == library::ItemKind::CommandAlias;
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
    keywordLabel_ = AddControl(WC_STATICW, alias ? L"Alias" : L"Keyword",
                               SS_LEFT, 0);
    keyword_ =
        AddControl(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IdKeyword);
    valueLabel_ = AddControl(
        WC_STATICW,
        kind_ == library::ItemKind::Snippet ? L"Text" :
        (webSearch ? L"URL template (use %s for the query)" : L"Target"),
        SS_LEFT, 0);
    DWORD valueStyle = WS_TABSTOP;
    if (kind_ == library::ItemKind::Snippet) {
      valueStyle |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
    } else {
      valueStyle |= ES_AUTOHSCROLL;
    }
    value_ = AddControl(WC_EDITW, L"", valueStyle, IdValue);
    save_ = AddControl(WC_BUTTONW, L"Save",
                       WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = AddControl(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
    const wchar_t* accessibleName = appAlias
        ? L"App"
        : (commandAlias
               ? L"Command"
               : (kind_ == library::ItemKind::Snippet
                      ? L"Snippet name"
                      : L"Quicklink name (optional)"));
    SetAccessibleName(name_, accessibleName);
    SetAccessibleName(keyword_, appAlias ? L"App alias" :
                                  (commandAlias ? L"Command alias"
                                                : L"Keyword"));
    SetAccessibleName(value_, webSearch ? L"Web search URL template" :
        (kind_ == library::ItemKind::Snippet ? L"Snippet text"
                                             : L"Quicklink target"));

    if (kind_ == library::ItemKind::Snippet) {
      SetWindowTextW(name_, snippet_.name.c_str());
      SetWindowTextW(keyword_, snippet_.keyword.c_str());
      SetWindowTextW(value_, snippet_.text.c_str());
    } else if (kind_ == library::ItemKind::Quicklink) {
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
    const int contentWidth = std::max(1, width - 2 * margin);
    int y = margin;
    if (kind_ != library::ItemKind::WebSearch) {
      MoveWindow(nameLabel_, margin, y, contentWidth, labelHeight, TRUE);
      y += labelHeight;
      MoveWindow(name_, margin, y, contentWidth,
                 (kind_ == library::ItemKind::AppAlias ||
                  kind_ == library::ItemKind::CommandAlias)
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
        kind_ == library::ItemKind::CommandAlias) {
      const int buttonWidth = px(90);
      const int buttonHeight = px(30);
      const int buttonsTop = std::max(y, height - margin - buttonHeight);
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
    MoveWindow(value_, margin, y, contentWidth,
               std::max(editHeight, buttonsTop - y - gap), TRUE);
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
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Snippet",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else if (kind_ == library::ItemKind::Quicklink) {
      quicklink_.name = snippets::Trim(ControlText(name_));
      quicklink_.keyword = snippets::Trim(ControlText(keyword_));
      quicklink_.target = snippets::Trim(ControlText(value_));
      if (const auto error = library::ValidateQuicklink(
              quicklink_, quicklinks_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Quicklink",
                    MB_OK | MB_ICONWARNING);
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
        MessageBoxW(hwnd_, error->c_str(), L"Invalid App Alias",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else if (kind_ == library::ItemKind::CommandAlias) {
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
      if (const auto error = library::ValidateCommandAlias(
              commandAlias_, commandAliases_, aliases_, snippets_,
              quicklinks_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Command Alias",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else {
      webSearch_.keyword = library::NormalizeKeyword(ControlText(keyword_));
      webSearch_.urlTemplate = snippets::Trim(ControlText(value_));
      if (const auto error = library::ValidateWebSearch(
              webSearch_, searches_, editingIndex_)) {
        MessageBoxW(hwnd_, error->c_str(), L"Invalid Web Search",
                    MB_OK | MB_ICONWARNING);
        return;
      }
    }
    accepted_ = true;
    DestroyWindow(hwnd_);
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        RefreshFont();
        return 0;
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
        RefreshFont();
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
          DestroyWindow(hwnd_);
          return 0;
        }
        break;
      case WM_CLOSE:
        DestroyWindow(hwnd_);
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
  bool accepted_ = false;
  HWND nameLabel_ = nullptr;
  HWND name_ = nullptr;
  HWND keywordLabel_ = nullptr;
  HWND keyword_ = nullptr;
  HWND valueLabel_ = nullptr;
  HWND value_ = nullptr;
  HWND save_ = nullptr;
  HWND cancel_ = nullptr;
};

class ManagerWindow {
 public:
  ManagerWindow(HWND owner, ManagerData data, ManagerCallbacks callbacks,
                library::ItemKind initialKind, std::wstring initialAppId)
      : owner_(owner),
        data_(std::move(data)),
        callbacks_(std::move(callbacks)),
        kind_(initialKind),
        initialAppId_(std::move(initialAppId)) {}

  ~ManagerWindow() {
    if (font_) DeleteObject(font_);
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
    CenterOwnedWindow(hwnd_, owner_);
    EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG message{};
    while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        SendMessageW(hwnd_, WM_COMMAND, IDCANCEL, 0);
        continue;
      }
      if (!IsDialogMessageW(hwnd_, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
  }

 private:
  static void Register() {
    static const bool registered = [] {
      WNDCLASSEXW wc{sizeof(wc)};
      wc.lpfnWndProc = &ManagerWindow::WindowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
    HFONT next = CreateDialogFont(hwnd_);
    if (!next) return;
    const HFONT previous = font_;
    font_ = next;
    for (HWND control : {tabs_, list_, add_, edit_, remove_, reload_, openFile_,
                         close_, status_}) {
      UseDefaultFont(control, font_);
    }
    if (previous) DeleteObject(previous);
  }

  void LayoutCurrentClient() const {
    RECT client{};
    if (GetClientRect(hwnd_, &client)) Layout(client.right, client.bottom);
  }

  void CreateControls() {
    tabs_ = AddControl(WC_TABCONTROLW, L"", WS_TABSTOP, IdTabs);
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
    TabCtrl_SetCurSel(tabs_, static_cast<int>(kind_));

    list_ = AddControl(WC_LISTVIEWW, L"",
                       WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                           LVS_SHOWSELALWAYS,
                       IdList);
    SetAccessibleName(list_, L"Library items");
    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
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
    RefreshFont();
    Refresh();
    if ((kind_ == library::ItemKind::AppAlias ||
         kind_ == library::ItemKind::CommandAlias) &&
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
      } else {
        const auto alias = std::find_if(
            data_.commandAliases.begin(), data_.commandAliases.end(),
            [&](const auto& item) { return item.stableId == initialAppId_; });
        if (alias != data_.commandAliases.end()) {
          sourceIndex = static_cast<std::size_t>(
              std::distance(data_.commandAliases.begin(), alias));
        }
      }
      if (!sourceIndex) {
        AddItem();
      } else {
        for (int row = 0; row < ListView_GetItemCount(list_); ++row) {
          LVITEMW item{LVIF_PARAM};
          item.iItem = row;
          if (ListView_GetItem(list_, &item) &&
              item.lParam == static_cast<LPARAM>(*sourceIndex)) {
            ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            EditItem();
            break;
          }
        }
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
    const int tabHeight = px(30);
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

  bool Writable() const {
    if (kind_ == library::ItemKind::Snippet) return data_.snippetsWritable;
    if (kind_ == library::ItemKind::Quicklink) return data_.quicklinksWritable;
    if (kind_ == library::ItemKind::CommandAlias) {
      return data_.settingsWritable &&
             static_cast<bool>(callbacks_.saveCommandAliases);
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

  void Refresh() {
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
    } else if (kind_ == library::ItemKind::Quicklink) {
      int row = 0;
      for (const auto index :
           library::SortedQuicklinkIndices(data_.quicklinks)) {
        const auto& link = data_.quicklinks[index];
        std::wstring detail = link.target;
        const auto keyword = library::NormalizeKeyword(link.keyword);
        const auto duplicates = std::count_if(
            data_.quicklinks.begin(), data_.quicklinks.end(),
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
    } else if (kind_ == library::ItemKind::CommandAlias) {
      int row = 0;
      for (const auto index :
           library::SortedCommandAliasIndices(data_.commandAliases)) {
        const auto& alias = data_.commandAliases[index];
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
    const auto selected = SelectedIndex();
    const bool canAdd = Writable() &&
                        (kind_ != library::ItemKind::CommandAlias ||
                         data_.commandAliases.size() < commands::Catalog().size());
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
        : (kind_ == library::ItemKind::Quicklink ? data_.quicklinksMessage
                                                  : data_.settingsMessage);
    const bool integrationMissing =
        kind_ == library::ItemKind::CommandAlias &&
        !callbacks_.saveCommandAliases;
    SetWindowTextW(status_,
                   integrationMissing
                       ? L"Command alias persistence is not connected."
                       : message.empty()
                       ? (Writable() ? L"Changes are saved immediately."
                                     : L"Editing is unavailable.")
                       : message.c_str());
  }

  void ShowResult(const library::OperationResult& result) {
    if (!result.succeeded) {
      MessageBoxW(hwnd_, result.message.c_str(), L"FeatherCast Library",
                  MB_OK | MB_ICONWARNING);
    }
  }

  void AddItem() {
    if (!Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, data_.quicklinks,
                        data_.appAliases, data_.commandAliases,
                        data_.availableApps,
                        data_.webSearches, std::nullopt, initialAppId_);
    if (!editor.Run()) return;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.push_back(editor.Snippet());
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate.push_back(editor.Quicklink());
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.push_back(editor.Alias());
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::CommandAlias) {
      auto candidate = data_.commandAliases;
      candidate.push_back(editor.CommandAlias());
      const auto result = callbacks_.saveCommandAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.commandAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate.push_back(editor.WebSearch());
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  void EditItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    EditorWindow editor(hwnd_, kind_, data_.snippets, data_.quicklinks,
                        data_.appAliases, data_.commandAliases,
                        data_.availableApps,
                        data_.webSearches, selected);
    if (!editor.Run()) return;
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate[*selected] = editor.Snippet();
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate[*selected] = editor.Quicklink();
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate[*selected] = editor.Alias();
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::CommandAlias) {
      auto candidate = data_.commandAliases;
      candidate[*selected] = editor.CommandAlias();
      const auto result = callbacks_.saveCommandAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.commandAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate[*selected] = editor.WebSearch();
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  void DeleteItem() {
    const auto selected = SelectedIndex();
    if (!selected || !Writable()) return;
    if (MessageBoxW(hwnd_, L"Delete the selected library item?",
                    L"FeatherCast Library",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
      return;
    }
    if (kind_ == library::ItemKind::Snippet) {
      auto candidate = data_.snippets;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveSnippets(candidate);
      ShowResult(result);
      if (result.succeeded) data_.snippets = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::Quicklink) {
      auto candidate = data_.quicklinks;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveQuicklinks(candidate);
      ShowResult(result);
      if (result.succeeded) data_.quicklinks = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::AppAlias) {
      auto candidate = data_.appAliases;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveAppAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.appAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else if (kind_ == library::ItemKind::CommandAlias) {
      auto candidate = data_.commandAliases;
      candidate.erase(candidate.begin() +
                      static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveCommandAliases(candidate);
      ShowResult(result);
      if (result.succeeded) data_.commandAliases = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    } else {
      auto candidate = data_.webSearches;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*selected));
      const auto result = callbacks_.saveWebSearches(candidate);
      ShowResult(result);
      if (result.succeeded) data_.webSearches = std::move(candidate);
      else if (callbacks_.reload) data_ = callbacks_.reload();
    }
    Refresh();
  }

  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
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
        RefreshFont();
        LayoutCurrentClient();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
      }
      case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IdTabs && header->code == TCN_SELCHANGE) {
          kind_ = static_cast<library::ItemKind>(TabCtrl_GetCurSel(tabs_));
          initialAppId_.clear();
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
            Refresh();
            return 0;
          case IdOpenFile:
            if (kind_ == library::ItemKind::WebSearch &&
                callbacks_.restoreDefaultWebSearches) {
              const auto result = callbacks_.restoreDefaultWebSearches();
              ShowResult(result);
              if (callbacks_.reload) data_ = callbacks_.reload();
              Refresh();
            } else if (callbacks_.openSnippetsFile) {
              callbacks_.openSnippetsFile();
            }
            return 0;
          case IDCANCEL:
            DestroyWindow(hwnd_);
            return 0;
        }
        break;
      case WM_CLOSE:
        DestroyWindow(hwnd_);
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
  HWND tabs_ = nullptr;
  HWND list_ = nullptr;
  HWND add_ = nullptr;
  HWND edit_ = nullptr;
  HWND remove_ = nullptr;
  HWND reload_ = nullptr;
  HWND openFile_ = nullptr;
  HWND close_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace

void ShowLibraryManager(HWND owner, ManagerData data,
                        ManagerCallbacks callbacks,
                        library::ItemKind initialKind,
                        std::wstring initialAppId) {
  ManagerWindow(owner, std::move(data), std::move(callbacks), initialKind,
                std::move(initialAppId)).Run();
}

}  // namespace feathercast::library_ui
