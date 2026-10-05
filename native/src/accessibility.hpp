#pragma once

#include <windows.h>
#include <oleacc.h>
#include <servprov.h>
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
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
  // UIA live region politeness. Status text that changes on its own should be
  // Polite (or Assertive for errors) and announced with NotifyStatusChanged.
  LiveSetting liveSetting = LiveSetting::Off;
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

// Increments a COM reference count unless it already reached zero, i.e. the
// object is being destroyed and must not be revived.
inline bool TryAddReference(std::atomic<ULONG>& references) noexcept {
  ULONG current = references.load(std::memory_order_acquire);
  while (current != 0) {
    if (references.compare_exchange_weak(current, current + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

// An accessibility object that Disconnect can sever from its clients.
class Tracked {
 public:
  virtual bool TryAddRefTracked() noexcept = 0;
  virtual void DisconnectClients() noexcept = 0;
  virtual void ReleaseTracked() noexcept = 0;

 protected:
  ~Tracked() = default;
};

// Shared by every object handed out for one window. Disconnect clears the
// model, so objects that clients still hold fail instead of reaching a model
// or window that no longer exists.
struct Connection {
  Connection(Model* connectedModel, HWND connectedHwnd)
      : model(connectedModel), hwnd(connectedHwnd) {}

  void Track(Tracked* object) {
    std::lock_guard lock(mutex);
    objects.push_back(object);
  }

  void Untrack(Tracked* object) noexcept {
    std::lock_guard lock(mutex);
    std::erase(objects, object);
  }

  std::atomic<Model*> model;
  const HWND hwnd;
  std::mutex mutex;
  std::vector<Tracked*> objects;
};

inline std::mutex& ConnectionRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::map<HWND, std::shared_ptr<Connection>>& ConnectionRegistry() {
  static std::map<HWND, std::shared_ptr<Connection>> registry;
  return registry;
}

inline std::shared_ptr<Connection> ConnectionFor(Model* model, HWND hwnd) {
  if (!hwnd) return std::make_shared<Connection>(model, hwnd);
  std::lock_guard lock(ConnectionRegistryMutex());
  auto& connection = ConnectionRegistry()[hwnd];
  if (!connection || connection->model.load(std::memory_order_acquire) != model) {
    // A reused window handle must not reach the previous owner's model.
    if (connection) connection->model.store(nullptr, std::memory_order_release);
    connection = std::make_shared<Connection>(model, hwnd);
  }
  return connection;
}

class ChildProvider final : public IServiceProvider,
                            public IAccessibleEx,
                            public IRawElementProviderSimple,
                            public IInvokeProvider,
                            public IValueProvider,
                            public IRangeValueProvider,
                            public IToggleProvider,
                            public ISelectionItemProvider,
                            private Tracked {
 public:
  ChildProvider(std::shared_ptr<Connection> connection, int child,
                IAccessible* parent)
      : connection_(std::move(connection)), child_(child), parent_(parent) {
    if (parent_) parent_->AddRef();
    connection_->Track(this);
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

  // A fragment inside an MSAA-hosted window is identified relative to its
  // host: UiaAppendRuntimeId followed by an id unique within that window.
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtimeId) override {
    if (!runtimeId) return E_POINTER;
    *runtimeId = nullptr;
    if (!ConnectedModel()) return UIA_E_ELEMENTNOTAVAILABLE;
    SAFEARRAY* ids = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!ids) return E_OUTOFMEMORY;
    const int values[2] = {UiaAppendRuntimeId, child_};
    for (LONG index = 0; index < 2; ++index) {
      int value = values[index];
      const HRESULT hr = SafeArrayPutElement(ids, &index, &value);
      if (FAILED(hr)) {
        SafeArrayDestroy(ids);
        return hr;
      }
    }
    *runtimeId = ids;
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
    const auto item = CurrentItem(ConnectedModel());
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
    // A check box is operated through Toggle; also exposing Invoke would give
    // clients two different actions for the same control.
    if (pattern == UIA_InvokePatternId && !item->defaultAction.empty() &&
        item->role != ROLE_SYSTEM_CHECKBUTTON) {
      return QueryInterface(IID_IInvokeProvider,
                            reinterpret_cast<void**>(provider));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                              VARIANT* result) override {
    if (!result) return E_POINTER;
    VariantInit(result);
    const auto item = CurrentItem(ConnectedModel());
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
      case UIA_LiveSettingPropertyId:
        result->vt = VT_I4;
        result->lVal = static_cast<LONG>(item->liveSetting);
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
    Model* const model = ConnectedModel();
    const auto item = CurrentItem(model);
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0)
      return E_ACCESSDENIED;
    if (item->defaultAction.empty()) return UIA_E_NOTSUPPORTED;
    model->AccessibleInvokeChild(connection_->hwnd, child_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    Model* const model = ConnectedModel();
    const auto item = CurrentItem(model);
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_READONLY)) != 0)
      return E_ACCESSDENIED;
    return model->AccessibleSetValue(connection_->hwnd, child_,
                                     value ? value : L"");
  }

  HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    const auto item = CurrentItem(ConnectedModel());
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *value = SysAllocString(item->value.c_str());
    return *value ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* readOnly) override {
    if (!readOnly) return E_POINTER;
    const auto item = CurrentItem(ConnectedModel());
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *readOnly = (item->state & STATE_SYSTEM_READONLY) != 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(double value) override {
    Model* const model = ConnectedModel();
    const auto item = CurrentItem(model);
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item->rangeValue) return UIA_E_NOTSUPPORTED;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_READONLY)) != 0)
      return E_ACCESSDENIED;
    if (value < item->rangeMinimum || value > item->rangeMaximum) {
      return E_INVALIDARG;
    }
    return model->AccessibleSetRangeValue(connection_->hwnd, child_, value);
  }

  HRESULT STDMETHODCALLTYPE get_Value(double* value) override {
    if (!value) return E_POINTER;
    const auto item = CurrentItem(ConnectedModel());
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

  // Toggling a check box is its own action; it does not depend on the item
  // also advertising an MSAA default action.
  HRESULT STDMETHODCALLTYPE Toggle() override {
    Model* const model = ConnectedModel();
    const auto item = CurrentItem(model);
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (item->role != ROLE_SYSTEM_CHECKBUTTON) return UIA_E_NOTSUPPORTED;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0)
      return E_ACCESSDENIED;
    model->AccessibleInvokeChild(connection_->hwnd, child_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override {
    if (!state) return E_POINTER;
    const auto item = CurrentItem(ConnectedModel());
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    *state = (item->state & STATE_SYSTEM_MIXED) != 0
                 ? ToggleState_Indeterminate
                 : ((item->state & STATE_SYSTEM_CHECKED) != 0
                        ? ToggleState_On
                        : ToggleState_Off);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Select() override {
    Model* const model = ConnectedModel();
    const auto item = CurrentItem(model);
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0)
      return E_ACCESSDENIED;
    model->AccessibleFocusChild(connection_->hwnd, child_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
    return UIA_E_NOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* selected) override {
    if (!selected) return E_POINTER;
    const auto item = CurrentItem(ConnectedModel());
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
    connection_->Untrack(this);
    if (parent_) parent_->Release();
  }

  bool TryAddRefTracked() noexcept override {
    return TryAddReference(references_);
  }

  void DisconnectClients() noexcept override {
    UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(this));
    CoDisconnectObject(static_cast<IAccessibleEx*>(this), 0);
  }

  void ReleaseTracked() noexcept override { Release(); }

  Model* ConnectedModel() const noexcept {
    return connection_->model.load(std::memory_order_acquire);
  }

  std::optional<Item> CurrentItem(Model* model) const {
    if (!model) return std::nullopt;
    const auto items = model->AccessibleItems(connection_->hwnd);
    if (child_ <= 0 || child_ > static_cast<int>(items.size())) {
      return std::nullopt;
    }
    return items[static_cast<size_t>(child_ - 1)];
  }

  HRESULT RangeProperty(double* value, double Item::*member) const {
    if (!value) return E_POINTER;
    const auto item = CurrentItem(ConnectedModel());
    if (!item) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!item->rangeValue) return UIA_E_NOTSUPPORTED;
    *value = (*item).*member;
    return S_OK;
  }

  std::atomic<ULONG> references_ = 1;
  std::shared_ptr<Connection> connection_;
  int child_ = 0;
  IAccessible* parent_ = nullptr;
};

}  // namespace detail

