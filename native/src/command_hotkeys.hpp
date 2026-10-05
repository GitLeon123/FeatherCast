#pragma once

#include "automation.hpp"
#include "command_catalog.hpp"

namespace feathercast::commands {

// Transactional registration keeps the previous assignments if a new chord is unavailable.
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
    if (const auto error = Register(desired)) {
      Clear();
      Register(previous);
      return error;
    }
    assignments_ = desired;
    return std::nullopt;
  }
 private:
  std::optional<std::wstring> Register(const std::map<std::wstring, std::wstring>& values) {
    int id = 0x3000;
    for (const auto& [stableId, text] : values) {
      if (!commands::Find(stableId)) return L"That command is no longer available.";
      if (const auto error = automation::ValidateShortcut(text)) return error;
      const auto spec = shortcut::ToHotKeySpec(shortcut::ParseShortcut(text));
      if (!RegisterHotKey(window_, id, spec.modifiers, spec.vk)) return L"The shortcut " + text + L" is already used by Windows, FeatherCast or another app.";
      ids_[id++] = stableId;
    }
    return std::nullopt;
  }
  HWND window_ = nullptr;
  std::map<int, std::wstring> ids_;
  std::map<std::wstring, std::wstring> assignments_;
};

}  // namespace feathercast::commands
