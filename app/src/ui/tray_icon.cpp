#include "pch.hpp"

#include "ui/tray_icon.hpp"

#include <shellapi.h>
#include <string>

#include "core/logger.hpp"
#include "ui/settings_view_model.hpp"

namespace minimize::ui {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kToggleEnabled = 2999;
constexpr UINT kShowSettings = 3000;
constexpr UINT kRepairWindows = 3001;
constexpr UINT kExit = 3002;
constexpr UINT kPauseTenMinutes = 3004;
constexpr UINT kPauseOneHour = 3005;
constexpr UINT kPauseUntilRestart = 3006;
constexpr UINT kResume = 3007;
constexpr UINT kPreview = 3008;
constexpr UINT kToggleCurrentApplication = 3009;
constexpr UINT kPauseFiveMinutes = 3010;
constexpr UINT kPauseThirtyMinutes = 3011;
constexpr UINT kPauseTwoHours = 3012;
constexpr UINT kProfileBase = 3100;

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                      size);
  return result;
}

}  // namespace

void TrayIcon::Initialize() {
  taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
}

const wchar_t* TrayIcon::Tooltip(const SettingsViewModel& view_model) {
  if (view_model.temporarily_paused) {
    return view_model.paused_until_restart ? L"Minimize Effect \u2014 Paused until restart"
                                           : L"Minimize Effect \u2014 Paused temporarily";
  }
  return view_model.enabled ? L"Minimize Effect \u2014 Enabled" : L"Minimize Effect \u2014 Paused";
}

bool TrayIcon::Add(HWND owner, const SettingsViewModel& view_model) {
  if (owner == nullptr || IsWindowVisible(owner)) {
    if (owner != nullptr) KillTimer(owner, kRetryTimerId);
    return false;
  }
  if (added_) return true;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = owner;
  icon.uID = kIconId;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kCallbackMessage;
  icon.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
  wcscpy_s(icon.szTip, Tooltip(view_model));
  added_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
  if (added_) {
    KillTimer(owner, kRetryTimerId);
  } else {
    SetTimer(owner, kRetryTimerId, 1000, nullptr);
    core::LogDebug(L"Tray", L"Tray icon add failed; retry scheduled");
  }
  return added_;
}

void TrayIcon::Remove(HWND owner) {
  if (owner == nullptr) return;
  KillTimer(owner, kRetryTimerId);
  if (!added_) return;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = owner;
  icon.uID = kIconId;
  Shell_NotifyIconW(NIM_DELETE, &icon);
  added_ = false;
}

