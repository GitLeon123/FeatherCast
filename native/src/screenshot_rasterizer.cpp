#include "screenshot_rasterizer.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <new>
#include <string_view>
#include <utility>

namespace feathercast::screenshot {
namespace {

constexpr std::uint64_t kMaxRenderedPixels = 40ULL * 1000 * 1000;
constexpr std::uint64_t kMaxRenderWorkingBytes = 768ULL * 1024 * 1024;

void SetError(std::wstring* error, const wchar_t* message) {
  if (error) *error = message;
}

bool ValidRectIn(Rect rect, Rect bounds) {
  rect = Normalize(rect);
  bounds = Normalize(bounds);
  return !rect.Empty() && rect.left >= bounds.left && rect.top >= bounds.top &&
         rect.right <= bounds.right && rect.bottom <= bounds.bottom;
}

struct LocalRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  [[nodiscard]] bool Empty() const noexcept {
    return right <= left || bottom <= top;
  }
};

LocalRect ToLocal(Rect rect, Rect crop) {
  rect = Normalize(rect);
  return {rect.left - crop.left, rect.top - crop.top,
          rect.right - crop.left, rect.bottom - crop.top};
}

LocalRect Clip(LocalRect rect, int width, int height) {
  rect.left = std::clamp(rect.left, 0, width);
  rect.top = std::clamp(rect.top, 0, height);
  rect.right = std::clamp(rect.right, 0, width);
  rect.bottom = std::clamp(rect.bottom, 0, height);
  return rect;
}

void BlendPixel(RenderedImage& image, int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
      y >= static_cast<int>(image.height)) {
    return;
  }
  auto* pixel = image.pixels.data() +
                static_cast<std::size_t>(y) * image.stride +
                static_cast<std::size_t>(x) * 4;
  const std::uint32_t alpha = color.alpha;
  const std::uint32_t inverse = 255u - alpha;
  pixel[0] = static_cast<std::uint8_t>(
      (static_cast<std::uint32_t>(color.blue) * alpha +
       static_cast<std::uint32_t>(pixel[0]) * inverse + 127u) /
      255u);
  pixel[1] = static_cast<std::uint8_t>(
      (static_cast<std::uint32_t>(color.green) * alpha +
       static_cast<std::uint32_t>(pixel[1]) * inverse + 127u) /
      255u);
  pixel[2] = static_cast<std::uint8_t>(
      (static_cast<std::uint32_t>(color.red) * alpha +
       static_cast<std::uint32_t>(pixel[2]) * inverse + 127u) /
      255u);
  pixel[3] = 255;
}

void DrawDisk(RenderedImage& image, int centerX, int centerY, int radius,
              Color color) {
  radius = std::max(0, radius);
  const int radiusSquared = radius * radius;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if (x * x + y * y <= radiusSquared) {
        BlendPixel(image, centerX + x, centerY + y, color);
      }
    }
  }
}

void DrawLine(RenderedImage& image, Point first, Point second,
              std::uint32_t strokeWidth, Color color) {
  const int dx = second.x - first.x;
  const int dy = second.y - first.y;
  const int steps = std::max(std::abs(dx), std::abs(dy));
  const int radius = std::max(0, static_cast<int>(strokeWidth + 1) / 2);
  if (steps == 0) {
    DrawDisk(image, first.x, first.y, radius, color);
    return;
  }
  for (int step = 0; step <= steps; ++step) {
    const int x = first.x + dx * step / steps;
    const int y = first.y + dy * step / steps;
    DrawDisk(image, x, y, radius, color);
  }
}

std::vector<Point> PointsFor(const Annotation& annotation) {
  if (!annotation.points.empty()) return annotation.points;
  const Rect rect = Normalize(annotation.bounds);
  return {{rect.left, rect.top}, {rect.right, rect.bottom}};
}

void DrawRectangle(RenderedImage& image, LocalRect rect,
                   std::uint32_t strokeWidth, Color color) {
  if (rect.Empty()) return;
  DrawLine(image, {rect.left, rect.top}, {rect.right, rect.top}, strokeWidth,
           color);
  DrawLine(image, {rect.right, rect.top}, {rect.right, rect.bottom},
           strokeWidth, color);
  DrawLine(image, {rect.right, rect.bottom}, {rect.left, rect.bottom},
           strokeWidth, color);
  DrawLine(image, {rect.left, rect.bottom}, {rect.left, rect.top}, strokeWidth,
           color);
}

