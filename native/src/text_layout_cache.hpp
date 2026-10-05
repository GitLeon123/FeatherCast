#pragma once

#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>

namespace feathercast::ui {

enum class TextLayoutStyle : std::uint8_t {
  Inherit,
  SingleLineEllipsis,
  MeasureSingleLine,
};

struct TextLayoutOptions {
  TextLayoutStyle style = TextLayoutStyle::Inherit;
  bool centerHorizontally = false;
  bool centerVertically = false;
  bool operator==(const TextLayoutOptions&) const = default;
};

// UI-thread owned, device-independent layouts. Formats must remain immutable
// until Clear(); entries retain their format so its address cannot be reused.
// Lookups borrow the text and avoid allocating a string on a cache hit.
class TextLayoutCache {
 public:
  explicit TextLayoutCache(std::size_t capacity = 256,
                           std::size_t characterBudget = 32 * 1024)
      : capacity_(capacity), characterBudget_(characterBudget) {}

  TextLayoutCache(const TextLayoutCache&) = delete;
  TextLayoutCache& operator=(const TextLayoutCache&) = delete;

  Microsoft::WRL::ComPtr<IDWriteTextLayout> Get(
      IDWriteFactory* factory, std::wstring_view text, IDWriteTextFormat* format,
      float width, float height, TextLayoutOptions options = {}) {
    if (!factory || !format || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0 || height <= 0 ||
        text.size() > std::numeric_limits<UINT32>::max()) {
      return nullptr;
    }
    const View view{text, format, width, height, options};
    const bool cacheable = capacity_ != 0 && text.size() <= kMaxTextLength &&
                           text.size() <= characterBudget_;
    if (cacheable) {
      const auto found = entries_.find(view);
      if (found != entries_.end()) {
        order_.splice(order_.end(), order_, found->second.order);
        return found->second.layout;
      }
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(
            text.empty() ? L"" : text.data(), static_cast<UINT32>(text.size()), format, width, height,
            layout.GetAddressOf()))) {
      return nullptr;
    }
    if (options.style != TextLayoutStyle::Inherit) {
      if (FAILED(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) {
        return nullptr;
      }
      const DWRITE_TRIMMING trimming{
          options.style == TextLayoutStyle::SingleLineEllipsis
              ? DWRITE_TRIMMING_GRANULARITY_CHARACTER
              : DWRITE_TRIMMING_GRANULARITY_NONE,
          0, 0};
      Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
      if (options.style == TextLayoutStyle::SingleLineEllipsis &&
          FAILED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        return nullptr;
      }
      if (FAILED(layout->SetTrimming(&trimming, ellipsis.Get()))) return nullptr;
    }
    if (options.centerHorizontally &&
        FAILED(layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER))) {
      return nullptr;
    }
    if (options.centerVertically &&
        FAILED(layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER))) {
      return nullptr;
    }
    if (!cacheable) return layout;

    while (!entries_.empty() &&
           (entries_.size() >= capacity_ ||
            text.size() > characterBudget_ - characters_)) {
      const Key* oldest = order_.front();
      characters_ -= oldest->text.size();
      entries_.erase(entries_.find(*oldest));
      order_.pop_front();
    }
    auto [entry, inserted] = entries_.try_emplace(
        Key{std::wstring(text), format, width, height, options});
    if (inserted) {
      entry->second.layout = layout;
      entry->second.format = format;
      entry->second.order = order_.insert(order_.end(), &entry->first);
      characters_ += text.size();
    }
    return layout;
  }

  void Clear() {
    order_.clear();
    entries_.clear();
    characters_ = 0;
  }

  [[nodiscard]] std::size_t Size() const { return entries_.size(); }
  [[nodiscard]] std::size_t Characters() const { return characters_; }

 private:
  struct View {
    std::wstring_view text;
    IDWriteTextFormat* format;
    float width;
    float height;
    TextLayoutOptions options;
  };
  struct Key {
    std::wstring text;
    IDWriteTextFormat* format;
    float width;
    float height;
    TextLayoutOptions options;
    operator View() const { return {text, format, width, height, options}; }
  };
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(View value) const {
      std::size_t hash = std::hash<std::wstring_view>{}(value.text);
      const auto mix = [&hash](std::size_t part) {
        hash ^= part + 0x9e3779b9u + (hash << 6) + (hash >> 2);
      };
      mix(std::hash<IDWriteTextFormat*>{}(value.format));
      mix(std::hash<float>{}(value.width));
      mix(std::hash<float>{}(value.height));
      mix(static_cast<std::size_t>(value.options.style));
      mix(value.options.centerHorizontally);
      mix(value.options.centerVertically);
      return hash;
    }
  };
  struct Equal {
    using is_transparent = void;
    bool operator()(View left, View right) const {
      return left.text == right.text && left.format == right.format &&
             left.width == right.width && left.height == right.height &&
             left.options == right.options;
    }
  };
  struct Entry {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    std::list<const Key*>::iterator order;
  };

  static constexpr std::size_t kMaxTextLength = 1024;
  std::size_t capacity_;
  std::size_t characterBudget_;
  std::size_t characters_ = 0;
  std::unordered_map<Key, Entry, Hash, Equal> entries_;
  std::list<const Key*> order_;
};

}  // namespace feathercast::ui
