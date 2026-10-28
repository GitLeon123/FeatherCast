#include "dpapi_scope.hpp"
#include "storage.hpp"
#include "test_framework.hpp"

#include "sqlite3.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path TestTempRoot() {
  wchar_t tempPath[MAX_PATH]{};
  const DWORD length = GetTempPathW(MAX_PATH, tempPath);
  assert(length > 0 && length < MAX_PATH);
  return std::filesystem::path(tempPath) /
         (L"FeatherCastStorageTrustTests-" +
          std::to_wstring(GetCurrentProcessId()));
}

void ExecSql(const std::filesystem::path& databasePath, const char* sql) {
  sqlite3* db = nullptr;
  assert(sqlite3_open16(databasePath.c_str(), &db) == SQLITE_OK);
  char* error = nullptr;
  const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
  if (error) sqlite3_free(error);
  assert(result == SQLITE_OK);
  assert(sqlite3_close(db) == SQLITE_OK);
}

long long RowCount(const std::filesystem::path& databasePath,
                   const char* table) {
  sqlite3* db = nullptr;
  assert(sqlite3_open16(databasePath.c_str(), &db) == SQLITE_OK);

  const std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ";";
  sqlite3_stmt* statement = nullptr;
  assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) ==
         SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const long long count = sqlite3_column_int64(statement, 0);
  assert(sqlite3_finalize(statement) == SQLITE_OK);
  assert(sqlite3_close(db) == SQLITE_OK);
  return count;
}

bool Contains(std::string_view value, std::string_view expected) {
  return value.find(expected) != std::string_view::npos;
}

}  // namespace

int main() {
  // This test account may have no user DPAPI master key.
  feathercast::dpapi::AllowMachineScopeFallbackForTests();
  const auto tempRoot = TestTempRoot();
  const auto databasePath = tempRoot / L"feathercast.db";
  std::error_code ec;
  std::filesystem::remove_all(tempRoot, ec);

  feathercast::storage::Storage storage;
  assert(storage.Open(databasePath));

  const feathercast::storage::FileIndexEntry file{
      L"C:\\Users\\Leon\\Documents\\trust-test.txt",
      L"trust-test.txt",
      false,
      L"C:\\Users\\Leon\\Documents\\trust-test.txt",
      100,
      42,
      200};

  assert(storage.ReplaceFileIndex({file}));
  assert(storage.AddClipboardEntry(L"clipboard trust test",
                                   L"clipboard trust test", 300, 50));
  assert(RowCount(databasePath, "file_index") == 1);
  assert(RowCount(databasePath, "clipboard_history") == 1);

  assert(storage.ClearFileIndex());
  assert(storage.LoadFileIndex().empty());
  assert(RowCount(databasePath, "file_index") == 0);

  assert(storage.ClearClipboardHistory());
  assert(storage.LoadClipboardHistory().empty());
  assert(RowCount(databasePath, "clipboard_history") == 0);

  // Deleting one entry removes only that row, keeps the others in order, and
  // a repeated delete of the same id is harmless.
  const auto first = storage.AddClipboardEntry(L"first", L"first", 310, 50);
  const auto second = storage.AddClipboardEntry(L"second", L"second", 311, 50);
  const auto third = storage.AddClipboardEntry(L"third", L"third", 312, 50);
  assert(first && second && third);
  assert(storage.PinClipboard(third->id, true, 50));
  assert(storage.DeleteClipboardEntry(second->id));
  auto remaining = storage.LoadClipboardHistory();
  assert(remaining.size() == 2);
  for (const auto& entry : remaining) assert(entry.id != second->id);
  assert(RowCount(databasePath, "clipboard_history") == 2);
  assert(storage.DeleteClipboardEntry(second->id));
  assert(storage.DeleteClipboardEntry(987654321));
  assert(RowCount(databasePath, "clipboard_history") == 2);
  // Pinned entries can be deleted too; deletion is an explicit user action.
  assert(storage.DeleteClipboardEntry(third->id));
  remaining = storage.LoadClipboardHistory();
  assert(remaining.size() == 1);
  assert(remaining.front().id == first->id);
  assert(remaining.front().text == L"first");
  assert(storage.ClearClipboardHistory());
  assert(RowCount(databasePath, "clipboard_history") == 0);

  assert(storage.ReplaceFileIndex({file}));
  const auto blockedEntry = storage.AddClipboardEntry(
      L"clipboard trust test", L"clipboard trust test", 301, 50);
  assert(blockedEntry);

  ExecSql(
      databasePath,
      "CREATE TRIGGER block_file_index_clear "
      "BEFORE DELETE ON file_index "
      "BEGIN SELECT RAISE(ABORT, 'file index clear blocked'); END;"
      "CREATE TRIGGER block_clipboard_clear "
      "BEFORE DELETE ON clipboard_history "
      "BEGIN SELECT RAISE(ABORT, 'clipboard clear blocked'); END;");

  assert(!storage.ClearFileIndex());
  const auto fileError = storage.LastError();
  assert(fileError.code != SQLITE_OK);
  assert(Contains(fileError.message, "file index clear blocked"));
  assert(storage.LoadFileIndex().size() == 1);
  assert(RowCount(databasePath, "file_index") == 1);

  assert(!storage.ClearClipboardHistory());
  const auto clipboardError = storage.LastError();
  assert(clipboardError.code != SQLITE_OK);
  assert(Contains(clipboardError.message, "clipboard clear blocked"));
  const auto clipboard = storage.LoadClipboardHistory();
  assert(clipboard.size() == 1);
  assert(clipboard.front().text == L"clipboard trust test");
  assert(RowCount(databasePath, "clipboard_history") == 1);

  // A failed single-entry delete reports the database error and keeps the row.
  assert(!storage.DeleteClipboardEntry(blockedEntry->id));
  const auto deleteError = storage.LastError();
  assert(deleteError.code != SQLITE_OK);
  assert(Contains(deleteError.message, "clipboard clear blocked"));
  assert(storage.LoadClipboardHistory().size() == 1);
  assert(RowCount(databasePath, "clipboard_history") == 1);

  storage.Close();
  std::filesystem::remove_all(tempRoot, ec);
  assert(!ec);
  return 0;
}
