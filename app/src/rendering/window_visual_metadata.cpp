#include "pch.hpp"

#include "rendering/window_visual_metadata.hpp"

#include <algorithm>
#include <dwmapi.h>
#include <wil/resource.h>

namespace minimize::rendering {
namespace {

float WindowCornerRadius(HWND window) {
  if (window == nullptr || !IsWindow(window) || IsZoomed(window)) return 0.0f;

  DWORD corner_preference = 0;
  constexpr auto kWindowCornerPreference = static_cast<DWMWINDOWATTRIBUTE>(33);
  if (FAILED(DwmGetWindowAttribute(window, kWindowCornerPreference, &corner_preference,
                                   sizeof(corner_preference))) ||
      corner_preference == 1) {
    return 0.0f;
  }
  const int base_radius = corner_preference == 3 ? 8 : 12;
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
  metadata.corner_radius = metadata.window_region.is_set ? 0.0f : WindowCornerRadius(window);

  const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  metadata.is_layered = (extended_style & WS_EX_LAYERED) != 0;
  if (metadata.is_layered) {
    COLORREF color_key = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    metadata.has_per_pixel_alpha =
        GetLayeredWindowAttributes(window, &color_key, &alpha, &flags) == FALSE;
  }

  if (!IsZoomed(window)) {
    const UINT dpi = std::max(GetDpiForWindow(window), 96U);
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    metadata.shadow_radius = (metadata.is_layered ? 14.0f : 18.0f) * dpi_scale;
    metadata.shadow_opacity = metadata.is_layered ? 0.20f : 0.28f;
  }
  return metadata;
}

}  // namespace minimize::rendering
