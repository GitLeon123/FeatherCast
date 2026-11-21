#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace feathercast::core {

struct SearchItem {
  std::wstring id;
  std::wstring path;
  std::wstring kind;
  std::wstring source;
  std::wstring name;
  std::wstring processName;
  std::wstring targetPath;
  std::wstring launchTarget;
  std::wstring exe;
  std::vector<std::wstring> keywords;
  std::vector<std::wstring> aliases;
  bool systemEssential = false;
  bool pinned = false;
  int usageCount = 0;
  long long lastUsed = 0;
};

inline std::wstring Trim(std::wstring value) {
  auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
  auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return std::iswspace(ch) != 0; }).base();
  if (first >= last) return L"";
  return std::wstring(first, last);
}

inline std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

inline std::wstring LowerInvariant(std::wstring_view value) {
  if (value.empty()) return L"";
  const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr, 0);
  if (needed <= 0) return Lower(std::wstring(value));
  std::wstring out(static_cast<size_t>(needed), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                    value.data(), static_cast<int>(value.size()),
                    out.data(), needed, nullptr, nullptr, 0) <= 0) {
    return Lower(std::wstring(value));
  }
  return out;
}

// Stroke and dotless letters have no NFD decomposition, so they keep their
// mark. Map the common ones to the base letter, preserving case so the folded
// text still lines up with BoundaryText.
inline wchar_t FoldStrokeLetter(wchar_t ch) {
  switch (ch) {
    case L'Ø': return L'O';  // Ø
    case L'ø': return L'o';  // ø
    case L'Đ': return L'D';  // Đ
    case L'đ': return L'd';  // đ
    case L'Ħ': return L'H';  // Ħ
    case L'ħ': return L'h';  // ħ
    case L'ı': return L'i';  // ı
    case L'Ŀ': case L'Ł': return L'L';  // Ŀ Ł
    case L'ŀ': case L'ł': case L'ƚ': return L'l';  // ŀ ł ƚ
    case L'Ŧ': return L'T';  // Ŧ
    case L'ŧ': return L't';  // ŧ
    case L'ƀ': return L'b';  // ƀ
    case L'Ɨ': return L'I';  // Ɨ
    case L'Ǥ': return L'G';  // Ǥ
    case L'ǥ': return L'g';  // ǥ
    default: return ch;
  }
}

