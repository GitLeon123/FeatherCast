#include "snippets_io.hpp"

#include "filesystem_semantics.hpp"
#include "settings.hpp"

#include <array>
#include <fstream>
#include <sstream>

namespace feathercast::snippets_io {

FileFingerprint Inspect(const std::filesystem::path& path) {
  FileFingerprint fingerprint;
  std::error_code ec;
  fingerprint.exists = std::filesystem::exists(path, ec);
  if (ec) return {};
  fingerprint.inspected = true;
  if (!fingerprint.exists) return fingerprint;
  fingerprint.size = std::filesystem::file_size(path, ec);
  if (ec) return {};
  fingerprint.writeTime = std::filesystem::last_write_time(path, ec);
  if (ec) return {};
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::uint64_t hash = 14695981039346656037ULL;
  std::array<char, 8192> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    for (std::streamsize index = 0; index < file.gcount(); ++index) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
      hash *= 1099511628211ULL;
    }
  }
  if (!file.eof()) return {};
  fingerprint.contentHash = hash;
  return fingerprint;
}

LoadResult Load(const std::filesystem::path& path) {
  LoadResult result;
  result.fingerprint = Inspect(path);
  if (!result.fingerprint.inspected) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not inspect snippets.json. Editing is disabled.";
    return result;
  }
  if (!result.fingerprint.exists) return result;

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not read snippets.json. Editing is disabled.";
    return result;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.good() && !file.eof()) {
    result.status = LoadStatus::Invalid;
    result.message = L"Could not finish reading snippets.json. Editing is disabled.";
    return result;
  }

  auto parsed = snippets::ParseSnippetsDocument(buffer.str());
  switch (parsed.status) {
    case snippets::ParseStatus::Valid:
      result.snippets = std::move(parsed.snippets);
      result.status = LoadStatus::Valid;
      break;
    case snippets::ParseStatus::InvalidDocument:
      result.status = LoadStatus::Invalid;
      result.message = L"snippets.json is invalid. Open or reload the file before editing.";
      break;
    case snippets::ParseStatus::InvalidEntry:
      result.status = LoadStatus::Invalid;
      result.message = L"snippets.json contains an invalid entry. Editing is disabled.";
      break;
  }
  return result;
}

std::string Serialize(const std::vector<snippets::Snippet>& snippets) {
  std::ostringstream out;
  out << "{\n  \"snippets\": [";
  for (std::size_t index = 0; index < snippets.size(); ++index) {
    if (index) out << ",";
    const auto& snippet = snippets[index];
    out << "\n    {\"keyword\": \"" << settings::JsonEscape(snippet.keyword)
        << "\", \"name\": \"" << settings::JsonEscape(snippet.name)
        << "\", \"text\": \"" << settings::JsonEscape(snippet.text)
        << "\"}";
  }
  if (!snippets.empty()) out << "\n  ";
  out << "]\n}\n";
  return out.str();
}

SaveResult Save(const std::filesystem::path& path,
                const std::vector<snippets::Snippet>& snippets,
                const FileFingerprint& expected) {
  const auto current = Inspect(path);
  if (!current.inspected) {
    return {false, current,
            L"Could not inspect snippets.json. Reload it before saving."};
  }
  if (!(current == expected)) {
    return {false, current,
            L"snippets.json changed outside FeatherCast. Reload it before saving."};
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return {false, expected, L"Could not create the snippets directory."};
  }
  using filesystem_semantics::ReplaceStatus;
  switch (filesystem_semantics::ReplaceFileDurably(path, Serialize(snippets))) {
    case ReplaceStatus::Replaced:
      break;
    case ReplaceStatus::CreateFailed:
      return {false, expected, L"Could not create snippets.json.tmp."};
    case ReplaceStatus::WriteFailed:
      return {false, expected, L"Could not finish writing snippets.json."};
    case ReplaceStatus::ReplaceFailed:
      return {false, expected, L"Could not replace snippets.json."};
  }
  return {true, Inspect(path), L"Library saved."};
}

}  // namespace feathercast::snippets_io
