#include "pch.hpp"

#include "platform/windows/display_info.hpp"

#include <vector>

#include "platform/windows/process_info.hpp"
#include "platform/windows/window_state.hpp"

namespace minimize::platform {

RECT GetVirtualScreenRect() {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  return RECT{
      .left = left,
      .top = top,
      .right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN),
      .bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN),
  };
}

namespace {

double QueryDisplayConfigRefreshRate(const MONITORINFOEXW& monitor_info) {
  constexpr UINT32 kFlags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
  double max_hertz = 0.0;

  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG result = GetDisplayConfigBufferSizes(kFlags, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) break;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    result =
        QueryDisplayConfig(kFlags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER) continue;
    if (result != ERROR_SUCCESS) break;

    paths.resize(path_count);
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(source_name);
      source_name.header.adapterId = path.sourceInfo.adapterId;
      source_name.header.id = path.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS ||
          _wcsicmp(source_name.viewGdiDeviceName, monitor_info.szDevice) != 0) {
        continue;
      }
      const DISPLAYCONFIG_RATIONAL refresh = path.targetInfo.refreshRate;
      if (refresh.Numerator != 0 && refresh.Denominator != 0) {
        const double hertz =
            static_cast<double>(refresh.Numerator) / static_cast<double>(refresh.Denominator);
        if (hertz > max_hertz) max_hertz = hertz;
      }
    }
    break;
  }
  return max_hertz;
}

}  // namespace

std::optional<double> GetMonitorRefreshRateHz(HMONITOR monitor) {
  if (monitor == nullptr) return std::nullopt;
  MONITORINFOEXW monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!GetMonitorInfoW(monitor, &monitor_info)) return std::nullopt;

  double max_hertz = QueryDisplayConfigRefreshRate(monitor_info);

  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsExW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
      (mode.dmFields & DM_DISPLAYFREQUENCY) != 0 && mode.dmDisplayFrequency > 1) {
    const double enum_hertz = static_cast<double>(mode.dmDisplayFrequency);
    if (enum_hertz > max_hertz) max_hertz = enum_hertz;
  }

  if (max_hertz > 0.0) return max_hertz;
  return std::nullopt;
}

std::optional<RECT> GetMonitorWorkArea(HWND window, const std::optional<RECT>& fallback) {
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr && fallback.has_value()) {
    monitor = MonitorFromRect(&*fallback, MONITOR_DEFAULTTONEAREST);
  }
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) return std::nullopt;
  return monitor_info.rcWork;
}

bool IsFullscreenForegroundWindow(HWND ignored_window, HWND second_ignored_window) {
  HWND foreground = GetForegroundWindow();
  if (foreground == nullptr || foreground == ignored_window ||
      foreground == second_ignored_window || !IsWindow(foreground) ||
      WindowProcessId(foreground) == GetCurrentProcessId() || !IsWindowVisible(foreground) ||
      IsIconic(foreground) || IsZoomed(foreground) ||
      !IsInterestingTopLevelWindow(foreground, ignored_window)) {
    return false;
  }
  RECT window_rect{};
  const auto extended_bounds = GetExtendedFrameBounds(foreground);
  if (extended_bounds.has_value()) {
    window_rect = *extended_bounds;
  } else if (!GetWindowRect(foreground, &window_rect)) {
    return false;
  }
  HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) return false;
  constexpr LONG kTolerance = 2;
  const bool covers_monitor = window_rect.left <= monitor_info.rcMonitor.left + kTolerance &&
                              window_rect.top <= monitor_info.rcMonitor.top + kTolerance &&
                              window_rect.right >= monitor_info.rcMonitor.right - kTolerance &&
                              window_rect.bottom >= monitor_info.rcMonitor.bottom - kTolerance;
  if (!covers_monitor) return false;
  const LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
  return (style & WS_CAPTION) == 0 || (style & WS_THICKFRAME) == 0;
}

}  // namespace minimize::platform