inline std::wstring FoldDiacritics(std::wstring_view value) {
  if (value.empty()) return L"";
  const int needed = NormalizeString(NormalizationD, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (needed <= 0) return std::wstring(value);
  std::wstring decomposed(static_cast<size_t>(needed), L'\0');
  const int written = NormalizeString(NormalizationD, value.data(), static_cast<int>(value.size()),
                                      decomposed.data(), needed);
  if (written <= 0) return std::wstring(value);
  decomposed.resize(static_cast<size_t>(written));

  std::array<WORD, 256> stackTypes{};
  std::vector<WORD> heapTypes;
  WORD* types = stackTypes.data();
  if (decomposed.size() > stackTypes.size()) {
    heapTypes.resize(decomposed.size());
    types = heapTypes.data();
  }
  const bool haveTypes =
      GetStringTypeW(CT_CTYPE3, decomposed.data(),
                     static_cast<int>(decomposed.size()), types) != FALSE;

  std::wstring out;
  out.reserve(decomposed.size());
  for (size_t i = 0; i < decomposed.size(); ++i) {
    const wchar_t ch = decomposed[i];
    if (const wchar_t base = FoldStrokeLetter(ch); base != ch) {
      out.push_back(base);
      continue;
    }
    // Windows also flags real letters (ø, ł, ı) as diacritics; only marks
    // that are not letters themselves are dropped.
    if (haveTypes &&
        (types[i] & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0 &&
        (types[i] & C3_ALPHA) == 0) {
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

// Trims `value` in place and, with `collapse`, turns every internal
// whitespace run into a single space.
inline void TrimWhitespaceInPlace(std::wstring& value, bool collapse = true) {
  size_t length = 0;
  bool pendingSpace = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const wchar_t ch = value[i];
    if (std::iswspace(ch)) {
      if (length != 0) {
        if (!collapse) value[length++] = ch;
        pendingSpace = collapse;
      }
      continue;
    }
    if (pendingSpace) {
      value[length++] = L' ';
      pendingSpace = false;
    }
    value[length++] = ch;
  }
  if (!collapse) {
    while (length != 0 && std::iswspace(value[length - 1])) --length;
  }
  value.resize(length);
}

// Folds before lowercasing so "İ" (I + combining dot) ends up as "i".
inline std::wstring NormalizeText(std::wstring_view value,
                                  bool collapseWhitespace) {
  // Plain ASCII can bypass the Windows Unicode calls. Windows classifies the
  // caret and grave accent as diacritics, so those stay on the Unicode path to
  // preserve the existing folding rules even in otherwise ASCII text.
  std::wstring normalized;
  if (std::all_of(value.begin(), value.end(),
                  [](wchar_t ch) {
                    return ch < 0x80 && ch != L'^' && ch != L'`';
                  })) {
    normalized.assign(value);
    for (auto& ch : normalized) {
      if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    }
  } else {
    normalized = LowerInvariant(FoldDiacritics(value));
  }
  TrimWhitespaceInPlace(normalized, collapseWhitespace);
  return normalized;
}

inline std::wstring Normalize(std::wstring_view value) {
  return NormalizeText(value, true);
}

enum class AliasValidationError : unsigned char {
  None,
  Empty,
  Multiline,
  ReservedPrefix,
};

struct AliasValidationResult {
  bool valid = false;
  AliasValidationError error = AliasValidationError::Empty;
  std::wstring value;
  std::wstring normalized;
};

inline std::wstring NormalizeAlias(const std::wstring& value) {
  return Normalize(value);
}

inline AliasValidationResult ValidateAlias(const std::wstring& value) {
  if (value.find_first_of(L"\r\n") != std::wstring::npos) {
    return {false, AliasValidationError::Multiline, {}, {}};
  }
  std::wstring trimmed = Trim(value);
  if (trimmed.empty()) {
    return {false, AliasValidationError::Empty, {}, {}};
  }
  if (trimmed.front() == L'@' || trimmed.front() == L'>' ||
      trimmed.front() == L':') {
    return {false, AliasValidationError::ReservedPrefix, {}, {}};
  }
  std::wstring normalized = NormalizeAlias(trimmed);
  if (normalized.empty()) {
    return {false, AliasValidationError::Empty, {}, {}};
  }
  return {true, AliasValidationError::None, std::move(trimmed),
          std::move(normalized)};
}

inline bool AliasesEqual(const std::wstring& left,
                         const std::wstring& right) {
  const auto leftValidation = ValidateAlias(left);
  const auto rightValidation = ValidateAlias(right);
  return leftValidation.valid && rightValidation.valid &&
         leftValidation.normalized == rightValidation.normalized;
}

inline bool SetUniqueAlias(
    std::map<std::wstring, std::wstring>& aliases,
    const std::wstring& targetId, const std::wstring& alias) {
  const std::wstring trimmedTarget = Trim(targetId);
  const auto validation = ValidateAlias(alias);
  if (trimmedTarget.empty() || !validation.valid) return false;
  for (const auto& [existingTarget, existingAlias] : aliases) {
    if (existingTarget != trimmedTarget &&
        NormalizeAlias(existingAlias) == validation.normalized) {
      return false;
    }
  }
  aliases[trimmedTarget] = validation.value;
  return true;
}

inline std::wstring StableInvocationKey(std::wstring_view kind,
                                        const std::wstring& stableId) {
  // Persisted keys keep internal whitespace as it was; only queries and
  // display text collapse it.
  const std::wstring normalized = NormalizeText(stableId, false);
  if (kind.empty() || normalized.empty()) return L"";
  return std::wstring(kind) + L":" + normalized;
}

struct TokenSpan {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  [[nodiscard]] std::wstring_view View(std::wstring_view text) const noexcept {
    return text.substr(offset, length);
  }
};

inline bool IsAlnumFast(wchar_t ch) noexcept {
  if (ch < 0x80) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') ||
           (ch >= L'A' && ch <= L'Z');
  }
  return std::iswalnum(ch) != 0;
}

inline std::vector<TokenSpan> TokenSpansNormalized(std::wstring_view value) {
  std::vector<TokenSpan> out;
  size_t i = 0;
  const size_t n = value.size();
  while (i < n) {
    while (i < n && !IsAlnumFast(value[i])) ++i;
    const size_t start = i;
    while (i < n && IsAlnumFast(value[i])) ++i;
    if (i > start) {
      out.push_back({static_cast<std::uint32_t>(start),
                     static_cast<std::uint32_t>(i - start)});
    }
  }
  return out;
}

inline std::vector<std::wstring> TokensNormalized(std::wstring_view value) {
  std::vector<std::wstring> out;
  const auto spans = TokenSpansNormalized(value);
  out.reserve(spans.size());
  for (const auto& span : spans) {
    out.emplace_back(span.View(value));
  }
  return out;
}

inline std::vector<std::wstring> Tokens(std::wstring_view value) {
  return TokensNormalized(Normalize(value));
}

inline std::wstring Acronym(std::wstring_view value) {
  std::wstring out;
  for (const auto& token : Tokens(value)) {
    if (!token.empty()) out.push_back(token.front());
  }
  return out;
}

// Case-preserved copy of `value` whose indices line up with `normalized`
// (Normalize(value)), so BoundaryBefore can see camelCase humps at positions
// found in the normalized text. Normalization can change the length (Hangul
// syllables decompose into jamo, precomposed accents lose their marks,
// whitespace runs collapse), so the same decomposition, mark stripping and
// whitespace collapsing is applied without the case mapping. If the lengths
// still disagree, the normalized text is the safe fallback: it keeps every
// word separator, only camelCase boundaries are lost.
inline std::wstring BoundaryText(const std::wstring& value,
                                 const std::wstring& normalized) {
  std::wstring trimmed = value;
  TrimWhitespaceInPlace(trimmed);
  if (trimmed.size() == normalized.size() &&
      std::all_of(trimmed.begin(), trimmed.end(),
                  [](wchar_t ch) { return ch < 0x80; })) {
    // ASCII never decomposes; equal lengths mean no mark was stripped.
    return trimmed;
  }
  std::wstring folded = FoldDiacritics(trimmed);
  TrimWhitespaceInPlace(folded);
  if (folded.size() != normalized.size()) return normalized;
  return folded;
}

inline bool BoundaryBefore(std::wstring_view text, size_t index) noexcept {
  if (index == 0) return true;
  if (index >= text.size()) return false;
  const wchar_t ch = text[index - 1];
  if (ch < 0x80) {
    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' ||
        ch == L'\v' || ch == L'\f' || ch == L'.' || ch == L'_' ||
        ch == L'-' || ch == L'\\' || ch == L'/') {
      return true;
    }
    const wchar_t next = text[index];
    if (next < 0x80) {
      return (ch >= L'a' && ch <= L'z') && (next >= L'A' && next <= L'Z');
    }
    return (ch >= L'a' && ch <= L'z') && std::iswupper(next) != 0;
  }
  if (std::iswspace(ch)) return true;
  return std::iswlower(ch) && std::iswupper(text[index]);
}

inline int DamerauLevenshteinDistance(std::wstring_view a, std::wstring_view b, int maxDistance) {
  const int m = static_cast<int>(a.size());
  const int n = static_cast<int>(b.size());
  if (std::abs(m - n) > maxDistance) return maxDistance + 1;
  if (m == 0) return n;
  if (n == 0) return m;

  const size_t columns = static_cast<size_t>(n) + 1;
  constexpr size_t kInlineColumns = 64;
  if (columns <= kInlineColumns && maxDistance <= 250) {
    std::array<std::array<std::uint8_t, kInlineColumns>, 3> inlineRows;
    std::uint8_t* previousPrevious = inlineRows[0].data();
    std::uint8_t* previous = inlineRows[1].data();
    std::uint8_t* current = inlineRows[2].data();
    const auto outsideBand = static_cast<std::uint8_t>(maxDistance + 1);
    const int initLast = std::min(n, maxDistance);
    for (int j = 0; j <= initLast; ++j) previous[j] = static_cast<std::uint8_t>(j);
    if (initLast < n) previous[initLast + 1] = outsideBand;

    for (int i = 1; i <= m; ++i) {
      std::uint8_t rowBest = outsideBand;
      const int firstColumn = std::max(1, i - maxDistance);
      const int lastColumn = std::min(n, i + maxDistance);
      current[0] = i <= maxDistance ? static_cast<std::uint8_t>(i) : outsideBand;
      if (firstColumn > 1) current[firstColumn - 1] = outsideBand;
      if (lastColumn < n) current[lastColumn + 1] = outsideBand;
      const wchar_t ai = a[static_cast<size_t>(i - 1)];
      for (int j = firstColumn; j <= lastColumn; ++j) {
        const wchar_t bj = b[static_cast<size_t>(j - 1)];
        const std::uint8_t cost = ai == bj ? 0 : 1;
        std::uint8_t value = static_cast<std::uint8_t>(previous[j] + 1);
        const std::uint8_t ins = static_cast<std::uint8_t>(current[j - 1] + 1);
        if (ins < value) value = ins;
        const std::uint8_t sub = static_cast<std::uint8_t>(previous[j - 1] + cost);
        if (sub < value) value = sub;
        if (i > 1 && j > 1 && ai == b[static_cast<size_t>(j - 2)] &&
            a[static_cast<size_t>(i - 2)] == bj) {
          const std::uint8_t trans =
              static_cast<std::uint8_t>(previousPrevious[j - 2] + 1);
          if (trans < value) value = trans;
        }
        if (value > outsideBand) value = outsideBand;
        current[j] = value;
        if (value < rowBest) rowBest = value;
      }
      if (rowBest > maxDistance && i > maxDistance + 1) return maxDistance + 1;
      std::swap(previousPrevious, previous);
      std::swap(previous, current);
    }
    return previous[n];
  }

  thread_local std::vector<int> largeRows;
  largeRows.resize(columns * 3);
  int* previousPrevious = largeRows.data();
  int* previous = previousPrevious + columns;
  int* current = previous + columns;
  const int outsideBand = maxDistance + 1;
  std::fill_n(previousPrevious, columns, outsideBand);
  std::fill_n(previous, columns, outsideBand);
  std::fill_n(current, columns, outsideBand);
  for (int j = 0; j <= n; ++j) {
    if (j <= maxDistance) previous[j] = j;
  }

  for (int i = 1; i <= m; ++i) {
    int rowBest = maxDistance + 1;
    const int firstColumn = std::max(1, i - maxDistance);
    const int lastColumn = std::min(n, i + maxDistance);
    current[0] = i <= maxDistance ? i : outsideBand;
    if (firstColumn > 1) current[firstColumn - 1] = outsideBand;
    if (lastColumn < n) current[lastColumn + 1] = outsideBand;
    for (int j = firstColumn; j <= lastColumn; ++j) {
      const int cost = a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 1)] ? 0 : 1;
      int value = std::min({
        previous[j] + 1,
        current[j - 1] + 1,
        previous[j - 1] + cost,
      });
      if (i > 1 && j > 1 &&
          a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 2)] &&
          a[static_cast<size_t>(i - 2)] == b[static_cast<size_t>(j - 1)]) {
        value = std::min(value, previousPrevious[j - 2] + 1);
      }
      value = std::min(value, outsideBand);
      current[j] = value;
      rowBest = std::min(rowBest, value);
    }
    if (rowBest > maxDistance && i > maxDistance + 1) return maxDistance + 1;
    std::swap(previousPrevious, previous);
    std::swap(previous, current);
  }

  return previous[n];
}

