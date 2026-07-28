#include "pch.hpp"

#include "settings/settings_validator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "settings/exclusion_rules.hpp"

namespace minimize::settings {
namespace {

template <std::size_t Size>
bool IsOneOf(std::string_view value, const std::array<std::string_view, Size>& choices) {
  return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool IsValidEasing(std::string_view value) {
  constexpr std::array choices = {
      std::string_view{"Linear"},      std::string_view{"Ease In"}, std::string_view{"Ease Out"},
      std::string_view{"Ease In Out"}, std::string_view{"Cubic"},   std::string_view{"Back"},
      std::string_view{"Elastic"},     std::string_view{"Custom"},
  };
  return IsOneOf(value, choices);
}

bool IsValidStyle(std::string_view value) {
  constexpr std::array choices = {
      std::string_view{"Genie classic"},
      std::string_view{"Genie curvy"},
      std::string_view{"Squash"},
      std::string_view{"Classic Minimize"},
  };
  return IsOneOf(value, choices);
}

}  // namespace

AppSettings SettingsValidator::Normalize(AppSettings settings) {
  settings.minimize_duration = std::clamp(settings.minimize_duration, 0.10f, 2.00f);
  settings.restore_duration = std::clamp(settings.restore_duration, 0.10f, 2.00f);
  settings.cancel_duration = std::clamp(settings.cancel_duration, 0.10f, 2.00f);
  settings.minimize_strength = std::clamp(settings.minimize_strength, 0.25f, 1.00f);
  if (!IsValidEasing(settings.minimize_easing)) settings.minimize_easing = "Ease In Out";
  if (!IsValidEasing(settings.restore_easing)) settings.restore_easing = "Ease In Out";
  if (!IsValidEasing(settings.cancel_easing)) settings.cancel_easing = "Linear";
  settings.minimize_custom_bezier.ClampHandles();
  settings.restore_custom_bezier.ClampHandles();
  settings.cancel_custom_bezier.ClampHandles();
  if (settings.animation_style == "Classic Minimize") {
    settings.animation_style = "Genie classic";
  }
  if (!IsValidStyle(settings.animation_style)) settings.animation_style = "Genie classic";
  if (settings.quality_mode != "automatic" && settings.quality_mode != "best_quality" &&
      settings.quality_mode != "power_saving") {
    settings.quality_mode = "automatic";
  }
  if (settings.fade_strength != "No fade" && settings.fade_strength != "Subtle" &&
      settings.fade_strength != "Strong") {
    settings.fade_strength = "Subtle";
  }
  if (settings.close_behavior != "exit" && settings.close_behavior != "tray") {
    settings.close_behavior = "exit";
  }
  NormalizeExcludedApplications(&settings.excluded_applications);
  for (HotkeyBinding& hotkey : settings.hotkeys) {
    hotkey.modifiers &= 0x000fu;
    if (hotkey.virtual_key > 254u) hotkey.virtual_key = 0;
  }
  return settings;
}

bool SettingsValidator::IsValid(const AppSettings& settings) {
  return SettingsValidator::Normalize(settings) == settings;
}

}  // namespace minimize::settings
