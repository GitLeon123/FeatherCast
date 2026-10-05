#include "accessibility.hpp"
#include "accessibility_projection.hpp"
#include "test_framework.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

using feathercast::accessibility::Item;

VARIANT Child(LONG id) {
  VARIANT child;
  VariantInit(&child);
  child.vt = VT_I4;
  child.lVal = id;
  return child;
}

std::wstring TakeString(BSTR value) {
  assert(value != nullptr);
  std::wstring result(value, SysStringLen(value));
  SysFreeString(value);
  return result;
}

Item StatusItem(
    const feathercast::accessibility_projection::LiveStatusProjection& status) {
  Item item;
  item.name = L"Search status";
  item.value = status.value;
  item.description = status.description;
  item.role = status.alert ? ROLE_SYSTEM_ALERT : ROLE_SYSTEM_STATICTEXT;
  item.state = STATE_SYSTEM_READONLY | STATE_SYSTEM_FOCUSABLE;
  if (!status.visible) {
    item.state |= STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE;
  }
  item.screenRect = RECT{10, 50, 310, 75};
  return item;
}

class TestModel final : public feathercast::accessibility::Model {
 public:
  std::wstring AccessibleWindowName(HWND) const override {
    return L"FeatherCast Search";
  }

  std::vector<Item> AccessibleItems(HWND) const override { return items; }

  int AccessibleFocusedChild(HWND) const override { return focusedChild; }

  void AccessibleFocusChild(HWND, int child) override {
    focusedChild = child;
    focusRequests.push_back(child);
  }

  void AccessibleInvokeChild(HWND, int child) override {
    invokeRequests.push_back(child);
  }

  HRESULT AccessibleSetValue(HWND, int child,
                             const std::wstring& value) override {
    if (child == feathercast::accessibility_projection::SearchChild()) {
      focusedChild = child;
    }
    valueRequests.emplace_back(child, value);
    return S_OK;
  }

  HRESULT AccessibleSetRangeValue(HWND, int child, double value) override {
    rangeRequests.emplace_back(child, value);
    return S_OK;
  }

  std::vector<Item> items;
  int focusedChild = feathercast::accessibility_projection::SearchChild();
  std::vector<int> focusRequests;
  std::vector<int> invokeRequests;
  std::vector<std::pair<int, std::wstring>> valueRequests;
  std::vector<std::pair<int, double>> rangeRequests;
};

void VerifyLiveStatusProjection() {
  using namespace feathercast::accessibility_projection;

  static_assert(SearchChild() == 1);
  static_assert(StatusChild() == 2);
  static_assert(SettingsChild() == 3);
  static_assert(ResultChild(0) == 4);
  static_assert(ResultChild(4) == 8);
  static_assert(PreviewChild(0) == 4);
  static_assert(PreviewChild(4) == 8);

  const auto hidden =
      ProjectLiveStatus(false, false, L"", false, false, std::nullopt);
  assert(hidden.kind == LiveStatusKind::Hidden);
  assert(!hidden.visible);
  assert(hidden.value.empty());
  assert(!hidden.alert);

  const auto indexing =
      ProjectLiveStatus(true, true, L"No results", true, true, std::nullopt);
  assert(indexing.kind == LiveStatusKind::Loading);
  assert(indexing.visible);
  assert(indexing.value == L"Loading file index...");
  assert(indexing.description ==
         L"The saved file index is loading in the background.");

  const auto searching =
      ProjectLiveStatus(false, true, L"", false, false, std::nullopt);
  assert(searching.kind == LiveStatusKind::Loading);
  assert(searching.visible);
  assert(searching.value == L"Searching...");
  assert(searching.description ==
         L"Results cannot be activated until this search finishes.");

  const auto empty = ProjectLiveStatus(false, false, L"No matching apps",
                                       true, true, std::nullopt);
  assert(empty.kind == LiveStatusKind::Empty);
  assert(empty.visible);
  assert(empty.value == L"No matching apps");
  assert(empty.description == empty.value);

  const auto previewLoading =
      ProjectLiveStatus(false, false, L"", true, false, std::nullopt);
  assert(previewLoading.kind == LiveStatusKind::Preview);
  assert(previewLoading.visible);
  assert(previewLoading.value == L"Loading preview...");

  const auto previewReady =
      ProjectLiveStatus(false, false, L"", true, true, std::nullopt);
  assert(previewReady.kind == LiveStatusKind::Preview);
  assert(previewReady.visible);
  assert(previewReady.value == L"Preview ready");

  const feathercast::app::StatusMessage error{
      feathercast::app::StatusSeverity::Error, L"Search failed"};
  const auto projectedError =
      ProjectLiveStatus(true, true, L"No results", true, true, error);
  assert(projectedError.kind == LiveStatusKind::Error);
  assert(projectedError.visible);
  assert(projectedError.alert);
  assert(projectedError.value == L"Search failed");
  assert(projectedError.description == L"Search failed");

  const Item hiddenItem = StatusItem(hidden);
  assert(hiddenItem.name == L"Search status");
  assert(hiddenItem.role == ROLE_SYSTEM_STATICTEXT);
  assert((hiddenItem.state & STATE_SYSTEM_INVISIBLE) != 0);
  assert((hiddenItem.state & STATE_SYSTEM_UNAVAILABLE) != 0);

  const Item errorItem = StatusItem(projectedError);
  assert(errorItem.role == ROLE_SYSTEM_ALERT);
  assert((errorItem.state & STATE_SYSTEM_INVISIBLE) == 0);
  assert((errorItem.state & STATE_SYSTEM_UNAVAILABLE) == 0);
}

