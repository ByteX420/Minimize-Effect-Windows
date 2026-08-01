#pragma once

#include <optional>
#include <vector>
#include <windows.h>

namespace minimize::platform {

[[nodiscard]] std::optional<RECT> GetExtendedFrameBounds(HWND window);
[[nodiscard]] std::optional<WINDOWPLACEMENT> GetWindowPlacementSnapshot(HWND window);
[[nodiscard]] bool IsInterestingTopLevelWindow(HWND window, HWND ignored_window = nullptr);
[[nodiscard]] std::vector<HWND> EnumerateTopLevelWindows(HWND ignored_window = nullptr);
void SetDwmTransitionsDisabled(HWND window, bool disabled);
[[nodiscard]] bool SetWindowCloaked(HWND window, bool cloaked);
[[nodiscard]] bool SetOwnedWindowRegion(HWND window, HRGN region, bool redraw);
[[nodiscard]] bool BringWindowToCaptureForeground(HWND window);
[[nodiscard]] bool IsExactForegroundWindow(HWND window, HWND ignored_window = nullptr);

}  // namespace minimize::platform