inline double ScoreText(const std::wstring& query, const std::wstring& target) {
  const std::wstring q = Normalize(query);
  const std::wstring t = Normalize(target);
  // Case-preserved copy (same indices as t) so BoundaryBefore can see camelCase.
  const std::wstring raw = BoundaryText(target, t);
  if (q.empty() || t.empty()) return -1;
  if (t == q) return 5000;
  if (t.rfind(q, 0) == 0) return 3200 - std::min<int>(static_cast<int>(t.size() - q.size()), 200);

  const std::wstring acronym = Acronym(t);
  if (!acronym.empty()) {
    if (acronym == q) return 3000;
    if (acronym.rfind(q, 0) == 0) return 2300 - std::min<int>(static_cast<int>(acronym.size() - q.size()), 100);
  }

  const size_t substring = t.find(q);
  if (substring != std::wstring::npos) {
    return (BoundaryBefore(raw, substring) ? 2400 : 1800) - static_cast<double>(substring);
  }

  if (q.size() > 4) {
    const int maxDistance = q.size() >= 8 ? 2 : 1;
    int bestDistance = maxDistance + 1;
    int bestLengthDelta = maxDistance + 1;
    for (const auto& token : Tokens(t)) {
      const int lengthDelta = std::abs(static_cast<int>(token.size()) - static_cast<int>(q.size()));
      if (lengthDelta > maxDistance) continue;
      const int distance = DamerauLevenshteinDistance(q, token, maxDistance);
      if (distance < bestDistance || (distance == bestDistance && lengthDelta < bestLengthDelta)) {
        bestDistance = distance;
        bestLengthDelta = lengthDelta;
      }
    }
    if (bestDistance <= maxDistance) {
      return 1550.0 - static_cast<double>(bestDistance * 220 + bestLengthDelta * 15);
    }
  }

  size_t qi = 0;
  double score = 0;
  int previous = -2;
  for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
    if (t[ti] == q[qi]) {
      score += 16;
      if (static_cast<int>(ti) == previous + 1) score += 12;
      if (BoundaryBefore(raw, ti)) score += 10;
      previous = static_cast<int>(ti);
      ++qi;
    }
  }
  if (qi < q.size()) return -1;
  return score - std::max<int>(0, static_cast<int>(t.size() - q.size()));
}

inline std::wstring JoinKeywords(const std::vector<std::wstring>& keywords);
inline double UsageBoost(int usageCount, long long lastUsed, long long now);

enum class MatchClass : unsigned char {
  None = 0,
  General = 1,
  Typo = 2,
  FieldPrefix = 3,
  NameBoundary = 4,
  NamePrefix = 5,
  ExactName = 6,
};

enum class SearchFieldKind : unsigned char {
  Name,
  Alias,
  Keywords,
  Process,
  Path,
};

inline std::uint64_t CharBit(wchar_t ch) noexcept {
  if (ch >= L'a' && ch <= L'z') {
    return std::uint64_t{1} << static_cast<unsigned>(ch - L'a');
  }
  if (ch >= L'0' && ch <= L'9') {
    return std::uint64_t{1} << static_cast<unsigned>(26 + (ch - L'0'));
  }
  return std::uint64_t{1} << static_cast<unsigned>(36 + (static_cast<unsigned>(ch) % 28));
}

inline std::uint64_t BuildCharMask(std::wstring_view text) noexcept {
  std::uint64_t mask = 0;
  for (const wchar_t ch : text) mask |= CharBit(ch);
  return mask;
}

inline std::uint64_t TokenLengthWindowMask(size_t queryLen, int maxDistance) noexcept {
  const size_t lo = queryLen > static_cast<size_t>(maxDistance)
                        ? queryLen - static_cast<size_t>(maxDistance)
                        : 0;
  const size_t hi = queryLen + static_cast<size_t>(maxDistance);
  std::uint64_t mask = 0;
  if (lo >= 63) return std::uint64_t{1} << 63;
  const size_t cappedHi = std::min<size_t>(hi, 63);
  for (size_t len = lo; len <= cappedHi; ++len) {
    mask |= std::uint64_t{1} << static_cast<unsigned>(len);
  }
  return mask;
}

