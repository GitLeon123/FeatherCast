#pragma once
#include "app_types.hpp"

namespace feathercast::search {

// Count capacities (including inline string storage conservatively) once per
// corpus revision, never on keystrokes. Transient result payloads are not cached.
inline std::size_t TextBytes(const std::wstring& value) { return (value.capacity() + 1) * sizeof(wchar_t); }
inline std::size_t TextBytes(const std::vector<std::wstring>& values) {
  std::size_t bytes = values.capacity() * sizeof(std::wstring);
  for (const auto& value : values) bytes += TextBytes(value);
  return bytes;
}
inline std::size_t SearchItemBytes(const core::SearchItem& item) {
  std::size_t bytes = TextBytes(item.keywords);
  for (const auto* value : {&item.id, &item.path, &item.kind, &item.source, &item.name,
                          &item.processName, &item.targetPath, &item.launchTarget, &item.exe}) bytes += TextBytes(*value);
  return bytes;
}
inline std::size_t SnapshotBytes(const app::SearchSnapshot& snapshot) {
  std::size_t bytes = sizeof(snapshot);
  for (const auto* items : {&snapshot.pool, &snapshot.pinned, &snapshot.recent,
       &snapshot.windowItems, &snapshot.system, &snapshot.systemFolders,
       &snapshot.commandItems, &snapshot.snippetItems, &snapshot.clipboardItems, &snapshot.gameItems}) {
    bytes += items->capacity() * sizeof(app::DisplayItem);
    for (const auto& item : *items) {
      if (item.isExtension || item.isAction || item.utility || item.timerRequest || item.isCapability ||
          item.isCalculator || item.isConversion || item.isRunCommand || item.isSymbol || item.isWebSearch) return SIZE_MAX;
      for (const auto* value : {&item.app.id, &item.app.name, &item.app.path, &item.app.source,
           &item.app.launchTarget, &item.app.targetPath, &item.app.args, &item.app.cwd, &item.app.appUserModelId,
           &item.app.iconKey, &item.app.gameProvider, &item.window.name, &item.window.exe,
           &item.window.processName, &item.window.iconKey, &item.snippet.keyword, &item.snippet.name,
           &item.snippet.text, &item.clipboard.id, &item.clipboard.text, &item.clipboard.preview,
           &item.commandName, &item.commandDetail}) bytes += TextBytes(*value);
      bytes += TextBytes(item.app.keywords) + TextBytes(item.commandKeywords);
    }
  }
  bytes += snapshot.searchItems.capacity() * sizeof(core::PreparedSearchItem);
  for (const auto& item : snapshot.searchItems) {
    bytes += SearchItemBytes(item.item) + TextBytes(item.normalizedName) + TextBytes(item.lowerName);
    bytes += item.fields.capacity() * sizeof(core::PreparedField);
    for (const auto& field : item.fields) bytes += TextBytes(field.raw) + TextBytes(field.normalized) + TextBytes(field.tokens) + TextBytes(field.acronym);
  }
  for (const auto* items : {&snapshot.clipboardSearchItems, &snapshot.gameSearchItems}) {
    bytes += items->capacity() * sizeof(core::SearchItem);
    for (const auto& item : *items) bytes += SearchItemBytes(item);
  }
  return bytes;
}

}  // namespace feathercast::search