void DrawEllipse(RenderedImage& image, LocalRect rect,
                 std::uint32_t strokeWidth, Color color) {
  if (rect.Empty()) return;
  const double centerX = (rect.left + rect.right) / 2.0;
  const double centerY = (rect.top + rect.bottom) / 2.0;
  const double radiusX = std::max(1.0, (rect.right - rect.left) / 2.0);
  const double radiusY = std::max(1.0, (rect.bottom - rect.top) / 2.0);
  const int samples = std::max(
      32, static_cast<int>(std::ceil(2.0 * 3.14159265358979323846 *
                                     std::max(radiusX, radiusY))));
  const int radius = std::max(0, static_cast<int>(strokeWidth + 1) / 2);
  for (int index = 0; index < samples; ++index) {
    const double angle = 2.0 * 3.14159265358979323846 * index / samples;
    DrawDisk(image, static_cast<int>(std::lround(centerX + radiusX * std::cos(angle))),
             static_cast<int>(std::lround(centerY + radiusY * std::sin(angle))),
             radius, color);
  }
}

void DrawArrow(RenderedImage& image, Point first, Point second,
               std::uint32_t strokeWidth, Color color) {
  DrawLine(image, first, second, strokeWidth, color);
  const double angle = std::atan2(static_cast<double>(second.y - first.y),
                                  static_cast<double>(second.x - first.x));
  const double length = std::max(8.0, static_cast<double>(strokeWidth) * 3.0);
  const double leftAngle = angle + 2.65;
  const double rightAngle = angle - 2.65;
  DrawLine(image, second,
           {static_cast<int>(std::lround(second.x + length * std::cos(leftAngle))),
            static_cast<int>(std::lround(second.y + length * std::sin(leftAngle)))},
           strokeWidth, color);
  DrawLine(image, second,
           {static_cast<int>(std::lround(second.x + length * std::cos(rightAngle))),
            static_cast<int>(std::lround(second.y + length * std::sin(rightAngle)))},
           strokeWidth, color);
}

void DrawText(RenderedImage& image, const Annotation& annotation, Rect crop) {
  if (annotation.text.empty()) return;
  const LocalRect local = Clip(
      ToLocal(AnnotationTextBounds(annotation), crop),
      static_cast<int>(image.width), static_cast<int>(image.height));
  if (local.Empty()) return;
  const std::size_t width = static_cast<std::size_t>(local.right - local.left);
  const std::size_t height = static_cast<std::size_t>(local.bottom - local.top);
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::size_t>::max() / height / 4) {
    return;
  }
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = static_cast<LONG>(width);
  info.bmiHeader.biHeight = -static_cast<LONG>(height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits,
                                         nullptr, 0)
                       : nullptr;
  if (!dc || !bitmap || !bits) {
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    return;
  }
  for (std::size_t row = 0; row < height; ++row) {
    std::memcpy(static_cast<std::uint8_t*>(bits) + row * width * 4,
                image.pixels.data() +
                    static_cast<std::size_t>(local.top) * image.stride +
                    row * image.stride + static_cast<std::size_t>(local.left) * 4,
                width * 4);
  }
  const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
  const int fontHeight = -static_cast<int>(std::max<std::uint32_t>(12,
                                                                    annotation.fontSize));
  std::wstring family = annotation.fontFamily;
  const std::size_t comma = family.find(L',');
  if (comma != std::wstring::npos) family.resize(comma);
  const auto first = family.find_first_not_of(L" \t");
  const auto last = family.find_last_not_of(L" \t");
  family = first == std::wstring::npos
               ? L"Segoe UI"
               : family.substr(first, last - first + 1);
  HFONT font = CreateFontW(fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, family.c_str());
  const HGDIOBJ previousFont = font ? SelectObject(dc, font) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(annotation.color.red, annotation.color.green,
                       annotation.color.blue));
  RECT target{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  DrawTextW(dc, annotation.text.c_str(), static_cast<int>(annotation.text.size()),
            &target, DT_NOPREFIX | DT_WORDBREAK | DT_EXPANDTABS);
  if (previousFont) SelectObject(dc, previousFont);
  if (font) DeleteObject(font);
  if (previousBitmap) SelectObject(dc, previousBitmap);
  for (std::size_t row = 0; row < height; ++row) {
    std::memcpy(image.pixels.data() +
                    static_cast<std::size_t>(local.top) * image.stride +
                    row * image.stride + static_cast<std::size_t>(local.left) * 4,
                static_cast<const std::uint8_t*>(bits) + row * width * 4,
                width * 4);
    for (std::size_t column = 0; column < width; ++column) {
      image.pixels[static_cast<std::size_t>(local.top) * image.stride +
                   row * image.stride +
                   (static_cast<std::size_t>(local.left) + column) * 4 + 3] =
          255;
    }
  }
  DeleteObject(bitmap);
  DeleteDC(dc);
}

