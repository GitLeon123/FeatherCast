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

  std::vector<Item> items;
  int focusedChild = feathercast::accessibility_projection::SearchChild();
  std::vector<int> focusRequests;
  std::vector<int> invokeRequests;
};

void VerifyLiveStatusProjection() {
  using namespace feathercast::accessibility_projection;

  static_assert(SearchChild() == 1);
  static_assert(StatusChild() == 2);
  static_assert(ResultChild(0) == 3);
  static_assert(ResultChild(4) == 7);
  static_assert(PreviewChild(0) == 3);
  static_assert(PreviewChild(4) == 7);

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

  TestModel model;
  model.items = {std::move(search), StatusItem(searching), std::move(result)};

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
  assert(childCount == 3);

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
  assert(accessible->get_accDefaultAction(Child(SearchChild()), &text) ==
         S_FALSE);
  assert(text == nullptr);

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
  assert(model.invokeRequests.size() == 1);
  assert(model.invokeRequests.front() == resultChild);

  VARIANT destination;
  assert(accessible->accNavigate(NAVDIR_FIRSTCHILD, Child(CHILDID_SELF),
                                 &destination) == S_OK);
  assert(destination.vt == VT_I4 && destination.lVal == SearchChild());
  assert(accessible->accNavigate(NAVDIR_NEXT, Child(StatusChild()),
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

  accessible->Release();
}

}  // namespace

int main() {
  VerifyLiveStatusProjection();
  VerifyAccessibleModelTransport();
  return 0;
}
