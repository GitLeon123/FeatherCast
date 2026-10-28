#include "app_types.hpp"
#include "color_utilities.hpp"
#include "core.hpp"
#include "search_pipeline.hpp"
#include "settings.hpp"
#include "settings_catalog.hpp"
#include "test_framework.hpp"
#include "text_utilities.hpp"
#include "web_search.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace text = feathercast::text_utilities;
namespace color = feathercast::color_utilities;
namespace web = feathercast::web_search;
using feathercast::app::DisplayItem;
using feathercast::app::HitType;
using feathercast::app::QueryRequest;
using feathercast::app::ResultsCollection;
using feathercast::app::Section;

const text::Result* FindText(const std::vector<text::Result>& results,
                             std::wstring_view stableId) {
  for (const auto& result : results) {
    if (result.stableId == stableId) return &result;
  }
  return nullptr;
}

const Section* FindSection(const ResultsCollection& results,
                           std::wstring_view title) {
  for (const auto& section : results.sections) {
    if (section.title == title) return &section;
  }
  return nullptr;
}

bool UniqueSectionTitles(const ResultsCollection& results) {
  std::set<std::wstring> titles;
  for (const auto& section : results.sections) {
    if (!titles.insert(section.title).second) return false;
  }
  return true;
}

void TestBase64() {
  assert(text::Base64Encode("") == "");
  assert(text::Base64Encode("h") == "aA==");
  assert(text::Base64Encode("he") == "aGU=");
  assert(text::Base64Encode("hel") == "aGVs");
  assert(text::Base64Encode("hello") == "aGVsbG8=");

  assert(text::Base64Decode(L"aGVsbG8=") == std::optional<std::string>("hello"));
  // Padding is optional, and line-wrapped input decodes as one block.
  assert(text::Base64Decode(L"aGVsbG8") == std::optional<std::string>("hello"));
  assert(text::Base64Decode(L"aGVs\r\nbG8=") == std::optional<std::string>("hello"));
  // The URL-safe alphabet decodes to the same bytes as "+/8=".
  assert(text::Base64Decode(L"+/8=") == std::optional<std::string>("\xFB\xFF"));
  assert(text::Base64Decode(L"-_8") == std::optional<std::string>("\xFB\xFF"));

  assert(!text::Base64Decode(L""));
  assert(!text::Base64Decode(L"a"));           // a lone symbol carries 6 bits
  assert(!text::Base64Decode(L"aGVs$G8="));    // not in either alphabet
  assert(!text::Base64Decode(L"aGVsbG8=x"));   // data after padding
  assert(!text::Base64Decode(L"aG==="));       // more than two "="
  assert(!text::Base64Decode(L"aGU=="));       // padding past the block
}

void TestPercentEncoding() {
  assert(text::PercentEncode("a b&c") == "a%20b%26c");
  assert(text::PercentEncode("-_.~AZaz09") == "-_.~AZaz09");
  assert(text::PercentEncode("\xC3\xBC/?") == "%C3%BC%2F%3F");

  assert(text::PercentDecode("a%20b%26c") == "a b&c");
  assert(text::PercentDecode("a+b") == "a b");
  assert(text::PercentDecode("%c3%BC") == "\xC3\xBC");
  // A "%" that does not start an escape stays literal.
  assert(text::PercentDecode("100%") == "100%");
  assert(text::PercentDecode("%zz%4") == "%zz%4");
}