void ApplyPixelate(RenderedImage& image, LocalRect rect) {
  rect = Clip(rect, static_cast<int>(image.width),
              static_cast<int>(image.height));
  constexpr int blockSize = 8;
  for (int top = rect.top; top < rect.bottom; top += blockSize) {
    for (int left = rect.left; left < rect.right; left += blockSize) {
      const int right = std::min(rect.right, left + blockSize);
      const int bottom = std::min(rect.bottom, top + blockSize);
      std::uint64_t blue = 0;
      std::uint64_t green = 0;
      std::uint64_t red = 0;
      std::uint64_t count = 0;
      for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
          const auto* pixel = image.pixels.data() +
                              static_cast<std::size_t>(y) * image.stride +
                              static_cast<std::size_t>(x) * 4;
          blue += pixel[0];
          green += pixel[1];
          red += pixel[2];
          ++count;
        }
      }
      if (!count) continue;
      const Color average{static_cast<std::uint8_t>(red / count),
                          static_cast<std::uint8_t>(green / count),
                          static_cast<std::uint8_t>(blue / count), 255};
      for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) BlendPixel(image, x, y, average);
      }
    }
  }
}

void ApplyBlur(RenderedImage& image, LocalRect rect) {
  rect = Clip(rect, static_cast<int>(image.width),
              static_cast<int>(image.height));
  if (rect.Empty()) return;
  constexpr int radius = 3;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  std::vector<std::uint8_t> source(static_cast<std::size_t>(width) * height * 4);
  std::vector<std::uint8_t> horizontal(source.size());
  for (int y = 0; y < height; ++y) {
    std::memcpy(source.data() + static_cast<std::size_t>(y) * width * 4,
                image.pixels.data() +
                    static_cast<std::size_t>(rect.top + y) * image.stride +
                    static_cast<std::size_t>(rect.left) * 4,
                static_cast<std::size_t>(width) * 4);
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint32_t sums[4]{};
      std::uint32_t count = 0;
      for (int sample = std::max(0, x - radius);
           sample <= std::min(width - 1, x + radius); ++sample) {
        const auto* pixel = source.data() +
                            (static_cast<std::size_t>(y) * width + sample) * 4;
        for (int channel = 0; channel < 4; ++channel) sums[channel] += pixel[channel];
        ++count;
      }
      auto* pixel = horizontal.data() +
                    (static_cast<std::size_t>(y) * width + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        pixel[channel] = static_cast<std::uint8_t>(sums[channel] / count);
      }
    }
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint32_t sums[4]{};
      std::uint32_t count = 0;
      for (int sample = std::max(0, y - radius);
           sample <= std::min(height - 1, y + radius); ++sample) {
        const auto* pixel = horizontal.data() +
                            (static_cast<std::size_t>(sample) * width + x) * 4;
        for (int channel = 0; channel < 4; ++channel) sums[channel] += pixel[channel];
        ++count;
      }
      auto* destination = image.pixels.data() +
                          static_cast<std::size_t>(rect.top + y) * image.stride +
                          static_cast<std::size_t>(rect.left + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        destination[channel] = static_cast<std::uint8_t>(sums[channel] / count);
      }
      destination[3] = 255;
    }
  }
}

}  // namespace

