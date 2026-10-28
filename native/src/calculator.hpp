#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace feathercast::calculator {

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

// Formats `value` with at most `significant` significant digits and at most
// `maxDecimals` decimals, without trailing zeros, so binary rounding noise
// ("1650000.0000000002") never shows. Plain notation covers
// 1e-9 <= |value| < 1e15; beyond that scientific notation stays readable.
inline std::wstring FormatSignificant(double value, int significant,
                                      int maxDecimals = 64) {
  if (!std::isfinite(value)) return L"";
  if (value == 0.0) return L"0";

  std::wostringstream stream;
  stream.imbue(std::locale::classic());
  const double magnitude = std::fabs(value);
  if (magnitude >= 1e15 || magnitude < 1e-9) {
    stream << std::setprecision(significant) << value;
    return stream.str();
  }

  const int integerDigits = static_cast<int>(std::floor(std::log10(magnitude))) + 1;
  const int decimals = (std::min)(maxDecimals, (std::max)(0, significant - integerDigits));
  stream << std::fixed << std::setprecision(decimals) << value;
  std::wstring out = stream.str();
  if (out.find(L'.') != std::wstring::npos) {
    while (!out.empty() && out.back() == L'0') out.pop_back();
    if (!out.empty() && out.back() == L'.') out.pop_back();
  }
  if (out == L"-0") out = L"0";
  return out;
}

inline std::wstring FormatNumber(double value) {
  return FormatSignificant(value, 15);
}

// The user's locale decimal separator, read once. Only '.' and ',' are
// recognized; any other separator falls back to '.'.
inline wchar_t UserDecimalSeparator() {
  static const wchar_t separator = [] {
    wchar_t buffer[8]{};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buffer,
                        static_cast<int>(std::size(buffer))) > 0 &&
        buffer[0] == L',' && buffer[1] == L'\0') {
      return L',';
    }
    return L'.';
  }();
  return separator;
}

// Reads the run of digits, '.' and ',' at text[pos] into `out` with '.' as
// its decimal point and advances `pos` past it. `decimal` (the locale's
// separator) is always the decimal point. The other separator groups
// thousands only in a well-formed grouped number: one to three digits (not
// starting with 0), then one or more separator-plus-three-digit groups, then
// an optional decimal
// part ("1,234,567.5" with '.' as the decimal). Anywhere else a single other
// separator still reads as a decimal point, so "1,5" and "1.5" work in every
// locale. Returns false for a run without digits and for malformed mixes
// ("1.000,5" with '.' as the decimal) rather than guessing.
inline bool ReadDecimal(const std::wstring& text, size_t& pos, wchar_t decimal,
                        std::wstring& out) {
  if (decimal != L',') decimal = L'.';
  const wchar_t group = decimal == L',' ? L'.' : L',';
  auto isDigit = [&](size_t index) {
    return index < text.size() && text[index] >= L'0' && text[index] <= L'9';
  };
  size_t end = pos;
  size_t digits = 0;
  size_t decimals = 0;
  size_t groups = 0;
  for (; end < text.size(); ++end) {
    const wchar_t ch = text[end];
    if (ch >= L'0' && ch <= L'9') ++digits;
    else if (ch == decimal) ++decimals;
    else if (ch == group) ++groups;
    else break;
  }
  if (digits == 0 || decimals > 1) return false;

  bool grouped = false;
  if (groups != 0) {
    // A grouped number never starts with 0: "0,500" is a half.
    size_t index = pos;
    while (isDigit(index)) ++index;
    grouped = index > pos && index - pos <= 3 && text[pos] != L'0';
    size_t groupCount = 0;
    while (grouped && index < end && text[index] == group) {
      grouped = isDigit(index + 1) && isDigit(index + 2) && isDigit(index + 3) &&
                !isDigit(index + 4);
      index += 4;
      ++groupCount;
    }
    if (grouped && index < end && text[index] == decimal) {
      ++index;
      while (isDigit(index)) ++index;
    }
    grouped = grouped && groupCount != 0 && index == end;
    if (!grouped && (groups > 1 || decimals != 0)) return false;
  }

  out.clear();
  out.reserve(end - pos);
  for (size_t i = pos; i < end; ++i) {
    const wchar_t ch = text[i];
    if (ch == group) {
      if (!grouped) out.push_back(L'.');
    } else {
      out.push_back(ch == decimal ? L'.' : ch);
    }
  }
  pos = end;
  return true;
}

