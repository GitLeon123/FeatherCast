#pragma once

#include "app_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace feathercast::system_settings {

inline constexpr wchar_t kIdPrefix[] = L"windows-settings:";
inline constexpr wchar_t kAdvancedIdPrefix[] = L"windows-settings:advanced-";

inline app::AppEntry Entry(std::wstring id, std::wstring name,
                           std::wstring uri,
                           std::vector<std::wstring> keywords) {
  app::AppEntry entry;
  entry.id = kIdPrefix + std::move(id);
  entry.name = std::move(name);
  entry.source = L"windows-settings";
  entry.launchType = app::LaunchType::Shell;
  entry.launchTarget = std::move(uri);
  entry.systemEssential = true;
  entry.keywords = std::move(keywords);
  return entry;
}

// Classic Control Panel applets, MMC snap-ins, and system tools. The target is
// opened with ShellExecute; args are passed through when present.
inline app::AppEntry Advanced(std::wstring id, std::wstring name,
                              std::wstring target, std::wstring args,
                              std::vector<std::wstring> keywords) {
  keywords.push_back(L"advanced");
  auto entry = Entry(L"advanced-" + std::move(id), std::move(name),
                     std::move(target), std::move(keywords));
  entry.args = std::move(args);
  return entry;
}

inline bool IsAdvanced(const app::AppEntry& entry) {
  return entry.id.starts_with(kAdvancedIdPrefix);
}

// A plain file name (e.g. "gpedit.msc") that has to exist on this edition of
// Windows for the entry to work. Empty for URIs and always-present launchers.
inline std::wstring RequiredFile(const app::AppEntry& entry) {
  if (!IsAdvanced(entry) || entry.launchTarget.find(L':') != std::wstring::npos ||
      entry.launchTarget == L"control.exe" || entry.launchTarget == L"rundll32.exe") {
    return L"";
  }
  return entry.launchTarget;
}

