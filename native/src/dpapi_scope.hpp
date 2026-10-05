#pragma once

#include <atomic>

namespace feathercast::dpapi {

// Isolated test accounts have no user DPAPI master key, so user-scoped
// protection fails there. Test executables opt in to a machine-scoped
// fallback for their own process. The app never enables it, and nothing
// outside the process (such as an inherited environment variable) can, so
// its data always stays user-scoped.
inline std::atomic<bool>& MachineScopeFallbackFlag() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

inline void AllowMachineScopeFallbackForTests() {
  MachineScopeFallbackFlag().store(true, std::memory_order_relaxed);
}

inline bool MachineScopeFallbackAllowed() {
  return MachineScopeFallbackFlag().load(std::memory_order_relaxed);
}

}  // namespace feathercast::dpapi