void TrayIcon::UpdateTooltip(HWND owner, const SettingsViewModel& view_model) {
  if (owner == nullptr || !added_) return;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = owner;
  icon.uID = kIconId;
  icon.uFlags = NIF_TIP;
  wcscpy_s(icon.szTip, Tooltip(view_model));
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void TrayIcon::ShowUpdateAvailable(HWND owner, std::wstring_view version) {
  if (owner == nullptr || !added_) return;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = owner;
  icon.uID = kIconId;
  icon.uFlags = NIF_INFO;
  icon.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
  wcscpy_s(icon.szInfoTitle, L"Minimize Effect update available");
  const std::wstring message =
      L"Version " + std::wstring(version) + L" is ready. Open settings when you want to update.";
  wcsncpy_s(icon.szInfo, message.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

bool TrayIcon::IsTaskbarCreatedMessage(UINT message) const {
  return taskbar_created_message_ != 0 && message == taskbar_created_message_;
}

void TrayIcon::OnTaskbarCreated() { added_ = false; }

TrayCommand TrayIcon::HandleCallback(HWND owner, LPARAM parameter,
                                     const SettingsViewModel& view_model) const {
  if (parameter == WM_LBUTTONUP || parameter == WM_LBUTTONDBLCLK ||
      parameter == NIN_BALLOONUSERCLICK) {
    return TrayCommand{.kind = TrayCommandKind::kShowSettings};
  }
  if (parameter != WM_RBUTTONUP) return {};
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return {};
  AppendMenuW(menu, MF_STRING | (view_model.enabled ? MF_CHECKED : MF_UNCHECKED), kToggleEnabled,
              L"Minimize Effect Enabled");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (view_model.temporarily_paused) {
    AppendMenuW(menu, MF_STRING, kResume, L"Resume Minimize Effect");
  } else {
    const UINT pause_flags = view_model.enabled ? MF_STRING : MF_STRING | MF_GRAYED;
    HMENU pause_menu = CreatePopupMenu();
    if (pause_menu != nullptr) {
      AppendMenuW(pause_menu, pause_flags, kPauseFiveMinutes, L"5 minutes");
      AppendMenuW(pause_menu, pause_flags, kPauseTenMinutes, L"10 minutes");
      AppendMenuW(pause_menu, pause_flags, kPauseThirtyMinutes, L"30 minutes");
      AppendMenuW(pause_menu, pause_flags, kPauseOneHour, L"1 hour");
      AppendMenuW(pause_menu, pause_flags, kPauseTwoHours, L"2 hours");
      AppendMenuW(pause_menu, pause_flags, kPauseUntilRestart, L"Until next restart");
      AppendMenuW(menu, MF_POPUP | pause_flags, reinterpret_cast<UINT_PTR>(pause_menu),
                  L"Pause for");
    }
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kPreview, L"Preview animation");

  if (!view_model.motion_profiles.empty()) {
    HMENU profiles = CreatePopupMenu();
    if (profiles != nullptr) {
      for (std::size_t index = 0; index < view_model.motion_profiles.size(); ++index) {
        const std::wstring name = Utf8ToWide(view_model.motion_profiles[index].name);
        AppendMenuW(profiles, MF_STRING, kProfileBase + static_cast<UINT>(index), name.c_str());
      }
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profiles), L"Motion profile");
    }
  }

  if (!view_model.tray_current_application.empty()) {
    const std::wstring executable = Utf8ToWide(view_model.tray_current_application);
    const std::wstring label =
        (view_model.tray_current_application_excluded ? L"Enable effect for "
                                                      : L"Disable effect for ") +
        executable;
    AppendMenuW(menu, MF_STRING, kToggleCurrentApplication, label.c_str());
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kShowSettings, L"Settings");
  AppendMenuW(menu, MF_STRING, kRepairWindows, L"Repair Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Exit");
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(owner);
  SetTimer(owner, kMenuTimerId, 16, nullptr);
  const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                       cursor.x, cursor.y, 0, owner, nullptr);
  KillTimer(owner, kMenuTimerId);
  DestroyMenu(menu);
  if (selected >= kProfileBase &&
      selected < kProfileBase + static_cast<UINT>(view_model.motion_profiles.size())) {
    return TrayCommand{
        .kind = TrayCommandKind::kApplyProfile,
        .profile_name = view_model.motion_profiles[selected - kProfileBase].name,
    };
  }
  switch (selected) {
    case kToggleEnabled:
      return TrayCommand{.kind = TrayCommandKind::kToggleEnabled};
    case kShowSettings:
      return TrayCommand{.kind = TrayCommandKind::kShowSettings};
    case kRepairWindows:
      return TrayCommand{.kind = TrayCommandKind::kRepairWindows};
    case kExit:
      return TrayCommand{.kind = TrayCommandKind::kExit};
    case kPauseFiveMinutes:
      return TrayCommand{.kind = TrayCommandKind::kPauseForMinutes, .pause_minutes = 5};
    case kPauseTenMinutes:
      return TrayCommand{.kind = TrayCommandKind::kPauseForMinutes, .pause_minutes = 10};
    case kPauseThirtyMinutes:
      return TrayCommand{.kind = TrayCommandKind::kPauseForMinutes, .pause_minutes = 30};
    case kPauseOneHour:
      return TrayCommand{.kind = TrayCommandKind::kPauseForMinutes, .pause_minutes = 60};
    case kPauseTwoHours:
      return TrayCommand{.kind = TrayCommandKind::kPauseForMinutes, .pause_minutes = 120};
    case kPauseUntilRestart:
      return TrayCommand{.kind = TrayCommandKind::kPauseUntilRestart};
    case kResume:
      return TrayCommand{.kind = TrayCommandKind::kResume};
    case kPreview:
      return TrayCommand{.kind = TrayCommandKind::kPreview};
    case kToggleCurrentApplication:
      return TrayCommand{.kind = TrayCommandKind::kToggleCurrentApplication};
    default:
      return {};
  }
}

}  // namespace minimize::ui
