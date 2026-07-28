#include "pch.hpp"

#include "rendering/window_visual_metadata.hpp"

#include <algorithm>
#include <dwmapi.h>
#include <wil/resource.h>

namespace minimize::rendering {
namespace {

bool RectsNearlyEqual(const RECT& left, const RECT& right, LONG tolerance) {
  return std::abs(left.left - right.left) <= tolerance &&
         std::abs(left.top - right.top) <= tolerance &&
         std::abs(left.right - right.right) <= tolerance &&
         std::abs(left.bottom - right.bottom) <= tolerance;
}

bool IsSnapped(HWND window) {
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  RECT current_rect{};
  if (!GetWindowPlacement(window, &placement) || !GetWindowRect(window, &current_rect) ||
      placement.showCmd != SW_SHOWNORMAL) {
    return false;
  }

  RECT normal_rect = placement.rcNormalPosition;
  const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((extended_style & WS_EX_TOOLWINDOW) == 0) {
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
      OffsetRect(&normal_rect, monitor_info.rcWork.left - monitor_info.rcMonitor.left,
                 monitor_info.rcWork.top - monitor_info.rcMonitor.top);
    }
  }

  const LONG tolerance =
      std::max<LONG>(1, MulDiv(2, std::max(GetDpiForWindow(window), 96U), 96));
  return !RectsNearlyEqual(current_rect, normal_rect, tolerance);
}

float WindowCornerRadius(HWND window) {
  if (window == nullptr || !IsWindow(window) || IsZoomed(window) || IsSnapped(window)) return 0.0f;

  DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DEFAULT;
  constexpr auto kWindowCornerPreference = static_cast<DWMWINDOWATTRIBUTE>(33);
  if (FAILED(DwmGetWindowAttribute(window, kWindowCornerPreference, &corner_preference,
                                   sizeof(corner_preference))) ||
      corner_preference == DWMWCP_DONOTROUND) {
    return 0.0f;
  }

  if (corner_preference == DWMWCP_DEFAULT) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & (WS_CAPTION | WS_THICKFRAME)) == 0) return 0.0f;
  }

  // Windows 11's effective DWM radii are 8 px for normal top-level windows and
  // 4 px for the small-corner preference at 96 DPI.
  const int base_radius = corner_preference == DWMWCP_ROUNDSMALL ? 4 : 8;
  return static_cast<float>(MulDiv(base_radius, std::max(GetDpiForWindow(window), 96U), 96));
}

Region WindowRegion(HWND window) {
  Region result;
  wil::unique_hrgn region(CreateRectRgn(0, 0, 0, 0));
  if (!region) return result;

  const int region_type = GetWindowRgn(window, region.get());
  if (region_type == ERROR) return result;

  result.is_set = true;
  const DWORD bytes = GetRegionData(region.get(), 0, nullptr);
  if (bytes >= sizeof(RGNDATAHEADER)) {
    std::vector<std::byte> storage(bytes);
    auto* data = reinterpret_cast<RGNDATA*>(storage.data());
    if (GetRegionData(region.get(), bytes, data) == bytes) {
      const auto* rectangles = reinterpret_cast<const RECT*>(data->Buffer);
      result.rectangles.assign(rectangles, rectangles + data->rdh.nCount);
    }
  }
  return result;
}

}  // namespace

WindowVisualMetadata QueryWindowVisualMetadata(HWND window) {
  WindowVisualMetadata metadata;
  if (window == nullptr || !IsWindow(window)) return metadata;

  metadata.window_region = WindowRegion(window);

  const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  metadata.is_layered = (extended_style & WS_EX_LAYERED) != 0;
  if (metadata.is_layered) {
    COLORREF color_key = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    metadata.has_per_pixel_alpha =
        GetLayeredWindowAttributes(window, &color_key, &alpha, &flags) == FALSE;
  }
  metadata.corner_radius =
      metadata.window_region.is_set || metadata.has_per_pixel_alpha ? 0.0f
                                                                    : WindowCornerRadius(window);

  if (!IsZoomed(window)) {
    const UINT dpi = std::max(GetDpiForWindow(window), 96U);
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    metadata.shadow_radius = (metadata.is_layered ? 14.0f : 18.0f) * dpi_scale;
    metadata.shadow_opacity = metadata.is_layered ? 0.20f : 0.28f;
  }
  return metadata;
}

}  // namespace minimize::rendering