class Window final : public IAccessible,
                     public IServiceProvider,
                     public IAccessibleEx,
                     public IRawElementProviderSimple,
                     private detail::Tracked {
 public:
  Window(Model* model, HWND hwnd)
      : Window(detail::ConnectionFor(model, hwnd)) {}

  explicit Window(std::shared_ptr<detail::Connection> connection)
      : connection_(std::move(connection)), hwnd_(connection_->hwnd) {
    connection_->Track(this);
  }

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
    Model* const model = ConnectedModel();
    if (!model) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto items = model->AccessibleItems(hwnd_);
    if (child <= 0 || child > static_cast<long>(items.size())) {
      return E_INVALIDARG;
    }
    auto* provider = new detail::ChildProvider(
        connection_, static_cast<int>(child), static_cast<IAccessible*>(this));
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

  // The window element itself is identified by its HWND host provider.
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
    return ConnectedModel() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                              VARIANT* result) override {
    if (!result) return E_POINTER;
    VariantInit(result);
    Model* const model = ConnectedModel();
    if (!model) return UIA_E_ELEMENTNOTAVAILABLE;
    switch (property) {
      case UIA_NamePropertyId:
        return detail::SetVariantString(model->AccessibleWindowName(hwnd_),
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
    if (!ConnectedModel()) return UIA_E_ELEMENTNOTAVAILABLE;
    return hwnd_ ? UiaHostProviderFromHwnd(hwnd_, provider) : S_OK;
  }

  // The client area's parent is the window's standard OBJID_WINDOW object.
  HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
    if (!parent) return E_POINTER;
    *parent = nullptr;
    if (!ConnectedModel()) return RPC_E_DISCONNECTED;
    if (!hwnd_) return S_FALSE;
    return AccessibleObjectFromWindow(hwnd_, static_cast<DWORD>(OBJID_WINDOW),
                                      IID_IDispatch,
                                      reinterpret_cast<void**>(parent));
  }
  HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override {
    if (!count) return E_POINTER;
    *count = 0;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    *count = static_cast<LONG>(model->AccessibleItems(hwnd_).size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** child) override {
    if (!child) return E_POINTER;
    *child = nullptr;
    return ConnectedModel() ? S_FALSE : RPC_E_DISCONNECTED;
  }
  HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
    if (!name) return E_POINTER;
    *name = nullptr;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    if (IsSelf(child)) {
      const std::wstring value = model->AccessibleWindowName(hwnd_);
      *name = SysAllocString(value.c_str());
      return *name ? S_OK : E_OUTOFMEMORY;
    }
    const auto item = ItemFor(model, child);
    if (!item) return E_INVALIDARG;
    *name = SysAllocString(item->name.c_str());
    return *name ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto item = ItemFor(model, child);
    if (!item || item->value.empty()) return S_FALSE;
    *value = SysAllocString(item->value.c_str());
    return *value ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* description) override {
    if (!description) return E_POINTER;
    *description = nullptr;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto item = ItemFor(model, child);
    if (!item || item->description.empty()) return S_FALSE;
    *description = SysAllocString(item->description.c_str());
    return *description ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
    if (!role) return E_POINTER;
    VariantInit(role);
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    if (IsSelf(child)) {
      role->vt = VT_I4;
      role->lVal = ROLE_SYSTEM_WINDOW;
      return S_OK;
    }
    const auto item = ItemFor(model, child);
    if (!item) return E_INVALIDARG;
    role->vt = VT_I4;
    role->lVal = item->role;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
    if (!state) return E_POINTER;
    VariantInit(state);
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    if (IsSelf(child)) {
      state->vt = VT_I4;
      state->lVal = IsWindowVisible(hwnd_) ? 0 : STATE_SYSTEM_INVISIBLE;
      return S_OK;
    }
    const auto item = ItemFor(model, child);
    if (!item) return E_INVALIDARG;
    state->vt = VT_I4;
    state->lVal = item->state;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* help) override {
    if (help) *help = nullptr;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT, LONG* topic) override {
    if (file) *file = nullptr;
    if (topic) *topic = 0;
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child,
                                                      BSTR* shortcut) override {
    if (!shortcut) return E_POINTER;
    *shortcut = nullptr;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto item = ItemFor(model, child);
    if (!item || item->keyboardShortcut.empty()) return S_FALSE;
    *shortcut = SysAllocString(item->keyboardShortcut.c_str());
    return *shortcut ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focus) override {
    if (!focus) return E_POINTER;
    VariantInit(focus);
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const LONG child = model->AccessibleFocusedChild(hwnd_);
    focus->vt = VT_I4;
    const auto items = model->AccessibleItems(hwnd_);
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
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const LONG child = model->AccessibleSelectedChild(hwnd_);
    selection->vt = VT_I4;
    const auto items = model->AccessibleItems(hwnd_);
    if (child > 0 && child <= static_cast<LONG>(items.size()) &&
        (items[static_cast<size_t>(child - 1)].state &
         (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) == 0) {
      selection->lVal = child;
    } else {
      selection->lVal = CHILDID_SELF;
    }
    return S_OK;
  }
  // A check box names the action it performs now ("Check" or "Uncheck"), so
  // the MSAA default action stays in step with the toggle state.
  HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
    if (!action) return E_POINTER;
    *action = nullptr;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto item = ItemFor(model, child);
    if (!item) return S_FALSE;
    std::wstring text = item->defaultAction;
    if (item->role == ROLE_SYSTEM_CHECKBUTTON) {
      text = (item->state & STATE_SYSTEM_CHECKED) != 0 ? L"Uncheck" : L"Check";
    }
    if (text.empty()) return S_FALSE;
    *action = SysAllocString(text.c_str());
    return *action ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE accSelect(LONG flags, VARIANT child) override {
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto id = ChildId(child);
    const auto item = ItemFor(model, child);
    if (!id || !item) return E_INVALIDARG;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0 ||
        (item->state & STATE_SYSTEM_FOCUSABLE) == 0) {
      return E_ACCESSDENIED;
    }
    if ((flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION)) != 0) {
      model->AccessibleFocusChild(hwnd_, *id);
      return S_OK;
    }
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE accLocation(LONG* left, LONG* top, LONG* width, LONG* height,
                                        VARIANT child) override {
    if (!left || !top || !width || !height) return E_POINTER;
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    RECT rect{};
    if (IsSelf(child)) {
      GetWindowRect(hwnd_, &rect);
    } else {
      const auto item = ItemFor(model, child);
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
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const LONG count = static_cast<LONG>(model->AccessibleItems(hwnd_).size());
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
  // Points outside the window report VT_EMPTY (S_FALSE); points inside it that
  // miss every visible child report the window itself. Without a window
  // handle only the children's own rectangles are known.
  HRESULT STDMETHODCALLTYPE accHitTest(LONG x, LONG y, VARIANT* child) override {
    if (!child) return E_POINTER;
    VariantInit(child);
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const POINT point{x, y};
    if (hwnd_) {
      RECT windowRect{};
      if (!GetWindowRect(hwnd_, &windowRect) || !PtInRect(&windowRect, point)) {
        return S_FALSE;
      }
    }
    const auto items = model->AccessibleItems(hwnd_);
    for (size_t i = 0; i < items.size(); ++i) {
      if ((items[i].state & STATE_SYSTEM_INVISIBLE) != 0) continue;
      if (PtInRect(&items[i].screenRect, point)) {
        child->vt = VT_I4;
        child->lVal = static_cast<LONG>(i + 1);
        return S_OK;
      }
    }
    if (!hwnd_) return S_FALSE;
    child->vt = VT_I4;
    child->lVal = CHILDID_SELF;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto id = ChildId(child);
    const auto item = ItemFor(model, child);
    if (!id || !item) return E_INVALIDARG;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0) {
      return E_ACCESSDENIED;
    }
    model->AccessibleInvokeChild(hwnd_, *id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override {
    Model* const model = ConnectedModel();
    if (!model) return RPC_E_DISCONNECTED;
    const auto id = ChildId(child);
    const auto item = ItemFor(model, child);
    if (!id || !item) return E_INVALIDARG;
    if ((item->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) != 0) {
      return E_ACCESSDENIED;
    }
    return model->AccessibleSetValue(hwnd_, *id, value ? value : L"");
  }

 private:
  ~Window() { connection_->Untrack(this); }

  bool TryAddRefTracked() noexcept override {
    return detail::TryAddReference(references_);
  }

  void DisconnectClients() noexcept override {
    UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(this));
    CoDisconnectObject(static_cast<IAccessible*>(this), 0);
  }

  void ReleaseTracked() noexcept override { Release(); }

  Model* ConnectedModel() const noexcept {
    return connection_->model.load(std::memory_order_acquire);
  }

  static bool IsSelf(const VARIANT& child) {
    return child.vt == VT_I4 && child.lVal == CHILDID_SELF;
  }
  static std::optional<int> ChildId(const VARIANT& child) {
    if (child.vt != VT_I4 || child.lVal <= 0) return std::nullopt;
    return static_cast<int>(child.lVal);
  }
  std::optional<Item> ItemFor(Model* model, const VARIANT& child) const {
    const auto id = ChildId(child);
    if (!id) return std::nullopt;
    const auto items = model->AccessibleItems(hwnd_);
    if (*id > static_cast<int>(items.size())) return std::nullopt;
    return items[static_cast<size_t>(*id - 1)];
  }

  std::atomic<ULONG> references_ = 1;
  std::shared_ptr<detail::Connection> connection_;
  HWND hwnd_ = nullptr;
};

// Answers WM_GETOBJECT for the client area. UIA reaches the items through the
// MSAA-to-UIA bridge (IAccessibleEx), which supplies the fragment navigation
// the simple providers here do not implement; returning a Window for
// UiaRootObjectId would hand UIA a root without any children.
inline LRESULT HandleGetObject(Model* model, HWND hwnd, WPARAM wParam, LPARAM lParam) {
  if (static_cast<LONG>(lParam) != OBJID_CLIENT) return 0;
  auto* accessible = new Window(detail::ConnectionFor(model, hwnd));
  const LRESULT result = LresultFromObject(
      IID_IAccessible, wParam, static_cast<IAccessible*>(accessible));
  accessible->Release();
  return result;
}

// Call from WM_DESTROY (or WM_NCDESTROY) of every window that answers
// WM_GETOBJECT with HandleGetObject. Objects that clients still hold stop
// reaching the model, and their proxies are released.
inline void Disconnect(HWND hwnd) {
  if (!hwnd) return;
  std::shared_ptr<detail::Connection> connection;
  {
    std::lock_guard lock(detail::ConnectionRegistryMutex());
    auto& registry = detail::ConnectionRegistry();
    const auto found = registry.find(hwnd);
    if (found == registry.end()) return;
    connection = std::move(found->second);
    registry.erase(found);
  }
  connection->model.store(nullptr, std::memory_order_release);
  UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);

  std::vector<detail::Tracked*> live;
  {
    std::lock_guard lock(connection->mutex);
    for (auto* object : connection->objects) {
      if (object->TryAddRefTracked()) live.push_back(object);
    }
  }
  for (auto* object : live) {
    object->DisconnectClients();
    object->ReleaseTracked();
  }
}

// Announces a status child whose text changed. Give the item a LiveSetting
// other than Off so screen readers treat it as a live region.
inline void NotifyStatusChanged(HWND hwnd, LONG child) {
  if (!hwnd) return;
  NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, child);
  NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, hwnd, OBJID_CLIENT, child);
}

}  // namespace feathercast::accessibility