void TestHashes() {
  assert(text::HashHex(text::HashAlgorithm::Sha256, "hello") ==
         std::optional<std::wstring>(
             L"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"));
  assert(text::HashHex(text::HashAlgorithm::Sha1, "hello") ==
         std::optional<std::wstring>(L"aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"));
  assert(text::HashHex(text::HashAlgorithm::Md5, "hello") ==
         std::optional<std::wstring>(L"5d41402abc4b2a76b9719d911017c592"));
  assert(text::HashHex(text::HashAlgorithm::Sha256, "") ==
         std::optional<std::wstring>(
             L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  assert(text::HexString(std::string_view("\x00\x01\xAB", 3)) == L"0001ab");
  assert(text::HexString(std::string_view("\x00\x01\xAB", 3), true) == L"00 01 ab");
}

void TestJson() {
  assert(text::CheckJson("{\"a\":1}").valid);
  assert(text::CheckJson(" [1, 2.5e3, \"x\", true, null] ").valid);
  // Error positions are one-based characters.
  const auto badValue = text::CheckJson("[1, x]");
  assert(!badValue.valid && badValue.errorPosition == 5);
  const auto trailing = text::CheckJson("{\"a\":1} x");
  assert(!trailing.valid && trailing.errorPosition == 9);
  // Positions count characters, not UTF-8 bytes.
  const auto afterUmlaut = text::CheckJson("[\"\xC3\xBC\", x]");
  assert(!afterUmlaut.valid && afterUmlaut.errorPosition == 7);

  assert(text::ReformatJson("{\"a\":[1,2],\"b\":{}}", true) ==
         "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {}\n}");
  assert(text::ReformatJson("{ \"a b\" : [ 1 , \"c, d\" ] , \"e\" : [ ] }", false) ==
         "{\"a b\":[1,\"c, d\"],\"e\":[]}");
  // Escaped quotes inside strings do not end the string.
  assert(text::ReformatJson("[\"x\\\"y\" , 1]", false) == "[\"x\\\"y\",1]");
}

void TestPreview() {
  assert(text::Preview(L"") == L"(empty)");
  assert(text::Preview(L" \n\t ") == L"(whitespace)");
  assert(text::Preview(L"  a \n  b  ") == L"a b");
  const std::wstring longText(200, L'x');
  const auto preview = text::Preview(longText);
  assert(preview.size() == text::kPreviewLength + 1);
  assert(preview.back() == L'…');
  // Truncation never splits a surrogate pair.
  std::wstring emoji(text::kPreviewLength - 1, L'y');
  emoji += L"\U0001F600 tail";
  const auto emojiPreview = text::Preview(emoji);
  assert(emojiPreview.size() == text::kPreviewLength);
  assert(emojiPreview[text::kPreviewLength - 2] == L'y');
  assert(emojiPreview.back() == L'…');
}

void TestEvaluate() {
  // Only explicit keywords followed by text produce results.
  assert(text::Evaluate(L"hello world").empty());
  assert(text::Evaluate(L"base64").empty());
  assert(text::Evaluate(L"base64    ").empty());
  assert(text::Evaluate(L"basement plans").empty());
  assert(text::Evaluate(L"json hello").empty());
  assert(text::Evaluate(L"md").empty());

  const auto encoded = text::Evaluate(L"  B64 hello ");
  assert(encoded.size() == 1 && encoded[0].stableId == L"text-base64");
  assert(encoded[0].value == L"aGVsbG8=" && encoded[0].title == L"aGVsbG8=");
  assert(encoded[0].detail == L"Base64 encoded · 5 bytes");

  const auto decoded = text::Evaluate(L"base64d aGVsbG8");
  assert(decoded.size() == 1 && decoded[0].stableId == L"text-base64-decode");
  assert(decoded[0].value == L"hello");
  assert(decoded[0].detail == L"Base64 decoded · 5 bytes");
  // Bytes that are not readable text are shown as hex.
  const auto binary = text::Evaluate(L"b64d AAEC");
  assert(binary.size() == 1 && binary[0].value == L"00 01 02");
  assert(binary[0].detail.ends_with(L"of binary data, shown as hex"));
  const auto invalid = text::Evaluate(L"base64d !!!");
  assert(invalid.size() == 1 && invalid[0].stableId == L"text-base64-invalid");
  assert(invalid[0].value.empty() && invalid[0].title == L"Invalid Base64");

  const auto url = text::Evaluate(L"urlencode a b&c");
  assert(url.size() == 1 && url[0].value == L"a%20b%26c");
  const auto urlDecoded = text::Evaluate(L"urldecode caf%C3%A9+au+lait");
  assert(urlDecoded.size() == 1 && urlDecoded[0].value == L"café au lait");

  const auto sha = text::Evaluate(L"sha256 hello");
  assert(sha.size() == 1 && sha[0].stableId == L"text-sha256");
  assert(sha[0].value ==
         L"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
  assert(sha[0].detail == L"SHA-256 of 5 bytes (UTF-8)");
  const auto all = text::Evaluate(L"hash hello");
  assert(all.size() == 3);
  assert(all[0].stableId == L"text-sha256" && all[1].stableId == L"text-sha1" &&
         all[2].stableId == L"text-md5");
  assert(all[2].value == L"5d41402abc4b2a76b9719d911017c592");
  assert(text::Evaluate(L"md5 hello").size() == 1);
  assert(text::Evaluate(L"SHA1 hello")[0].value ==
         L"aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");

  const auto json = text::Evaluate(L"json {\"a\":1}");
  assert(json.size() == 2);
  const auto* formatted = FindText(json, L"text-json-format");
  const auto* minified = FindText(json, L"text-json-minify");
  assert(formatted && formatted->value == L"{\n  \"a\": 1\n}");
  assert(formatted->detail == L"Formatted JSON · 3 lines");
  // The title is a single-line preview of the formatted value.
  assert(formatted->title == L"{ \"a\": 1 }");
  assert(minified && minified->value == L"{\"a\":1}");
  assert(minified->detail == L"Minified JSON · 7 characters");
  const auto badJson = text::Evaluate(L"json [1, x]");
  assert(badJson.size() == 1 && badJson[0].stableId == L"text-json-invalid");
  assert(badJson[0].value.empty());
  assert(badJson[0].detail == L"Syntax error near character 5");

  const auto tooLong =
      text::Evaluate(L"sha256 " + std::wstring(text::kMaxInputLength + 1, L'a'));
  assert(tooLong.size() == 1 && tooLong[0].stableId == L"text-too-long");
  assert(tooLong[0].value.empty());
  std::wstring loneSurrogate = L"base64 a";
  loneSurrogate.push_back(static_cast<wchar_t>(0xD800));
  const auto invalidText = text::Evaluate(loneSurrogate);
  assert(invalidText.size() == 1 && invalidText[0].stableId == L"text-invalid");
}

void TestColors() {
  const auto shortHex = color::Parse(L"#f80");
  assert(shortHex && shortHex->rgba.red == 255 && shortHex->rgba.green == 136 &&
         shortHex->rgba.blue == 0 && shortHex->rgba.alpha == 1.0);
  const auto rows = color::Evaluate(L" #FF8800 ");
  assert(rows.size() == 3);
  assert(rows[0].stableId == L"color-hex" && rows[0].value == L"#ff8800");
  assert(rows[1].stableId == L"color-rgb" && rows[1].value == L"rgb(255, 136, 0)");
  assert(rows[2].stableId == L"color-hsl" && rows[2].value == L"hsl(32, 100%, 50%)");
  assert(rows[0].swatch == 0xFF8800FFu && rows[2].swatch == 0xFF8800FFu);
  assert(color::Evaluate(L"#f80")[0].value == L"#ff8800");

  const auto translucent = color::Evaluate(L"#ff880080");
  assert(translucent.size() == 3 && translucent[0].value == L"#ff880080");
  assert(translucent[1].value == L"rgba(255, 136, 0, 0.5)");
  assert(translucent[2].value == L"hsla(32, 100%, 50%, 0.5)");
  assert(translucent[0].swatch == 0xFF880080u);

  const auto modern = color::Evaluate(L"rgb(255 136 0 / 50%)");
  assert(modern.size() == 3 && modern[0].value == L"#ff880080");
  const auto blue = color::Evaluate(L"RGBA(0, 0, 255, 1)");
  assert(blue.size() == 3 && blue[0].value == L"#0000ff");
  assert(blue[1].value == L"rgb(0, 0, 255)" && blue[2].value == L"hsl(240, 100%, 50%)");
  const auto percent = color::Evaluate(L"rgb(100%, 0%, 50%)");
  assert(percent.size() == 3 && percent[0].value == L"#ff0080");
  // A missing ")" is accepted while typing.
  assert(color::Evaluate(L"rgb(255, 0, 0")[0].value == L"#ff0000");

  // HSL input keeps the typed values in its own row.
  const auto green = color::Evaluate(L"hsl(120, 100%, 25%)");
  assert(green.size() == 3 && green[0].value == L"#008000");
  assert(green[1].value == L"rgb(0, 128, 0)" && green[2].value == L"hsl(120, 100%, 25%)");
  assert(color::Evaluate(L"hsl(210deg 50% 40%)")[2].value == L"hsl(210, 50%, 40%)");
  assert(color::Evaluate(L"hsl(32.5, 100%, 50%)")[2].value == L"hsl(32.5, 100%, 50%)");
  assert(color::Evaluate(L"hsla(-90, 100%, 50%, 0.25)")[2].value ==
         L"hsla(270, 100%, 50%, 0.25)");
  assert(color::Evaluate(L"hsl(0, 0%, 100%)")[0].value == L"#ffffff");

  for (const auto* rejected :
       {L"#12", L"#ff88000", L"#ggg", L"red", L"hello world", L"rgb(256, 0, 0)",
        L"rgb(1, 2)", L"rgb(1, 2, 3, 4, 5)", L"rgb(1, 2, 3) x", L"rgb(1px, 2, 3)",
        L"hsl(0, 101%, 50%)", L"hsl(10turn, 50%, 50%)", L"rgba(0, 0, 0, 2)",
        L"cmyk(0, 0, 0, 0)"}) {
    assert(color::Evaluate(rejected).empty());
  }
}

void TestWebSearchHelpers() {
  assert(web::ExpandTemplate(L"https://x.test/?q=%s&alt=%s", L"a+b") ==
         L"https://x.test/?q=a+b&alt=a+b");
  assert(web::ExpandTemplate(L"https://x.test/?q=", L"a") == L"https://x.test/?q=a");
  assert(web::ExpandTemplate(L"https://x.test/?p=100%25&q=%s", L"z") ==
         L"https://x.test/?p=100%25&q=z");

  const auto engines = feathercast::settings::DefaultSearchEngines();
  assert(web::EngineName(L"g", engines.at(L"g")) == L"Google");
  assert(web::EngineName(L"ddg", engines.at(L"ddg")) == L"DuckDuckGo");
  assert(web::EngineName(L"yt", engines.at(L"yt")) == L"YouTube");
  assert(web::EngineName(L"gh", engines.at(L"gh")) == L"GitHub");
  assert(web::EngineName(L"w", engines.at(L"w")) == L"Wikipedia");
  assert(web::EngineName(L"ex", L"https://docs.example.co.uk/?q=%s") == L"Example");
  assert(web::EngineName(L"ex", L"https://user@search.example.com:8080/?q=%s") ==
         L"Example");
  assert(web::EngineName(L"local", L"search:%s") == L"local");

  assert(web::ResolveEngine(engines, L"w")->first == L"w");
  assert(web::ResolveEngine(engines, L"G")->first == L"g");
  // A removed engine falls back to the first keyword instead of disabling search.
  assert(web::ResolveEngine(engines, L"missing")->first == L"ddg");
  assert(web::ResolveEngine({}, L"g") == nullptr);

  // Keyword order: ddg, g, gh, w, yt.
  assert(web::StepEngine(engines, L"g", 1) == L"gh");
  assert(web::StepEngine(engines, L"g", -1) == L"ddg");
  assert(web::StepEngine(engines, L"yt", 1) == L"ddg");
  assert(web::StepEngine(engines, L"ddg", -1) == L"yt");
  assert(web::StepEngine(engines, L"missing", 1) == L"g");
  assert(web::StepEngine({}, L"g", 1) == L"g");
}

void TestSettings() {
  feathercast::app::Settings settings;
  assert(settings.webSearchFallbackEnabled);
  assert(settings.webSearchFallbackEngine == L"g");
  settings.webSearchFallbackEnabled = false;
  settings.webSearchFallbackEngine = L"ddg";
  const auto reparsed = feathercast::settings::ParseSettings(
      feathercast::settings::SerializeSettings(settings));
  assert(!reparsed.webSearchFallbackEnabled);
  assert(reparsed.webSearchFallbackEngine == L"ddg");

  std::wstring error;
  assert(feathercast::settings_catalog::ValidateCatalog(&error));
  const auto* toggle =
      feathercast::settings_catalog::Find(HitType::WebSearchFallbackToggle);
  assert(toggle && toggle->category == feathercast::app::SettingsCategory::Results);
  assert(toggle->kind == feathercast::settings_catalog::ControlKind::Toggle);
  assert(!feathercast::settings_catalog::Checked(HitType::WebSearchFallbackToggle,
                                                  settings));
  assert(feathercast::settings_catalog::AccessibleValue(
             HitType::WebSearchFallbackToggle, settings) == L"Off");
  assert(feathercast::settings_catalog::AccessibleValue(HitType::WebSearchEngineDown,
                                                        settings) == L"DuckDuckGo");
  settings.webSearchFallbackEngine = L"w";
  assert(feathercast::settings_catalog::AccessibleValue(HitType::WebSearchEngineUp,
                                                        settings) == L"Wikipedia");
  settings.searchEngines.clear();
  assert(feathercast::settings_catalog::AccessibleValue(HitType::WebSearchEngineUp,
                                                        settings) == L"None");
  const auto order = feathercast::settings_catalog::AccessibilityOrder(
      feathercast::app::SettingsCategory::Results);
  const auto toggleAt = std::find(order.begin(), order.end(),
                                  HitType::WebSearchFallbackToggle);
  assert(toggleAt != order.end() && toggleAt + 2 < order.end());
  assert(toggleAt[1] == HitType::WebSearchEngineDown &&
         toggleAt[2] == HitType::WebSearchEngineUp);
}

std::shared_ptr<feathercast::app::SearchSnapshot> MakeSnapshot() {
  auto snapshot = std::make_shared<feathercast::app::SearchSnapshot>();
  const auto add = [&](DisplayItem item, const std::wstring& kind,
                       std::vector<std::wstring> aliases = {}) {
    feathercast::core::SearchItem searchable;
    searchable.id = item.Key();
    searchable.name = item.Name();
    searchable.kind = kind;
    searchable.source = item.app.source;
    searchable.aliases = std::move(aliases);
    snapshot->pool.push_back(item);
    snapshot->searchItems.push_back(feathercast::core::PrepareSearchItem(searchable));
    if (kind == L"app") snapshot->appItems.push_back(std::move(item));
  };
  DisplayItem notepad;
  notepad.app.id = L"app:notepad";
  notepad.app.name = L"Notepad";
  notepad.app.source = L"shortcut";
  add(notepad, L"app");
  DisplayItem editor;
  editor.app.id = L"app:editor";
  editor.app.name = L"Visual Editor";
  editor.app.source = L"shortcut";
  add(editor, L"app", {L"ve"});
  DisplayItem window;
  window.isWindow = true;
  window.window.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234));
  window.window.name = L"ve";
  add(window, L"window");
  return snapshot;
}

QueryRequest MakeRequest(const std::shared_ptr<feathercast::app::SearchSnapshot>& snapshot,
                         std::wstring query) {
  QueryRequest request;
  request.query = std::move(query);
  request.limit = 20;
  request.snapshot = snapshot;
  request.searchEngines = feathercast::settings::DefaultSearchEngines();
  request.webSearchFallback = true;
  request.webSearchFallbackEngine = L"g";
  return request;
}

void TestPipeline() {
  const auto snapshot = MakeSnapshot();
  using feathercast::search_pipeline::ComputeResults;

  // Nothing local matches: the web fallback is the last section.
  auto request = MakeRequest(snapshot, L"zzqx unknown");
  auto results = ComputeResults(request);
  assert(UniqueSectionTitles(results));
  assert(!results.sections.empty() && results.sections.back().title == L"Search the web");
  const auto& fallback = results.sections.back().items.at(0);
  assert(fallback.isWebSearch);
  assert(fallback.webSearchUrl == L"https://www.google.com/search?q=zzqx+unknown");
  assert(fallback.webSearchLabel == L"Search Google for “zzqx unknown”");

  request.webSearchFallback = false;
  assert(!FindSection(ComputeResults(request), L"Search the web"));
  request.webSearchFallback = true;
  request.webSearchFallbackEngine = L"removed";
  results = ComputeResults(request);
  assert(FindSection(results, L"Search the web")->items.at(0).webSearchUrl ==
         L"https://duckduckgo.com/?q=zzqx+unknown");
  request.searchEngines.clear();
  assert(!FindSection(ComputeResults(request), L"Search the web"));

  // Strong local matches, instant answers and one-letter queries skip it.
  for (const auto* query :
       {L"notepad", L"note", L"g cats", L"2+2", L"#ff8800", L"sha256 hello", L"z"}) {
    const auto answered = ComputeResults(MakeRequest(snapshot, query));
    assert(UniqueSectionTitles(answered));
    assert(!FindSection(answered, L"Search the web"));
  }

  // Keyword searches replace every "%s" and name the engine.
  request = MakeRequest(snapshot, L"t a b");
  request.searchEngines[L"t"] = L"https://search.example.org/?q=%s&echo=%s";
  results = ComputeResults(request);
  const auto* keyword = FindSection(results, L"Web Search");
  assert(keyword && keyword->items.size() == 1);
  assert(keyword->items[0].webSearchUrl ==
         L"https://search.example.org/?q=a+b&echo=a+b");
  assert(keyword->items[0].webSearchLabel == L"Search Example for “a b”");

  // Color and text utilities.
  results = ComputeResults(MakeRequest(snapshot, L"#ff8800"));
  const auto* colors = FindSection(results, L"Color");
  assert(colors && colors->items.size() == 3);
  assert(colors->items[0].utility &&
         colors->items[0].utility->kind == feathercast::app::UtilityKind::Color);
  assert(colors->items[0].utility->swatch == 0xFF8800FFu);
  assert(colors->items[1].utility->value == L"rgb(255, 136, 0)");
  results = ComputeResults(MakeRequest(snapshot, L"hash hello"));
  const auto* utilities = FindSection(results, L"Utilities");
  assert(utilities && utilities->items.size() == 3);
  assert(utilities->items[0].utility->kind == feathercast::app::UtilityKind::Text);
  assert(utilities->items[0].utility->detail == L"SHA-256 of 5 bytes (UTF-8)");
  assert(!utilities->items[0].utility->swatch);

  // An alias invocation and an exact name match share one "Best match".
  results = ComputeResults(MakeRequest(snapshot, L"ve"));
  assert(UniqueSectionTitles(results));
  assert(results.sections.front().title == L"Best match");
  assert(results.sections.front().items.front().app.id == L"app:editor");

  // "@" inside a scope is search text, not a request for scope suggestions.
  request = MakeRequest(snapshot, L"@");
  results = ComputeResults(request);
  assert(FindSection(results, L"Search scopes"));
  request.scope = feathercast::search_scope::Scope::Apps;
  results = ComputeResults(request);
  assert(!FindSection(results, L"Search scopes"));
  assert(!FindSection(results, L"Search the web"));
  for (const auto& item : results.flatItems) assert(!item.isCapability);
}

}  // namespace

int main() {
  TestBase64();
  TestPercentEncoding();
  TestHashes();
  TestJson();
  TestPreview();
  TestEvaluate();
  TestColors();
  TestWebSearchHelpers();
  TestSettings();
  TestPipeline();
  std::puts("text utilities tests passed");
  return 0;
}
