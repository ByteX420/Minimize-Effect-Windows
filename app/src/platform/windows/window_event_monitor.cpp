#include "pch.hpp"

#include "platform/windows/window_event_monitor.hpp"

#include <iostream>
#include <sstream>

#include "core/logger.hpp"

namespace minimize::platform {
namespace {

std::wstring EventName(DWORD event) {
  switch (event) {
    case EVENT_SYSTEM_MINIMIZESTART:
      return L"EVENT_SYSTEM_MINIMIZESTART";
    case EVENT_SYSTEM_MINIMIZEEND:
      return L"EVENT_SYSTEM_MINIMIZEEND";
    case EVENT_OBJECT_SHOW:
      return L"EVENT_OBJECT_SHOW";
    case EVENT_SYSTEM_FOREGROUND:
      return L"EVENT_SYSTEM_FOREGROUND";
    case EVENT_OBJECT_STATECHANGE:
      return L"EVENT_OBJECT_STATECHANGE";
    default:
      return L"EVENT_" + std::to_wstring(event);
  }
}

std::wstring WindowBrief(HWND window) {
  std::wstringstream ss;
  ss << L"hwnd=0x" << std::hex << reinterpret_cast<std::uintptr_t>(window) << std::dec;
  if (window == nullptr || !IsWindow(window)) {
    ss << L" invalid";
    return ss.str();
  }

  wchar_t class_name[256]{};
  GetClassNameW(window, class_name, 256);
  wchar_t title[256]{};
  GetWindowTextW(window, title, 256);
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const BOOL has_placement = GetWindowPlacement(window, &placement);
  ss << L" class=\"" << class_name << L"\" title=\"" << title << L"\"" << L" visible="
     << (IsWindowVisible(window) != FALSE) << L" iconic=" << (IsIconic(window) != FALSE);
  if (has_placement) {
    ss << L" showCmd=" << placement.showCmd;
  }
  return ss.str();
}

}  // namespace

WindowEventMonitor* WindowEventMonitor::active_monitor_ = nullptr;

WindowEventMonitor::~WindowEventMonitor() { Stop(); }

bool WindowEventMonitor::Start(WindowCallback minimize_start_callback,
                               WindowCallback restore_start_callback,
                               WindowSeenCallback window_seen_callback) {
  Stop();
  minimize_start_callback_ = std::move(minimize_start_callback);
  restore_start_callback_ = std::move(restore_start_callback);
  window_seen_callback_ = std::move(window_seen_callback);
  active_monitor_ = this;

  constexpr DWORD kHookFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
  minimize_hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr,
                                   &WindowEventMonitor::HandleWinEvent, 0, 0, kHookFlags);
  restore_hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                                  &WindowEventMonitor::HandleWinEvent, 0, 0, kHookFlags);
  show_hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                               &WindowEventMonitor::HandleWinEvent, 0, 0, kHookFlags);
  foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                     &WindowEventMonitor::HandleWinEvent, 0, 0, kHookFlags);
  state_change_hook_ = SetWinEventHook(EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE, nullptr,
                                       &WindowEventMonitor::HandleWinEvent, 0, 0, kHookFlags);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &WindowEventMonitor::MessageWindowProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"MinimizeShellHookWindow";
  RegisterClassExW(&wc);

  message_window_ = CreateWindowExW(0, L"MinimizeShellHookWindow", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, wc.hInstance, nullptr);
  if (message_window_) {
    RegisterShellHookWindow(message_window_);
    shell_hook_message_ = RegisterWindowMessageW(L"SHELLHOOK");
  }

  if (minimize_hook_ == nullptr || restore_hook_ == nullptr || show_hook_ == nullptr ||
      foreground_hook_ == nullptr || state_change_hook_ == nullptr) {
    std::wcerr << L"SetWinEventHook failed.\n";
    minimize::core::LogDebug(L"WinEvent",
                             L"SetWinEventHook failed error=" + std::to_wstring(GetLastError()));
    Stop();
    return false;
  }

  minimize::core::LogDebug(L"WinEvent", L"Started WinEvent hooks and shell hook window");
  return true;
}

void WindowEventMonitor::Stop() {
  if (minimize_hook_ != nullptr) {
    UnhookWinEvent(minimize_hook_);
    minimize_hook_ = nullptr;
  }
  if (restore_hook_ != nullptr) {
    UnhookWinEvent(restore_hook_);
    restore_hook_ = nullptr;
  }
  if (show_hook_ != nullptr) {
    UnhookWinEvent(show_hook_);
    show_hook_ = nullptr;
  }
  if (foreground_hook_ != nullptr) {
    UnhookWinEvent(foreground_hook_);
    foreground_hook_ = nullptr;
  }
  if (state_change_hook_ != nullptr) {
    UnhookWinEvent(state_change_hook_);
    state_change_hook_ = nullptr;
  }
  if (message_window_ != nullptr) {
    DeregisterShellHookWindow(message_window_);
    DestroyWindow(message_window_);
    message_window_ = nullptr;
  }
  if (active_monitor_ == this) {
    active_monitor_ = nullptr;
  }
}

