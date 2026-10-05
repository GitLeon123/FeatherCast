#include "app_types.hpp"
#include "theme.hpp"
#include "ui_renderer.hpp"

#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace feathercast::ui {

const app::HitTarget* HitRegions::At(float x, float y) const noexcept {
  for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
    if (x >= it->rect.left && x <= it->rect.right &&
        y >= it->rect.top && y <= it->rect.bottom) {
      return &*it;
    }
  }
  return nullptr;
}

RenderFrameResult RenderTransparentFrame(
    ID2D1DeviceContext* context, const std::function<void()>& draw) {
  if (!context) return {};
  context->BeginDraw();
  context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  draw();
  const HRESULT result = context->EndDraw();
  return {result, result == D2DERR_RECREATE_TARGET};
}

std::wstring ResolveInstalledFontFamily(const std::wstring& familyList,
                                        IDWriteFactory* factory) {
  Microsoft::WRL::ComPtr<IDWriteFactory> shared;
  if (!factory) {
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(shared.GetAddressOf())))) {
      return theme::kFallbackFontFamily;
    }
    factory = shared.Get();
  }
  Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
  if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) ||
      !collection) {
    return theme::kFallbackFontFamily;
  }
  return theme::ResolveFontFamily(
      familyList, [&collection](const std::wstring& family) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(
                   collection->FindFamilyName(family.c_str(), &index, &exists)) &&
               exists;
      });
}

}  // namespace feathercast::ui
