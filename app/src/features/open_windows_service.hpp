#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "features/window_exclusion_service.hpp"

namespace minimize::features {

struct OpenWindowInfo {
  HWND window = nullptr;
  DWORD process_id = 0;
  std::wstring title;
  std::string executable_name;
  RECT bounds{};
  RECT monitor_bounds{};
  HMONITOR monitor = nullptr;
  int monitor_index = 0;
  bool minimized = false;
  bool foreground = false;
  bool maximized = false;
  bool minimize_excluded = false;
};

struct OpenMonitorInfo {
  HMONITOR monitor = nullptr;
  RECT bounds{};
  RECT work_area{};
  int index = 0;            // 0-based; label number = index + 1 (primary is always 1)
  std::string label;        // "1", "2", ...
  std::string device_name;  // MONITORINFOEX.szDevice (persisted exclusion key)
  bool is_primary = false;
  bool minimize_excluded = false;
};

struct OpenWindowsSnapshot {
  std::vector<OpenMonitorInfo> monitors;
  std::vector<OpenWindowInfo> windows;
  RECT virtual_bounds{};
};

// Pure layout helper: map a desktop rect into a view rectangle.
[[nodiscard]] RECT MapDesktopRectToView(const RECT& desktop_rect, const RECT& desktop_space,
                                        float view_x, float view_y, float view_w, float view_h);

class OpenWindowsService final {
public:
  explicit OpenWindowsService(WindowExclusionService& exclusions);

  // Refresh enumeration (not every ImGui frame — callers throttle).
  OpenWindowsSnapshot Capture(HWND overlay_window, HWND settings_window,
                              HWND last_active_window = nullptr) const;

private:
  WindowExclusionService& exclusions_;
};

}  // namespace minimize::features
