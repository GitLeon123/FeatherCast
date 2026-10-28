#pragma once

// Color converter for queries such as "#ff8800", "rgb(255 136 0 / 50%)" or
// "hsl(32, 100%, 50%)". Produces HEX, RGB and HSL spellings of the same color.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace feathercast::color_utilities {

struct Rgba {
  int red = 0;
  int green = 0;
  int blue = 0;
  double alpha = 1.0;
};

struct Hsl {
  double hue = 0;         // degrees, [0, 360)
  double saturation = 0;  // percent
  double lightness = 0;   // percent
};

struct Parsed {
  Rgba rgba;
  // Set when the input was hsl()/hsla(), so that row keeps the typed values
  // instead of a lossy round trip through 8-bit channels.
  std::optional<Hsl> hsl;
};

struct Result {
  std::wstring stableId;
  std::wstring title;
  std::wstring value;
  std::wstring detail;
  std::vector<std::wstring> keywords;
  // 0xRRGGBBAA, for the swatch drawn in place of the result icon.
  std::uint32_t swatch = 0;
};

namespace detail {

inline bool IsSpace(wchar_t ch) {
  return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

inline int HexDigit(wchar_t ch) {
  if (ch >= L'0' && ch <= L'9') return ch - L'0';
  if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
  if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
  return -1;
}

// A finite decimal number with an optional unit suffix ("%", "deg").
struct Number {
  double value = 0;
  std::wstring unit;
};

inline std::optional<Number> ParseNumber(std::wstring_view token) {
  std::string ascii;
  std::size_t index = 0;
  for (; index < token.size(); ++index) {
    const wchar_t ch = token[index];
    if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch == L'+' ||
        ch == L'e' || ch == L'E') {
      // An "e" only belongs to the number when digits follow ("1e2", not "deg").
      if ((ch == L'e' || ch == L'E') &&
          (index + 1 >= token.size() ||
           !((token[index + 1] >= L'0' && token[index + 1] <= L'9') ||
             token[index + 1] == L'-' || token[index + 1] == L'+'))) {
        break;
      }
      ascii.push_back(static_cast<char>(ch));
    } else {
      break;
    }
  }
  if (ascii.empty()) return std::nullopt;
  const char* begin = ascii.data();
  if (*begin == '+') ++begin;
  double parsed = 0;
  const auto result = std::from_chars(begin, ascii.data() + ascii.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != ascii.data() + ascii.size() ||
      !std::isfinite(parsed)) {
    return std::nullopt;
  }
  Number number;
  number.value = parsed;
  for (; index < token.size(); ++index) {
    wchar_t ch = token[index];
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    number.unit.push_back(ch);
  }
  return number;
}

inline std::optional<double> ParseAlpha(std::wstring_view token) {
  const auto number = ParseNumber(token);
  if (!number) return std::nullopt;
  double alpha = number->value;
  if (number->unit == L"%") alpha /= 100.0;
  else if (!number->unit.empty()) return std::nullopt;
  if (alpha < 0 || alpha > 1) return std::nullopt;
  return alpha;
}

inline int Channel(double value) {
  return static_cast<int>(std::lround(std::clamp(value, 0.0, 255.0)));
}

inline Rgba FromHsl(const Hsl& hsl, double alpha) {
  const double s = hsl.saturation / 100.0;
  const double l = hsl.lightness / 100.0;
  const double chroma = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
  const double sector = hsl.hue / 60.0;
  const double x = chroma * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
  double r = 0, g = 0, b = 0;
  if (sector < 1) { r = chroma; g = x; }
  else if (sector < 2) { r = x; g = chroma; }
  else if (sector < 3) { g = chroma; b = x; }
  else if (sector < 4) { g = x; b = chroma; }
  else if (sector < 5) { r = x; b = chroma; }
  else { r = chroma; b = x; }
  const double m = l - chroma / 2.0;
  return {Channel((r + m) * 255.0), Channel((g + m) * 255.0),
          Channel((b + m) * 255.0), alpha};
}

inline Hsl ToHsl(const Rgba& rgba) {
  const double r = rgba.red / 255.0;
  const double g = rgba.green / 255.0;
  const double b = rgba.blue / 255.0;
  const double high = std::max({r, g, b});
  const double low = std::min({r, g, b});
  Hsl hsl;
  hsl.lightness = (high + low) / 2.0 * 100.0;
  const double delta = high - low;
  if (delta <= 0) return hsl;
  const double l = hsl.lightness / 100.0;
  hsl.saturation = delta / (1.0 - std::fabs(2.0 * l - 1.0)) * 100.0;
  double hue = 0;
  if (high == r) hue = std::fmod((g - b) / delta, 6.0);
  else if (high == g) hue = (b - r) / delta + 2.0;
  else hue = (r - g) / delta + 4.0;
  hue *= 60.0;
  if (hue < 0) hue += 360.0;
  hsl.hue = hue;
  return hsl;
}

// Up to `decimals` fractional digits, without trailing zeros.
inline std::wstring FormatNumber(double value, int decimals) {
  const double scale = std::pow(10.0, decimals);
  double rounded = std::round(value * scale) / scale;
  if (rounded == 0) rounded = 0;  // no "-0"
  std::wstring text = std::to_wstring(rounded);
  if (const auto dot = text.find(L'.'); dot != std::wstring::npos) {
    text.resize(std::min(text.size(), dot + 1 + static_cast<std::size_t>(decimals)));
    while (!text.empty() && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
  }
  return text;
}

inline std::wstring HexByte(int value) {
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  return {kHex[(value >> 4) & 0x0F], kHex[value & 0x0F]};
}

inline int AlphaByte(double alpha) {
  return static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
}

// Splits "a, b, c / d" or "a b c / d" into its arguments.
inline std::vector<std::wstring> Arguments(std::wstring_view inner) {
  std::vector<std::wstring> arguments;
  std::wstring current;
  for (const wchar_t ch : inner) {
    if (IsSpace(ch) || ch == L',' || ch == L'/') {
      if (!current.empty()) arguments.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) arguments.push_back(std::move(current));
  return arguments;
}

}  // namespace detail

inline std::optional<Parsed> Parse(std::wstring_view input) {
  while (!input.empty() && detail::IsSpace(input.front())) input.remove_prefix(1);
  while (!input.empty() && detail::IsSpace(input.back())) input.remove_suffix(1);
  if (input.size() < 4 || input.size() > 64) return std::nullopt;

  if (input.front() == L'#') {
    const std::wstring_view digits = input.substr(1);
    if (digits.size() != 3 && digits.size() != 4 && digits.size() != 6 &&
        digits.size() != 8) {
      return std::nullopt;
    }
    int values[8] = {};
    for (std::size_t index = 0; index < digits.size(); ++index) {
      values[index] = detail::HexDigit(digits[index]);
      if (values[index] < 0) return std::nullopt;
    }
    Parsed parsed;
    if (digits.size() <= 4) {
      parsed.rgba = {values[0] * 17, values[1] * 17, values[2] * 17, 1.0};
      if (digits.size() == 4) parsed.rgba.alpha = values[3] * 17 / 255.0;
    } else {
      parsed.rgba = {values[0] * 16 + values[1], values[2] * 16 + values[3],
                     values[4] * 16 + values[5], 1.0};
      if (digits.size() == 8) parsed.rgba.alpha = (values[6] * 16 + values[7]) / 255.0;
    }
    return parsed;
  }

  std::wstring lowered(input);
  for (auto& ch : lowered) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  const std::size_t open = lowered.find(L'(');
  if (open == std::wstring::npos) return std::nullopt;
  std::wstring function = lowered.substr(0, open);
  while (!function.empty() && detail::IsSpace(function.back())) function.pop_back();
  const bool rgb = function == L"rgb" || function == L"rgba";
  const bool hsl = function == L"hsl" || function == L"hsla";
  if (!rgb && !hsl) return std::nullopt;
  std::wstring_view inner = std::wstring_view(lowered).substr(open + 1);
  // A missing ")" is accepted while the user is still typing.
  if (const std::size_t close = inner.find(L')'); close != std::wstring_view::npos) {
    if (close + 1 != inner.size()) return std::nullopt;
    inner = inner.substr(0, close);
  }
  const auto arguments = detail::Arguments(inner);
  if (arguments.size() != 3 && arguments.size() != 4) return std::nullopt;

  double alpha = 1.0;
  if (arguments.size() == 4) {
    const auto parsedAlpha = detail::ParseAlpha(arguments[3]);
    if (!parsedAlpha) return std::nullopt;
    alpha = *parsedAlpha;
  }

  Parsed parsed;
  if (rgb) {
    int channels[3] = {};
    for (std::size_t index = 0; index < 3; ++index) {
      const auto number = detail::ParseNumber(arguments[index]);
      if (!number) return std::nullopt;
      double value = number->value;
      if (number->unit == L"%") {
        if (value < 0 || value > 100) return std::nullopt;
        // "* 255 / 100" keeps 50% at exactly 127.5 ("* 2.55" rounds down).
        value = value * 255.0 / 100.0;
      } else if (!number->unit.empty() || value < 0 || value > 255) {
        return std::nullopt;
      }
      channels[index] = detail::Channel(value);
    }
    parsed.rgba = {channels[0], channels[1], channels[2], alpha};
    return parsed;
  }

  const auto hue = detail::ParseNumber(arguments[0]);
  const auto saturation = detail::ParseNumber(arguments[1]);
  const auto lightness = detail::ParseNumber(arguments[2]);
  if (!hue || !saturation || !lightness) return std::nullopt;
  if (!hue->unit.empty() && hue->unit != L"deg") return std::nullopt;
  for (const auto* percent : {&*saturation, &*lightness}) {
    if ((!percent->unit.empty() && percent->unit != L"%") || percent->value < 0 ||
        percent->value > 100) {
      return std::nullopt;
    }
  }
  Hsl color;
  color.hue = std::fmod(hue->value, 360.0);
  if (color.hue < 0) color.hue += 360.0;
  color.saturation = saturation->value;
  color.lightness = lightness->value;
  parsed.rgba = detail::FromHsl(color, alpha);
  parsed.hsl = color;
  return parsed;
}

inline std::wstring FormatHex(const Rgba& color) {
  std::wstring text = L"#" + detail::HexByte(color.red) + detail::HexByte(color.green) +
                      detail::HexByte(color.blue);
  if (const int alpha = detail::AlphaByte(color.alpha); alpha != 255) {
    text += detail::HexByte(alpha);
  }
  return text;
}

inline std::wstring FormatRgb(const Rgba& color) {
  const std::wstring channels = std::to_wstring(color.red) + L", " +
                                std::to_wstring(color.green) + L", " +
                                std::to_wstring(color.blue);
  if (detail::AlphaByte(color.alpha) == 255) return L"rgb(" + channels + L")";
  return L"rgba(" + channels + L", " + detail::FormatNumber(color.alpha, 2) + L")";
}

inline std::wstring FormatHsl(const Hsl& hsl, double alpha) {
  const std::wstring values = detail::FormatNumber(hsl.hue, 1) + L", " +
                              detail::FormatNumber(hsl.saturation, 1) + L"%, " +
                              detail::FormatNumber(hsl.lightness, 1) + L"%";
  if (detail::AlphaByte(alpha) == 255) return L"hsl(" + values + L")";
  return L"hsla(" + values + L", " + detail::FormatNumber(alpha, 2) + L")";
}

inline std::uint32_t Swatch(const Rgba& color) {
  return (static_cast<std::uint32_t>(color.red) << 24) |
         (static_cast<std::uint32_t>(color.green) << 16) |
         (static_cast<std::uint32_t>(color.blue) << 8) |
         static_cast<std::uint32_t>(detail::AlphaByte(color.alpha));
}

// HEX, RGB and HSL rows for a color literal, or nothing for any other query.
inline std::vector<Result> Evaluate(std::wstring_view query) {
  std::vector<Result> results;
  const auto parsed = Parse(query);
  if (!parsed) return results;
  const Rgba& color = parsed->rgba;
  Hsl hsl = parsed->hsl ? *parsed->hsl : detail::ToHsl(color);
  if (!parsed->hsl) {
    hsl.hue = std::round(hsl.hue);
    if (hsl.hue >= 360) hsl.hue = 0;
    hsl.saturation = std::round(hsl.saturation);
    hsl.lightness = std::round(hsl.lightness);
  }
  const std::uint32_t swatch = Swatch(color);
  const std::vector<std::wstring> keywords = {L"color", L"colour", L"convert"};
  const auto add = [&](const wchar_t* stableId, std::wstring text,
                       const wchar_t* label) {
    results.push_back({stableId, text, text, label, keywords, swatch});
  };
  add(L"color-hex", FormatHex(color), L"HEX color");
  add(L"color-rgb", FormatRgb(color), L"RGB color");
  add(L"color-hsl", FormatHsl(hsl, color.alpha), L"HSL color");
  return results;
}

}  // namespace feathercast::color_utilities