void CALLBACK WindowEventMonitor::HandleWinEvent(HWINEVENTHOOK hook, DWORD event, HWND window,
                                                 LONG object_id, LONG child_id, DWORD event_thread,
                                                 DWORD event_time) {
  (void)hook;
  (void)event_thread;
  (void)event_time;
  if (active_monitor_ != nullptr) {
    active_monitor_->OnWinEvent(event, window, object_id, child_id);
  }
}

LRESULT CALLBACK WindowEventMonitor::MessageWindowProc(HWND hwnd, UINT msg, WPARAM w_param,
                                                       LPARAM l_param) {
  if (active_monitor_ != nullptr) {
    return active_monitor_->OnShellMessage(msg, w_param, l_param);
  }
  return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void WindowEventMonitor::HandleMinimizeStart(HWND window) {
  minimize::core::LogTrace(L"WinEvent", L"HandleMinimizeStart " + WindowBrief(window));
  if (minimize_start_callback_) {
    minimize_start_callback_(window);
  }
}

void WindowEventMonitor::HandleRestoreStart(HWND window) {
  minimize::core::LogTrace(L"WinEvent", L"HandleRestoreStart " + WindowBrief(window));
  if (restore_start_callback_) {
    restore_start_callback_(window);
  }
}

LRESULT WindowEventMonitor::OnShellMessage(UINT msg, WPARAM w_param, LPARAM l_param) {
  if (msg != shell_hook_message_ || shell_hook_message_ == 0) {
    return DefWindowProcW(message_window_, msg, w_param, l_param);
  }

  const int shell_code = static_cast<int>(w_param);
  if (shell_code != HSHELL_GETMINRECT) {
    return DefWindowProcW(message_window_, msg, w_param, l_param);
  }

  const auto* info = reinterpret_cast<const SHELLHOOKINFO*>(l_param);
  if (info == nullptr) {
    return DefWindowProcW(message_window_, msg, w_param, l_param);
  }

  HWND window = info->hwnd;
  minimize::core::LogTrace(L"WinEvent", L"ShellHook HSHELL_GETMINRECT " + WindowBrief(window));
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(window, &placement)) {
    minimize::core::LogTrace(L"WinEvent", L"ShellHook GetWindowPlacement failed error=" +
                                              std::to_wstring(GetLastError()) + L" " +
                                              WindowBrief(window));
    return DefWindowProcW(message_window_, msg, w_param, l_param);
  }

  if (placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE ||
      IsIconic(window)) {
    HandleMinimizeStart(window);
  } else {
    minimize::core::LogTrace(L"WinEvent",
                             L"ShellHook ignored because window is not minimized showCmd=" +
                                 std::to_wstring(placement.showCmd) + L" " + WindowBrief(window));
  }

  return DefWindowProcW(message_window_, msg, w_param, l_param);
}

void WindowEventMonitor::OnWinEvent(DWORD event, HWND window, LONG object_id, LONG child_id) {
  if (window == nullptr) {
    minimize::core::LogTrace(L"WinEvent",
                             L"OnWinEvent ignored null window event=" + EventName(event));
    return;
  }

  minimize::core::LogTrace(L"WinEvent", L"OnWinEvent event=" + EventName(event) + L" object_id=" +
                                            std::to_wstring(object_id) + L" child_id=" +
                                            std::to_wstring(child_id) + L" " + WindowBrief(window));

  if (event == EVENT_SYSTEM_MINIMIZESTART) {
    HandleMinimizeStart(window);
    return;
  }

  if (event == EVENT_SYSTEM_MINIMIZEEND) {
    HandleRestoreStart(window);
    return;
  }

  if (object_id != OBJID_WINDOW || child_id != CHILDID_SELF) {
    minimize::core::LogTrace(L"WinEvent",
                             L"OnWinEvent ignored non-window object event=" + EventName(event) +
                                 L" object_id=" + std::to_wstring(object_id) + L" child_id=" +
                                 std::to_wstring(child_id) + L" " + WindowBrief(window));
    return;
  }

  if (window_seen_callback_) {
    minimize::core::LogTrace(L"WinEvent", L"OnWinEvent dispatch window_seen event=" +
                                              EventName(event) + L" " + WindowBrief(window));
    window_seen_callback_(window, event);
  }
}

}  // namespace minimize::platform
