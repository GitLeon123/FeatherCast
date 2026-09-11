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

  std::vector<Item> items;
  int focusedChild = feathercast::accessibility_projection::SearchChild();
  std::vector<int> focusRequests;
  std::vector<int> invokeRequests;
  std::vector<std::pair<int, std::wstring>> valueRequests;
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
  assert(model.invokeRequests.size() == 1);
  assert(model.invokeRequests.front() == SettingsChild());

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
  assert(model.invokeRequests.size() == 2);
  assert(model.invokeRequests.front() == SettingsChild());
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
  assert(accessible->accHitTest(500, 500, &hit) == S_OK);
  assert(hit.vt == VT_I4 && hit.lVal == CHILDID_SELF);

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

}  // namespace

int main() {
  VerifyLiveStatusProjection();
  VerifyAccessibleModelTransport();
  VerifyScreenshotEditorAccessibility();
  return 0;
}
