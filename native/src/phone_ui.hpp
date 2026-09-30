#pragma once

// "FeatherCast Phone" window: pairing screen with QR codes and a dashboard
// for the linked phone (notifications, photos, clipboard, devices).
// The window keeps its model while hidden, so feed it every service event.

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "phone_service.hpp"
#include "theme.hpp"

namespace feathercast::phone_ui {

struct State {
  bool enabled = false;
  bool running = false;
  std::string error;
  bool apkAvailable = false;
  bool clipboardSync = true;
  bool notificationToasts = true;
  bool lowBatteryAlert = true;
  std::vector<phone::PairedDevice> devices;
};

struct Callbacks {
  std::function<void(bool)> setEnabled;
  std::function<void(bool)> setClipboardSync;
  std::function<void(bool)> setNotificationToasts;
  std::function<std::string()> createPairingUri;
  std::function<std::string()> apkUrl;
  std::function<void(const std::string&)> copyToPc;
  std::function<void()> sendPcClipboard;
  std::function<void()> requestPhotos;
  std::function<void(const std::string&)> requestPhoto;
  std::function<void(const std::string&)> dismissNotification;
  std::function<void(const std::string&)> forgetDevice;
  std::function<void(bool)> setLowBatteryAlert;
  std::function<void()> findPhone;  // starts or stops ringing
  std::function<void(const std::string&)> mediaCommand;
  std::function<void()> pickFilesToSend;
  std::function<void(const std::vector<std::wstring>&)> sendFiles;
  std::function<void()> requestSmsThreads;
  // Opens the conversation in the launcher, where replies are typed.
  std::function<void(const phone::SmsThread&)> openSmsThread;
};

class PhoneWindow {
 public:
  PhoneWindow();
  ~PhoneWindow();
  PhoneWindow(const PhoneWindow&) = delete;
  PhoneWindow& operator=(const PhoneWindow&) = delete;

  void SetCallbacks(Callbacks callbacks);
  void SetTheme(const theme::Theme& theme, theme::Color accent);
  void SetMotionPolicy(bool fade, bool spatial, bool controls);
  void SetState(State state);
  void OnEvent(const phone::Event& event);

  // Shows (creating on first use) and activates the window. When pairing is
  // true, the pairing screen is shown even if a phone is already paired.
  void Show(HWND owner, bool pairing = false);
  void Close();
  bool Visible() const;
  HWND Hwnd() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace feathercast::phone_ui
