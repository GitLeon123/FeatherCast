#pragma once

// Pure unit and currency conversion engine, mirroring calculator.hpp so it can
// be unit-tested and run on the search worker thread. Currency conversions are
// resolved against a caller-supplied rate table (code -> units per 1 USD); the
// network fetch and caching of that table live in the host application.

#include "calculator.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feathercast::converter {

struct Result {
  std::wstring expression;
  std::wstring display;
  double value = 0.0;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring Lower(std::wstring value) {
  for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
  return value;
}

inline std::wstring Upper(std::wstring value) {
  for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
  return value;
}

// At most six decimals for everyday magnitudes and six significant digits
// below one, so "1 b to tb" reads 1e-12 rather than 0.
inline std::wstring FormatNumber(double value) {
  const double magnitude = std::fabs(value);
  if (magnitude >= 1.0 && magnitude < 1e15) {
    return calculator::FormatSignificant(value, 15, 6);
  }
  return calculator::FormatSignificant(value, 6);
}

// A measurement unit identified by its category and factor to the category's
// base unit. Temperature uses a dedicated code instead of a linear factor.
struct Unit {
  std::wstring_view category;
  double factor = 1.0;
  std::wstring_view tempCode;  // non-empty only for temperature units
};

namespace detail {

struct UnitEntry {
  std::wstring_view name;
  Unit unit;
};

inline constexpr UnitEntry kUnits[] = {
    {L"acre", {L"area", 4046.8564224, L""}},
    {L"acres", {L"area", 4046.8564224, L""}},
    {L"b", {L"data", 1.0, L""}},
    {L"bps", {L"data-rate", 1.0, L""}},
    {L"byte", {L"data", 1.0, L""}},
    {L"bytes", {L"data", 1.0, L""}},
    {L"c", {L"temp", 0.0, L"c"}},
    {L"celsius", {L"temp", 0.0, L"c"}},
    {L"centimeter", {L"length", 0.01, L""}},
    {L"centimeters", {L"length", 0.01, L""}},
    {L"cm", {L"length", 0.01, L""}},
    {L"cup", {L"volume", 0.2365882365, L""}},
    {L"cups", {L"volume", 0.2365882365, L""}},
    {L"day", {L"time", 86400.0, L""}},
    {L"days", {L"time", 86400.0, L""}},
    {L"deg", {L"angle", 1.0, L""}},
    {L"degree", {L"angle", 1.0, L""}},
    {L"degrees", {L"angle", 1.0, L""}},
    {L"f", {L"temp", 0.0, L"f"}},
    {L"fahrenheit", {L"temp", 0.0, L"f"}},
    {L"feet", {L"length", 0.3048, L""}},
    {L"floz", {L"volume", 0.0295735296, L""}},
    {L"foot", {L"length", 0.3048, L""}},
    {L"ft", {L"length", 0.3048, L""}},
    {L"ft2", {L"area", 0.09290304, L""}},
    {L"g", {L"mass", 1.0, L""}},
    {L"gal", {L"volume", 3.785411784, L""}},
    {L"gallon", {L"volume", 3.785411784, L""}},
    {L"gallons", {L"volume", 3.785411784, L""}},
    {L"gb", {L"data", 1000000000.0, L""}},
    {L"gbps", {L"data-rate", 1000000000.0, L""}},
    {L"gib", {L"data", 1073741824.0, L""}},
    {L"gigabyte", {L"data", 1000000000.0, L""}},
    {L"gigabytes", {L"data", 1000000000.0, L""}},
    {L"gram", {L"mass", 1.0, L""}},
    {L"grams", {L"mass", 1.0, L""}},
    {L"h", {L"time", 3600.0, L""}},
    {L"ha", {L"area", 10000.0, L""}},
    {L"hectare", {L"area", 10000.0, L""}},
    {L"hectares", {L"area", 10000.0, L""}},
    {L"horsepower", {L"power", 745.699872, L""}},
    {L"hour", {L"time", 3600.0, L""}},
    {L"hours", {L"time", 3600.0, L""}},
    {L"hp", {L"power", 745.699872, L""}},
    {L"hr", {L"time", 3600.0, L""}},
    {L"hrs", {L"time", 3600.0, L""}},
    {L"in", {L"length", 0.0254, L""}},
    {L"inch", {L"length", 0.0254, L""}},
    {L"inches", {L"length", 0.0254, L""}},
    {L"j", {L"energy", 1.0, L""}},
    {L"joule", {L"energy", 1.0, L""}},
    {L"joules", {L"energy", 1.0, L""}},
    {L"k", {L"temp", 0.0, L"k"}},
    {L"kb", {L"data", 1000.0, L""}},
    {L"kbps", {L"data-rate", 1000.0, L""}},
    {L"kelvin", {L"temp", 0.0, L"k"}},
    {L"kg", {L"mass", 1000.0, L""}},
    {L"kib", {L"data", 1024.0, L""}},
    {L"kilobyte", {L"data", 1000.0, L""}},
    {L"kilobytes", {L"data", 1000.0, L""}},
    {L"kilogram", {L"mass", 1000.0, L""}},
    {L"kilograms", {L"mass", 1000.0, L""}},
    {L"kilojoule", {L"energy", 1000.0, L""}},
    {L"kilojoules", {L"energy", 1000.0, L""}},
    {L"kilometer", {L"length", 1000.0, L""}},
    {L"kilometers", {L"length", 1000.0, L""}},
    {L"kilometre", {L"length", 1000.0, L""}},
    {L"kilowatt", {L"power", 1000.0, L""}},
    {L"kilowatt-hour", {L"energy", 3600000.0, L""}},
    {L"kilowatt-hours", {L"energy", 3600000.0, L""}},
    {L"kilowatthour", {L"energy", 3600000.0, L""}},
    {L"kilowatthours", {L"energy", 3600000.0, L""}},
    {L"kilowatts", {L"power", 1000.0, L""}},
    {L"kj", {L"energy", 1000.0, L""}},
    {L"km", {L"length", 1000.0, L""}},
    {L"km/h", {L"speed", 1000.0 / 3600.0, L""}},
    {L"km2", {L"area", 1000000.0, L""}},
    {L"kmh", {L"speed", 1000.0 / 3600.0, L""}},
    {L"kmph", {L"speed", 1000.0 / 3600.0, L""}},
    {L"kn", {L"speed", 1852.0 / 3600.0, L""}},
    {L"knot", {L"speed", 1852.0 / 3600.0, L""}},
    {L"knots", {L"speed", 1852.0 / 3600.0, L""}},
    {L"kph", {L"speed", 1000.0 / 3600.0, L""}},
    {L"kw", {L"power", 1000.0, L""}},
    {L"kwh", {L"energy", 3600000.0, L""}},
    {L"l", {L"volume", 1.0, L""}},
    {L"lb", {L"mass", 453.59237, L""}},
    {L"lbs", {L"mass", 453.59237, L""}},
    {L"liter", {L"volume", 1.0, L""}},
    {L"liters", {L"volume", 1.0, L""}},
    {L"litre", {L"volume", 1.0, L""}},
    {L"litres", {L"volume", 1.0, L""}},
    {L"m", {L"length", 1.0, L""}},
    {L"m/s", {L"speed", 1.0, L""}},
    {L"m2", {L"area", 1.0, L""}},
    {L"mb", {L"data", 1000000.0, L""}},
    {L"mbps", {L"data-rate", 1000000.0, L""}},
    {L"megabyte", {L"data", 1000000.0, L""}},
    {L"megabytes", {L"data", 1000000.0, L""}},
    {L"meter", {L"length", 1.0, L""}},
    {L"meters", {L"length", 1.0, L""}},
    {L"metre", {L"length", 1.0, L""}},
    {L"metres", {L"length", 1.0, L""}},
    {L"mg", {L"mass", 0.001, L""}},
    {L"mi", {L"length", 1609.344, L""}},
    {L"mi/h", {L"speed", 0.44704, L""}},
    {L"mi2", {L"area", 2589988.110336, L""}},
    {L"mib", {L"data", 1048576.0, L""}},
    {L"mile", {L"length", 1609.344, L""}},
    {L"miles", {L"length", 1609.344, L""}},
    {L"milligram", {L"mass", 0.001, L""}},
    {L"milligrams", {L"mass", 0.001, L""}},
    {L"milliliter", {L"volume", 0.001, L""}},
    {L"milliliters", {L"volume", 0.001, L""}},
    {L"millimeter", {L"length", 0.001, L""}},
    {L"millimeters", {L"length", 0.001, L""}},
    {L"millisecond", {L"time", 0.001, L""}},
    {L"milliseconds", {L"time", 0.001, L""}},
    {L"min", {L"time", 60.0, L""}},
    {L"mins", {L"time", 60.0, L""}},
    {L"minute", {L"time", 60.0, L""}},
    {L"minutes", {L"time", 60.0, L""}},
    {L"ml", {L"volume", 0.001, L""}},
    {L"mm", {L"length", 0.001, L""}},
    {L"mph", {L"speed", 0.44704, L""}},
    {L"mps", {L"speed", 1.0, L""}},
    {L"ms", {L"time", 0.001, L""}},
    {L"nauticalmile", {L"length", 1852.0, L""}},
    {L"nmi", {L"length", 1852.0, L""}},
    {L"ounce", {L"mass", 28.349523125, L""}},
    {L"ounces", {L"mass", 28.349523125, L""}},
    {L"oz", {L"mass", 28.349523125, L""}},
    {L"pint", {L"volume", 0.473176473, L""}},
    {L"pints", {L"volume", 0.473176473, L""}},
    {L"pound", {L"mass", 453.59237, L""}},
    {L"pounds", {L"mass", 453.59237, L""}},
    {L"pt", {L"volume", 0.473176473, L""}},
    {L"qt", {L"volume", 0.946352946, L""}},
    {L"quart", {L"volume", 0.946352946, L""}},
    {L"quarts", {L"volume", 0.946352946, L""}},
    {L"rad", {L"angle", 57.2957795130823, L""}},
    {L"radian", {L"angle", 57.2957795130823, L""}},
    {L"radians", {L"angle", 57.2957795130823, L""}},
    {L"s", {L"time", 1.0, L""}},
    {L"sec", {L"time", 1.0, L""}},
    {L"second", {L"time", 1.0, L""}},
    {L"seconds", {L"time", 1.0, L""}},
    {L"secs", {L"time", 1.0, L""}},
    {L"sqft", {L"area", 0.09290304, L""}},
    {L"sqkm", {L"area", 1000000.0, L""}},
    {L"sqm", {L"area", 1.0, L""}},
    {L"sqmi", {L"area", 2589988.110336, L""}},
    {L"st", {L"mass", 6350.29318, L""}},
    {L"stone", {L"mass", 6350.29318, L""}},
    {L"stones", {L"mass", 6350.29318, L""}},
    {L"t", {L"mass", 1000000.0, L""}},
    {L"tb", {L"data", 1000000000000.0, L""}},
    {L"terabyte", {L"data", 1000000000000.0, L""}},
    {L"terabytes", {L"data", 1000000000000.0, L""}},
    {L"ton", {L"mass", 1000000.0, L""}},
    {L"tonne", {L"mass", 1000000.0, L""}},
    {L"tonnes", {L"mass", 1000000.0, L""}},
    {L"w", {L"power", 1.0, L""}},
    {L"watt", {L"power", 1.0, L""}},
    {L"watt-hour", {L"energy", 3600.0, L""}},
    {L"watt-hours", {L"energy", 3600.0, L""}},
    {L"watthour", {L"energy", 3600.0, L""}},
    {L"watthours", {L"energy", 3600.0, L""}},
    {L"watts", {L"power", 1.0, L""}},
    {L"week", {L"time", 604800.0, L""}},
    {L"weeks", {L"time", 604800.0, L""}},
    {L"wh", {L"energy", 3600.0, L""}},
    {L"yard", {L"length", 0.9144, L""}},
    {L"yards", {L"length", 0.9144, L""}},
    {L"yd", {L"length", 0.9144, L""}},
    {L"\u00B0c", {L"temp", 0.0, L"c"}},
    {L"\u00B0f", {L"temp", 0.0, L"f"}},
    {L"\u00BAc", {L"temp", 0.0, L"c"}},
    {L"\u00BAf", {L"temp", 0.0, L"f"}},
};

consteval bool UnitsStrictlySorted() {
  for (std::size_t i = 1; i < std::size(kUnits); ++i) {
    if (!(kUnits[i - 1].name < kUnits[i].name)) return false;
  }
  return true;
}
static_assert(UnitsStrictlySorted(), "kUnits must be strictly sorted");

inline const Unit* FindUnit(std::wstring_view lower) {
  const auto it = std::lower_bound(
      std::begin(kUnits), std::end(kUnits), lower,
      [](const UnitEntry& entry, std::wstring_view key) {
        return entry.name < key;
      });
  if (it == std::end(kUnits) || it->name != lower) return nullptr;
  return &it->unit;
}

inline std::wstring_view CurrencySymbolCode(wchar_t ch) {
  switch (ch) {
    case L'$': return L"USD";
    case L'\u20AC': return L"EUR";
    case L'\u00A3': return L"GBP";
    case L'\u00A5': return L"JPY";
    case L'\u20B9': return L"INR";
    case L'\u20A9': return L"KRW";
    case L'\u20BD': return L"RUB";
    case L'\u20BA': return L"TRY";
    case L'\u20AA': return L"ILS";
    case L'\u0E3F': return L"THB";
    default: return {};
  }
}

}  // namespace detail

// Display name of a temperature unit: "\u00B0C", "\u00B0F" or "K".
inline std::wstring TemperatureSymbol(std::wstring_view code) {
  if (code == L"k") return L"K";
  return code == L"f" ? L"\u00B0F" : L"\u00B0C";
}

inline double ToCelsius(double value, std::wstring_view code) {
  if (code == L"f") return (value - 32.0) * 5.0 / 9.0;
  if (code == L"k") return value - 273.15;
  return value;
}

inline double FromCelsius(double celsius, std::wstring_view code) {
  if (code == L"f") return celsius * 9.0 / 5.0 + 32.0;
  if (code == L"k") return celsius + 273.15;
  return celsius;
}

inline std::vector<std::wstring> SplitTokens(const std::wstring& text) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (const wchar_t ch : text) {
    if (std::iswspace(ch)) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

inline bool IsConnector(const std::wstring& token) {
  return token == L"to" || token == L"in" || token == L"into" || token == L"as";
}

// Parses the decimal number at the start of `text` ("5", "-3,5", "1.5e3",
// "10km") and returns its length, or 0 when there is none. Separators follow
// the calculator (calculator::ReadDecimal): `decimal` is the locale's decimal
// point, the other separator groups thousands only in a well-formed grouped
// number ("1,000 km" with '.' as the decimal) and otherwise is a decimal point
// too, so "1,5 km" is one and a half kilometers everywhere. Malformed mixes
// ("1.000,5" with '.' as the decimal) are rejected rather than guessed.
// Unlike wcstod this never reads hex ("0x"), "inf" or "nan". An exponent needs
// digits, so "5eur" stays 5 followed by "eur".
inline size_t ParseAmount(const std::wstring& text, double& value,
                          wchar_t decimal = calculator::UserDecimalSeparator()) {
  size_t pos = 0;
  const bool hasSign = pos < text.size() && (text[pos] == L'+' || text[pos] == L'-');
  if (hasSign) ++pos;
  std::wstring number;
  if (!calculator::ReadDecimal(text, pos, decimal, number)) return 0;
  calculator::ReadExponent(text, pos, number);
  if (hasSign) number.insert(number.begin(), text.front());
  wchar_t* end = nullptr;
  value = std::wcstod(number.c_str(), &end);
  if (end != number.c_str() + number.size() || !std::isfinite(value)) return 0;
  return pos;
}

// True when the token begins with a parseable number (e.g. "5", "-3.2", "10km").
inline bool LeadingNumber(const std::wstring& token,
                          wchar_t decimal = calculator::UserDecimalSeparator()) {
  double ignored = 0.0;
  return ParseAmount(token, ignored, decimal) > 0;
}

// English words that are also ISO codes ("top", "try", "all", "mad", "pen")
// must not turn "top 10" into a currency conversion. Without an explicit
// connector a lowercase code only counts when it is one of these common ones;
// capitals and currency symbols always count.
inline bool IsCommonCurrencyCode(const std::wstring& lower) {
  static constexpr const wchar_t* kCodes[] = {
      L"usd", L"eur", L"gbp", L"jpy", L"chf", L"cad", L"aud", L"cny",
      L"inr", L"sek", L"nok", L"dkk", L"pln", L"czk", L"huf", L"brl",
      L"mxn", L"krw", L"rub", L"zar", L"nzd", L"sgd", L"hkd"};
  return std::any_of(std::begin(kCodes), std::end(kCodes),
                     [&](const wchar_t* code) { return lower == code; });
}

// Replaces currency symbols with their spaced ISO code so "$5" becomes " USD 5",
// letting the regular tokenizer/parser take over.
inline std::wstring ExpandCurrencySymbols(const std::wstring& text) {
  bool hasSymbol = false;
  for (const wchar_t ch : text) {
    if (!detail::CurrencySymbolCode(ch).empty()) {
      hasSymbol = true;
      break;
    }
  }
  if (!hasSymbol) return text;
  std::wstring out;
  out.reserve(text.size() + 8);
  for (const wchar_t ch : text) {
    const auto code = detail::CurrencySymbolCode(ch);
    if (!code.empty()) {
      out.push_back(L' ');
      out.append(code);
      out.push_back(L' ');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// Parses "<number> <fromUnit> [to|in|into|as] <toUnit>" and converts. Also
// accepts a currency-first amount ("USD 5"), "=" as a connector ("USD 5 = GBP"),
// and a lone currency ("USD 5") which converts to defaultCurrency when supplied.
// Returns nullopt for anything that is not a recognized cross-unit/currency
// conversion, leaving plain arithmetic to the calculator. `decimalSeparator`
// is the locale's decimal point ('.' or ','); tests pass it explicitly.
inline std::optional<Result> TryConvert(std::wstring input, const std::map<std::wstring, double>& rates = {},
                                        const std::wstring& defaultCurrency = L"",
                                        wchar_t decimalSeparator = calculator::UserDecimalSeparator()) {
  bool hasDigit = false;
  for (const wchar_t ch : input) {
    if (ch >= L'0' && ch <= L'9') {
      hasDigit = true;
      break;
    }
  }
  if (!hasDigit) return std::nullopt;

  const std::wstring original = Trim(std::move(input));
  if (original.empty()) return std::nullopt;

  // Normalize the query so the existing number-first parser can handle it, while
  // keeping `original` intact for display. Expand currency symbols ("$" -> "USD"),
  // treat "=" as a connector, then swap a leading "<currency> <number>" into
  // "<number> <currency>".
  std::wstring normalized = ExpandCurrencySymbols(original);
  for (size_t pos; (pos = normalized.find(L'=')) != std::wstring::npos;) {
    normalized.replace(pos, 1, L" to ");
  }
  {
    std::vector<std::wstring> head = SplitTokens(normalized);
    if (head.size() >= 2 && !LeadingNumber(head[0], decimalSeparator) &&
        LeadingNumber(head[1], decimalSeparator)) {
      std::swap(head[0], head[1]);
      normalized.clear();
      for (size_t i = 0; i < head.size(); ++i) {
        if (i) normalized.push_back(L' ');
        normalized += head[i];
      }
    }
  }

  // Parse the leading numeric amount.
  normalized = Trim(std::move(normalized));
  double amount = 0.0;
  const size_t numberLength = ParseAmount(normalized, amount, decimalSeparator);
  if (numberLength == 0) return std::nullopt;

  std::wstring remainder = Trim(normalized.substr(numberLength));
  if (remainder.empty()) return std::nullopt;

  // Normalize arrow connectors into a spaced word so tokenizing is uniform.
  for (const auto* arrow : {L"->", L">"}) {
    size_t pos;
    while ((pos = remainder.find(arrow)) != std::wstring::npos) {
      remainder.replace(pos, std::wcslen(arrow), L" to ");
    }
  }

  const std::vector<std::wstring> tokens = SplitTokens(remainder);
  std::wstring fromToken;
  std::wstring toToken;
  bool explicitConnector = false;
  if (tokens.size() == 1) {
    // A lone currency ("5 USD") converts to the caller's locale currency, unless
    // they are the same (which would be a pointless "5 EUR = 5 EUR").
    if (!defaultCurrency.empty() && Upper(tokens[0]) != Upper(defaultCurrency)) {
      fromToken = tokens[0];
      toToken = defaultCurrency;
    }
  } else if (tokens.size() == 2) {
    fromToken = tokens[0];
    toToken = tokens[1];
  } else if (tokens.size() >= 3) {
    // Find a connector that is not the first or last token.
    for (size_t i = 1; i + 1 < tokens.size(); ++i) {
      if (IsConnector(Lower(tokens[i]))) {
        fromToken = tokens[i - 1];
        toToken = tokens[i + 1];
        explicitConnector = true;
        break;
      }
    }
  }
  if (fromToken.empty() || toToken.empty()) return std::nullopt;

  // Currency conversion takes priority when both tokens are known codes.
  if (!rates.empty()) {
    // A code that is also spelled like a unit ("cup" vs. the Cuban peso CUP)
    // means the unit unless it is written in capitals. Without a connector
    // ("top 10", "10 try"), a lowercase code must be a common one.
    auto currencyRate = [&](const std::wstring& token) {
      const std::wstring code = Upper(token);
      if (token != code) {
        const std::wstring lower = Lower(token);
        if (detail::FindUnit(lower) != nullptr ||
            (!explicitConnector && !IsCommonCurrencyCode(lower))) {
          return rates.end();
        }
      }
      return rates.find(code);
    };
    const auto fromRate = currencyRate(fromToken);
    const auto toRate = currencyRate(toToken);
    if (fromRate != rates.end() && toRate != rates.end() && fromRate->second > 0.0) {
      const double usd = amount / fromRate->second;
      const double converted = usd * toRate->second;
      const std::wstring display = FormatNumber(converted) + L" " + toRate->first;
      return Result{original, display, converted};
    }
    // If exactly one side is a currency code, it is an unresolved currency
    // query (e.g. unknown code) rather than a unit conversion.
    if ((fromRate != rates.end()) != (toRate != rates.end())) return std::nullopt;
  }

  const std::wstring fromLower = Lower(fromToken);
  const std::wstring toLower = Lower(toToken);
  const Unit* fromUnit = detail::FindUnit(fromLower);
  const Unit* toUnit = detail::FindUnit(toLower);
  if (!fromUnit || !toUnit) return std::nullopt;
  if (fromUnit->category != toUnit->category) return std::nullopt;

  double converted = 0.0;
  if (fromUnit->category == L"temp") {
    const double celsius = ToCelsius(amount, fromUnit->tempCode);
    converted = FromCelsius(celsius, toUnit->tempCode);
  } else {
    if (toUnit->factor == 0.0) return std::nullopt;
    converted = amount * fromUnit->factor / toUnit->factor;
  }

  if (!std::isfinite(converted)) return std::nullopt;
  const std::wstring unitName = fromUnit->category == L"temp"
                                    ? TemperatureSymbol(toUnit->tempCode)
                                    : toLower;
  const std::wstring display = FormatNumber(converted) + L" " + unitName;
  return Result{original, display, converted};
}

}  // namespace feathercast::converter
