#pragma once

#include "screenshot_editor.hpp"

#include <optional>
#include <string>
#include <vector>

namespace feathercast::screenshot {

struct RenderedImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool Valid() const noexcept {
    return width > 0 && height > 0 &&
           static_cast<std::uint64_t>(stride) >=
               static_cast<std::uint64_t>(width) * 4u &&
           pixels.size() >= static_cast<std::size_t>(stride) * height;
  }
};

[[nodiscard]] std::optional<RenderedImage> Render(
    const Draft& draft, Rect crop, const std::vector<Annotation>& annotations,
    std::wstring* error = nullptr);

}  // namespace feathercast::screenshot
