#pragma once

#include "app_types.hpp"

#include <iterator>
#include <string>
#include <string_view>
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

namespace detail {

struct RawSetting {
  const wchar_t* id;
  const wchar_t* name;
  const wchar_t* target;
  const wchar_t* args;
  const wchar_t* keywords;  // '|'-separated
  bool advanced;
};

inline std::vector<std::wstring> SplitPipe(std::wstring_view text) {
  std::vector<std::wstring> out;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find(L'|', start);
    if (end == std::wstring_view::npos) {
      if (start < text.size()) out.emplace_back(text.substr(start));
      break;
    }
    if (end > start) out.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

inline constexpr RawSetting kCatalog[] = {
    // System
    {L"display", L"Display Settings", L"ms-settings:display", L"",
     L"monitor|screen|resolution|hdr|scale|brightness", false},
    {L"display-advanced", L"Advanced Display Settings",
     L"ms-settings:display-advanced", L"",
     L"refresh rate|hz|monitor|display information", false},
    {L"night-light", L"Night Light", L"ms-settings:nightlight", L"",
     L"blue light|warm|display", false},
    {L"graphics", L"Graphics Settings", L"ms-settings:display-advancedgraphics",
     L"", L"gpu|gpu preference|hardware accelerated gpu scheduling", false},
    {L"sound", L"Sound Settings", L"ms-settings:sound", L"",
     L"audio|volume|microphone|speaker|output|input", false},
    {L"sound-devices", L"All Sound Devices", L"ms-settings:sound-devices", L"",
     L"audio devices|speakers|microphones|headphones", false},
    {L"volume-mixer", L"Volume Mixer", L"ms-settings:apps-volume", L"",
     L"app volume|audio|sound", false},
    {L"notifications", L"Notification Settings", L"ms-settings:notifications",
     L"", L"alerts|banners|toasts", false},
    {L"focus", L"Focus & Do Not Disturb", L"ms-settings:quiethours", L"",
     L"do not disturb|focus assist|quiet hours", false},
    {L"power", L"Power & Battery", L"ms-settings:powersleep", L"",
     L"power|sleep|battery|screen timeout|power mode", false},
    {L"battery-saver", L"Battery Saver", L"ms-settings:batterysaver", L"",
     L"energy saver|battery|power", false},
    {L"storage", L"Storage Settings", L"ms-settings:storagesense", L"",
     L"disk|space|storage sense|cleanup|drives", false},
    {L"multitasking", L"Multitasking", L"ms-settings:multitasking", L"",
     L"snap|snap windows|alt tab|virtual desktops", false},
    {L"clipboard", L"Windows Clipboard Settings", L"ms-settings:clipboard", L"",
     L"clipboard|win v|clipboard sync", false},
    {L"projecting", L"Projecting to This PC", L"ms-settings:project", L"",
     L"miracast|wireless display|cast", false},
    {L"remote-desktop", L"Remote Desktop", L"ms-settings:remotedesktop", L"",
     L"rdp|remote|remote access", false},
    {L"about", L"About This PC", L"ms-settings:about", L"",
     L"system info|device specifications|rename pc|windows version|specs",
     false},
    {L"troubleshoot", L"Troubleshoot", L"ms-settings:troubleshoot", L"",
     L"troubleshooters|fix problems", false},
    {L"recovery", L"Recovery", L"ms-settings:recovery", L"",
     L"reset this pc|advanced startup|go back|reinstall", false},
    {L"activation", L"Activation", L"ms-settings:activation", L"",
     L"product key|license|windows edition", false},

    // Bluetooth & devices
    {L"bluetooth", L"Bluetooth & Devices", L"ms-settings:bluetooth", L"",
     L"devices|wireless|pair", false},
    {L"connected-devices", L"Devices", L"ms-settings:connecteddevices", L"",
     L"add device|other devices", false},
    {L"printers", L"Printers & Scanners", L"ms-settings:printers", L"",
     L"printer|scanner|print", false},
    {L"mouse", L"Mouse Settings", L"ms-settings:mousetouchpad", L"",
     L"mouse|pointer speed|scroll|cursor", false},
    {L"touchpad", L"Touchpad Settings", L"ms-settings:devices-touchpad", L"",
     L"trackpad|gestures|tap", false},
    {L"typing", L"Typing Settings", L"ms-settings:typing", L"",
     L"autocorrect|spell check|touch keyboard|text suggestions", false},
    {L"pen", L"Pen & Windows Ink", L"ms-settings:pen", L"", L"stylus|ink",
     false},
    {L"autoplay", L"AutoPlay", L"ms-settings:autoplay", L"",
     L"removable drive|memory card", false},
    {L"usb", L"USB Settings", L"ms-settings:usb", L"",
     L"usb notifications|usb power", false},
    {L"camera", L"Camera Settings", L"ms-settings:camera", L"",
     L"webcam|cameras", false},
    {L"mobile-devices", L"Mobile Devices", L"ms-settings:mobile-devices", L"",
     L"phone link|phone", false},

    // Network & internet
    {L"network", L"Network & Internet", L"ms-settings:network-status", L"",
     L"network status|internet|connection", false},
    {L"wifi", L"Wi-Fi Settings", L"ms-settings:network-wifi", L"",
     L"wifi|wireless|wlan|known networks", false},
    {L"ethernet", L"Ethernet Settings", L"ms-settings:network-ethernet", L"",
     L"lan|cable|ip address|dns", false},
    {L"vpn", L"VPN Settings", L"ms-settings:network-vpn", L"",
     L"vpn|virtual private network", false},
    {L"proxy", L"Proxy Settings", L"ms-settings:network-proxy", L"",
     L"proxy server|pac", false},
    {L"hotspot", L"Mobile Hotspot", L"ms-settings:network-mobilehotspot", L"",
     L"hotspot|tethering|share internet", false},
    {L"airplane-mode", L"Airplane Mode", L"ms-settings:network-airplanemode",
     L"", L"flight mode|airplane", false},
    {L"network-advanced", L"Advanced Network Settings",
     L"ms-settings:network-advancedsettings", L"",
     L"network adapters|network reset|sharing|data usage", false},
    {L"data-usage", L"Data Usage", L"ms-settings:datausage", L"",
     L"data limit|metered|bandwidth", false},

    // Personalization
    {L"background", L"Background", L"ms-settings:personalization-background",
     L"", L"wallpaper|desktop background|personalization", false},
    {L"colors", L"Colors", L"ms-settings:colors", L"",
     L"dark mode|light mode|accent color|theme|transparency", false},
    {L"themes", L"Themes", L"ms-settings:themes", L"",
     L"theme|desktop icons|personalization", false},
    {L"lock-screen", L"Lock Screen", L"ms-settings:lockscreen", L"",
     L"lockscreen|screen saver|spotlight", false},
    {L"start", L"Start Menu Settings", L"ms-settings:personalization-start",
     L"", L"start|pins|recommended", false},
    {L"taskbar", L"Taskbar Settings", L"ms-settings:taskbar", L"",
     L"taskbar|system tray|taskbar alignment|widgets", false},
    {L"fonts", L"Fonts", L"ms-settings:fonts", L"", L"typeface|install font",
     false},

    // Apps
    {L"installed-apps", L"Installed Apps", L"ms-settings:appsfeatures", L"",
     L"applications|uninstall|programs|apps and features", false},
    {L"default-apps", L"Default Apps", L"ms-settings:defaultapps", L"",
     L"default browser|file associations|open with", false},
    {L"optional-features", L"Optional Features",
     L"ms-settings:optionalfeatures", L"", L"features on demand|add feature",
     false},
    {L"startup-apps", L"Startup Apps", L"ms-settings:startupapps", L"",
     L"startup|autostart|launch at login", false},
    {L"apps-for-websites", L"Apps for Websites",
     L"ms-settings:appsforwebsites", L"", L"links|web apps", false},
    {L"video-playback", L"Video Playback", L"ms-settings:videoplayback", L"",
     L"hdr video|video", false},
    {L"offline-maps", L"Offline Maps", L"ms-settings:maps", L"", L"maps",
     false},

    // Accounts
    {L"your-info", L"Your Info", L"ms-settings:yourinfo", L"",
     L"account|microsoft account|profile picture", false},
    {L"email-accounts", L"Email & Accounts", L"ms-settings:emailandaccounts",
     L"", L"email|accounts|calendar accounts", false},
    {L"sign-in-options", L"Sign-in Options", L"ms-settings:signinoptions", L"",
     L"password|pin|windows hello|fingerprint|face|dynamic lock", false},
    {L"work-school", L"Access Work or School", L"ms-settings:workplace", L"",
     L"work account|school account|domain|azure ad", false},
    {L"other-users", L"Other Users", L"ms-settings:otherusers", L"",
     L"add user|users|guest|accounts", false},
    {L"family", L"Family", L"ms-settings:family-group", L"",
     L"family safety|child account", false},
    {L"windows-backup", L"Windows Backup", L"ms-settings:backup", L"",
     L"backup|onedrive|sync settings", false},

    // Time & language
    {L"date-time", L"Date & Time", L"ms-settings:dateandtime", L"",
     L"clock|time zone|timezone|time sync", false},
    {L"language", L"Language & Region", L"ms-settings:regionlanguage", L"",
     L"language|region|display language|keyboard layout|input method", false},
    {L"speech", L"Speech Settings", L"ms-settings:speech", L"",
     L"voice|speech recognition|text to speech", false},

    // Gaming
    {L"game-bar", L"Game Bar", L"ms-settings:gaming-gamebar", L"",
     L"xbox game bar|gaming", false},
    {L"game-mode", L"Game Mode", L"ms-settings:gaming-gamemode", L"",
     L"gaming|performance", false},
    {L"captures", L"Captures", L"ms-settings:gaming-gamedvr", L"",
     L"game recording|game dvr|screen recording", false},

    // Accessibility
    {L"text-size", L"Text Size", L"ms-settings:easeofaccess-display", L"",
     L"font size|bigger text|accessibility", false},
    {L"visual-effects", L"Visual Effects",
     L"ms-settings:easeofaccess-visualeffects", L"",
     L"animations|transparency effects|scrollbars|accessibility", false},
    {L"mouse-pointer", L"Mouse Pointer & Touch",
     L"ms-settings:easeofaccess-mousepointer", L"",
     L"cursor size|pointer color|accessibility", false},
    {L"text-cursor", L"Text Cursor", L"ms-settings:easeofaccess-cursor", L"",
     L"caret|text cursor indicator|accessibility", false},
    {L"magnifier", L"Magnifier", L"ms-settings:easeofaccess-magnifier", L"",
     L"zoom|accessibility", false},
    {L"color-filters", L"Color Filters",
     L"ms-settings:easeofaccess-colorfilter", L"",
     L"color blind|grayscale|accessibility", false},
    {L"contrast-themes", L"Contrast Themes",
     L"ms-settings:easeofaccess-highcontrast", L"",
     L"high contrast|accessibility", false},
    {L"narrator", L"Narrator", L"ms-settings:easeofaccess-narrator", L"",
     L"screen reader|accessibility", false},
    {L"accessibility-audio", L"Accessibility Audio",
     L"ms-settings:easeofaccess-audio", L"", L"mono audio|accessibility",
     false},
    {L"captions", L"Captions", L"ms-settings:easeofaccess-closedcaptioning",
     L"", L"subtitles|live captions|accessibility", false},
    {L"accessibility-keyboard", L"Accessibility Keyboard",
     L"ms-settings:easeofaccess-keyboard", L"",
     L"sticky keys|filter keys|on screen keyboard|accessibility", false},

    // Privacy & security
    {L"windows-security", L"Windows Security", L"ms-settings:windowsdefender",
     L"", L"defender|antivirus|virus protection|firewall", false},
    {L"privacy", L"Privacy & Security", L"ms-settings:privacy", L"",
     L"permissions|app permissions|diagnostics", false},
    {L"location", L"Location Privacy", L"ms-settings:privacy-location", L"",
     L"location|gps", false},
    {L"camera-privacy", L"Camera Privacy", L"ms-settings:privacy-webcam", L"",
     L"webcam access|camera access", false},
    {L"microphone-privacy", L"Microphone Privacy",
     L"ms-settings:privacy-microphone", L"", L"microphone access|mic access",
     false},
    {L"find-my-device", L"Find My Device", L"ms-settings:findmydevice", L"",
     L"lost device|locate", false},
    {L"for-developers", L"For Developers", L"ms-settings:developers", L"",
     L"developer mode|dev mode|sudo|dev drive", false},

    // Windows Update
    {L"windows-update", L"Windows Update", L"ms-settings:windowsupdate", L"",
     L"updates|security|patches", false},
    {L"update-history", L"Update History",
     L"ms-settings:windowsupdate-history", L"",
     L"installed updates|uninstall updates", false},
    {L"update-options", L"Windows Update Advanced Options",
     L"ms-settings:windowsupdate-options", L"",
     L"active hours|optional updates|advanced options", false},
    {L"insider", L"Windows Insider Program", L"ms-settings:windowsinsider", L"",
     L"insider|preview builds|beta", false},
    {L"delivery-optimization", L"Delivery Optimization",
     L"ms-settings:delivery-optimization", L"",
     L"download bandwidth|peer updates", false},

    // Advanced Windows settings and tools
    {L"system-settings", L"Advanced System Settings",
     L"SystemPropertiesAdvanced.exe", L"",
     L"system properties|virtual memory|page file|startup and recovery|user profiles",
     true},
    {L"environment-variables", L"Environment Variables", L"rundll32.exe",
     L"sysdm.cpl,EditEnvironmentVariables",
     L"path|env|system variables|user variables", true},
    {L"performance-options", L"Performance Options",
     L"SystemPropertiesPerformance.exe", L"",
     L"visual effects|virtual memory|page file|best performance", true},
    {L"system-protection", L"System Protection",
     L"SystemPropertiesProtection.exe", L"",
     L"restore point|create restore point|system restore", true},
    {L"system-restore", L"System Restore", L"rstrui.exe", L"",
     L"restore point|roll back", true},
    {L"remote-settings", L"Remote Settings", L"SystemPropertiesRemote.exe", L"",
     L"remote assistance|remote desktop|rdp", true},
    {L"computer-name", L"Computer Name", L"SystemPropertiesComputerName.exe",
     L"", L"rename computer|workgroup|domain|hostname", true},
    {L"device-installation", L"Device Installation Settings",
     L"SystemPropertiesHardware.exe", L"", L"driver updates|hardware", true},
    {L"device-manager", L"Device Manager", L"devmgmt.msc", L"",
     L"drivers|hardware|devices", true},
    {L"disk-management", L"Disk Management", L"diskmgmt.msc", L"",
     L"partitions|volumes|format drive|drives", true},
    {L"computer-management", L"Computer Management", L"compmgmt.msc", L"",
     L"management console|mmc", true},
    {L"services", L"Services", L"services.msc", L"",
     L"windows services|background services|startup type", true},
    {L"event-viewer", L"Event Viewer", L"eventvwr.msc", L"",
     L"logs|event log|errors", true},
    {L"task-scheduler", L"Task Scheduler", L"taskschd.msc", L"",
     L"scheduled tasks|cron", true},
    {L"group-policy", L"Group Policy Editor", L"gpedit.msc", L"",
     L"gpedit|local group policy|policies", true},
    {L"security-policy", L"Local Security Policy", L"secpol.msc", L"",
     L"secpol|security policies", true},
    {L"local-users", L"Local Users and Groups", L"lusrmgr.msc", L"",
     L"lusrmgr|users|groups", true},
    {L"user-accounts", L"User Accounts (netplwiz)", L"netplwiz.exe", L"",
     L"netplwiz|auto login|user accounts", true},
    {L"certificates", L"Certificate Manager", L"certmgr.msc", L"",
     L"certificates|certmgr|ssl", true},
    {L"firewall-advanced", L"Windows Firewall with Advanced Security",
     L"wf.msc", L"", L"firewall|inbound rules|outbound rules|ports", true},
    {L"firewall", L"Windows Defender Firewall", L"firewall.cpl", L"",
     L"firewall|allow app through firewall", true},
    {L"registry-editor", L"Registry Editor", L"regedit.exe", L"",
     L"regedit|registry", true},
    {L"system-configuration", L"System Configuration", L"msconfig.exe", L"",
     L"msconfig|boot options|safe mode", true},
    {L"system-information", L"System Information", L"msinfo32.exe", L"",
     L"msinfo32|hardware info|specs", true},
    {L"resource-monitor", L"Resource Monitor", L"resmon.exe", L"",
     L"resmon|cpu|memory|disk activity|network activity", true},
    {L"performance-monitor", L"Performance Monitor", L"perfmon.exe", L"",
     L"perfmon|performance counters", true},
    {L"network-connections", L"Network Connections", L"ncpa.cpl", L"",
     L"network adapters|adapter settings|ncpa|ip address|dns", true},
    {L"network-sharing-center", L"Network and Sharing Center", L"control.exe",
     L"/name Microsoft.NetworkAndSharingCenter", L"network|sharing", true},
    {L"sharing-settings", L"Advanced Sharing Settings", L"control.exe",
     L"/name Microsoft.NetworkAndSharingCenter /page Advanced",
     L"network discovery|file sharing|printer sharing", true},
    {L"power-options", L"Power Options", L"powercfg.cpl", L"",
     L"power plan|power scheme|lid|power buttons", true},
    {L"power-settings", L"Advanced Power Settings", L"control.exe",
     L"powercfg.cpl,,3", L"power plan|processor power|usb suspend", true},
    {L"sound-control-panel", L"Sound Control Panel", L"mmsys.cpl", L"",
     L"playback devices|recording devices|audio|sound", true},
    {L"mouse-properties", L"Mouse Properties", L"main.cpl", L"",
     L"mouse|double click speed|pointers", true},
    {L"internet-options", L"Internet Options", L"inetcpl.cpl", L"",
     L"internet properties|proxy|certificates", true},
    {L"programs-features", L"Programs and Features", L"appwiz.cpl", L"",
     L"uninstall|programs|add remove programs", true},
    {L"windows-features", L"Turn Windows Features On or Off",
     L"optionalfeatures.exe", L"", L"windows features|hyper v|wsl|sandbox|.net",
     true},
    {L"region-classic", L"Region Settings", L"intl.cpl", L"",
     L"region|formats|system locale|unicode", true},
    {L"date-time-classic", L"Date and Time Properties", L"timedate.cpl", L"",
     L"clock|time server|internet time", true},
    {L"credential-manager", L"Credential Manager", L"control.exe",
     L"/name Microsoft.CredentialManager",
     L"saved passwords|windows credentials|credentials", true},
    {L"devices-printers", L"Devices and Printers", L"control.exe", L"printers",
     L"printers|devices", true},
    {L"indexing-options", L"Indexing Options", L"control.exe",
     L"/name Microsoft.IndexingOptions", L"search index|indexing", true},
    {L"folder-options", L"File Explorer Options", L"control.exe", L"folders",
     L"folder options|hidden files|file extensions|show hidden", true},
    {L"color-management", L"Color Management", L"colorcpl.exe", L"",
     L"icc|color profile", true},
    {L"color-calibration", L"Display Color Calibration", L"dccw.exe", L"",
     L"calibrate display|gamma", true},
    {L"disk-cleanup", L"Disk Cleanup", L"cleanmgr.exe", L"",
     L"clean up|free space|temporary files", true},
    {L"optimize-drives", L"Optimize Drives", L"dfrgui.exe", L"",
     L"defragment|defrag|trim", true},
    {L"recovery-drive", L"Recovery Drive", L"RecoveryDrive.exe", L"",
     L"recovery usb|boot drive", true},
    {L"memory-diagnostic", L"Windows Memory Diagnostic", L"MdSched.exe", L"",
     L"ram test|memory test", true},
    {L"tpm", L"TPM Management", L"tpm.msc", L"",
     L"trusted platform module|tpm", true},
    {L"shared-folders", L"Shared Folders", L"fsmgmt.msc", L"",
     L"shares|open files|sessions", true},
    {L"odbc", L"ODBC Data Sources", L"odbcad32.exe", L"",
     L"odbc|data sources|dsn", true},
    {L"component-services", L"Component Services", L"comexp.msc", L"",
     L"com|dcom", true},
};

}  // namespace detail

inline std::vector<app::AppEntry> Catalog() {
  std::vector<app::AppEntry> out;
  out.reserve(std::size(detail::kCatalog));
  for (const auto& raw : detail::kCatalog) {
    auto keywords = detail::SplitPipe(raw.keywords);
    if (raw.advanced) {
      out.push_back(Advanced(raw.id, raw.name, raw.target, raw.args,
                             std::move(keywords)));
    } else {
      out.push_back(Entry(raw.id, raw.name, raw.target, std::move(keywords)));
    }
  }
  return out;
}

}  // namespace feathercast::system_settings