struct PreparedField {
  // Case-preserved text indexed like `normalized` (see BoundaryText).
  std::wstring raw;
  std::wstring normalized;
  std::vector<TokenSpan> tokens;
  std::wstring acronym;
  std::uint64_t charMask = 0;
  std::uint64_t tokenLenMask = 0;
  double weight = 1.0;
  SearchFieldKind kind = SearchFieldKind::Name;
};

struct PreparedSearchItem {
  SearchItem item;
  std::vector<PreparedField> fields;
  std::wstring normalizedName;
  std::vector<std::wstring> normalizedAliases;
  std::wstring lowerName;
  bool isWindow = false;
  bool isAliasSource = false;
};

struct SearchOptions {
  size_t limit = SIZE_MAX;
  long long now = 0;
  unsigned long long generation = 0;
  const std::atomic<unsigned long long>* latestGeneration = nullptr;
  // Zero selects a bounded automatic worker count. A caller that shares a
  // machine with the UI can force a smaller cap and avoid transient worker
  // oversubscription on large corpora.
  size_t maxWorkers = 0;
  // Optional eligible corpus indices. The caller keeps this list alive until
  // SearchPrepared returns; results still refer to the original corpus.
  const std::vector<size_t>* candidateIndices = nullptr;
};

inline std::wstring AcronymFromTokens(
    const std::vector<std::wstring>& tokens) {
  std::wstring out;
  out.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (!token.empty()) out.push_back(token.front());
  }
  return out;
}

inline std::wstring AcronymFromSpans(
    std::wstring_view text, const std::vector<TokenSpan>& spans) {
  std::wstring out;
  out.reserve(spans.size());
  for (const auto& span : spans) {
    if (span.length > 0) out.push_back(text[span.offset]);
  }
  return out;
}

inline PreparedField PrepareField(std::wstring text, double weight,
                                  SearchFieldKind kind) {
  PreparedField field;
  const bool fastAscii = std::all_of(
      text.begin(), text.end(),
      [](wchar_t ch) { return ch < 0x80 && ch != L'^' && ch != L'`'; });
  if (fastAscii) {
    field.raw.reserve(text.size());
    field.normalized.reserve(text.size());
    bool pendingSpace = false;
    for (const wchar_t ch : text) {
      if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' ||
          ch == L'\v' || ch == L'\f') {
        if (!field.raw.empty()) pendingSpace = true;
        continue;
      }
      if (pendingSpace) {
        field.raw.push_back(L' ');
        field.normalized.push_back(L' ');
        pendingSpace = false;
      }
      field.raw.push_back(ch);
      field.normalized.push_back(
          (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch);
    }
  } else {
    field.normalized = Normalize(text);
    field.raw = BoundaryText(text, field.normalized);
  }
  field.tokens = TokenSpansNormalized(field.normalized);
  field.charMask = BuildCharMask(field.normalized);
  std::uint64_t lenMask = 0;
  for (const auto& span : field.tokens) {
    lenMask |= std::uint64_t{1} << std::min<unsigned>(span.length, 63u);
  }
  field.tokenLenMask = lenMask;
  if (kind == SearchFieldKind::Name || kind == SearchFieldKind::Alias) {
    field.acronym = AcronymFromSpans(field.normalized, field.tokens);
  }
  field.weight = weight;
  field.kind = kind;
  return field;
}

inline PreparedSearchItem PrepareSearchItem(SearchItem item) {
  PreparedSearchItem prepared;
  prepared.item = std::move(item);
  const auto& stored = prepared.item;
  prepared.isWindow = stored.kind == L"window";
  prepared.isAliasSource = stored.source == L"alias";
  prepared.lowerName = Lower(stored.name);
  prepared.fields.reserve(1 + stored.aliases.size() +
                          (!stored.keywords.empty() ? 1 : 0) +
                          (!stored.processName.empty() ? 1 : 0) +
                          (!stored.targetPath.empty() || !stored.launchTarget.empty() ||
                                   !stored.exe.empty() ? 1 : 0));
  prepared.fields.push_back(
      PrepareField(stored.name, 1.0, SearchFieldKind::Name));
  prepared.normalizedName = prepared.fields.front().normalized;
  for (const auto& alias : stored.aliases) {
    const auto validation = ValidateAlias(alias);
    if (!validation.valid) continue;
    prepared.normalizedAliases.push_back(validation.normalized);
    prepared.fields.push_back(
        PrepareField(validation.value, 1.0, SearchFieldKind::Alias));
  }
  // Absent fields cannot match. In particular, static catalogs have no
  // process or path; retaining those fields costs memory and scoring work.
  const auto addField = [&](std::wstring text, double weight,
                            SearchFieldKind kind) {
    if (text.empty()) return;
    auto field = PrepareField(std::move(text), weight, kind);
    if (!field.normalized.empty()) {
      prepared.fields.push_back(std::move(field));
    }
  };
  addField(JoinKeywords(stored.keywords), 0.82, SearchFieldKind::Keywords);
  addField(stored.processName, 0.7, SearchFieldKind::Process);
  addField(!stored.targetPath.empty()
               ? stored.targetPath
               : (!stored.launchTarget.empty() ? stored.launchTarget : stored.exe),
           0.45, SearchFieldKind::Path);
  return prepared;
}

struct TextMatch {
  double score = -1.0;
  MatchClass matchClass = MatchClass::None;

  bool Matched() const { return matchClass != MatchClass::None; }
};

inline MatchClass ExactOrPrefixClass(SearchFieldKind kind, bool exact) {
  if (kind == SearchFieldKind::Name || kind == SearchFieldKind::Alias) {
    return exact ? MatchClass::ExactName : MatchClass::NamePrefix;
  }
  if (kind == SearchFieldKind::Keywords || kind == SearchFieldKind::Process) {
    return MatchClass::FieldPrefix;
  }
  return MatchClass::General;
}

struct PreparedQueryToken {
  std::wstring_view text;
  std::uint64_t charMask = 0;
  std::uint64_t lenWindowMask = 0;
  int maxDistance = 0;
};

inline PreparedQueryToken MakePreparedQueryToken(std::wstring_view token) noexcept {
  PreparedQueryToken out;
  out.text = token;
  out.charMask = BuildCharMask(token);
  if (token.size() > 4) {
    out.maxDistance = token.size() >= 8 ? 2 : 1;
    out.lenWindowMask = TokenLengthWindowMask(token.size(), out.maxDistance);
  }
  return out;
}

