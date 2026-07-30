#include "pch.hpp"

#include "features/diagnostics_service.hpp"

#include <cmath>
#include <dxgi.h>
#include <filesystem>
#include <format>
#include <shellapi.h>
#include <sstream>
#include <string_view>
#include <wrl/client.h>

#include "core/logger.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/taskbar_target_provider.hpp"
#include "rendering/d3d_device.hpp"

namespace minimize::features {
namespace {

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

}  // namespace

DiagnosticsSnapshot DiagnosticsService::Build(const DiagnosticsContext& context) const {
  DiagnosticsSnapshot snapshot;
  snapshot.effect = context.effect_active ? "Enabled" : "Paused";
  snapshot.hook = context.hook_installed ? "Installed" : "Not installed";
  snapshot.renderer = context.renderer_recovering ? "Recovering" : "Healthy";
  snapshot.d3d_device =
      context.d3d_device != nullptr && !context.d3d_device->IsDeviceLost() ? "OK" : "Unavailable";
  snapshot.active_animations = std::to_string(context.active_animations);
  snapshot.watchdog = "Per-window cleanup enabled";
  snapshot.startup_repair = context.startup_repair;
  snapshot.log_folder_size =
      std::format("{:.2f} MB", core::DebugLogFolderSize() / (1024.0 * 1024.0));
  snapshot.version = platform::ExecutableProductVersion();
  if (snapshot.version.empty()) snapshot.version = std::string("dev ") + __DATE__;
#ifdef _DEBUG
  snapshot.version += " (Debug)";
#endif

  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
    snapshot.windows_version = std::to_string(version.dwMajorVersion) + "." +
                               std::to_string(version.dwMinorVersion) + " build " +
                               std::to_string(version.dwBuildNumber);
  } else {
    snapshot.windows_version = "Unavailable";
  }

  snapshot.graphics_adapter = "Unavailable";
  if (context.d3d_device != nullptr && context.d3d_device->dxgi_device() != nullptr) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(context.d3d_device->dxgi_device()->GetAdapter(&adapter))) {
      DXGI_ADAPTER_DESC description{};
      if (SUCCEEDED(adapter->GetDesc(&description))) {
        snapshot.graphics_adapter = WideToUtf8(description.Description);
      }
    }
  }

  HWND reference_window = context.reference_window;
  if (reference_window == nullptr || !IsWindow(reference_window)) {
    reference_window = GetForegroundWindow();
  }
  HMONITOR monitor = reference_window == nullptr
                         ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)
                         : MonitorFromWindow(reference_window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
    snapshot.window_monitor = WideToUtf8(monitor_info.szDevice);
    const std::optional<double> refresh = platform::GetMonitorRefreshRateHz(monitor);
    snapshot.display_refresh = refresh.has_value()
                                   ? std::to_string(static_cast<int>(std::lround(*refresh))) + " Hz"
                                   : "Unavailable";
  } else {
    snapshot.window_monitor = "Unavailable";
    snapshot.display_refresh = "Unavailable";
  }

  snapshot.taskbar = "Unavailable";
  if (context.taskbar_targets != nullptr && reference_window != nullptr &&
      IsWindow(reference_window)) {
    RECT bounds{};
    if (GetWindowRect(reference_window, &bounds)) {
      const platform::TaskbarTarget target =
          context.taskbar_targets->GetTargetForWindow(reference_window, bounds);
      switch (target.edge) {
        case animation::MinimizeEdge::kLeft:
          snapshot.taskbar = "Left";
          break;
        case animation::MinimizeEdge::kTop:
          snapshot.taskbar = "Top";
          break;
        case animation::MinimizeEdge::kRight:
          snapshot.taskbar = "Right";
          break;
        case animation::MinimizeEdge::kBottom:
          snapshot.taskbar = "Bottom";
          break;
      }
    }
  }

  int monitor_count = 0;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT, LPARAM data) -> BOOL {
        ++*reinterpret_cast<int*>(data);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitor_count));
  const RECT virtual_screen = platform::GetVirtualScreenRect();
  snapshot.monitor_configuration = std::to_string(monitor_count) + " monitor(s), " +
                                   std::to_string(virtual_screen.right - virtual_screen.left) +
                                   "x" + std::to_string(virtual_screen.bottom - virtual_screen.top);

  std::ostringstream report;
  report << "Minimize Effect Diagnostics\r\n"
         << "Version: " << snapshot.version << "\r\n"
         << "Windows: " << snapshot.windows_version << "\r\n"
         << "Graphics adapter: " << snapshot.graphics_adapter << "\r\n"
         << "Effect: " << snapshot.effect << "\r\n"
         << "Hook: " << snapshot.hook << "\r\n"
         << "Renderer: " << snapshot.renderer << "\r\n"
         << "D3D Device: " << snapshot.d3d_device << "\r\n"
         << "Active animations: " << snapshot.active_animations << "\r\n"
         << "Watchdog: " << snapshot.watchdog << "\r\n"
         << "Display refresh: " << snapshot.display_refresh << "\r\n"
         << "Window monitor: " << snapshot.window_monitor << "\r\n"
         << "Taskbar: " << snapshot.taskbar << "\r\n"
         << "Monitors: " << snapshot.monitor_configuration << "\r\n"
         << "Startup repair: " << snapshot.startup_repair << "\r\n";
  snapshot.report = report.str();
  return snapshot;
}

bool DiagnosticsService::CopyReport(HWND owner, const std::string& report) const {
  const int required =
      MultiByteToWideChar(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), nullptr, 0);
  if (required <= 0 || !OpenClipboard(owner)) return false;
  EmptyClipboard();
  HGLOBAL memory =
      GlobalAlloc(GMEM_MOVEABLE, (static_cast<std::size_t>(required) + 1) * sizeof(wchar_t));
  if (memory == nullptr) {
    CloseClipboard();
    return false;
  }
  auto* text = static_cast<wchar_t*>(GlobalLock(memory));
  MultiByteToWideChar(CP_UTF8, 0, report.data(), static_cast<int>(report.size()), text, required);
  text[required] = L'\0';
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  return true;
}

bool DiagnosticsService::Execute(DiagnosticsAction action,
                                 const DiagnosticsActions& actions) const {
  switch (action) {
    case DiagnosticsAction::kCopy:
      return actions.build_report && CopyReport(actions.owner, actions.build_report());
    case DiagnosticsAction::kOpenLogFolder:
      return OpenLogFolder(actions.owner);
    case DiagnosticsAction::kRepairWindows:
      return actions.repair_windows && actions.repair_windows();
    case DiagnosticsAction::kRestartRenderer:
      return actions.restart_renderer && actions.restart_renderer();
#ifdef _DEBUG
    case DiagnosticsAction::kStressTest:
      return actions.run_stress_test && actions.run_stress_test();
#endif
  }
  return false;
}

bool DiagnosticsService::OpenLogFolder(HWND owner) const {
  const std::filesystem::path folder = std::filesystem::path(core::DebugLogPath()).parent_path();
  return reinterpret_cast<INT_PTR>(
             ShellExecuteW(owner, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

}  // namespace minimize::features