void VerifyAccessibleModelTransport() {
  using namespace feathercast::accessibility_projection;

  const auto searching =
      ProjectLiveStatus(false, true, L"", false, false, std::nullopt);

  Item search;
  search.name = L"Search";
  search.value = L"note";
  search.description = L"Type to search FeatherCast";
  search.defaultAction = L"Edit search";
  search.role = ROLE_SYSTEM_TEXT;
  search.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED;
  search.screenRect = RECT{10, 10, 310, 45};

  Item result;
  result.name = L"Notepad";
  result.value = L"Application";
  result.description = L"Desktop application";
  result.defaultAction = L"Open";
  result.role = ROLE_SYSTEM_LISTITEM;
  result.state = STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE |
                 STATE_SYSTEM_SELECTED;
  result.screenRect = RECT{10, 80, 310, 125};

  Item settings;
  settings.name = L"Open settings";
  settings.description = L"Open FeatherCast settings. Keyboard shortcut Ctrl+,";
  settings.defaultAction = L"Open settings";
  settings.keyboardShortcut = L"Ctrl+,";
  settings.role = ROLE_SYSTEM_PUSHBUTTON;
  settings.state = STATE_SYSTEM_FOCUSABLE;
  settings.screenRect = RECT{320, 10, 355, 45};

  TestModel model;
  model.items = {std::move(search), StatusItem(searching), std::move(settings),
                 std::move(result)};

  auto* accessible =
      new feathercast::accessibility::Window(&model, nullptr);

  void* queriedObject = nullptr;
  assert(accessible->QueryInterface(IID_IAccessible, &queriedObject) == S_OK);
  assert(queriedObject != nullptr);
  static_cast<IAccessible*>(queriedObject)->Release();
  assert(accessible->QueryInterface(IID_IStream, &queriedObject) ==
         E_NOINTERFACE);
  assert(queriedObject == nullptr);

  LONG childCount = 0;
  assert(accessible->get_accChildCount(&childCount) == S_OK);
  assert(childCount == 4);

  BSTR text = nullptr;
  assert(accessible->get_accName(Child(CHILDID_SELF), &text) == S_OK);
  assert(TakeString(text) == L"FeatherCast Search");

  VARIANT role;
  assert(accessible->get_accRole(Child(CHILDID_SELF), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_WINDOW);

  assert(accessible->get_accName(Child(SearchChild()), &text) == S_OK);
  assert(TakeString(text) == L"Search");
  assert(accessible->get_accValue(Child(SearchChild()), &text) == S_OK);
  assert(TakeString(text) == L"note");
  assert(accessible->get_accRole(Child(SearchChild()), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_TEXT);
  assert(accessible->get_accDefaultAction(Child(SearchChild()), &text) == S_OK);
  assert(TakeString(text) == L"Edit search");

  BSTR replacement = SysAllocString(L"calendar");
  assert(accessible->put_accValue(Child(SearchChild()), replacement) == S_OK);
  SysFreeString(replacement);
  assert(model.valueRequests.size() == 1);
  assert(model.valueRequests.front().first == SearchChild());
  assert(model.valueRequests.front().second == L"calendar");

  VARIANT state;
  assert(accessible->get_accState(Child(SearchChild()), &state) == S_OK);
  assert(state.vt == VT_I4);
  assert((state.lVal & STATE_SYSTEM_FOCUSABLE) != 0);
  assert((state.lVal & STATE_SYSTEM_FOCUSED) != 0);

  assert(accessible->get_accName(Child(StatusChild()), &text) == S_OK);
  assert(TakeString(text) == L"Search status");
  assert(accessible->get_accValue(Child(StatusChild()), &text) == S_OK);
  assert(TakeString(text) == L"Searching...");
  assert(accessible->get_accDescription(Child(StatusChild()), &text) == S_OK);
  assert(TakeString(text) ==
         L"Results cannot be activated until this search finishes.");
  assert(accessible->get_accRole(Child(StatusChild()), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_STATICTEXT);

  assert(accessible->get_accName(Child(SettingsChild()), &text) == S_OK);
  assert(TakeString(text) == L"Open settings");
  assert(accessible->get_accDescription(Child(SettingsChild()), &text) == S_OK);
  assert(TakeString(text) ==
         L"Open FeatherCast settings. Keyboard shortcut Ctrl+,");
  assert(accessible->get_accRole(Child(SettingsChild()), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PUSHBUTTON);
  assert(accessible->get_accDefaultAction(Child(SettingsChild()), &text) == S_OK);
  assert(TakeString(text) == L"Open settings");
  assert(accessible->get_accKeyboardShortcut(Child(SettingsChild()), &text) ==
         S_OK);
  assert(TakeString(text) == L"Ctrl+,");

  IServiceProvider* services = nullptr;
  assert(accessible->QueryInterface(IID_IServiceProvider,
                                    reinterpret_cast<void**>(&services)) ==
         S_OK);
  IAccessibleEx* rootBridge = nullptr;
  assert(services->QueryService(IID_IAccessibleEx, IID_IAccessibleEx,
                                reinterpret_cast<void**>(&rootBridge)) ==
         S_OK);
  IAccessibleEx* settingsBridge = nullptr;
  assert(rootBridge->GetObjectForChild(SettingsChild(), &settingsBridge) ==
         S_OK);
  IRawElementProviderSimple* settingsProvider = nullptr;
  assert(settingsBridge->QueryInterface(
             IID_IRawElementProviderSimple,
             reinterpret_cast<void**>(&settingsProvider)) == S_OK);
  VARIANT property;
  VariantInit(&property);
  assert(settingsProvider->GetPropertyValue(UIA_NamePropertyId, &property) ==
         S_OK);
  assert(property.vt == VT_BSTR);
  assert(std::wstring(property.bstrVal, SysStringLen(property.bstrVal)) ==
         L"Open settings");
  VariantClear(&property);
  assert(settingsProvider->GetPropertyValue(UIA_AcceleratorKeyPropertyId,
                                            &property) == S_OK);
  assert(property.vt == VT_BSTR);
  assert(std::wstring(property.bstrVal, SysStringLen(property.bstrVal)) ==
         L"Ctrl+,");
  VariantClear(&property);
  IUnknown* pattern = nullptr;
  assert(settingsProvider->GetPatternProvider(UIA_InvokePatternId, &pattern) ==
         S_OK);
  assert(pattern != nullptr);
  IInvokeProvider* invokeProvider = nullptr;
  assert(pattern->QueryInterface(IID_IInvokeProvider,
                                 reinterpret_cast<void**>(&invokeProvider)) ==
         S_OK);
  assert(invokeProvider->Invoke() == S_OK);
  invokeProvider->Release();
  pattern->Release();
  settingsProvider->Release();
  settingsBridge->Release();
  const auto verifyPattern = [&](int childId, PATTERNID patternId) {
    IAccessibleEx* childBridge = nullptr;
    assert(rootBridge->GetObjectForChild(childId, &childBridge) == S_OK);
    IRawElementProviderSimple* childProvider = nullptr;
    assert(childBridge->QueryInterface(
               IID_IRawElementProviderSimple,
               reinterpret_cast<void**>(&childProvider)) == S_OK);
    IUnknown* childPattern = nullptr;
    assert(childProvider->GetPatternProvider(patternId, &childPattern) == S_OK);
    assert(childPattern != nullptr);
    childPattern->Release();
    childProvider->Release();
    childBridge->Release();
  };
  verifyPattern(SearchChild(), UIA_ValuePatternId);
  verifyPattern(ResultChild(0), UIA_SelectionItemPatternId);
  rootBridge->Release();
  services->Release();
  assert(model.invokeRequests.size() == 1);
  assert(model.invokeRequests.front() == SettingsChild());

  const int resultChild = ResultChild(0);
  assert(accessible->get_accName(Child(resultChild), &text) == S_OK);
  assert(TakeString(text) == L"Notepad");
  assert(accessible->get_accValue(Child(resultChild), &text) == S_OK);
  assert(TakeString(text) == L"Application");
  assert(accessible->get_accDescription(Child(resultChild), &text) == S_OK);
  assert(TakeString(text) == L"Desktop application");
  assert(accessible->get_accRole(Child(resultChild), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_LISTITEM);
  assert(accessible->get_accDefaultAction(Child(resultChild), &text) == S_OK);
  assert(TakeString(text) == L"Open");
  assert(accessible->accDoDefaultAction(Child(SettingsChild())) == S_OK);
  assert(model.invokeRequests.size() == 2);
  assert(model.invokeRequests.back() == SettingsChild());

  VARIANT focus;
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == SearchChild());
  assert(accessible->get_accSelection(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == SearchChild());

  assert(accessible->accSelect(SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION,
                               Child(resultChild)) == S_OK);
  assert(model.focusRequests.size() == 1);
  assert(model.focusRequests.front() == resultChild);
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.lVal == resultChild);

  assert(accessible->accDoDefaultAction(Child(resultChild)) == S_OK);
  assert(model.invokeRequests.size() == 3);
  assert(model.invokeRequests.back() == resultChild);

  VARIANT destination;
  assert(accessible->accNavigate(NAVDIR_FIRSTCHILD, Child(CHILDID_SELF),
                                 &destination) == S_OK);
  assert(destination.vt == VT_I4 && destination.lVal == SearchChild());
  assert(accessible->accNavigate(NAVDIR_NEXT, Child(StatusChild()),
                                 &destination) == S_OK);
  assert(destination.lVal == SettingsChild());
  assert(accessible->accNavigate(NAVDIR_NEXT, Child(SettingsChild()),
                                 &destination) == S_OK);
  assert(destination.lVal == resultChild);
  assert(accessible->accNavigate(NAVDIR_LASTCHILD, Child(CHILDID_SELF),
                                 &destination) == S_OK);
  assert(destination.lVal == resultChild);

  VARIANT hit;
  assert(accessible->accHitTest(20, 90, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == resultChild);
  // Without a window handle a point outside every child is not ours.
  assert(accessible->accHitTest(500, 500, &hit) == S_FALSE);
  assert(hit.vt == VT_EMPTY);

  assert(accessible->get_accName(Child(99), &text) == E_INVALIDARG);
  assert(accessible->accSelect(SELFLAG_TAKEFOCUS,
                               Child(CHILDID_SELF)) == E_INVALIDARG);
  assert(accessible->accDoDefaultAction(Child(CHILDID_SELF)) == E_INVALIDARG);

  // A model update must never expose a stale numeric child as focus. This
  // covers a focused result disappearing and the same reset after editing the
  // search field or moving focus to the settings gear.
  model.focusedChild = resultChild;
  assert(accessible->accSelect(SELFLAG_TAKEFOCUS, Child(resultChild)) == S_OK);
  replacement = SysAllocString(L"meeting");
  assert(accessible->put_accValue(Child(SearchChild()), replacement) == S_OK);
  SysFreeString(replacement);
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == SearchChild());

  model.focusedChild = SettingsChild();
  replacement = SysAllocString(L"gear then type");
  assert(accessible->put_accValue(Child(SearchChild()), replacement) == S_OK);
  SysFreeString(replacement);
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == SearchChild());

  model.focusedChild = resultChild;
  model.items.pop_back();
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == CHILDID_SELF);
  assert(accessible->get_accSelection(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == CHILDID_SELF);
  assert(accessible->get_accName(Child(resultChild), &text) == E_INVALIDARG);
  assert(accessible->accSelect(SELFLAG_TAKEFOCUS, Child(resultChild)) ==
         E_INVALIDARG);

  assert(ResultFocus(L"result:notepad") != SearchFocus());
  assert(SettingsFocus() != CloseSettingsFocus());

  accessible->Release();
}

void VerifyScreenshotEditorAccessibility() {
  using namespace feathercast::accessibility;

  Item selectButton;
  selectButton.name = L"Select";
  selectButton.defaultAction = L"Select";
  selectButton.role = ROLE_SYSTEM_PUSHBUTTON;
  selectButton.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED;
  selectButton.screenRect = RECT{10, 10, 70, 42};

  Item undoButton;
  undoButton.name = L"Undo";
  undoButton.defaultAction = L"Undo";
  undoButton.role = ROLE_SYSTEM_PUSHBUTTON;
  undoButton.state = STATE_SYSTEM_UNAVAILABLE;
  undoButton.screenRect = RECT{74, 10, 130, 42};

  Item textItem;
  textItem.name = L"Annotation text";
  textItem.value = L"Header note";
  textItem.description = L"Type the text to place on the screenshot.";
  textItem.defaultAction = L"Edit annotation text";
  textItem.role = ROLE_SYSTEM_TEXT;
  textItem.state = STATE_SYSTEM_FOCUSABLE;
  textItem.screenRect = RECT{100, 100, 300, 150};

  Item cropItem;
  cropItem.name = L"Screenshot selection";
  cropItem.description =
      L"Use arrow keys to move. Hold Shift to resize and Ctrl for 10 pixel increments.";
  cropItem.defaultAction = L"Move or resize selection";
  cropItem.role = ROLE_SYSTEM_SLIDER;
  cropItem.value = L"x 100, y 100, 200 by 150 pixels";
  cropItem.state = STATE_SYSTEM_READONLY | STATE_SYSTEM_FOCUSABLE;
  cropItem.screenRect = RECT{100, 100, 300, 250};

  Item statusItem;
  statusItem.name = L"Screenshot status";
  statusItem.value = L"Preparing screenshot...";
  statusItem.role = ROLE_SYSTEM_STATICTEXT;
  statusItem.state = STATE_SYSTEM_READONLY;
  statusItem.screenRect = RECT{16, 700, 984, 730};

  TestModel model;
  model.items = {std::move(selectButton), std::move(undoButton),
                 std::move(textItem), std::move(cropItem),
                 std::move(statusItem)};
  model.focusedChild = 1;

  auto* accessible = new Window(&model, nullptr);

  LONG childCount = 0;
  assert(accessible->get_accChildCount(&childCount) == S_OK);
  assert(childCount == 5);

  BSTR text = nullptr;
  VARIANT role;
  VARIANT state;

  // Child 1: Select button
  assert(accessible->get_accName(Child(1), &text) == S_OK);
  assert(TakeString(text) == L"Select");
  assert(accessible->get_accRole(Child(1), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PUSHBUTTON);
  assert(accessible->get_accState(Child(1), &state) == S_OK);
  assert((state.lVal & STATE_SYSTEM_FOCUSED) != 0);

  // Child 2: Undo button (unavailable)
  assert(accessible->get_accName(Child(2), &text) == S_OK);
  assert(TakeString(text) == L"Undo");
  assert(accessible->get_accState(Child(2), &state) == S_OK);
  assert((state.lVal & STATE_SYSTEM_UNAVAILABLE) != 0);

  // Child 3: Text item
  assert(accessible->get_accName(Child(3), &text) == S_OK);
  assert(TakeString(text) == L"Annotation text");
  assert(accessible->get_accValue(Child(3), &text) == S_OK);
  assert(TakeString(text) == L"Header note");
  assert(accessible->get_accRole(Child(3), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_TEXT);

  // Child 4: Crop / selection slider
  assert(accessible->get_accName(Child(4), &text) == S_OK);
  assert(TakeString(text) == L"Screenshot selection");
  assert(accessible->get_accValue(Child(4), &text) == S_OK);
  assert(TakeString(text) == L"x 100, y 100, 200 by 150 pixels");
  assert(accessible->get_accRole(Child(4), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_SLIDER);

  // Child 5: Status text
  assert(accessible->get_accName(Child(5), &text) == S_OK);
  assert(TakeString(text) == L"Screenshot status");
  assert(accessible->get_accValue(Child(5), &text) == S_OK);
  assert(TakeString(text) == L"Preparing screenshot...");
  assert(accessible->get_accRole(Child(5), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_STATICTEXT);

  // Focus
  VARIANT focus;
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == 1);

  // Hit testing
  VARIANT hit;
  assert(accessible->accHitTest(25, 25, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == 1);
  assert(accessible->accHitTest(200, 200, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == 4);

  // Navigation
  VARIANT destination;
  assert(accessible->accNavigate(NAVDIR_FIRSTCHILD, Child(CHILDID_SELF),
                                 &destination) == S_OK);
  assert(destination.lVal == 1);
  assert(accessible->accNavigate(NAVDIR_NEXT, Child(1), &destination) == S_OK);
  assert(destination.lVal == 2);
  assert(accessible->accNavigate(NAVDIR_LASTCHILD, Child(CHILDID_SELF),
                                 &destination) == S_OK);
  assert(destination.lVal == 5);

  accessible->Release();
}

void VerifyUnavailableFocusIsNeverReported() {
  Item enabled;
  enabled.name = L"Enabled control";
  enabled.defaultAction = L"Activate";
  enabled.role = ROLE_SYSTEM_PUSHBUTTON;
  enabled.state = STATE_SYSTEM_FOCUSABLE;
  enabled.screenRect = RECT{10, 10, 100, 42};

  Item unavailable;
  unavailable.name = L"Unavailable control";
  unavailable.defaultAction = L"Activate";
  unavailable.role = ROLE_SYSTEM_PUSHBUTTON;
  unavailable.state = STATE_SYSTEM_UNAVAILABLE;
  unavailable.screenRect = RECT{110, 10, 220, 42};

  TestModel model;
  model.items = {enabled, unavailable};
  model.focusedChild = 2;
  auto* accessible = new feathercast::accessibility::Window(&model, nullptr);

  VARIANT focus;
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == CHILDID_SELF);
  assert(accessible->get_accSelection(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == CHILDID_SELF);

  VARIANT disabledState;
  assert(accessible->get_accState(Child(2), &disabledState) == S_OK);
  assert((disabledState.lVal & STATE_SYSTEM_UNAVAILABLE) != 0);
  assert((disabledState.lVal & STATE_SYSTEM_FOCUSABLE) == 0);
  assert(accessible->accSelect(SELFLAG_TAKEFOCUS, Child(2)) ==
         E_ACCESSDENIED);
  assert(accessible->accDoDefaultAction(Child(2)) == E_ACCESSDENIED);

  model.focusedChild = 1;
  assert(accessible->get_accFocus(&focus) == S_OK);
  assert(focus.vt == VT_I4 && focus.lVal == 1);

  accessible->Release();
}

void VerifyUiaRangeValuePattern() {
  Item volume;
  volume.name = L"Volume";
  volume.value = L"42%";
  volume.role = ROLE_SYSTEM_SLIDER;
  volume.state = STATE_SYSTEM_FOCUSABLE;
  volume.rangeValue = 42.0;
  volume.rangeSmallChange = 1.0;
  volume.rangeLargeChange = 10.0;

  Item mute;
  mute.name = L"Mute audio";
  mute.defaultAction = L"Mute audio";
  mute.role = ROLE_SYSTEM_CHECKBUTTON;
  mute.state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_CHECKED;

  TestModel model;
  model.items = {std::move(volume), std::move(mute)};
  auto* accessible = new feathercast::accessibility::Window(&model, nullptr);
  IAccessibleEx* bridge = nullptr;
  assert(accessible->QueryInterface(IID_IAccessibleEx,
                                    reinterpret_cast<void**>(&bridge)) ==
         S_OK);
  IAccessibleEx* child = nullptr;
  assert(bridge->GetObjectForChild(1, &child) == S_OK);
  IRawElementProviderSimple* provider = nullptr;
  assert(child->QueryInterface(IID_IRawElementProviderSimple,
                               reinterpret_cast<void**>(&provider)) == S_OK);
  IUnknown* pattern = nullptr;
  assert(provider->GetPatternProvider(UIA_RangeValuePatternId, &pattern) ==
         S_OK);
  assert(pattern != nullptr);
  IRangeValueProvider* range = nullptr;
  assert(pattern->QueryInterface(IID_IRangeValueProvider,
                                 reinterpret_cast<void**>(&range)) == S_OK);
  double value = 0.0;
  assert(range->get_Value(&value) == S_OK && value == 42.0);
  assert(range->get_Maximum(&value) == S_OK && value == 100.0);
  assert(range->get_SmallChange(&value) == S_OK && value == 1.0);
  assert(range->SetValue(65.0) == S_OK);
  assert(model.rangeRequests.size() == 1);
  assert(model.rangeRequests.front().first == 1);
  assert(model.rangeRequests.front().second == 65.0);
  IAccessibleEx* toggleChild = nullptr;
  assert(bridge->GetObjectForChild(2, &toggleChild) == S_OK);
  IRawElementProviderSimple* toggleProvider = nullptr;
  assert(toggleChild->QueryInterface(
             IID_IRawElementProviderSimple,
             reinterpret_cast<void**>(&toggleProvider)) == S_OK);
  IUnknown* togglePattern = nullptr;
  assert(toggleProvider->GetPatternProvider(UIA_TogglePatternId,
                                            &togglePattern) == S_OK);
  IToggleProvider* toggle = nullptr;
  assert(togglePattern->QueryInterface(IID_IToggleProvider,
                                       reinterpret_cast<void**>(&toggle)) ==
         S_OK);
  ToggleState toggleState = ToggleState_Off;
  assert(toggle->get_ToggleState(&toggleState) == S_OK);
  assert(toggleState == ToggleState_On);
  assert(toggle->Toggle() == S_OK);
  assert(model.invokeRequests.size() == 1 &&
         model.invokeRequests.front() == 2);
  toggle->Release();
  togglePattern->Release();
  toggleProvider->Release();
  toggleChild->Release();
  range->Release();
  pattern->Release();
  provider->Release();
  child->Release();
  bridge->Release();
  accessible->Release();
}

Item CheckBoxItem(bool checked) {
  Item item;
  item.name = L"Mute audio";
  item.role = ROLE_SYSTEM_CHECKBUTTON;
  item.state = STATE_SYSTEM_FOCUSABLE | (checked ? STATE_SYSTEM_CHECKED : 0);
  item.screenRect = RECT{110, 110, 150, 140};
  return item;
}

IRawElementProviderSimple* ChildElement(IAccessible* accessible, long child) {
  IAccessibleEx* bridge = nullptr;
  assert(accessible->QueryInterface(IID_IAccessibleEx,
                                    reinterpret_cast<void**>(&bridge)) ==
         S_OK);
  IAccessibleEx* childObject = nullptr;
  assert(bridge->GetObjectForChild(child, &childObject) == S_OK);
  IRawElementProviderSimple* element = nullptr;
  assert(childObject->QueryInterface(IID_IRawElementProviderSimple,
                                     reinterpret_cast<void**>(&element)) ==
         S_OK);
  childObject->Release();
  bridge->Release();
  return element;
}

void VerifyUiaRuntimeIdLiveSettingAndCheckBox() {
  Item status;
  status.name = L"Status";
  status.role = ROLE_SYSTEM_STATICTEXT;
  status.state = STATE_SYSTEM_READONLY;
  status.liveSetting = Polite;

  // No MSAA default action: Toggle must still work.
  TestModel model;
  model.items = {std::move(status), CheckBoxItem(false)};
  auto* accessible = new feathercast::accessibility::Window(&model, nullptr);

  // A child's runtime id is relative to its host window.
  IRawElementProviderSimple* checkBox = ChildElement(accessible, 2);
  IAccessibleEx* checkBoxEx = nullptr;
  assert(checkBox->QueryInterface(IID_IAccessibleEx,
                                  reinterpret_cast<void**>(&checkBoxEx)) ==
         S_OK);
  SAFEARRAY* runtimeId = nullptr;
  assert(checkBoxEx->GetRuntimeId(&runtimeId) == S_OK);
  assert(runtimeId != nullptr);
  LONG lower = -1;
  LONG upper = -1;
  assert(SafeArrayGetLBound(runtimeId, 1, &lower) == S_OK && lower == 0);
  assert(SafeArrayGetUBound(runtimeId, 1, &upper) == S_OK && upper == 1);
  int idPart = 0;
  LONG index = 0;
  assert(SafeArrayGetElement(runtimeId, &index, &idPart) == S_OK);
  assert(idPart == UiaAppendRuntimeId);
  index = 1;
  assert(SafeArrayGetElement(runtimeId, &index, &idPart) == S_OK);
  assert(idPart == 2);
  SafeArrayDestroy(runtimeId);
  checkBoxEx->Release();

  // A check box offers Toggle, not Invoke.
  IUnknown* pattern = nullptr;
  assert(checkBox->GetPatternProvider(UIA_InvokePatternId, &pattern) == S_OK);
  assert(pattern == nullptr);
  assert(checkBox->GetPatternProvider(UIA_TogglePatternId, &pattern) == S_OK);
  assert(pattern != nullptr);
  IToggleProvider* toggle = nullptr;
  assert(pattern->QueryInterface(IID_IToggleProvider,
                                 reinterpret_cast<void**>(&toggle)) == S_OK);
  pattern->Release();
  ToggleState toggleState = ToggleState_On;
  assert(toggle->get_ToggleState(&toggleState) == S_OK);
  assert(toggleState == ToggleState_Off);
  assert(toggle->Toggle() == S_OK);
  assert(model.invokeRequests == std::vector<int>{2});

  // The MSAA default action names what activating the box does now.
  BSTR text = nullptr;
  assert(accessible->get_accDefaultAction(Child(2), &text) == S_OK);
  assert(TakeString(text) == L"Check");
  model.items[1] = CheckBoxItem(true);
  assert(accessible->get_accDefaultAction(Child(2), &text) == S_OK);
  assert(TakeString(text) == L"Uncheck");
  assert(toggle->get_ToggleState(&toggleState) == S_OK);
  assert(toggleState == ToggleState_On);
  assert(accessible->get_accDefaultAction(Child(1), &text) == S_FALSE);
  assert(text == nullptr);

  // A disabled check box cannot be toggled.
  model.items[1].state |= STATE_SYSTEM_UNAVAILABLE;
  assert(toggle->Toggle() == E_ACCESSDENIED);
  assert(model.invokeRequests.size() == 1);
  toggle->Release();
  checkBox->Release();

  // Live regions are exposed through the UIA LiveSetting property.
  IRawElementProviderSimple* statusElement = ChildElement(accessible, 1);
  VARIANT live;
  assert(statusElement->GetPropertyValue(UIA_LiveSettingPropertyId, &live) ==
         S_OK);
  assert(live.vt == VT_I4 && live.lVal == Polite);
  statusElement->Release();
  IRawElementProviderSimple* checkElement = ChildElement(accessible, 2);
  assert(checkElement->GetPropertyValue(UIA_LiveSettingPropertyId, &live) ==
         S_OK);
  assert(live.vt == VT_I4 && live.lVal == Off);
  checkElement->Release();

  // Without a window there is no parent object.
  IDispatch* parent = static_cast<IDispatch*>(accessible);
  assert(accessible->get_accParent(&parent) == S_FALSE);
  assert(parent == nullptr);

  accessible->Release();
}

void VerifyWindowHitTestParentAndDisconnect() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  assert(SUCCEEDED(com));
  HWND hwnd = CreateWindowExW(0, L"STATIC", L"Accessibility test", WS_POPUP,
                              100, 100, 200, 100, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  assert(hwnd != nullptr);

  TestModel model;
  model.items = {CheckBoxItem(false)};
  model.items[0].defaultAction = L"Toggle mute";
  auto* accessible = new feathercast::accessibility::Window(&model, hwnd);

  VARIANT hit;
  assert(accessible->accHitTest(120, 120, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == 1);
  assert(accessible->accHitTest(250, 180, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == CHILDID_SELF);
  assert(accessible->accHitTest(500, 500, &hit) == S_FALSE);
  assert(hit.vt == VT_EMPTY);
  assert(accessible->accHitTest(99, 150, &hit) == S_FALSE);
  assert(hit.vt == VT_EMPTY);

  // The client object's parent is the window's own accessible object.
  IDispatch* parent = nullptr;
  assert(accessible->get_accParent(&parent) == S_OK);
  assert(parent != nullptr);
  IAccessible* parentAccessible = nullptr;
  assert(parent->QueryInterface(IID_IAccessible,
                                reinterpret_cast<void**>(&parentAccessible)) ==
         S_OK);
  VARIANT role;
  assert(parentAccessible->get_accRole(Child(CHILDID_SELF), &role) == S_OK);
  assert(role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_WINDOW);
  parentAccessible->Release();
  parent->Release();

  IRawElementProviderSimple* element = ChildElement(accessible, 1);
  VARIANT name;
  assert(element->GetPropertyValue(UIA_NamePropertyId, &name) == S_OK);
  assert(name.vt == VT_BSTR);
  VariantClear(&name);

  // After Disconnect, objects that clients still hold fail instead of
  // reaching the model.
  feathercast::accessibility::Disconnect(hwnd);
  LONG count = -1;
  assert(accessible->get_accChildCount(&count) == RPC_E_DISCONNECTED);
  assert(accessible->accDoDefaultAction(Child(1)) == RPC_E_DISCONNECTED);
  assert(accessible->accHitTest(120, 120, &hit) == RPC_E_DISCONNECTED);
  assert(element->GetPropertyValue(UIA_NamePropertyId, &name) ==
         UIA_E_ELEMENTNOTAVAILABLE);
  IUnknown* pattern = nullptr;
  assert(element->GetPatternProvider(UIA_TogglePatternId, &pattern) ==
         UIA_E_ELEMENTNOTAVAILABLE);
  assert(pattern == nullptr);
  IAccessibleEx* elementEx = nullptr;
  assert(element->QueryInterface(IID_IAccessibleEx,
                                 reinterpret_cast<void**>(&elementEx)) == S_OK);
  SAFEARRAY* runtimeId = nullptr;
  assert(elementEx->GetRuntimeId(&runtimeId) == UIA_E_ELEMENTNOTAVAILABLE);
  assert(runtimeId == nullptr);
  elementEx->Release();
  assert(model.invokeRequests.empty());

  // A window that answers again gets a fresh connection.
  auto* fresh = new feathercast::accessibility::Window(&model, hwnd);
  assert(fresh->get_accChildCount(&count) == S_OK && count == 1);

  // A reused handle with a different model cuts off the previous owner.
  TestModel other;
  auto* reused = new feathercast::accessibility::Window(&other, hwnd);
  assert(fresh->get_accChildCount(&count) == RPC_E_DISCONNECTED);
  assert(reused->get_accChildCount(&count) == S_OK && count == 0);
  feathercast::accessibility::Disconnect(hwnd);
  assert(reused->get_accChildCount(&count) == RPC_E_DISCONNECTED);
  feathercast::accessibility::Disconnect(hwnd);  // Repeated calls are harmless.

  reused->Release();
  fresh->Release();
  element->Release();
  accessible->Release();
  DestroyWindow(hwnd);
  CoUninitialize();
}

}  // namespace

int main() {
  VerifyLiveStatusProjection();
  VerifyAccessibleModelTransport();
  VerifyScreenshotEditorAccessibility();
  VerifyUnavailableFocusIsNeverReported();
  VerifyUiaRangeValuePattern();
  VerifyUiaRuntimeIdLiveSettingAndCheckBox();
  VerifyWindowHitTestParentAndDisconnect();
  return 0;
}