inline TextMatch MatchPreparedTextWithMask(const PreparedQueryToken& queryToken,
                                           const PreparedField& target,
                                           bool allowApproximate = true) {
  const std::wstring_view q = queryToken.text;
  const std::wstring& t = target.normalized;
  const std::wstring& raw = target.raw;
  if (q.empty() || t.empty()) return {};

  const std::uint64_t missingChars = queryToken.charMask & ~target.charMask;
  if (missingChars == 0) {
    if (t == q) return {5000.0, ExactOrPrefixClass(target.kind, true)};
    if (t.size() >= q.size() && t.compare(0, q.size(), q) == 0) {
      return {3200.0 -
                  std::min<int>(static_cast<int>(t.size() - q.size()), 200),
              ExactOrPrefixClass(target.kind, false)};
    }

    if ((target.kind == SearchFieldKind::Name ||
         target.kind == SearchFieldKind::Alias) &&
        !target.acronym.empty()) {
      if (target.acronym == q) return {3000.0, MatchClass::NamePrefix};
      if (target.acronym.size() >= q.size() &&
          target.acronym.compare(0, q.size(), q) == 0) {
        return {2300.0 - std::min<int>(
                             static_cast<int>(target.acronym.size() - q.size()),
                             100),
                MatchClass::NameBoundary};
      }
    }

    const size_t substring = t.find(q);
    if (substring != std::wstring::npos) {
      const bool boundary = BoundaryBefore(raw, substring);
      MatchClass matchClass = MatchClass::General;
      if ((target.kind == SearchFieldKind::Name ||
           target.kind == SearchFieldKind::Alias) &&
          boundary) {
        matchClass = MatchClass::NameBoundary;
      }
      return {(boundary ? 2400.0 : 1800.0) -
                  static_cast<double>(substring),
              matchClass};
    }
  }

  if (!allowApproximate) return {};

  if (queryToken.maxDistance > 0 &&
      (target.tokenLenMask & queryToken.lenWindowMask) != 0 &&
      std::popcount(missingChars) <= queryToken.maxDistance) {
    const int maxDistance = queryToken.maxDistance;
    int currentMaxDist = maxDistance;
    int bestDistance = maxDistance + 1;
    int bestLengthDelta = maxDistance + 1;
    for (const auto& span : target.tokens) {
      const int lengthDelta = std::abs(static_cast<int>(span.length) -
                                       static_cast<int>(q.size()));
      if (lengthDelta > currentMaxDist) continue;
      const int distance =
          DamerauLevenshteinDistance(q, span.View(t), currentMaxDist);
      if (distance < bestDistance ||
          (distance == bestDistance && lengthDelta < bestLengthDelta)) {
        bestDistance = distance;
        bestLengthDelta = lengthDelta;
        if (bestDistance <= 1) {
          currentMaxDist = 1;
          if (bestLengthDelta == 0) break;
        }
      }
    }
    if (bestDistance <= maxDistance) {
      return {1550.0 - static_cast<double>(bestDistance * 220 +
                                           bestLengthDelta * 15),
              target.kind == SearchFieldKind::Path ? MatchClass::General
                                                   : MatchClass::Typo};
    }
  }

  if (missingChars != 0) return {};

  size_t qi = 0;
  double score = 0.0;
  int previous = -2;
  for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
    if (t[ti] == q[qi]) {
      score += 16.0;
      if (static_cast<int>(ti) == previous + 1) score += 12.0;
      if (BoundaryBefore(raw, ti)) score += 10.0;
      previous = static_cast<int>(ti);
      ++qi;
    }
  }
  if (qi < q.size()) return {};
  return {score -
              std::max<int>(0, static_cast<int>(t.size() - q.size())),
          MatchClass::General};
}

inline TextMatch MatchPreparedText(const std::wstring& normalizedQuery,
                                   const PreparedField& target,
                                   bool allowApproximate = true) {
  return MatchPreparedTextWithMask(MakePreparedQueryToken(normalizedQuery),
                                   target, allowApproximate);
}

struct ItemScore {
  MatchClass matchClass = MatchClass::None;
  double text = -1.0;
  double personalization = 0.0;

  bool Matched() const { return matchClass != MatchClass::None; }
  double RankedValue() const {
    if (!Matched()) return -1.0;
    return static_cast<double>(matchClass) * 100000.0 + text +
           personalization;
  }
};

inline bool BetterItemScore(const ItemScore& a, const ItemScore& b) {
  if (a.matchClass != b.matchClass) return a.matchClass > b.matchClass;
  const double aCombined = a.text + a.personalization;
  const double bCombined = b.text + b.personalization;
  if (aCombined != bCombined) return aCombined > bCombined;
  return a.text > b.text;
}

inline MatchClass MaximumFieldMatchClass(SearchFieldKind kind) {
  switch (kind) {
    case SearchFieldKind::Name:
    case SearchFieldKind::Alias: return MatchClass::ExactName;
    case SearchFieldKind::Keywords:
    case SearchFieldKind::Process: return MatchClass::FieldPrefix;
    case SearchFieldKind::Path: return MatchClass::General;
  }
  return MatchClass::ExactName;
}

