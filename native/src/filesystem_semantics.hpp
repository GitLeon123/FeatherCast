#pragma once

#include <system_error>

namespace feathercast::filesystem_semantics {

enum class Presence { Missing, Present, Error };

inline Presence ClassifyPresence(bool exists, const std::error_code& error) {
  if (error) return Presence::Error;
  return exists ? Presence::Present : Presence::Missing;
}

}  // namespace feathercast::filesystem_semantics