inline std::vector<app::AppEntry> Catalog() {
  return {
      // System
      Entry(L"display", L"Display Settings", L"ms-settings:display",
            {L"monitor", L"screen", L"resolution", L"hdr", L"scale", L"brightness"}),
      Entry(L"display-advanced", L"Advanced Display Settings",
            L"ms-settings:display-advanced",
            {L"refresh rate", L"hz", L"monitor", L"display information"}),
      Entry(L"night-light", L"Night Light", L"ms-settings:nightlight",
            {L"blue light", L"warm", L"display"}),
      Entry(L"graphics", L"Graphics Settings", L"ms-settings:display-advancedgraphics",
            {L"gpu", L"gpu preference", L"hardware accelerated gpu scheduling"}),
      Entry(L"sound", L"Sound Settings", L"ms-settings:sound",
            {L"audio", L"volume", L"microphone", L"speaker", L"output", L"input"}),
      Entry(L"sound-devices", L"All Sound Devices", L"ms-settings:sound-devices",
            {L"audio devices", L"speakers", L"microphones", L"headphones"}),
      Entry(L"volume-mixer", L"Volume Mixer", L"ms-settings:apps-volume",
            {L"app volume", L"audio", L"sound"}),
      Entry(L"notifications", L"Notification Settings", L"ms-settings:notifications",
            {L"alerts", L"banners", L"toasts"}),
      Entry(L"focus", L"Focus & Do Not Disturb", L"ms-settings:quiethours",
            {L"do not disturb", L"focus assist", L"quiet hours"}),
      Entry(L"power", L"Power & Battery", L"ms-settings:powersleep",
            {L"power", L"sleep", L"battery", L"screen timeout", L"power mode"}),
      Entry(L"battery-saver", L"Battery Saver", L"ms-settings:batterysaver",
            {L"energy saver", L"battery", L"power"}),
      Entry(L"storage", L"Storage Settings", L"ms-settings:storagesense",
            {L"disk", L"space", L"storage sense", L"cleanup", L"drives"}),
      Entry(L"multitasking", L"Multitasking", L"ms-settings:multitasking",
            {L"snap", L"snap windows", L"alt tab", L"virtual desktops"}),
      Entry(L"clipboard", L"Windows Clipboard Settings", L"ms-settings:clipboard",
            {L"clipboard", L"win v", L"clipboard sync"}),
      Entry(L"projecting", L"Projecting to This PC", L"ms-settings:project",
            {L"miracast", L"wireless display", L"cast"}),
      Entry(L"remote-desktop", L"Remote Desktop", L"ms-settings:remotedesktop",
            {L"rdp", L"remote", L"remote access"}),
      Entry(L"about", L"About This PC", L"ms-settings:about",
            {L"system info", L"device specifications", L"rename pc", L"windows version",
             L"specs"}),
      Entry(L"troubleshoot", L"Troubleshoot", L"ms-settings:troubleshoot",
            {L"troubleshooters", L"fix problems"}),
      Entry(L"recovery", L"Recovery", L"ms-settings:recovery",
            {L"reset this pc", L"advanced startup", L"go back", L"reinstall"}),
      Entry(L"activation", L"Activation", L"ms-settings:activation",
            {L"product key", L"license", L"windows edition"}),

      // Bluetooth & devices
      Entry(L"bluetooth", L"Bluetooth & Devices", L"ms-settings:bluetooth",
            {L"devices", L"wireless", L"pair"}),
      Entry(L"connected-devices", L"Devices", L"ms-settings:connecteddevices",
            {L"add device", L"other devices"}),
      Entry(L"printers", L"Printers & Scanners", L"ms-settings:printers",
            {L"printer", L"scanner", L"print"}),
      Entry(L"mouse", L"Mouse Settings", L"ms-settings:mousetouchpad",
            {L"mouse", L"pointer speed", L"scroll", L"cursor"}),
      Entry(L"touchpad", L"Touchpad Settings", L"ms-settings:devices-touchpad",
            {L"trackpad", L"gestures", L"tap"}),
      Entry(L"typing", L"Typing Settings", L"ms-settings:typing",
            {L"autocorrect", L"spell check", L"touch keyboard", L"text suggestions"}),
      Entry(L"pen", L"Pen & Windows Ink", L"ms-settings:pen",
            {L"stylus", L"ink"}),
      Entry(L"autoplay", L"AutoPlay", L"ms-settings:autoplay",
            {L"removable drive", L"memory card"}),
      Entry(L"usb", L"USB Settings", L"ms-settings:usb",
            {L"usb notifications", L"usb power"}),
      Entry(L"camera", L"Camera Settings", L"ms-settings:camera",
            {L"webcam", L"cameras"}),
      Entry(L"mobile-devices", L"Mobile Devices", L"ms-settings:mobile-devices",
            {L"phone link", L"phone"}),

      // Network & internet
      Entry(L"network", L"Network & Internet", L"ms-settings:network-status",
            {L"network status", L"internet", L"connection"}),
      Entry(L"wifi", L"Wi-Fi Settings", L"ms-settings:network-wifi",
            {L"wifi", L"wireless", L"wlan", L"known networks"}),
      Entry(L"ethernet", L"Ethernet Settings", L"ms-settings:network-ethernet",
            {L"lan", L"cable", L"ip address", L"dns"}),
      Entry(L"vpn", L"VPN Settings", L"ms-settings:network-vpn",
            {L"vpn", L"virtual private network"}),
      Entry(L"proxy", L"Proxy Settings", L"ms-settings:network-proxy",
            {L"proxy server", L"pac"}),
      Entry(L"hotspot", L"Mobile Hotspot", L"ms-settings:network-mobilehotspot",
            {L"hotspot", L"tethering", L"share internet"}),
      Entry(L"airplane-mode", L"Airplane Mode", L"ms-settings:network-airplanemode",
            {L"flight mode", L"airplane"}),
      Entry(L"network-advanced", L"Advanced Network Settings",
            L"ms-settings:network-advancedsettings",
            {L"network adapters", L"network reset", L"sharing", L"data usage"}),
      Entry(L"data-usage", L"Data Usage", L"ms-settings:datausage",
            {L"data limit", L"metered", L"bandwidth"}),

      // Personalization
      Entry(L"background", L"Background", L"ms-settings:personalization-background",
            {L"wallpaper", L"desktop background", L"personalization"}),
      Entry(L"colors", L"Colors", L"ms-settings:colors",
            {L"dark mode", L"light mode", L"accent color", L"theme", L"transparency"}),
      Entry(L"themes", L"Themes", L"ms-settings:themes",
            {L"theme", L"desktop icons", L"personalization"}),
      Entry(L"lock-screen", L"Lock Screen", L"ms-settings:lockscreen",
            {L"lockscreen", L"screen saver", L"spotlight"}),
      Entry(L"start", L"Start Menu Settings", L"ms-settings:personalization-start",
            {L"start", L"pins", L"recommended"}),
      Entry(L"taskbar", L"Taskbar Settings", L"ms-settings:taskbar",
            {L"taskbar", L"system tray", L"taskbar alignment", L"widgets"}),
      Entry(L"fonts", L"Fonts", L"ms-settings:fonts",
            {L"typeface", L"install font"}),

      // Apps
      Entry(L"installed-apps", L"Installed Apps", L"ms-settings:appsfeatures",
            {L"applications", L"uninstall", L"programs", L"apps and features"}),
      Entry(L"default-apps", L"Default Apps", L"ms-settings:defaultapps",
            {L"default browser", L"file associations", L"open with"}),
      Entry(L"optional-features", L"Optional Features", L"ms-settings:optionalfeatures",
            {L"features on demand", L"add feature"}),
      Entry(L"startup-apps", L"Startup Apps", L"ms-settings:startupapps",
            {L"startup", L"autostart", L"launch at login"}),
      Entry(L"apps-for-websites", L"Apps for Websites", L"ms-settings:appsforwebsites",
            {L"links", L"web apps"}),
      Entry(L"video-playback", L"Video Playback", L"ms-settings:videoplayback",
            {L"hdr video", L"video"}),
      Entry(L"offline-maps", L"Offline Maps", L"ms-settings:maps",
            {L"maps"}),

      // Accounts
      Entry(L"your-info", L"Your Info", L"ms-settings:yourinfo",
            {L"account", L"microsoft account", L"profile picture"}),
      Entry(L"email-accounts", L"Email & Accounts", L"ms-settings:emailandaccounts",
            {L"email", L"accounts", L"calendar accounts"}),
      Entry(L"sign-in-options", L"Sign-in Options", L"ms-settings:signinoptions",
            {L"password", L"pin", L"windows hello", L"fingerprint", L"face",
             L"dynamic lock"}),
      Entry(L"work-school", L"Access Work or School", L"ms-settings:workplace",
            {L"work account", L"school account", L"domain", L"azure ad"}),
      Entry(L"other-users", L"Other Users", L"ms-settings:otherusers",
            {L"add user", L"users", L"guest", L"accounts"}),
      Entry(L"family", L"Family", L"ms-settings:family-group",
            {L"family safety", L"child account"}),
      Entry(L"windows-backup", L"Windows Backup", L"ms-settings:backup",
            {L"backup", L"onedrive", L"sync settings"}),

      // Time & language
      Entry(L"date-time", L"Date & Time", L"ms-settings:dateandtime",
            {L"clock", L"time zone", L"timezone", L"time sync"}),
      Entry(L"language", L"Language & Region", L"ms-settings:regionlanguage",
            {L"language", L"region", L"display language", L"keyboard layout",
             L"input method"}),
      Entry(L"speech", L"Speech Settings", L"ms-settings:speech",
            {L"voice", L"speech recognition", L"text to speech"}),

      // Gaming
      Entry(L"game-bar", L"Game Bar", L"ms-settings:gaming-gamebar",
            {L"xbox game bar", L"gaming"}),
      Entry(L"game-mode", L"Game Mode", L"ms-settings:gaming-gamemode",
            {L"gaming", L"performance"}),
      Entry(L"captures", L"Captures", L"ms-settings:gaming-gamedvr",
            {L"game recording", L"game dvr", L"screen recording"}),

      // Accessibility
      Entry(L"text-size", L"Text Size", L"ms-settings:easeofaccess-display",
            {L"font size", L"bigger text", L"accessibility"}),
      Entry(L"visual-effects", L"Visual Effects", L"ms-settings:easeofaccess-visualeffects",
            {L"animations", L"transparency effects", L"scrollbars", L"accessibility"}),
      Entry(L"mouse-pointer", L"Mouse Pointer & Touch", L"ms-settings:easeofaccess-mousepointer",
            {L"cursor size", L"pointer color", L"accessibility"}),
      Entry(L"text-cursor", L"Text Cursor", L"ms-settings:easeofaccess-cursor",
            {L"caret", L"text cursor indicator", L"accessibility"}),
      Entry(L"magnifier", L"Magnifier", L"ms-settings:easeofaccess-magnifier",
            {L"zoom", L"accessibility"}),
      Entry(L"color-filters", L"Color Filters", L"ms-settings:easeofaccess-colorfilter",
            {L"color blind", L"grayscale", L"accessibility"}),
      Entry(L"contrast-themes", L"Contrast Themes", L"ms-settings:easeofaccess-highcontrast",
            {L"high contrast", L"accessibility"}),
      Entry(L"narrator", L"Narrator", L"ms-settings:easeofaccess-narrator",
            {L"screen reader", L"accessibility"}),
      Entry(L"accessibility-audio", L"Accessibility Audio", L"ms-settings:easeofaccess-audio",
            {L"mono audio", L"accessibility"}),
      Entry(L"captions", L"Captions", L"ms-settings:easeofaccess-closedcaptioning",
            {L"subtitles", L"live captions", L"accessibility"}),
      Entry(L"accessibility-keyboard", L"Accessibility Keyboard",
            L"ms-settings:easeofaccess-keyboard",
            {L"sticky keys", L"filter keys", L"on screen keyboard", L"accessibility"}),

      // Privacy & security
      Entry(L"windows-security", L"Windows Security", L"ms-settings:windowsdefender",
            {L"defender", L"antivirus", L"virus protection", L"firewall"}),
      Entry(L"privacy", L"Privacy & Security", L"ms-settings:privacy",
            {L"permissions", L"app permissions", L"diagnostics"}),
      Entry(L"location", L"Location Privacy", L"ms-settings:privacy-location",
            {L"location", L"gps"}),
      Entry(L"camera-privacy", L"Camera Privacy", L"ms-settings:privacy-webcam",
            {L"webcam access", L"camera access"}),
      Entry(L"microphone-privacy", L"Microphone Privacy", L"ms-settings:privacy-microphone",
            {L"microphone access", L"mic access"}),
      Entry(L"find-my-device", L"Find My Device", L"ms-settings:findmydevice",
            {L"lost device", L"locate"}),
      Entry(L"for-developers", L"For Developers", L"ms-settings:developers",
            {L"developer mode", L"dev mode", L"sudo", L"dev drive"}),

      // Windows Update
      Entry(L"windows-update", L"Windows Update", L"ms-settings:windowsupdate",
            {L"updates", L"security", L"patches"}),
      Entry(L"update-history", L"Update History", L"ms-settings:windowsupdate-history",
            {L"installed updates", L"uninstall updates"}),
      Entry(L"update-options", L"Windows Update Advanced Options",
            L"ms-settings:windowsupdate-options",
            {L"active hours", L"optional updates", L"advanced options"}),
      Entry(L"insider", L"Windows Insider Program", L"ms-settings:windowsinsider",
            {L"insider", L"preview builds", L"beta"}),
      Entry(L"delivery-optimization", L"Delivery Optimization",
            L"ms-settings:delivery-optimization",
            {L"download bandwidth", L"peer updates"}),

      // Advanced Windows settings and tools
      Advanced(L"system-settings", L"Advanced System Settings",
               L"SystemPropertiesAdvanced.exe", L"",
               {L"system properties", L"virtual memory", L"page file", L"startup and recovery",
                L"user profiles"}),
      Advanced(L"environment-variables", L"Environment Variables", L"rundll32.exe",
               L"sysdm.cpl,EditEnvironmentVariables",
               {L"path", L"env", L"system variables", L"user variables"}),
      Advanced(L"performance-options", L"Performance Options",
               L"SystemPropertiesPerformance.exe", L"",
               {L"visual effects", L"virtual memory", L"page file", L"best performance"}),
      Advanced(L"system-protection", L"System Protection",
               L"SystemPropertiesProtection.exe", L"",
               {L"restore point", L"create restore point", L"system restore"}),
      Advanced(L"system-restore", L"System Restore", L"rstrui.exe", L"",
               {L"restore point", L"roll back"}),
      Advanced(L"remote-settings", L"Remote Settings", L"SystemPropertiesRemote.exe", L"",
               {L"remote assistance", L"remote desktop", L"rdp"}),
      Advanced(L"computer-name", L"Computer Name", L"SystemPropertiesComputerName.exe",
               L"", {L"rename computer", L"workgroup", L"domain", L"hostname"}),
      Advanced(L"device-installation", L"Device Installation Settings",
               L"SystemPropertiesHardware.exe", L"",
               {L"driver updates", L"hardware"}),
      Advanced(L"device-manager", L"Device Manager", L"devmgmt.msc", L"",
               {L"drivers", L"hardware", L"devices"}),
      Advanced(L"disk-management", L"Disk Management", L"diskmgmt.msc", L"",
               {L"partitions", L"volumes", L"format drive", L"drives"}),
      Advanced(L"computer-management", L"Computer Management", L"compmgmt.msc", L"",
               {L"management console", L"mmc"}),
      Advanced(L"services", L"Services", L"services.msc", L"",
               {L"windows services", L"background services", L"startup type"}),
      Advanced(L"event-viewer", L"Event Viewer", L"eventvwr.msc", L"",
               {L"logs", L"event log", L"errors"}),
      Advanced(L"task-scheduler", L"Task Scheduler", L"taskschd.msc", L"",
               {L"scheduled tasks", L"cron"}),
      Advanced(L"group-policy", L"Group Policy Editor", L"gpedit.msc", L"",
               {L"gpedit", L"local group policy", L"policies"}),
      Advanced(L"security-policy", L"Local Security Policy", L"secpol.msc", L"",
               {L"secpol", L"security policies"}),
      Advanced(L"local-users", L"Local Users and Groups", L"lusrmgr.msc", L"",
               {L"lusrmgr", L"users", L"groups"}),
      Advanced(L"user-accounts", L"User Accounts (netplwiz)", L"netplwiz.exe", L"",
               {L"netplwiz", L"auto login", L"user accounts"}),
      Advanced(L"certificates", L"Certificate Manager", L"certmgr.msc", L"",
               {L"certificates", L"certmgr", L"ssl"}),
      Advanced(L"firewall-advanced", L"Windows Firewall with Advanced Security",
               L"wf.msc", L"", {L"firewall", L"inbound rules", L"outbound rules", L"ports"}),
      Advanced(L"firewall", L"Windows Defender Firewall", L"firewall.cpl", L"",
               {L"firewall", L"allow app through firewall"}),
      Advanced(L"registry-editor", L"Registry Editor", L"regedit.exe", L"",
               {L"regedit", L"registry"}),
      Advanced(L"system-configuration", L"System Configuration", L"msconfig.exe", L"",
               {L"msconfig", L"boot options", L"safe mode"}),
      Advanced(L"system-information", L"System Information", L"msinfo32.exe", L"",
               {L"msinfo32", L"hardware info", L"specs"}),
      Advanced(L"resource-monitor", L"Resource Monitor", L"resmon.exe", L"",
               {L"resmon", L"cpu", L"memory", L"disk activity", L"network activity"}),
      Advanced(L"performance-monitor", L"Performance Monitor", L"perfmon.exe", L"",
               {L"perfmon", L"performance counters"}),
      Advanced(L"network-connections", L"Network Connections", L"ncpa.cpl", L"",
               {L"network adapters", L"adapter settings", L"ncpa", L"ip address", L"dns"}),
      Advanced(L"network-sharing-center", L"Network and Sharing Center", L"control.exe",
               L"/name Microsoft.NetworkAndSharingCenter", {L"network", L"sharing"}),
      Advanced(L"sharing-settings", L"Advanced Sharing Settings", L"control.exe",
               L"/name Microsoft.NetworkAndSharingCenter /page Advanced",
               {L"network discovery", L"file sharing", L"printer sharing"}),
      Advanced(L"power-options", L"Power Options", L"powercfg.cpl", L"",
               {L"power plan", L"power scheme", L"lid", L"power buttons"}),
      Advanced(L"power-settings", L"Advanced Power Settings", L"control.exe",
               L"powercfg.cpl,,3", {L"power plan", L"processor power", L"usb suspend"}),
      Advanced(L"sound-control-panel", L"Sound Control Panel", L"mmsys.cpl", L"",
               {L"playback devices", L"recording devices", L"audio", L"sound"}),
      Advanced(L"mouse-properties", L"Mouse Properties", L"main.cpl", L"",
               {L"mouse", L"double click speed", L"pointers"}),
      Advanced(L"internet-options", L"Internet Options", L"inetcpl.cpl", L"",
               {L"internet properties", L"proxy", L"certificates"}),
      Advanced(L"programs-features", L"Programs and Features", L"appwiz.cpl", L"",
               {L"uninstall", L"programs", L"add remove programs"}),
      Advanced(L"windows-features", L"Turn Windows Features On or Off",
               L"optionalfeatures.exe", L"",
               {L"windows features", L"hyper v", L"wsl", L"sandbox", L".net"}),
      Advanced(L"region-classic", L"Region Settings", L"intl.cpl", L"",
               {L"region", L"formats", L"system locale", L"unicode"}),
      Advanced(L"date-time-classic", L"Date and Time Properties", L"timedate.cpl", L"",
               {L"clock", L"time server", L"internet time"}),
      Advanced(L"credential-manager", L"Credential Manager", L"control.exe",
               L"/name Microsoft.CredentialManager",
               {L"saved passwords", L"windows credentials", L"credentials"}),
      Advanced(L"devices-printers", L"Devices and Printers", L"control.exe", L"printers",
               {L"printers", L"devices"}),
      Advanced(L"indexing-options", L"Indexing Options", L"control.exe",
               L"/name Microsoft.IndexingOptions", {L"search index", L"indexing"}),
      Advanced(L"folder-options", L"File Explorer Options", L"control.exe", L"folders",
               {L"folder options", L"hidden files", L"file extensions", L"show hidden"}),
      Advanced(L"color-management", L"Color Management", L"colorcpl.exe", L"",
               {L"icc", L"color profile"}),
      Advanced(L"color-calibration", L"Display Color Calibration", L"dccw.exe", L"",
               {L"calibrate display", L"gamma"}),
      Advanced(L"disk-cleanup", L"Disk Cleanup", L"cleanmgr.exe", L"",
               {L"clean up", L"free space", L"temporary files"}),
      Advanced(L"optimize-drives", L"Optimize Drives", L"dfrgui.exe", L"",
               {L"defragment", L"defrag", L"trim"}),
      Advanced(L"recovery-drive", L"Recovery Drive", L"RecoveryDrive.exe", L"",
               {L"recovery usb", L"boot drive"}),
      Advanced(L"memory-diagnostic", L"Windows Memory Diagnostic", L"MdSched.exe", L"",
               {L"ram test", L"memory test"}),
      Advanced(L"tpm", L"TPM Management", L"tpm.msc", L"",
               {L"trusted platform module", L"tpm"}),
      Advanced(L"shared-folders", L"Shared Folders", L"fsmgmt.msc", L"",
               {L"shares", L"open files", L"sessions"}),
      Advanced(L"odbc", L"ODBC Data Sources", L"odbcad32.exe", L"",
               {L"odbc", L"data sources", L"dsn"}),
      Advanced(L"component-services", L"Component Services", L"comexp.msc", L"",
               {L"com", L"dcom"}),
  };
}

}  // namespace feathercast::system_settings