inline ItemScore ScorePreparedItemDetailedPrepared(
    const PreparedQueryToken& normalizedQueryToken,
    const std::vector<PreparedQueryToken>& queryTokens,
    const PreparedSearchItem& prepared,
    const std::set<std::wstring>& recentIds, long long now = 0) {
  const std::wstring_view normalizedQuery = normalizedQueryToken.text;
  double textScore = 0.0;
  MatchClass weakestClass = MatchClass::ExactName;
  if (queryTokens.empty()) {
    // A query without letters or digits ("." or "-") has no words to match.
    // Only a literal occurrence in a name, alias, keyword or process name
    // counts; nearly every path contains dots and separators.
    if (normalizedQuery.empty()) return {};
    TextMatch best;
    for (const auto& field : prepared.fields) {
      if (field.kind == SearchFieldKind::Path) continue;
      if (MaximumFieldMatchClass(field.kind) < best.matchClass) continue;
      TextMatch candidate =
          MatchPreparedTextWithMask(normalizedQueryToken, field, false);
      if (!candidate.Matched()) continue;
      candidate.score *= field.weight;
      if (!best.Matched() || candidate.matchClass > best.matchClass ||
          (candidate.matchClass == best.matchClass &&
           candidate.score > best.score)) {
        best = candidate;
      }
    }
    if (!best.Matched()) return {};
    weakestClass = best.matchClass;
    textScore = best.score;
  }
  for (const auto& token : queryTokens) {
    TextMatch best;
    for (size_t fieldIndex = 0; fieldIndex < prepared.fields.size();
         ++fieldIndex) {
      const auto& field = prepared.fields[fieldIndex];
      if (MaximumFieldMatchClass(field.kind) < best.matchClass) continue;
      // Approximate matches never outrank a field prefix or a stronger name
      // match. Literal matches in later fields can still improve the result.
      TextMatch candidate = MatchPreparedTextWithMask(
          token, field, best.matchClass <= MatchClass::Typo);
      if (!candidate.Matched()) continue;
      candidate.score *= field.weight;
      if (!best.Matched() || candidate.matchClass > best.matchClass ||
          (candidate.matchClass == best.matchClass &&
           candidate.score > best.score)) {
        best = candidate;
      }
      if (fieldIndex == 0 && candidate.matchClass == MatchClass::Typo) {
        // A name typo beats approximate matches in all lower-weight fields.
        // Only an exact/prefix/boundary match in those fields can supersede it,
        // so avoid repeating edit-distance work for keywords, process and path.
        for (size_t strongIndex = 1; strongIndex < prepared.fields.size();
             ++strongIndex) {
          const auto& strongField = prepared.fields[strongIndex];
          if (MaximumFieldMatchClass(strongField.kind) < best.matchClass) continue;
          TextMatch strong =
              MatchPreparedTextWithMask(token, strongField, false);
          if (!strong.Matched()) continue;
          strong.score *= strongField.weight;
          if (strong.matchClass > best.matchClass ||
              (strong.matchClass == best.matchClass &&
               strong.score > best.score)) {
            best = strong;
          }
        }
        break;
      }
    }
    if (!best.Matched()) return {};
    weakestClass = std::min(weakestClass, best.matchClass);
    textScore += best.score;
  }

  if (prepared.normalizedName == normalizedQuery) {
    weakestClass = MatchClass::ExactName;
    textScore += 2500.0;
  } else if (prepared.normalizedName.size() >= normalizedQuery.size() &&
             prepared.normalizedName.compare(0, normalizedQuery.size(),
                                             normalizedQuery) == 0) {
    weakestClass = std::max(weakestClass, MatchClass::NamePrefix);
    textScore += 600.0;
  }
  for (const auto& normalizedAlias : prepared.normalizedAliases) {
    if (normalizedAlias == normalizedQuery) {
      weakestClass = MatchClass::ExactName;
      textScore += 2500.0;
      break;
    }
    if (normalizedAlias.size() >= normalizedQuery.size() &&
        normalizedAlias.compare(0, normalizedQuery.size(), normalizedQuery) == 0) {
      weakestClass = std::max(weakestClass, MatchClass::NamePrefix);
      textScore += 600.0;
      break;
    }
  }

  const auto& item = prepared.item;
  double personalization = 0.0;
  if (!recentIds.empty() &&
      (recentIds.contains(item.id) || recentIds.contains(item.path))) {
    personalization += 260.0;
  }
  if (item.pinned) personalization += 1000.0;
  if (item.usageCount > 0 || item.lastUsed > 0) {
    personalization += UsageBoost(item.usageCount, item.lastUsed, now);
  }
  if (prepared.isWindow) personalization += 120.0;
  if (prepared.isAliasSource) personalization += 90.0;
  if (item.systemEssential) personalization += 70.0;
  return {weakestClass, textScore, personalization};
}

inline ItemScore ScorePreparedItemDetailed(
    const std::wstring& normalizedQuery,
    const std::vector<std::wstring>& queryTokens,
    const PreparedSearchItem& prepared,
    const std::set<std::wstring>& recentIds, long long now = 0) {
  std::vector<PreparedQueryToken> preparedTokens;
  preparedTokens.reserve(queryTokens.size());
  for (const auto& token : queryTokens) {
    preparedTokens.push_back(MakePreparedQueryToken(token));
  }
  return ScorePreparedItemDetailedPrepared(
      MakePreparedQueryToken(normalizedQuery), preparedTokens, prepared,
      recentIds, now);
}

inline double ScorePreparedItem(const std::wstring& normalizedQuery,
                                const std::vector<std::wstring>& queryTokens,
                                const PreparedSearchItem& prepared,
                                const std::set<std::wstring>& recentIds,
                                long long now = 0) {
  return ScorePreparedItemDetailed(normalizedQuery, queryTokens, prepared,
                                   recentIds, now)
      .RankedValue();
}

struct PreparedSearchMatch {
  size_t index = 0;
  ItemScore score;
};

inline bool BetterPreparedMatch(const PreparedSearchMatch& a,
                                const PreparedSearchMatch& b,
                                const std::vector<PreparedSearchItem>& items) {
  if (a.score.matchClass != b.score.matchClass) {
    return a.score.matchClass > b.score.matchClass;
  }
  const double aCombined = a.score.text + a.score.personalization;
  const double bCombined = b.score.text + b.score.personalization;
  if (aCombined != bCombined) return aCombined > bCombined;
  if (a.score.text != b.score.text) return a.score.text > b.score.text;
  const auto& aName = items[a.index].lowerName;
  const auto& bName = items[b.index].lowerName;
  if (aName != bName) return aName < bName;
  return a.index < b.index;
}

namespace detail {

// Reusable, idle-reaping helper pool for parallel corpus scoring. Avoids
// per-query OS thread creation/teardown while returning to 0 idle threads
// after 5 seconds of inactivity.
class SearchWorkerPool {
 public:
  static constexpr size_t kMaxHelpers = 3;
  static constexpr auto kIdleTimeout = std::chrono::seconds(5);

  static SearchWorkerPool& Instance() {
    static SearchWorkerPool pool;
    return pool;
  }

  ~SearchWorkerPool() {
    std::lock_guard busyLock(busyMutex_);
    std::array<std::jthread, kMaxHelpers> toJoin;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      for (size_t i = 0; i < kMaxHelpers; ++i) {
        toJoin[i] = std::move(slots_[i].thread);
        slots_[i].active = false;
      }
    }
    workCv_.notify_all();
    for (auto& thread : toJoin) {
      if (thread.joinable()) thread.join();
    }
  }