// Appends an exponent ("e3", "E-12") at text[pos] to `out`. It needs digits,
// so "5e" or "5eur" leave the 'e' for whatever follows.
inline void ReadExponent(const std::wstring& text, size_t& pos, std::wstring& out) {
  if (pos >= text.size() || (text[pos] != L'e' && text[pos] != L'E')) return;
  size_t index = pos + 1;
  if (index < text.size() && (text[index] == L'+' || text[index] == L'-')) ++index;
  if (index >= text.size() || text[index] < L'0' || text[index] > L'9') return;
  while (index < text.size() && text[index] >= L'0' && text[index] <= L'9') ++index;
  out.append(text, pos, index - pos);
  pos = index;
}

// "10:30" is a clock time and "2024-01-15" a date, not a division or a
// subtraction worth showing above apps. "10:2" still divides.
inline bool LooksLikeTimeOrDate(const std::wstring& text) {
  auto digitRun = [&](size_t& index) {
    const size_t start = index;
    while (index < text.size() && text[index] >= L'0' && text[index] <= L'9') ++index;
    return index - start;
  };
  size_t index = 0;
  const size_t first = digitRun(index);
  if (first >= 1 && first <= 2 && index < text.size() && text[index] == L':') {
    ++index;
    if (digitRun(index) != 2) return false;
    if (index < text.size() && text[index] == L':') {
      ++index;
      if (digitRun(index) != 2) return false;
    }
    return index == text.size();
  }
  if (first == 4 && index < text.size() && text[index] == L'-') {
    ++index;
    const size_t month = digitRun(index);
    if (month < 1 || month > 2 || index >= text.size() || text[index] != L'-') return false;
    ++index;
    const size_t day = digitRun(index);
    return day >= 1 && day <= 2 && index == text.size();
  }
  return false;
}

class Parser {
 public:
  explicit Parser(std::wstring text, wchar_t decimal = UserDecimalSeparator())
      : text_(std::move(text)), decimal_(decimal) {}

  bool Parse(double& value) {
    SkipSpaces();
    Parsed parsed;
    if (!ParseExpression(parsed)) return false;
    value = parsed.value;
    SkipSpaces();
    return pos_ == text_.size() && sawValue_ && sawCalculationSyntax_ && std::isfinite(value);
  }

 private:
  struct Parsed {
    double value = 0.0;
    bool percent = false;
  };

  // Grammar, loosest binding first:
  //   expression := term (('+' | '-') term)*
  //   term       := unary (('*' | 'x' | '/' | ':') unary)*
  //   unary      := ('+' | '-')* power
  //   power      := postfix [('^' | '**') unary]        (right associative)
  //   postfix    := primary '%'*
  //   primary    := number | hex | constant | function '(' expression ')'
  //               | '(' expression ')'
  // A sign binds looser than a power, so -2^2 is -4 while 2^-2 is 0.25 and
  // 2*-3 is -6. Function names need parentheses: "sin 30" or a bare "sqrt"
  // is not a calculation. Hex needs at least one digit after "0x". Numbers
  // follow ReadDecimal's separator rules and may carry an exponent ("1e-12").
  static constexpr double kPi = 3.14159265358979323846;
  // Bounds parentheses and power chains so pasted text cannot exhaust the
  // search worker's stack.
  static constexpr int kMaxDepth = 64;
  static constexpr size_t kMaxHexDigits = 16;

  class Nesting {
   public:
    explicit Nesting(int& depth) : depth_(depth) { ++depth_; }
    ~Nesting() { --depth_; }
    Nesting(const Nesting&) = delete;
    Nesting& operator=(const Nesting&) = delete;

   private:
    int& depth_;
  };

