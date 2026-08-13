#include "pch.hpp"

#include "features/open_windows_service.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "platform/windows/display_info.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/window_state.hpp"

namespace minimize::features {
namespace {

bool IsUsableRect(const RECT& rect) {
  return rect.right > rect.left && rect.bottom > rect.top && rect.left > -30000 &&
         rect.top > -30000;
}

std::wstring ReadWindowTitle(HWND window) {
  wchar_t title[512]{};
  const int length = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
  if (length <= 0) return L"(Untitled)";
  return std::wstring(title, title + length);
}

}  // namespace

RECT MapDesktopRectToView(const RECT& desktop_rect, const RECT& desktop_space, float view_x,
                          float view_y, float view_w, float view_h) {
  const float space_w = static_cast<float>(std::max(1L, desktop_space.right - desktop_space.left));
  const float space_h = static_cast<float>(std::max(1L, desktop_space.bottom - desktop_space.top));
  const float scale = std::min(view_w / space_w, view_h / space_h);
  const float used_w = space_w * scale;
  const float used_h = space_h * scale;
  const float origin_x = view_x + (view_w - used_w) * 0.5f;
  const float origin_y = view_y + (view_h - used_h) * 0.5f;

  const float left = origin_x + static_cast<float>(desktop_rect.left - desktop_space.left) * scale;
  const float top = origin_y + static_cast<float>(desktop_rect.top - desktop_space.top) * scale;
  const float right =
      origin_x + static_cast<float>(desktop_rect.right - desktop_space.left) * scale;
  const float bottom =
      origin_y + static_cast<float>(desktop_rect.bottom - desktop_space.top) * scale;

  RECT out{};
  out.left = static_cast<LONG>(std::floor(left));
  out.top = static_cast<LONG>(std::floor(top));
  out.right = static_cast<LONG>(std::ceil(right));
  out.bottom = static_cast<LONG>(std::ceil(bottom));
  if (out.right <= out.left) out.right = out.left + 1;
  if (out.bottom <= out.top) out.bottom = out.top + 1;
  return out;
}

OpenWindowsService::OpenWindowsService(WindowExclusionService& exclusions)
    : exclusions_(exclusions) {}

OpenWindowsSnapshot OpenWindowsService::Capture(HWND overlay_window, HWND settings_window,
                                                HWND last_active_window) const {
  exclusions_.PruneInvalidWindows();

  OpenWindowsSnapshot snapshot{};
  snapshot.virtual_bounds = platform::GetVirtualScreenRect();

  struct MonitorEnumContext {
    std::vector<OpenMonitorInfo>* monitors = nullptr;
  } monitor_context{&snapshot.monitors};

  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto* context = reinterpret_cast<MonitorEnumContext*>(parameter);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;
        OpenMonitorInfo entry{};
        entry.monitor = monitor;
        entry.bounds = info.rcMonitor;
        entry.work_area = info.rcWork;
        entry.is_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        entry.device_name = WindowExclusionService::MonitorDeviceName(monitor);
        context->monitors->push_back(entry);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitor_context));

  // Match Windows Settings numbering: primary is always 1; remaining 2..N left→right, top→bottom.
  std::stable_sort(snapshot.monitors.begin(), snapshot.monitors.end(),
                   [](const OpenMonitorInfo& a, const OpenMonitorInfo& b) {
                     if (a.is_primary != b.is_primary) return a.is_primary && !b.is_primary;
                     if (a.bounds.left != b.bounds.left) return a.bounds.left < b.bounds.left;
                     return a.bounds.top < b.bounds.top;
                   });
  for (int i = 0; i < static_cast<int>(snapshot.monitors.size()); ++i) {
    snapshot.monitors[i].index = i;
    snapshot.monitors[i].label = std::format("{}", i + 1);
    snapshot.monitors[i].minimize_excluded =
        exclusions_.IsDisplayExcluded(snapshot.monitors[i].device_name);
  }

  HWND foreground = GetForegroundWindow();
  const bool foreground_is_application =
      foreground != nullptr && platform::IsInterestingTopLevelWindow(foreground, overlay_window);
  if ((foreground == settings_window || !foreground_is_application) &&
      last_active_window != nullptr && IsWindow(last_active_window)) {
    foreground = last_active_window;
  }
  for (HWND window : platform::EnumerateTopLevelWindows(overlay_window)) {
    if (window == settings_window) continue;
    if (!platform::IsInterestingTopLevelWindow(window, overlay_window)) continue;
    if (window == settings_window) continue;

    const auto bounds = platform::GetExtendedFrameBounds(window);
    if (!bounds.has_value() || !IsUsableRect(*bounds)) continue;

    OpenWindowInfo info{};
    info.window = window;
    info.process_id = platform::WindowProcessId(window);
    info.title = ReadWindowTitle(window);
    const auto executable = platform::GetWindowExecutableName(window);
    info.executable_name = executable.value_or("");
    info.bounds = *bounds;
    info.minimized = IsIconic(window) != FALSE;
    info.foreground = window == foreground;
    info.maximized = IsZoomed(window) != FALSE;
    info.minimize_excluded = exclusions_.IsExcluded(window);
    info.monitor = MonitorFromRect(&info.bounds, MONITOR_DEFAULTTONEAREST);

    for (const OpenMonitorInfo& monitor : snapshot.monitors) {
      if (monitor.monitor == info.monitor) {
        info.monitor_bounds = monitor.bounds;
        info.monitor_index = monitor.index;
        break;
      }
    }
    if (!IsUsableRect(info.monitor_bounds) && !snapshot.monitors.empty()) {
      info.monitor_bounds = snapshot.monitors.front().bounds;
      info.monitor = snapshot.monitors.front().monitor;
      info.monitor_index = snapshot.monitors.front().index;
    }

    snapshot.windows.push_back(std::move(info));
  }

  // Z-order approximate: EnumWindows is front-to-back; reverse so later draws are on top.
  // Keep enumeration order for hit-testing from topmost first in UI.
  return snapshot;
}

}  // namespace minimize::features
