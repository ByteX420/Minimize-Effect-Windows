#pragma once

#include <array>
#include <string>
#include <vector>

#include "features/diagnostics_service.hpp"
#include "settings/app_settings.hpp"

namespace minimize::ui {

class SettingsViewModel final {
public:
  void Apply(const settings::AppSettings& settings);
  void SetPause(bool paused, bool until_restart);
  void SetHotkeyAvailability(settings::HotkeyAction action, bool available);

  bool enabled = true;
  bool temporarily_paused = false;
  bool paused_until_restart = false;
  std::array<settings::HotkeyBinding, static_cast<std::size_t>(settings::HotkeyAction::kCount)>
      hotkeys{};
  std::array<bool, static_cast<std::size_t>(settings::HotkeyAction::kCount)> hotkey_available{};
  features::DiagnosticsSnapshot diagnostics;
  float minimize_duration = settings::kDefaultMinimizeDuration;
  float restore_duration = settings::kDefaultRestoreDuration;
  float cancel_duration = settings::kDefaultCancelDuration;
  bool link_speeds = false;
  bool disable_animations_fullscreen = false;
  bool disable_effects_battery_saver = false;
  std::string minimize_easing = "Linear";
  std::string restore_easing = "Linear";
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
  bool run_at_startup = false;
  bool start_minimized = false;
  std::vector<std::string> excluded_applications;
};

}  // namespace minimize::ui
