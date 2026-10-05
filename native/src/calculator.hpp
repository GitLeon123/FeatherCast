#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iomanip>
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

inline std::wstring FormatNumber(double value) {
  if (!std::isfinite(value)) return L"";
  if (std::fabs(value) < 0.0000000001) value = 0.0;

  std::wostringstream stream;
  const double magnitude = std::fabs(value);
  if (magnitude >= 10000000000.0 || (magnitude > 0.0 && magnitude < 0.000001)) {
    stream << std::setprecision(10) << value;
    return stream.str();
  }

  stream << std::fixed << std::setprecision(10) << value;
  std::wstring out = stream.str();
  while (!out.empty() && out.back() == L'0') out.pop_back();
  if (!out.empty() && out.back() == L'.') out.pop_back();
  if (out == L"-0") out = L"0";
  return out;
}

class Parser {
 public:
  explicit Parser(std::wstring text) : text_(std::move(text)) {}

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
  // is not a calculation. Hex needs at least one digit after "0x".
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
        value.value += rhs.percent ? base * rhs.value : rhs.value;
        value.percent = false;
      } else if (Match(L'-')) {
        sawCalculationSyntax_ = true;
        Parsed rhs;
        if (!ParseTerm(rhs)) return false;
        const double base = value.value;
        value.value -= rhs.percent ? base * rhs.value : rhs.value;
        value.percent = false;
      } else {
        return true;
      }
    }
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
        if (!ParseUnary(rhs) || std::fabs(rhs.value) < 0.0000000001) return false;
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
    if (name != L"sin" && name != L"cos" && name != L"tan" && name != L"sqrt") {
      return false;
    }

    SkipSpaces();
    if (!Match(L'(')) return false;
    sawCalculationSyntax_ = true;
    Parsed argument;
    if (!ParseExpression(argument)) return false;
    SkipSpaces();
    if (!Match(L')')) return false;

    if (name == L"sqrt") {
      if (argument.value < 0.0) return false;
      value = Parsed{std::sqrt(argument.value), false};
      return true;
    }
    const double radians = argument.value * kPi / 180.0;
    if (name == L"sin") value.value = std::sin(radians);
    else if (name == L"cos") value.value = std::cos(radians);
    else value.value = std::tan(radians);
    value.percent = false;
    return std::isfinite(value.value);
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
    const size_t start = pos_;
    bool hasDigit = false;
    bool hasSeparator = false;
    while (pos_ < text_.size()) {
      const wchar_t ch = text_[pos_];
      if (ch >= L'0' && ch <= L'9') {
        hasDigit = true;
        ++pos_;
      } else if ((ch == L'.' || ch == L',') && !hasSeparator) {
        hasSeparator = true;
        ++pos_;
      } else {
        break;
      }
    }

    if (!hasDigit) return false;
    sawValue_ = true;
    std::wstring number = text_.substr(start, pos_ - start);
    std::replace(number.begin(), number.end(), L',', L'.');
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
  size_t pos_ = 0;
  int depth_ = 0;
  bool sawValue_ = false;
  bool sawCalculationSyntax_ = false;
};

inline std::optional<Result> TryEvaluate(std::wstring input) {
  input = Trim(std::move(input));
  if (input.empty()) return std::nullopt;
  if (input.front() == L'=') input = Trim(input.substr(1));
  if (input.empty()) return std::nullopt;

  double value = 0.0;
  Parser parser(input);
  if (!parser.Parse(value)) return std::nullopt;

  std::wstring display = FormatNumber(value);
  if (display.empty()) return std::nullopt;
  return Result{input, display, value};
}

}  // namespace feathercast::calculator
