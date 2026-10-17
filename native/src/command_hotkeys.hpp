#pragma once

#include "automation.hpp"
#include "command_catalog.hpp"

namespace feathercast::commands {

// Assign is transactional: it keeps the previous assignments if a new chord is
// unavailable. AssignAvailable registers what it can, for loading saved shortcuts.
class HotKeys {
 public:
  HotKeys() = default;
  HotKeys(const HotKeys&) = delete;
  HotKeys& operator=(const HotKeys&) = delete;
  ~HotKeys() { Clear(); }
  void Clear() {
    for (const auto& [id, unused] : ids_) UnregisterHotKey(window_, id);
    ids_.clear();
  }
  std::optional<app::CommandKind> Find(int id) const {
    const auto found = ids_.find(id);
    if (found == ids_.end()) return std::nullopt;
    const auto* descriptor = commands::Find(found->second);
    return descriptor ? std::optional(descriptor->kind) : std::nullopt;
  }
  std::optional<std::wstring> Assign(HWND window, const std::map<std::wstring, std::wstring>& desired) {
    const auto previous = assignments_;
    Clear();
    window_ = window;
    if (const auto error = Register(desired, true)) {
      Clear();
      // Best effort: another app may have taken one of the old chords meanwhile.
      Register(previous, false);
      return error;
    }
    assignments_ = desired;
    return std::nullopt;
  }
  // Registers every saved chord that is still available, so one chord taken by
  // another app does not disable the rest. Returns the first failure.
  std::optional<std::wstring> AssignAvailable(HWND window, const std::map<std::wstring, std::wstring>& desired) {
    Clear();
    window_ = window;
    assignments_ = desired;
    return Register(desired, false);
  }
 private:
  std::optional<std::wstring> Register(const std::map<std::wstring, std::wstring>& values, bool stopOnError) {
    std::optional<std::wstring> firstError;
    int id = 0x3000;
    for (const auto& [stableId, text] : values) {
      std::optional<std::wstring> error;
      if (!commands::Find(stableId)) {
        error = L"That command is no longer available.";
      } else if (auto invalid = automation::ValidateShortcut(text)) {
        error = std::move(invalid);
      } else {
        const auto spec = shortcut::ToHotKeySpec(shortcut::ParseShortcut(text));
        if (RegisterHotKey(window_, id, spec.modifiers, spec.vk)) {
          ids_[id++] = stableId;
          continue;
        }
        error = L"The shortcut " + text + L" is already used by Windows, FeatherCast or another app.";
      }
      if (stopOnError) return error;
      if (!firstError) firstError = std::move(error);
    }
    return firstError;
  }
  HWND window_ = nullptr;
  std::map<int, std::wstring> ids_;
  std::map<std::wstring, std::wstring> assignments_;
};

}  // namespace feathercast::commands
