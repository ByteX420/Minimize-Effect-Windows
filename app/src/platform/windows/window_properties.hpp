#pragma once

#include <cstdint>
#include <optional>
#include <windows.h>

namespace minimize::platform::windows::properties {

inline constexpr wchar_t kQueryWindowStateMessage[] = L"MinimizeQueryWindowState";

enum class WindowFlag {
  kAllowMinimize,
  kAllowRestore,
  kExcludedApplication,
  kIsMinimizing,
  kMovedOffscreen,
};

enum HookState : std::uint32_t {
  kHookStateNone = 0,
  kHookStateAllowMinimize = 1U << 0,
  kHookStateAllowRestore = 1U << 1,
  kHookStateExcluded = 1U << 2,
};

void StoreOriginalPlacement(HWND window, const RECT& rect);
[[nodiscard]] std::optional<RECT> ReadOriginalPlacement(HWND window);
void StoreWasMaximized(HWND window, bool was_maximized);
[[nodiscard]] bool WasMaximized(HWND window);
void SetFlag(HWND window, WindowFlag flag, bool value = true);
[[nodiscard]] bool HasFlag(HWND window, WindowFlag flag);
[[nodiscard]] std::uint32_t QueryHookState(HWND window);
[[nodiscard]] bool HasMinimizeState(HWND window);
void ClearMinimizeState(HWND window);
void DiscardState(HWND window);
void ClearAllState();
[[nodiscard]] bool MakeTransparent(HWND window);
void RestoreTransparency(HWND window);

}  // namespace minimize::platform::windows::properties
