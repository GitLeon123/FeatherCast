#pragma once
#include <windows.h>
#include "phone_screen_protocol.hpp"
#include "theme.hpp"
#include <functional>
#include <memory>

namespace feathercast::phone_ui {
struct ScreenCallbacks {
  std::function<std::string(bool)> start;
  std::function<void()> stop;
  std::function<bool(phone::ScreenInput)> input;
};
class PhoneScreenWindow {
 public:
  PhoneScreenWindow();
  ~PhoneScreenWindow();
  void SetCallbacks(ScreenCallbacks callbacks);
  void SetTheme(const theme::Theme& theme, theme::Color accent);
  void SetTextScale(float scale);
  void SetConnection(bool connected, bool available, std::string deviceName);
  void Show();
  void Close();
  void OnPacket(phone::ScreenPacket packet);  // thread-safe
  bool HandleMessage(MSG& message);
  HWND Hwnd() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace feathercast::phone_ui