std::optional<RenderedImage> Render(const Draft& draft, Rect crop,
                                    const std::vector<Annotation>& annotations,
                                    std::wstring* error) {
  if (!draft.Valid()) {
    SetError(error, L"The screenshot preview is no longer available.");
    return std::nullopt;
  }
  crop = Normalize(crop);
  if (!ValidRectIn(crop, draft.sourceBounds) || crop.Width() < 2 ||
      crop.Height() < 2) {
    SetError(error, L"The selected screenshot region is invalid.");
    return std::nullopt;
  }
  const std::uint64_t pixels = static_cast<std::uint64_t>(crop.Width()) *
                               static_cast<std::uint64_t>(crop.Height());
  if (pixels == 0 || pixels > kMaxRenderedPixels ||
      pixels > std::numeric_limits<std::size_t>::max() / 4) {
    SetError(error, L"The selected screenshot is too large to process.");
    return std::nullopt;
  }

  const std::uint64_t outputBytes = pixels * 4;
  const std::uint64_t sourceBytes = draft.pixels->size();
  if (sourceBytes > kMaxRenderWorkingBytes ||
      outputBytes > kMaxRenderWorkingBytes - sourceBytes) {
    SetError(error, L"The screenshot needs too much memory to process safely.");
    return std::nullopt;
  }
  std::uint64_t peakBytes = sourceBytes + outputBytes;
  for (const auto& annotation : annotations) {
    if (annotation.tool != Tool::Blur && annotation.tool != Tool::Text) {
      continue;
    }
    const LocalRect local = Clip(
        ToLocal(annotation.bounds, crop), static_cast<int>(crop.Width()),
        static_cast<int>(crop.Height()));
    const std::uint64_t area =
        static_cast<std::uint64_t>(std::max(0, local.right - local.left)) *
        static_cast<std::uint64_t>(std::max(0, local.bottom - local.top));
    const std::uint64_t scratchBytes =
        area > std::numeric_limits<std::uint64_t>::max() / 8
            ? std::numeric_limits<std::uint64_t>::max()
            : area * (annotation.tool == Tool::Blur ? 8 : 4);
    if (scratchBytes > std::numeric_limits<std::uint64_t>::max() -
                            sourceBytes - outputBytes ||
        sourceBytes + outputBytes + scratchBytes > kMaxRenderWorkingBytes) {
      SetError(error,
               L"The screenshot needs too much memory to process safely.");
      return std::nullopt;
    }
    peakBytes = std::max(peakBytes, sourceBytes + outputBytes + scratchBytes);
  }
  (void)peakBytes;

  try {
    const std::size_t sourceX = static_cast<std::size_t>(
        crop.left - draft.sourceBounds.left);
    const std::size_t sourceY = static_cast<std::size_t>(
        crop.top - draft.sourceBounds.top);
    RenderedImage result;
    result.width = static_cast<std::uint32_t>(crop.Width());
    result.height = static_cast<std::uint32_t>(crop.Height());
    result.stride = result.width * 4;
    result.pixels.resize(
        static_cast<std::size_t>(result.stride) * result.height);
    for (std::uint32_t row = 0; row < result.height; ++row) {
      const auto* source = draft.pixels->data() +
                           (sourceY + row) * draft.stride + sourceX * 4;
      std::memcpy(result.pixels.data() +
                      static_cast<std::size_t>(row) * result.stride,
                  source, result.stride);
    }
    for (std::size_t index = 3; index < result.pixels.size(); index += 4) {
      result.pixels[index] = 255;
    }

    for (const auto& annotation : annotations) {
      switch (annotation.tool) {
        case Tool::Blur:
          ApplyBlur(result, ToLocal(annotation.bounds, crop));
          break;
        case Tool::Pixelate:
          ApplyPixelate(result, ToLocal(annotation.bounds, crop));
          break;
        case Tool::Rectangle:
          DrawRectangle(result, ToLocal(annotation.bounds, crop),
                        annotation.strokeWidth, annotation.color);
          break;
        case Tool::Ellipse:
          DrawEllipse(result, ToLocal(annotation.bounds, crop),
                      annotation.strokeWidth, annotation.color);
          break;
        case Tool::Line:
        case Tool::Arrow: {
          auto points = PointsFor(annotation);
          if (points.size() >= 2) {
            Point first{points.front().x - crop.left,
                        points.front().y - crop.top};
            Point last{points.back().x - crop.left,
                       points.back().y - crop.top};
            if (annotation.tool == Tool::Arrow) {
              DrawArrow(result, first, last, annotation.strokeWidth,
                        annotation.color);
            } else {
              DrawLine(result, first, last, annotation.strokeWidth,
                       annotation.color);
            }
          }
          break;
        }
        case Tool::Freehand: {
          const auto points = PointsFor(annotation);
          for (std::size_t index = 1; index < points.size(); ++index) {
            DrawLine(result,
                     {points[index - 1].x - crop.left,
                      points[index - 1].y - crop.top},
                     {points[index].x - crop.left,
                      points[index].y - crop.top},
                     annotation.strokeWidth, annotation.color);
          }
          break;
        }
        case Tool::Text:
          DrawText(result, annotation, crop);
          break;
        case Tool::Select:
          break;
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    SetError(error, L"The screenshot needs too much memory to process safely.");
    return std::nullopt;
  }
}

}  // namespace feathercast::screenshot
