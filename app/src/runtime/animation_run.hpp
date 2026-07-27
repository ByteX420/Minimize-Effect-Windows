#pragma once

#include <chrono>
#include <windows.h>

#include "platform/windows/taskbar_target_provider.hpp"
#include "rendering/desktop_capture.hpp"
#include "rendering/overlay_window.hpp"
#include "runtime/run_state.hpp"

namespace minimize::runtime {

struct CachedSnapshot {
  HWND window = nullptr;
  RECT bounds{};
  rendering::CapturedTexture texture;
  platform::TaskbarTarget target{};
  bool was_maximized = false;
  bool moved_offscreen = false;
  RECT original_placement{};
  DWORD process_id = 0;
  ULONGLONG captured_at_ms = 0;
};

struct AnimationRun {
  rendering::OverlayWindow overlay;
  HWND animating_window = nullptr;
  HWND pending_native_minimize_window = nullptr;
  bool animating_restore = false;
  bool bulk_animation = false;
  ULONGLONG direction_started_ms = 0;
  RECT live_animation_bounds{};
  ULONGLONG last_animation_texture_refresh_ms = 0;
  HMONITOR animation_monitor = nullptr;
  std::chrono::steady_clock::duration animation_frame_interval{};
  std::chrono::steady_clock::time_point next_animation_frame_time{};
  bool live_animation_capture_enabled = false;
  RunState state = RunState::kIdle;
  ULONGLONG state_entered_ms = 0;
};

}  // namespace minimize::runtime
