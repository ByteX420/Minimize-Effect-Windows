#pragma once

#include <array>
#include <string>
#include <vector>

#include "animation/easing.hpp"
#include "settings/hotkey_binding.hpp"

namespace minimize::settings {

inline constexpr float kDefaultMinimizeDuration = 0.70f;
inline constexpr float kDefaultRestoreDuration = 0.70f;
inline constexpr float kDefaultCancelDuration = 0.35f;

struct UiWindowState {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  bool maximized = false;
  int selected_page = 0;
  float page_scroll = 0.0f;

  [[nodiscard]] bool HasPlacement() const { return right > left && bottom > top; }
  bool operator==(const UiWindowState&) const = default;
};

struct AppSettings {
  bool enabled = true;
  float minimize_duration = kDefaultMinimizeDuration;
  float restore_duration = kDefaultRestoreDuration;
  float cancel_duration = kDefaultCancelDuration;
  bool link_speeds = false;
  bool disable_animations_fullscreen = false;
  bool disable_effects_battery_saver = false;
  std::string minimize_easing = "Ease In Out";
  std::string restore_easing = "Ease In Out";
  std::string cancel_easing = "Linear";
  animation::CubicBezier minimize_custom_bezier = animation::CubicBezier::EaseInOut();
  animation::CubicBezier restore_custom_bezier = animation::CubicBezier::EaseInOut();
  animation::CubicBezier cancel_custom_bezier = animation::CubicBezier::EaseInOut();
  std::string animation_style = "Genie classic";
  std::string quality_mode = "automatic";
  float minimize_strength = 1.0f;
  std::string fade_strength = "Subtle";
  bool show_target_indicator = false;
  bool smart_skip_under_load = true;
  std::string close_behavior = "exit";
  bool start_minimized = false;
  bool run_at_startup = false;
  std::vector<std::string> excluded_applications;
  // Persisted GDI device names (MONITORINFOEX.szDevice), e.g. "\\\\.\\DISPLAY1".
  std::vector<std::string> excluded_displays;
  UiWindowState ui_window;
  std::array<HotkeyBinding, static_cast<std::size_t>(HotkeyAction::kCount)> hotkeys = {
      HotkeyBinding{.modifiers = 0x0001u | 0x0002u, .virtual_key = 'G'},
      HotkeyBinding{},
      HotkeyBinding{},
      HotkeyBinding{.modifiers = 0x0001u | 0x0002u, .virtual_key = 'M'},
      HotkeyBinding{.modifiers = 0x0001u | 0x0002u, .virtual_key = 'R'},
  };

  bool operator==(const AppSettings&) const = default;
};

}  // namespace minimize::settings