  bool ParseExpression(Parsed& value) {
    if (!ParseTerm(value)) return false;
    while (true) {
      SkipSpaces();
      if (Match(L'+')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseTerm(rhs)) return false;
        const double base = value.value;
        value.value = Sum(base, rhs.percent ? base * rhs.value : rhs.value);
        value.percent = false;
      } else if (Match(L'-')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseTerm(rhs)) return false;
        const double base = value.value;
        value.value = Sum(base, -(rhs.percent ? base * rhs.value : rhs.value));
        value.percent = false;
      } else {
        return true;
      }
    }
  }

  // Operands that cancel leave rounding noise ("0.1+0.2-0.3" is 5.6e-17).
  // A sum within a few ulps of its larger operand is zero.
  static double Sum(double lhs, double rhs) {
    const double sum = lhs + rhs;
    const double scale = (std::max)(std::fabs(lhs), std::fabs(rhs));
    return std::fabs(sum) <= scale * 4.0 * std::numeric_limits<double>::epsilon() ? 0.0 : sum;
  }

  bool ParseTerm(Parsed& value) {
    if (!ParseUnary(value)) return false;
    while (true) {
      SkipSpaces();
      if ((!Peek(L"**") && Match(L'*')) || Match(L'x') || Match(L'X')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseUnary(rhs)) return false;
        value.value *= rhs.value;
        value.percent = false;
      } else if (Match(L'/') || Match(L':')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseUnary(rhs) || rhs.value == 0.0) return false;
        value.value /= rhs.value;
        value.percent = false;
      } else {
        return true;
      }
    }
  }

  // A sign alone is not calculation syntax: "-3" and "--3" are just numbers,
  // while "1--3" is 4.
  bool ParseUnary(Parsed& value) {
    const Nesting nesting(depth_);
    if (depth_ > kMaxDepth) return false;
    bool negate = false;
    while (true) {
      SkipSpaces();
      if (Match(L'+')) continue;
      if (!Match(L'-')) break;
      negate = !negate;
    }
    if (!ParsePower(value)) return false;
    if (negate) value.value = -value.value;
    return true;
  }

  bool ParsePower(Parsed& value) {
    if (!ParsePostfix(value)) return false;
    SkipSpaces();
    if (Match(L"**") || Match(L'^')) {
      sawCalculationSyntax_ = true;
      Parsed rhs;
      if (!ParseUnary(rhs)) return false;
      value.value = std::pow(value.value, rhs.value);
      value.percent = false;
      return std::isfinite(value.value);
    }
    return true;
  }

  bool ParsePostfix(Parsed& value) {
    if (!ParsePrimary(value)) return false;
    while (true) {
      SkipSpaces();
      if (!Match(L'%')) return true;
      sawCalculationSyntax_ = true;
      value.value /= 100.0;
      value.percent = true;
    }
  }

  bool ParsePrimary(Parsed& value) {
    SkipSpaces();
    if (Match(L'(')) {
      sawCalculationSyntax_ = true;
      if (!ParseExpression(value)) return false;
      SkipSpaces();
      return Match(L')');
    }
    // Letters always name a function or constant. A number never starts with
    // one, so an unknown word fails here instead of being skipped over.
    if (pos_ < text_.size() && std::iswalpha(text_[pos_])) {
      return ParseFunctionOrConstant(value);
    }
    if (Peek(L"0x") || Peek(L"0X")) return ParseHex(value);
    return ParseNumber(value);
  }

  bool ParseFunctionOrConstant(Parsed& value) {
    const size_t start = pos_;
    while (pos_ < text_.size() && std::iswalpha(text_[pos_])) ++pos_;
    if (start == pos_) return false;

    std::wstring name = text_.substr(start, pos_ - start);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) {
      return static_cast<wchar_t>(std::towlower(ch));
    });
    if (name == L"log" && Match(L'2')) name = L"log2";

    if (name == L"pi") {
      value = Parsed{kPi, false};
      sawValue_ = true;
      return true;
    }
    if (name == L"e") {
      value = Parsed{std::exp(1.0), false};
      sawValue_ = true;
      return true;
    }
    static constexpr std::array<const wchar_t*, 12> kFunctions = {
        L"sin", L"cos", L"tan", L"sqrt", L"abs", L"round",
        L"floor", L"ceil", L"ln", L"log", L"log2", L"exp"};
    if (std::none_of(kFunctions.begin(), kFunctions.end(),
                     [&](const wchar_t* known) { return name == known; })) {
      return false;
    }

    SkipSpaces();
    if (!Match(L'(')) return false;
    sawCalculationSyntax_ = true;
    Parsed argument;
    if (!ParseExpression(argument)) return false;
    SkipSpaces();
    if (!Match(L')')) return false;

    const double x = argument.value;
    value.percent = false;
    if (name == L"sin" || name == L"cos" || name == L"tan") {
      return Trigonometry(name, x, value.value);
    }
    // log is base 10; ln is natural.
    if ((name == L"sqrt" && x < 0.0) ||
        ((name == L"ln" || name == L"log" || name == L"log2") && x <= 0.0)) {
      return false;
    }
    if (name == L"sqrt") value.value = std::sqrt(x);
    else if (name == L"abs") value.value = std::fabs(x);
    else if (name == L"round") value.value = std::round(x);
    else if (name == L"floor") value.value = std::floor(x);
    else if (name == L"ceil") value.value = std::ceil(x);
    else if (name == L"ln") value.value = std::log(x);
    else if (name == L"log") value.value = std::log10(x);
    else if (name == L"log2") value.value = std::log2(x);
    else value.value = std::exp(x);
    return std::isfinite(value.value);
  }

  // Arguments are degrees. Multiples of 90 are exact, so sin(180) is 0
  // rather than 1.2e-16, and tan(90) has no value instead of 1.6e16.
  static bool Trigonometry(const std::wstring& name, double degrees, double& result) {
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    if (std::fmod(degrees, 90.0) == 0.0) {
      static constexpr double kSin[] = {0.0, 1.0, 0.0, -1.0};
      static constexpr double kCos[] = {1.0, 0.0, -1.0, 0.0};
      const int quadrant = static_cast<int>(degrees / 90.0) & 3;
      if (name == L"tan" && kCos[quadrant] == 0.0) return false;
      result = name == L"sin" ? kSin[quadrant]
             : name == L"cos" ? kCos[quadrant]
                              : kSin[quadrant] / kCos[quadrant];
      return true;
    }
    const double radians = degrees * kPi / 180.0;
    result = name == L"sin" ? std::sin(radians)
           : name == L"cos" ? std::cos(radians)
                            : std::tan(radians);
    return std::isfinite(result);
  }

  // "0xff" is 255. A bare "0x" is rejected rather than read as 0 times
  // something, and so is a digit run too long to hold exactly.
  bool ParseHex(Parsed& value) {
    pos_ += 2;
    double parsed = 0.0;
    size_t digits = 0;
    while (pos_ < text_.size()) {
      const wchar_t ch = text_[pos_];
      int digit = -1;
      if (ch >= L'0' && ch <= L'9') digit = ch - L'0';
      else if (ch >= L'a' && ch <= L'f') digit = ch - L'a' + 10;
      else if (ch >= L'A' && ch <= L'F') digit = ch - L'A' + 10;
      if (digit < 0) break;
      parsed = parsed * 16.0 + digit;
      ++digits;
      ++pos_;
    }
    if (digits == 0 || digits > kMaxHexDigits) return false;
    sawValue_ = true;
    sawCalculationSyntax_ = true;
    value = Parsed{parsed, false};
    return true;
  }

  bool ParseNumber(Parsed& value) {
    std::wstring number;
    if (!ReadDecimal(text_, pos_, decimal_, number)) return false;
    ReadExponent(text_, pos_, number);
    sawValue_ = true;
    wchar_t* end = nullptr;
    value.value = std::wcstod(number.c_str(), &end);
    value.percent = false;
    return end && *end == L'\0' && std::isfinite(value.value);
  }

  void SkipSpaces() {
    while (pos_ < text_.size() && std::iswspace(text_[pos_])) ++pos_;
  }

  bool Match(wchar_t expected) {
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool Match(const wchar_t* expected) {
    if (!Peek(expected)) return false;
    pos_ += std::wcslen(expected);
    return true;
  }

  bool Peek(const wchar_t* expected) const {
    const size_t len = std::wcslen(expected);
    return pos_ + len <= text_.size() && text_.compare(pos_, len, expected) == 0;
  }

  std::wstring text_;
  wchar_t decimal_ = L'.';
  size_t pos_ = 0;
  int depth_ = 0;
  bool sawValue_ = false;
  bool sawCalculationSyntax_ = false;
};

// `decimalSeparator` is the locale's decimal point ('.' or ','); tests pass
// it explicitly.
inline std::optional<Result> TryEvaluate(
    std::wstring input, wchar_t decimalSeparator = UserDecimalSeparator()) {
  input = Trim(std::move(input));
  if (input.empty()) return std::nullopt;
  if (input.front() == L'=') input = Trim(input.substr(1));
  if (input.empty() || LooksLikeTimeOrDate(input)) return std::nullopt;

  double value = 0.0;
  Parser parser(input, decimalSeparator);
  if (!parser.Parse(value)) return std::nullopt;

  std::wstring display = FormatNumber(value);
  if (display.empty()) return std::nullopt;
  return Result{input, display, value};
}

}  // namespace feathercast::calculator
