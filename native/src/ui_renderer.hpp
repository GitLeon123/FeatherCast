#pragma once

#include "app_types.hpp"

#include <d2d1_1.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct IDWriteFactory;

namespace feathercast::ui {

class HitRegions {
 public:
  using Container = std::vector<app::HitTarget>;
  using iterator = Container::iterator;
  using const_iterator = Container::const_iterator;

  void clear() noexcept { regions_.clear(); }
  void push_back(app::HitTarget region) {
    regions_.push_back(std::move(region));
  }
  [[nodiscard]] iterator begin() noexcept { return regions_.begin(); }
  [[nodiscard]] iterator end() noexcept { return regions_.end(); }
  [[nodiscard]] const_iterator begin() const noexcept { return regions_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return regions_.end(); }
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return regions_.size(); }

  [[nodiscard]] const app::HitTarget* At(float x, float y) const noexcept;

  // Limits the regions added since `first` to the vertical band a clip drew
  // them in, dropping those left empty, so a row scrolled half out of view
  // cannot take clicks over the header or footer.
  void ClipFrom(std::size_t first, float top, float bottom) {
    if (first >= regions_.size()) return;
    for (auto it = regions_.begin() + static_cast<std::ptrdiff_t>(first);
         it != regions_.end();) {
      it->rect.top = std::max(it->rect.top, top);
      it->rect.bottom = std::min(it->rect.bottom, bottom);
      if (it->rect.bottom <= it->rect.top) {
        it = regions_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  Container regions_;
};

struct RenderFrameResult {
  HRESULT result = E_FAIL;
  bool recreateTarget = false;
};

// Owns the render pass boundary. The callback emits the view-model-specific
// drawing commands while this class consistently handles begin/clear/end and
// device-loss classification.
RenderFrameResult RenderTransparentFrame(
    ID2D1DeviceContext* context, const std::function<void()>& draw);

// First family of a comma-separated list that is installed on this system,
// else "Segoe UI". DirectWrite's CreateTextFormat succeeds for unknown family
// names, so callers must resolve the family through this before creating a
// text format. Pass the app's factory, or null to use the shared one.
std::wstring ResolveInstalledFontFamily(const std::wstring& familyList,
                                        IDWriteFactory* factory = nullptr);

}  // namespace feathercast::ui