  // Runs `fn(workerIndex)` for workerIndex in [0, workerCount), executing
  // workerIndex == 0 on the calling thread and [1, workerCount) on pooled
  // helper threads. Falls back inline if another search already owns the pool.
  template <typename Fn>
  void Run(size_t workerCount, Fn&& fn) {
    if (workerCount <= 1) {
      fn(0);
      return;
    }
    const size_t helpers = std::min(workerCount - 1, kMaxHelpers);
    std::unique_lock busyLock(busyMutex_, std::try_to_lock);
    if (!busyLock.owns_lock()) {
      for (size_t worker = 0; worker < workerCount; ++worker) fn(worker);
      return;
    }

    auto dispatch = [&](size_t worker) { fn(worker); };
    std::array<std::jthread, kMaxHelpers> retired;
    size_t spawnedHelpers = 0;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        busyLock.unlock();
        for (size_t worker = 0; worker < workerCount; ++worker) fn(worker);
        return;
      }
      for (size_t i = 0; i < helpers; ++i) {
        if (!slots_[i].active) {
          retired[i] = std::move(slots_[i].thread);
          slots_[i].lastBatch = batchId_;
          try {
            slots_[i].thread = std::jthread([this, i] { HelperLoop(i); });
            slots_[i].active = true;
          } catch (...) {
            break;
          }
        }
        ++spawnedHelpers;
      }
      if (spawnedHelpers > 0) {
        currentFn_ = &dispatch;
        currentInvoke_ = [](const void* raw, size_t worker) {
          (*static_cast<const decltype(dispatch)*>(raw))(worker);
        };
        activeHelpers_ = spawnedHelpers;
        remainingHelpers_ = spawnedHelpers;
        ++batchId_;
      }
    }
    for (auto& thread : retired) {
      if (thread.joinable()) thread.join();
    }
    if (spawnedHelpers == 0) {
      for (size_t worker = 0; worker < workerCount; ++worker) fn(worker);
      return;
    }

    struct BatchWaitGuard {
      SearchWorkerPool& pool;
      ~BatchWaitGuard() {
        std::unique_lock lock(pool.mutex_);
        pool.doneCv_.wait(lock, [&] { return pool.remainingHelpers_ == 0; });
        pool.currentFn_ = nullptr;
        pool.currentInvoke_ = nullptr;
      }
    } waitGuard{*this};

    workCv_.notify_all();
    fn(0);
    for (size_t worker = spawnedHelpers + 1; worker < workerCount; ++worker) {
      fn(worker);
    }
  }

 private:
  struct Slot {
    std::jthread thread;
    bool active = false;
    std::uint64_t lastBatch = 0;
  };

  void HelperLoop(size_t slotIndex) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      const void* fn = nullptr;
      void (*invoke)(const void*, size_t) = nullptr;
      {
        std::unique_lock lock(mutex_);
        const bool woke = workCv_.wait_for(lock, kIdleTimeout, [&] {
          return stopping_ ||
                 (batchId_ != slots_[slotIndex].lastBatch &&
                  slotIndex < activeHelpers_);
        });
        if (stopping_) {
          slots_[slotIndex].active = false;
          return;
        }
        if (!woke || batchId_ == slots_[slotIndex].lastBatch ||
            slotIndex >= activeHelpers_) {
          slots_[slotIndex].active = false;
          return;
        }
        slots_[slotIndex].lastBatch = batchId_;
        fn = currentFn_;
        invoke = currentInvoke_;
      }
      try {
        invoke(fn, slotIndex + 1);
      } catch (...) {
      }
      {
        std::lock_guard lock(mutex_);
        if (remainingHelpers_ > 0 && --remainingHelpers_ == 0) {
          doneCv_.notify_one();
        }
      }
    }
  }

  std::mutex busyMutex_;
  std::mutex mutex_;
  std::condition_variable workCv_;
  std::condition_variable doneCv_;
  std::array<Slot, kMaxHelpers> slots_{};
  const void* currentFn_ = nullptr;
  void (*currentInvoke_)(const void*, size_t) = nullptr;
  size_t activeHelpers_ = 0;
  size_t remainingHelpers_ = 0;
  std::uint64_t batchId_ = 0;
  bool stopping_ = false;
};

}  // namespace detail

// Score once without imposing a global sort. Callers that assemble separate
// sections can select each section's best matches without sorting the corpus.
// Nonblank results contain at most options.limit matches, in unspecified order.
// Blank queries preserve eligible corpus order and carry empty scores.
inline std::vector<PreparedSearchMatch> SearchPreparedMatches(
    const std::wstring& query, const std::vector<PreparedSearchItem>& items,
    const std::set<std::wstring>& recentIds = {}, SearchOptions options = {}) {
  if (options.limit == 0 ||
      (options.latestGeneration &&
       options.latestGeneration->load(std::memory_order_acquire) !=
           options.generation)) {
    return {};
  }
  const auto* candidates = options.candidateIndices;
  const size_t itemCount = candidates ? candidates->size() : items.size();
  if (itemCount == 0) return {};
  const auto corpusIndex = [&](size_t position) {
    return candidates ? (*candidates)[position] : position;
  };
  if (Trim(query).empty()) {
    const size_t count = std::min(itemCount, options.limit);
    std::vector<PreparedSearchMatch> all;
    all.reserve(count);
    for (size_t i = 0; i < itemCount && all.size() < count; ++i) {
      if ((i & 63u) == 0 && options.latestGeneration &&
          options.latestGeneration->load(std::memory_order_acquire) !=
              options.generation) return {};
      const size_t index = corpusIndex(i);
      if (index < items.size()) all.push_back({index, {}});
    }
    return all;
  }

  const std::wstring normalizedQuery = Normalize(query);
  const auto normalizedQueryToken = MakePreparedQueryToken(normalizedQuery);
  const auto tokenSpans = TokenSpansNormalized(normalizedQuery);
  std::vector<PreparedQueryToken> queryTokens;
  queryTokens.reserve(tokenSpans.size());
  for (const auto& span : tokenSpans) {
    queryTokens.push_back(MakePreparedQueryToken(span.View(normalizedQuery)));
  }

  // Mid-sized corpora already exceed a frame's scoring budget for typo
  // queries on one worker. Split them across at most two CPUs; very small
  // catalogs avoid thread creation, and larger corpora retain the four cap.
  const size_t automaticWorkers = itemCount >= 5000 && options.maxWorkers != 1
      ? std::min<size_t>(itemCount < 20000 ? 2 : 4,
                          std::max(1u, std::thread::hardware_concurrency()))
      : 1;
  const size_t workerCount = options.maxWorkers == 0
      ? automaticWorkers
      : std::clamp(options.maxWorkers, size_t{1}, automaticWorkers);
  std::vector<std::vector<PreparedSearchMatch>> buckets(workerCount);
  auto better = [&](const PreparedSearchMatch& a,
                    const PreparedSearchMatch& b) {
    return BetterPreparedMatch(a, b, items);
  };
  auto scoreRange = [&](size_t worker, size_t begin, size_t end) {
    auto& bucket = buckets[worker];
    // A range cannot produce more matches than it has items. When all of them
    // fit, building a heap before the final sort only adds work.
    const bool bounded = options.limit < end - begin;
    bucket.reserve(bounded ? std::min(end - begin, options.limit)
                           : std::min<size_t>(end - begin, 256));
    for (size_t position = begin; position < end; ++position) {
      if ((position & 63u) == 0 && options.latestGeneration &&
          options.latestGeneration->load(std::memory_order_acquire) != options.generation) {
        bucket.clear();
        return;
      }
      const size_t i = corpusIndex(position);
      if (i >= items.size()) continue;
      ItemScore score = ScorePreparedItemDetailedPrepared(
          normalizedQueryToken, queryTokens, items[i], recentIds, options.now);
      if (!score.Matched()) continue;
      PreparedSearchMatch candidate{i, score};
      if (!bounded) {
        bucket.push_back(std::move(candidate));
      } else if (bucket.size() < options.limit) {
        bucket.push_back(std::move(candidate));
        std::push_heap(bucket.begin(), bucket.end(), better);
      } else if (better(candidate, bucket.front())) {
        std::pop_heap(bucket.begin(), bucket.end(), better);
        bucket.back() = std::move(candidate);
        std::push_heap(bucket.begin(), bucket.end(), better);
      }
    }
  };

  if (workerCount == 1) {
    scoreRange(0, 0, itemCount);
  } else {
    const size_t chunk = (itemCount + workerCount - 1) / workerCount;
    detail::SearchWorkerPool::Instance().Run(workerCount, [&](size_t worker) {
      const size_t begin = worker * chunk;
      if (begin >= itemCount) return;
      const size_t end = std::min(itemCount, begin + chunk);
      scoreRange(worker, begin, end);
    });
  }
  if (options.latestGeneration &&
      options.latestGeneration->load(std::memory_order_acquire) != options.generation) {
    return {};
  }

  size_t totalMatches = 0;
  for (const auto& bucket : buckets) totalMatches += bucket.size();
  std::vector<PreparedSearchMatch> scored;
  if (workerCount == 1) {
    scored = std::move(buckets.front());
  } else {
    scored.reserve(totalMatches);
    for (auto& bucket : buckets) {
      scored.insert(scored.end(),
                    std::make_move_iterator(bucket.begin()),
                    std::make_move_iterator(bucket.end()));
    }
  }

  if (options.limit < scored.size()) {
    std::nth_element(scored.begin(), scored.begin() + options.limit,
                     scored.end(), better);
    scored.resize(options.limit);
  }
  return scored;
}

