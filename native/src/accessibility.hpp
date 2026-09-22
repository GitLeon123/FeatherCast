#pragma once

#include <windows.h>
#include <oleacc.h>
#include <servprov.h>
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace feathercast::accessibility {

struct Item {
  std::wstring name;
  std::wstring value;
  std::wstring description;
  std::wstring defaultAction;
  std::wstring keyboardShortcut;
  std::wstring key;
  std::optional<double> rangeValue;
  double rangeMinimum = 0.0;
  double rangeMaximum = 100.0;
  double rangeSmallChange = 1.0;
  double rangeLargeChange = 10.0;
  LONG role = ROLE_SYSTEM_LISTITEM;
  LONG state = STATE_SYSTEM_FOCUSABLE;
  RECT screenRect{};
};

class Model {
 public:
  virtual ~Model() = default;
  virtual std::wstring AccessibleWindowName(HWND hwnd) const = 0;
  virtual std::vector<Item> AccessibleItems(HWND hwnd) const = 0;
  virtual int AccessibleFocusedChild(HWND hwnd) const = 0;  // one-based; 0 = root
  virtual int AccessibleSelectedChild(HWND hwnd) const {
    return AccessibleFocusedChild(hwnd);
  }
  virtual void AccessibleFocusChild(HWND hwnd, int child) = 0;
  virtual void AccessibleInvokeChild(HWND hwnd, int child) = 0;
  virtual HRESULT AccessibleSetValue(HWND, int, const std::wstring&) {
    return E_NOTIMPL;
  }
  virtual HRESULT AccessibleSetRangeValue(HWND, int, double) {
    return E_NOTIMPL;
  }
};

namespace detail {

inline LONG UiaControlTypeForRole(LONG role) noexcept {
  switch (role) {
    case ROLE_SYSTEM_PUSHBUTTON: return UIA_ButtonControlTypeId;
    case ROLE_SYSTEM_CHECKBUTTON: return UIA_CheckBoxControlTypeId;
    case ROLE_SYSTEM_RADIOBUTTON: return UIA_RadioButtonControlTypeId;
    case ROLE_SYSTEM_TEXT: return UIA_EditControlTypeId;
    case ROLE_SYSTEM_LISTITEM: return UIA_ListItemControlTypeId;
    case ROLE_SYSTEM_PAGETAB: return UIA_TabItemControlTypeId;
    case ROLE_SYSTEM_SLIDER: return UIA_SliderControlTypeId;
    case ROLE_SYSTEM_ALERT:
    case ROLE_SYSTEM_STATICTEXT: return UIA_TextControlTypeId;
    default: return UIA_CustomControlTypeId;
  }
}

inline HRESULT SetVariantString(const std::wstring& value, VARIANT* result) {
  result->vt = VT_BSTR;
  result->bstrVal = SysAllocString(value.c_str());
  return result->bstrVal ? S_OK : E_OUTOFMEMORY;
}

class ChildProvider final : public IServiceProvider,
                            public IAccessibleEx,
                            public IRawElementProviderSimple,
                            public IInvokeProvider,
                            public IValueProvider,
                            public IRangeValueProvider,
                            public IToggleProvider,
                            public ISelectionItemProvider {
 public:
  ChildProvider(Model* model, HWND hwnd, int child, IAccessible* parent)
      : model_(model), hwnd_(hwnd), child_(child), parent_(parent) {
    if (parent_) parent_->AddRef();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IAccessibleEx) {
      *object = static_cast<IAccessibleEx*>(this);
    } else if (iid == IID_IServiceProvider) {
      *object = static_cast<IServiceProvider*>(this);
    } else if (iid == IID_IRawElementProviderSimple) {
      *object = static_cast<IRawElementProviderSimple*>(this);
    } else if (iid == IID_IInvokeProvider) {
      *object = static_cast<IInvokeProvider*>(this);
    } else if (iid == IID_IValueProvider) {
      *object = static_cast<IValueProvider*>(this);
    } else if (iid == IID_IRangeValueProvider) {
      *object = static_cast<IRangeValueProvider*>(this);
    } else if (iid == IID_IToggleProvider) {
      *object = static_cast<IToggleProvider*>(this);
    } else if (iid == IID_ISelectionItemProvider) {
      *object = static_cast<ISelectionItemProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid,
                                         void** object) override {
    if (service != IID_IAccessibleEx) return E_NOINTERFACE;
    return QueryInterface(iid, object);
  }

  HRESULT STDMETHODCALLTYPE GetObjectForChild(long, IAccessibleEx** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE GetIAccessiblePair(IAccessible** accessible,
                                                long* child) override {
    if (!accessible || !child) return E_POINTER;
    *accessible = parent_;
    *child = child_;
    if (parent_) parent_->AddRef();
    return parent_ ? S_OK : E_FAIL;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override {
    if (!runtimeId) return E_POINTER;
    *runtimeId = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ConvertReturnedElement(
      IRawElementProviderSimple*, IAccessibleEx** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(
      ProviderOptions* options) override {
    if (!options) return E_POINTER;
    *options = ProviderOptions_ServerSideProvider;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,
                                                IUnknown** provider) override {
    if (!provider) return E_POINTER;
    *provider = nullptr;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (pattern == UIA_ValuePatternId && item->role == ROLE_SYSTEM_TEXT) {
      return QueryInterface(IID_IValueProvider,
                            reinterpret_cast<void**>(provider));
    }
    if (pattern == UIA_RangeValuePatternId && item->rangeValue) {
      return QueryInterface(IID_IRangeValueProvider,
                            reinterpret_cast<void**>(provider));
    }
    if (pattern == UIA_TogglePatternId &&
        item->role == ROLE_SYSTEM_CHECKBUTTON) {
      return QueryInterface(IID_IToggleProvider,
                            reinterpret_cast<void**>(provider));
    }
    if (pattern == UIA_SelectionItemPatternId &&
        (item->role == ROLE_SYSTEM_LISTITEM ||
         item->role == ROLE_SYSTEM_PAGETAB ||
         item->role == ROLE_SYSTEM_RADIOBUTTON)) {
      return QueryInterface(IID_ISelectionItemProvider,
                            reinterpret_cast<void**>(provider));
    }
    if (pattern == UIA_InvokePatternId && !item->defaultAction.empty()) {
      return QueryInterface(IID_IInvokeProvider,
                            reinterpret_cast<void**>(provider));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                              VARIANT* result) override {
    if (!result) return E_POINTER;
    VariantInit(result);
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    switch (property) {
      case UIA_NamePropertyId:
        return SetVariantString(item->name, result);
      case UIA_AutomationIdPropertyId:
        return SetVariantString(item->key, result);
      case UIA_HelpTextPropertyId:
        return SetVariantString(item->description, result);
      case UIA_AcceleratorKeyPropertyId:
        return SetVariantString(item->keyboardShortcut, result);
      case UIA_ControlTypePropertyId:
        result->vt = VT_I4;
        result->lVal = UiaControlTypeForRole(item->role);
        return S_OK;
      case UIA_IsEnabledPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = (item->state & STATE_SYSTEM_UNAVAILABLE) == 0
                              ? VARIANT_TRUE
                              : VARIANT_FALSE;
        return S_OK;
      case UIA_IsKeyboardFocusablePropertyId:
        result->vt = VT_BOOL;
        result->boolVal = (item->state & STATE_SYSTEM_FOCUSABLE) != 0
                              ? VARIANT_TRUE
                              : VARIANT_FALSE;
        return S_OK;
      case UIA_HasKeyboardFocusPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = (item->state & STATE_SYSTEM_FOCUSED) != 0
                              ? VARIANT_TRUE
                              : VARIANT_FALSE;
        return S_OK;
      case UIA_IsOffscreenPropertyId:
        result->vt = VT_BOOL;
        result->boolVal =
            (item->state & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) !=
                    0
                ? VARIANT_TRUE
                : VARIANT_FALSE;
        return S_OK;
      default: return S_OK;
    }
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
      IRawElementProviderSimple** provider) override {
    if (!provider) return E_POINTER;
    *provider = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Invoke() override {
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0)
      return E_ACCESSDENIED;
    if (item->defaultAction.empty()) return UIA_E_NOTSUPPORTED;
    model_->AccessibleInvokeChild(hwnd_, child_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_READONLY)) != 0)
      return E_ACCESSDENIED;
    return model_->AccessibleSetValue(hwnd_, child_, value ? value : L"");
  }

  HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *value = SysAllocString(item->value.c_str());
    return *value ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* readOnly) override {
    if (!readOnly) return E_POINTER;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *readOnly = (item->state & STATE_SYSTEM_READONLY) != 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(double value) override {
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item->rangeValue) return UIA_E_NOTSUPPORTED;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_READONLY)) != 0)
      return E_ACCESSDENIED;
    if (value < item->rangeMinimum || value > item->rangeMaximum) {
      return E_INVALIDARG;
    }
    return model_->AccessibleSetRangeValue(hwnd_, child_, value);
  }

  HRESULT STDMETHODCALLTYPE get_Value(double* value) override {
    if (!value) return E_POINTER;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item->rangeValue) return UIA_E_NOTSUPPORTED;
    *value = *item->rangeValue;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Maximum(double* value) override {
    return RangeProperty(value, &Item::rangeMaximum);
  }

  HRESULT STDMETHODCALLTYPE get_Minimum(double* value) override {
    return RangeProperty(value, &Item::rangeMinimum);
  }

  HRESULT STDMETHODCALLTYPE get_LargeChange(double* value) override {
    return RangeProperty(value, &Item::rangeLargeChange);
  }

  HRESULT STDMETHODCALLTYPE get_SmallChange(double* value) override {
    return RangeProperty(value, &Item::rangeSmallChange);
  }

  HRESULT STDMETHODCALLTYPE Toggle() override { return Invoke(); }

  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override {
    if (!state) return E_POINTER;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *state = (item->state & STATE_SYSTEM_MIXED) != 0
                 ? ToggleState_Indeterminate
                 : ((item->state & STATE_SYSTEM_CHECKED) != 0
                        ? ToggleState_On
                        : ToggleState_Off);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Select() override {
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0)
      return E_ACCESSDENIED;
    model_->AccessibleFocusChild(hwnd_, child_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
    return UIA_E_NOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* selected) override {
    if (!selected) return E_POINTER;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *selected = (item->state & (STATE_SYSTEM_SELECTED | STATE_SYSTEM_FOCUSED)) !=
                0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_SelectionContainer(
      IRawElementProviderSimple** container) override {
    if (!container) return E_POINTER;
    *container = nullptr;
    if (!parent_) return E_FAIL;
    return parent_->QueryInterface(IID_IRawElementProviderSimple,
                                   reinterpret_cast<void**>(container));
  }

 private:
  ~ChildProvider() {
    if (parent_) parent_->Release();
  }

  std::optional<Item> CurrentItem() const {
    const auto items = model_->AccessibleItems(hwnd_);
    if (child_ <= 0 || child_ > static_cast<int>(items.size())) {
      return std::nullopt;
    }
    return items[static_cast<size_t>(child_ - 1)];
  }

  HRESULT RangeProperty(double* value, double Item::*member) const {
    if (!value) return E_POINTER;
    const auto item = CurrentItem();
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item->rangeValue) return UIA_E_NOTSUPPORTED;
    *value = (*item).*member;
    return S_OK;
  }

  std::atomic<ULONG> references_ = 1;
  Model* model_ = nullptr;
  HWND hwnd_ = nullptr;
  int child_ = 0;
  IAccessible* parent_ = nullptr;
};

}  // namespace detail

class Window final : public IAccessible,
                     public IServiceProvider,
                     public IAccessibleEx,
                     public IRawElementProviderSimple {
 public:
  Window(Model* model, HWND hwnd) : model_(model), hwnd_(hwnd) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) {
      *object = static_cast<IAccessible*>(this);
    } else if (iid == IID_IServiceProvider) {
      *object = static_cast<IServiceProvider*>(this);
    } else if (iid == IID_IAccessibleEx) {
      *object = static_cast<IAccessibleEx*>(this);
    } else if (iid == IID_IRawElementProviderSimple) {
      *object = static_cast<IRawElementProviderSimple*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
    if (!count) return E_POINTER;
    *count = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                                   EXCEPINFO*, UINT*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid,
                                         void** object) override {
    if (service != IID_IAccessibleEx) return E_NOINTERFACE;
    return QueryInterface(iid, object);
  }

  HRESULT STDMETHODCALLTYPE GetObjectForChild(long child,
                                               IAccessibleEx** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    const auto items = Items();
    if (child <= 0 || child > static_cast<long>(items.size())) {
      return E_INVALIDARG;
    }
    auto* provider = new detail::ChildProvider(
        model_, hwnd_, static_cast<int>(child), static_cast<IAccessible*>(this));
    *result = static_cast<IAccessibleEx*>(provider);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetIAccessiblePair(IAccessible** accessible,
                                                long* child) override {
    if (!accessible || !child) return E_POINTER;
    *accessible = static_cast<IAccessible*>(this);
    *child = CHILDID_SELF;
    AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override {
    if (!runtimeId) return E_POINTER;
    *runtimeId = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ConvertReturnedElement(
      IRawElementProviderSimple*, IAccessibleEx** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(
      ProviderOptions* options) override {
    if (!options) return E_POINTER;
    *options = ProviderOptions_ServerSideProvider;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID,
                                                IUnknown** provider) override {
    if (!provider) return E_POINTER;
    *provider = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                              VARIANT* result) override {
    if (!result) return E_POINTER;
    VariantInit(result);
    switch (property) {
      case UIA_NamePropertyId:
        return detail::SetVariantString(model_->AccessibleWindowName(hwnd_),
                                        result);
      case UIA_AutomationIdPropertyId:
        return detail::SetVariantString(L"FeatherCastWindow", result);
      case UIA_ControlTypePropertyId:
        result->vt = VT_I4;
        result->lVal = UIA_WindowControlTypeId;
        return S_OK;
      case UIA_NativeWindowHandlePropertyId:
        result->vt = VT_I4;
        result->lVal = static_cast<LONG>(reinterpret_cast<LONG_PTR>(hwnd_));
        return S_OK;
      case UIA_ProcessIdPropertyId:
        result->vt = VT_I4;
        result->lVal = static_cast<LONG>(GetCurrentProcessId());
        return S_OK;
      case UIA_IsEnabledPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = IsWindowEnabled(hwnd_) ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
      case UIA_IsKeyboardFocusablePropertyId:
        result->vt = VT_BOOL;
        result->boolVal = VARIANT_TRUE;
        return S_OK;
      case UIA_HasKeyboardFocusPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = GetFocus() == hwnd_ ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
      case UIA_IsOffscreenPropertyId:
        result->vt = VT_BOOL;
        result->boolVal = IsWindowVisible(hwnd_) ? VARIANT_FALSE : VARIANT_TRUE;
        return S_OK;
      default: return S_OK;
    }
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
      IRawElementProviderSimple** provider) override {
    if (!provider) return E_POINTER;
    *provider = nullptr;
    return hwnd_ ? UiaHostProviderFromHwnd(hwnd_, provider) : S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
    if (!parent) return E_POINTER;
    *parent = nullptr;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override {
    if (!count) return E_POINTER;
    *count = static_cast<LONG>(Items().size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override {
    if (!child) return E_POINTER;
    *child = nullptr;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
    if (!name) return E_POINTER;
    *name = nullptr;
    if (IsSelf(child)) {
      const std::wstring value = model_->AccessibleWindowName(hwnd_);
      *name = SysAllocString(value.c_str());
      return *name ? S_OK : E_OUTOFMEMORY;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    *name = SysAllocString(item->name.c_str());
    return *name ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->value.empty()) return S_FALSE;
    *value = SysAllocString(item->value.c_str());
    return *value ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override {
    if (!description) return E_POINTER;
    *description = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->description.empty()) return S_FALSE;
    *description = SysAllocString(item->description.c_str());
    return *description ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
    if (!role) return E_POINTER;
    VariantInit(role);
    role->vt = VT_I4;
    if (IsSelf(child)) {
      role->lVal = ROLE_SYSTEM_WINDOW;
      return S_OK;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    role->lVal = item->role;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
    if (!state) return E_POINTER;
    VariantInit(state);
    state->vt = VT_I4;
    if (IsSelf(child)) {
      state->lVal = IsWindowVisible(hwnd_) ? 0 : STATE_SYSTEM_INVISIBLE;
      return S_OK;
    }
    const auto item = ItemFor(child);
    if (!item) return E_INVALIDARG;
    state->lVal = item->state;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, LONG*) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child,
                                                      BSTR* shortcut) override {
    if (!shortcut) return E_POINTER;
    *shortcut = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->keyboardShortcut.empty()) return S_FALSE;
    *shortcut = SysAllocString(item->keyboardShortcut.c_str());
    return *shortcut ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
    if (!focus) return E_POINTER;
    VariantInit(focus);
    const LONG child = model_->AccessibleFocusedChild(hwnd_);
    focus->vt = VT_I4;
    const auto items = Items();
    if (child > 0 && child <= static_cast<LONG>(items.size()) &&
        (items[static_cast<size_t>(child - 1)].state &
         (STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_UNAVAILABLE |
          STATE_SYSTEM_INVISIBLE)) == STATE_SYSTEM_FOCUSABLE) {
      focus->lVal = child;
    } else {
      focus->lVal = CHILDID_SELF;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
    if (!selection) return E_POINTER;
    VariantInit(selection);
    const LONG child = model_->AccessibleSelectedChild(hwnd_);
    selection->vt = VT_I4;
    const auto items = Items();
    if (child > 0 && child <= static_cast<LONG>(items.size()) &&
        (items[static_cast<size_t>(child - 1)].state &
         (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) == 0) {
      selection->lVal = child;
    } else {
      selection->lVal = CHILDID_SELF;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
    if (!action) return E_POINTER;
    *action = nullptr;
    const auto item = ItemFor(child);
    if (!item || item->defaultAction.empty()) return S_FALSE;
    *action = SysAllocString(item->defaultAction.c_str());
    return *action ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE accSelect(LONG flags, VARIANT child) override {
    const auto id = ChildId(child);
    if (!id || !ItemFor(child)) return E_INVALIDARG;
    const auto item = ItemFor(child);
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0 ||
        (item->state & STATE_SYSTEM_FOCUSABLE) == 0) {
      return E_ACCESSDENIED;
    }
    if ((flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION)) != 0) {
      model_->AccessibleFocusChild(hwnd_, *id);
      return S_OK;
    }
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE accLocation(LONG* left, LONG* top, LONG* width, LONG* height,
                                        VARIANT child) override {
    if (!left || !top || !width || !height) return E_POINTER;
    RECT rect{};
    if (IsSelf(child)) {
      GetWindowRect(hwnd_, &rect);
    } else {
      const auto item = ItemFor(child);
      if (!item) return E_INVALIDARG;
      rect = item->screenRect;
    }
    *left = rect.left;
    *top = rect.top;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accNavigate(LONG direction, VARIANT start, VARIANT* destination) override {
    if (!destination) return E_POINTER;
    VariantInit(destination);
    const LONG count = static_cast<LONG>(Items().size());
    LONG id = IsSelf(start) ? 0 : start.lVal;
    if (!IsSelf(start) && (start.vt != VT_I4 || id <= 0 || id > count)) {
      return E_INVALIDARG;
    }
    if (id == 0 && direction != NAVDIR_FIRSTCHILD && direction != NAVDIR_LASTCHILD) {
      return S_FALSE;
    }
    if (direction == NAVDIR_FIRSTCHILD && id == 0 && count > 0) id = 1;
    else if (direction == NAVDIR_LASTCHILD && id == 0 && count > 0) id = count;
    else if (direction == NAVDIR_NEXT && id > 0 && id < count) ++id;
    else if (direction == NAVDIR_PREVIOUS && id > 1) --id;
    else return S_FALSE;
    destination->vt = VT_I4;
    destination->lVal = id;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accHitTest(LONG x, LONG y, VARIANT* child) override {
    if (!child) return E_POINTER;
    VariantInit(child);
    const auto items = Items();
    for (size_t i = 0; i < items.size(); ++i) {
      POINT point{x, y};
      if ((items[i].state & STATE_SYSTEM_INVISIBLE) != 0) continue;
      if (PtInRect(&items[i].screenRect, point)) {
        child->vt = VT_I4;
        child->lVal = static_cast<LONG>(i + 1);
        return S_OK;
      }
    }
    child->vt = VT_I4;
    child->lVal = CHILDID_SELF;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
    const auto id = ChildId(child);
    if (!id || !ItemFor(child)) return E_INVALIDARG;
    const auto item = ItemFor(child);
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0) {
      return E_ACCESSDENIED;
    }
    model_->AccessibleInvokeChild(hwnd_, *id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override {
    const auto id = ChildId(child);
    if (!id || !ItemFor(child)) return E_INVALIDARG;
    const auto item = ItemFor(child);
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0) {
      return E_ACCESSDENIED;
    }
    return model_->AccessibleSetValue(hwnd_, *id, value ? value : L"");
  }

 private:
  static bool IsSelf(const VARIANT& child) {
    return child.vt == VT_I4 && child.lVal == CHILDID_SELF;
  }
  static std::optional<int> ChildId(const VARIANT& child) {
    if (child.vt != VT_I4 || child.lVal <= 0) return std::nullopt;
    return static_cast<int>(child.lVal);
  }
  std::vector<Item> Items() const { return model_->AccessibleItems(hwnd_); }
  std::optional<Item> ItemFor(const VARIANT& child) const {
    const auto id = ChildId(child);
    if (!id) return std::nullopt;
    const auto items = Items();
    if (*id > static_cast<int>(items.size())) return std::nullopt;
    return items[static_cast<size_t>(*id - 1)];
  }

  std::atomic<ULONG> references_ = 1;
  Model* model_ = nullptr;
  HWND hwnd_ = nullptr;
};

inline LRESULT HandleGetObject(Model* model, HWND hwnd, WPARAM wParam, LPARAM lParam) {
  if (static_cast<LONG>(lParam) == UiaRootObjectId) {
    auto* provider = new Window(model, hwnd);
    const LRESULT result = UiaReturnRawElementProvider(
        hwnd, wParam, lParam, static_cast<IRawElementProviderSimple*>(provider));
    provider->Release();
    return result;
  }
  if (static_cast<LONG>(lParam) != OBJID_CLIENT) return 0;
  auto* accessible = new Window(model, hwnd);
  const LRESULT result = LresultFromObject(
      IID_IAccessible, wParam, static_cast<IAccessible*>(accessible));
  accessible->Release();
  return result;
}

}  // namespace feathercast::accessibility
