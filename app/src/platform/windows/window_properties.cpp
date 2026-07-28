#include "pch.hpp"

#include "platform/windows/window_properties.hpp"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace minimize::platform::windows::properties {
namespace {

struct TransparencyState {
  LONG_PTR extended_style = 0;
  BYTE alpha = 255;
  DWORD flags = 0;
  bool was_layered = false;
};

struct WindowState {
  std::optional<RECT> original_placement;
  std::optional<TransparencyState> transparency;
  bool was_maximized = false;
  bool allow_minimize = false;
  bool allow_restore = false;
  bool excluded_application = false;
  bool is_minimizing = false;
  bool moved_offscreen = false;
};

std::unordered_map<HWND, WindowState> g_window_states;
std::shared_mutex g_window_states_mutex;

bool IsUsableRect(const RECT& rect) {
  return rect.right > rect.left && rect.bottom > rect.top && rect.left > -30000 &&
         rect.top > -30000;
}

bool& FlagValue(WindowState& state, WindowFlag flag) {
  switch (flag) {
    case WindowFlag::kAllowMinimize:
      return state.allow_minimize;
    case WindowFlag::kAllowRestore:
      return state.allow_restore;
    case WindowFlag::kExcludedApplication:
      return state.excluded_application;
    case WindowFlag::kIsMinimizing:
      return state.is_minimizing;
    case WindowFlag::kMovedOffscreen:
      return state.moved_offscreen;
  }
  return state.is_minimizing;
}

bool FlagValue(const WindowState& state, WindowFlag flag) {
  switch (flag) {
    case WindowFlag::kAllowMinimize:
      return state.allow_minimize;
    case WindowFlag::kAllowRestore:
      return state.allow_restore;
    case WindowFlag::kExcludedApplication:
      return state.excluded_application;
    case WindowFlag::kIsMinimizing:
      return state.is_minimizing;
    case WindowFlag::kMovedOffscreen:
      return state.moved_offscreen;
  }
  return false;
}

bool IsEmpty(const WindowState& state) {
  return !state.original_placement.has_value() && !state.transparency.has_value() &&
         !state.was_maximized && !state.allow_minimize && !state.allow_restore &&
         !state.excluded_application && !state.is_minimizing && !state.moved_offscreen;
}

}  // namespace

void StoreOriginalPlacement(HWND window, const RECT& rect) {
  if (window == nullptr || !IsUsableRect(rect)) return;
  std::unique_lock lock(g_window_states_mutex);
  g_window_states[window].original_placement = rect;
}

std::optional<RECT> ReadOriginalPlacement(HWND window) {
  std::shared_lock lock(g_window_states_mutex);
  const auto found = g_window_states.find(window);
  if (found == g_window_states.end() || !found->second.original_placement.has_value() ||
      !IsUsableRect(*found->second.original_placement)) {
    return std::nullopt;
  }
  return found->second.original_placement;
}

void StoreWasMaximized(HWND window, bool was_maximized) {
  if (window == nullptr) return;
  std::unique_lock lock(g_window_states_mutex);
  g_window_states[window].was_maximized = was_maximized;
}

bool WasMaximized(HWND window) {
  std::shared_lock lock(g_window_states_mutex);
  const auto found = g_window_states.find(window);
  return found != g_window_states.end() && found->second.was_maximized;
}

void SetFlag(HWND window, WindowFlag flag, bool value) {
  if (window == nullptr) return;
  std::unique_lock lock(g_window_states_mutex);
  if (value) {
    FlagValue(g_window_states[window], flag) = true;
    return;
  }
  const auto found = g_window_states.find(window);
  if (found == g_window_states.end()) return;
  FlagValue(found->second, flag) = false;
  if (IsEmpty(found->second)) g_window_states.erase(found);
}

bool HasFlag(HWND window, WindowFlag flag) {
  std::shared_lock lock(g_window_states_mutex);
  const auto found = g_window_states.find(window);
  return found != g_window_states.end() && FlagValue(found->second, flag);
}

std::uint32_t QueryHookState(HWND window) {
  std::shared_lock lock(g_window_states_mutex);
  const auto found = g_window_states.find(window);
  if (found == g_window_states.end()) return kHookStateNone;
  std::uint32_t state = kHookStateNone;
  if (found->second.allow_minimize) state |= kHookStateAllowMinimize;
  if (found->second.allow_restore) state |= kHookStateAllowRestore;
  if (found->second.excluded_application) state |= kHookStateExcluded;
  return state;
}

bool HasMinimizeState(HWND window) {
  std::shared_lock lock(g_window_states_mutex);
  const auto found = g_window_states.find(window);
  if (found == g_window_states.end()) return false;
  const WindowState& state = found->second;
  return state.moved_offscreen || state.transparency.has_value() ||
         state.original_placement.has_value();
}

void ClearMinimizeState(HWND window) {
  if (window == nullptr) return;
  RestoreTransparency(window);
  std::unique_lock lock(g_window_states_mutex);
  g_window_states.erase(window);
}

void ClearAllState() {
  std::unique_lock lock(g_window_states_mutex);
  g_window_states.clear();
}

bool MakeTransparent(HWND window) {
  if (window == nullptr || !IsWindow(window)) return false;
  {
    std::shared_lock lock(g_window_states_mutex);
    const auto found = g_window_states.find(window);
    if (found != g_window_states.end() && found->second.transparency.has_value()) return true;
  }

  const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  BYTE alpha = 255;
  DWORD flags = 0;
  const bool was_layered = (extended_style & WS_EX_LAYERED) != 0;
  if (was_layered) GetLayeredWindowAttributes(window, nullptr, &alpha, &flags);
  {
    std::unique_lock lock(g_window_states_mutex);
    g_window_states[window].transparency = TransparencyState{
        .extended_style = extended_style,
        .alpha = alpha,
        .flags = flags,
        .was_layered = was_layered,
    };
  }

  SetLastError(ERROR_SUCCESS);
  const LONG_PTR updated_style =
      SetWindowLongPtrW(window, GWL_EXSTYLE, extended_style | WS_EX_LAYERED);
  if ((updated_style == 0 && GetLastError() != ERROR_SUCCESS) ||
      !SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA)) {
    RestoreTransparency(window);
    return false;
  }
  return true;
}

void RestoreTransparency(HWND window) {
  if (window == nullptr || !IsWindow(window)) return;
  std::optional<TransparencyState> transparency;
  {
    std::unique_lock lock(g_window_states_mutex);
    const auto found = g_window_states.find(window);
    if (found == g_window_states.end() || !found->second.transparency.has_value()) return;
    transparency = found->second.transparency;
    found->second.transparency.reset();
  }

  const LONG_PTR current_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (transparency->was_layered) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, current_style | WS_EX_LAYERED);
    SetLayeredWindowAttributes(window, 0, transparency->alpha, transparency->flags);
  } else {
    SetWindowLongPtrW(window, GWL_EXSTYLE, current_style & ~WS_EX_LAYERED);
  }
}

}  // namespace minimize::platform::windows::properties