inline std::vector<size_t> SearchPrepared(
    const std::wstring& query, const std::vector<PreparedSearchItem>& items,
    const std::set<std::wstring>& recentIds = {}, SearchOptions options = {}) {
  auto scored = SearchPreparedMatches(query, items, recentIds, options);
  if (!scored.empty() && scored.front().score.Matched()) {
    std::sort(scored.begin(), scored.end(), [&](const auto& a, const auto& b) {
      return BetterPreparedMatch(a, b, items);
    });
  }

  std::vector<size_t> out;
  out.reserve(scored.size());
  for (const auto& item : scored) out.push_back(item.index);
  return out;
}

inline std::wstring JoinKeywords(const std::vector<std::wstring>& keywords) {
  std::wstring out;
  size_t total = 0;
  for (const auto& keyword : keywords) {
    if (!keyword.empty()) total += keyword.size() + 1;
  }
  out.reserve(total);
  for (const auto& keyword : keywords) {
    if (!out.empty()) out.push_back(L' ');
    out += keyword;
  }
  return out;
}

// Log-scaled frequency + exponentially decayed recency (half-life 7 days).
// Ceiling ~2470 (500 launches, used just now) — enough to lift an actively used
// boundary-substring match (~2400) past a cold name-prefix match (~3200), while
// exact-name matches (5000 + 2500) stay out of reach. now == 0 means "no clock":
// any lastUsed > 0 then counts as fully fresh.
inline double UsageBoost(int usageCount, long long lastUsed, long long now) {
  if (usageCount <= 0 && lastUsed <= 0) return 0.0;
  static const std::array<double, 501> kLogBoost = [] {
    std::array<double, 501> table{};
    for (int i = 0; i <= 500; ++i) {
      table[static_cast<size_t>(i)] = 220.0 * std::log2(1.0 + i);
    }
    return table;
  }();
  double boost = usageCount <= 0
                     ? 0.0
                     : kLogBoost[static_cast<size_t>(std::min(usageCount, 500))];
  if (lastUsed > 0) {
    const double days = now > lastUsed ? static_cast<double>(now - lastUsed) / 86400.0 : 0.0;
    boost += 500.0 * std::exp2(-days / 7.0);
  }
  return boost;
}

// Scores an item against an already-normalized query and its tokens. Hoisting the
// query normalization/tokenization to the caller avoids recomputing it for every
// item on every keystroke; the scoring is otherwise identical to the public overload.
inline double ScoreItem(const std::wstring& normalizedQuery, const std::vector<std::wstring>& queryTokens,
                        const SearchItem& item, const std::set<std::wstring>& recentIds, long long now = 0) {
  return ScorePreparedItem(normalizedQuery, queryTokens,
                           PrepareSearchItem(item), recentIds, now);
}

inline double ScoreItem(const std::wstring& query, const SearchItem& item, const std::set<std::wstring>& recentIds, long long now = 0) {
  const std::wstring normalized = Normalize(query);
  return ScoreItem(normalized, TokensNormalized(normalized), item, recentIds,
                   now);
}

inline std::vector<size_t> Search(const std::wstring& query, const std::vector<SearchItem>& items, const std::set<std::wstring>& recentIds = {}, long long now = 0) {
  if (Trim(query).empty()) {
    std::vector<size_t> all;
    all.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) all.push_back(i);
    return all;
  }

  struct Scored {
    size_t index = 0;
    ItemScore score;
    std::wstring lowerName;
  };

  // Normalize/tokenize the query once instead of per item.
  const std::wstring normalizedQuery = Normalize(query);
  const std::vector<std::wstring> queryTokens =
      TokensNormalized(normalizedQuery);

  std::vector<Scored> scored;
  scored.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    ItemScore score = ScorePreparedItemDetailed(
        normalizedQuery, queryTokens, PrepareSearchItem(items[i]), recentIds,
        now);
    if (score.Matched()) {
      scored.push_back({i, score, Lower(items[i].name)});
    }
  }

  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (BetterItemScore(a.score, b.score)) return true;
    if (BetterItemScore(b.score, a.score)) return false;
    if (a.lowerName != b.lowerName) return a.lowerName < b.lowerName;
    return a.index < b.index;
  });

  std::vector<size_t> out;
  out.reserve(scored.size());
  for (const auto& item : scored) out.push_back(item.index);
  return out;
}

}  // namespace feathercast::core
