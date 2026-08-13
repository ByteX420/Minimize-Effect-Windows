#include "pch.hpp"

#include "features/settings_mutation_service.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "core/logger.hpp"
#include "platform/windows/startup_manager.hpp"
#include "settings/exclusion_rules.hpp"
#include "settings/settings_repository.hpp"
#include "settings/settings_serializer.hpp"
#include "settings/settings_validator.hpp"

namespace minimize::features {

bool SettingsMutationService::SetEnabled(bool enabled, const std::function<void()>& applied) {
  if (settings_.Get().enabled == enabled) return true;
  auto proposed = settings_.Get();
  proposed.enabled = enabled;
  if (!settings_.Update(std::move(proposed))) {
    core::LogDebug(L"Settings", L"Failed to persist enabled state");
    return false;
  }
  if (applied) applied();
  return true;
}

bool SettingsMutationService::SetAnimationDurations(float minimize, float restore, float cancel,
                                                    bool save) {
  auto proposed = settings_.Get();
  proposed.minimize_duration = std::clamp(minimize, 0.10f, 2.00f);
  proposed.restore_duration = std::clamp(restore, 0.10f, 2.00f);
  proposed.cancel_duration = std::clamp(cancel, 0.10f, 2.00f);
  const bool succeeded = !save || settings_.Update(proposed);
  if (!save) settings_.Preview(std::move(proposed));
  if (!succeeded) core::LogDebug(L"Settings", L"Failed to persist animation durations");
  return succeeded;
}

bool SettingsMutationService::SetLinkSpeeds(bool linked) {
  auto proposed = settings_.Get();
  proposed.link_speeds = linked;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetDisableAnimationsFullscreen(bool enabled,
                                                             const std::function<void()>& applied) {
  auto proposed = settings_.Get();
  proposed.disable_animations_fullscreen = enabled;
  if (!settings_.Update(std::move(proposed))) return false;
  if (applied) applied();
  return true;
}

bool SettingsMutationService::SetDisableEffectsBatterySaver(bool enabled,
                                                            const std::function<void()>& applied) {
  auto proposed = settings_.Get();
  proposed.disable_effects_battery_saver = enabled;
  if (!settings_.Update(std::move(proposed))) return false;
  if (applied) applied();
  return true;
}

bool SettingsMutationService::SetEasing(const std::string& minimize, const std::string& restore) {
  constexpr std::array names = {
      std::string_view{"Linear"},      std::string_view{"Ease In"}, std::string_view{"Ease Out"},
      std::string_view{"Ease In Out"}, std::string_view{"Cubic"},   std::string_view{"Back"},
      std::string_view{"Elastic"},     std::string_view{"Custom"},
  };
  const auto valid = [&names](std::string_view value) {
    return std::find(names.begin(), names.end(), value) != names.end();
  };
  if (!valid(minimize) || !valid(restore)) return false;
  auto proposed = settings_.Get();
  proposed.minimize_easing = minimize;
  proposed.restore_easing = restore;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetCustomEasingBezier(bool minimize, animation::CubicBezier bezier,
                                                    bool save) {
  bezier.ClampHandles();
  auto proposed = settings_.Get();
  if (minimize)
    proposed.minimize_custom_bezier = bezier;
  else
    proposed.restore_custom_bezier = bezier;
  if (!save) {
    settings_.Preview(std::move(proposed));
    return true;
  }
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetCancelEasing(const std::string& easing) {
  constexpr std::array names = {
      std::string_view{"Linear"},      std::string_view{"Ease In"}, std::string_view{"Ease Out"},
      std::string_view{"Ease In Out"}, std::string_view{"Cubic"},   std::string_view{"Back"},
      std::string_view{"Elastic"},     std::string_view{"Custom"},
  };
  if (std::find(names.begin(), names.end(), easing) == names.end()) return false;
  auto proposed = settings_.Get();
  proposed.cancel_easing = easing;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetCancelCustomBezier(animation::CubicBezier bezier, bool save) {
  bezier.ClampHandles();
  auto proposed = settings_.Get();
  proposed.cancel_custom_bezier = bezier;
  if (!save) {
    settings_.Preview(std::move(proposed));
    return true;
  }
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetAnimationStyle(const std::string& style) {
  if (style != "Genie classic" && style != "Genie curvy" && style != "Squash") return false;
  auto proposed = settings_.Get();
  proposed.animation_style = style;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetQualityMode(const std::string& mode) {
  if (mode != "automatic" && mode != "best_quality" && mode != "power_saving") return false;
  auto proposed = settings_.Get();
  proposed.quality_mode = mode;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetMinimizeStrength(float strength, bool save) {
  auto proposed = settings_.Get();
  proposed.minimize_strength = std::clamp(strength, 0.25f, 1.0f);
  const bool succeeded = !save || settings_.Update(proposed);
  if (!save) settings_.Preview(std::move(proposed));
  return succeeded;
}

bool SettingsMutationService::SetFadeStrength(const std::string& strength) {
  if (strength != "No fade" && strength != "Subtle" && strength != "Strong") return false;
  auto proposed = settings_.Get();
  proposed.fade_strength = strength;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::ResetMotionSettings() {
  // One write for the whole Motion tab so intermediate UpdateState reloads cannot clobber
  // fields that have not been saved yet (chained per-field setters were only lasting for speeds).
  const settings::AppSettings defaults{};
  auto proposed = settings_.Get();
  proposed.minimize_duration = defaults.minimize_duration;
  proposed.restore_duration = defaults.restore_duration;
  proposed.cancel_duration = defaults.cancel_duration;
  proposed.link_speeds = defaults.link_speeds;
  proposed.minimize_easing = defaults.minimize_easing;
  proposed.restore_easing = defaults.restore_easing;
  proposed.cancel_easing = defaults.cancel_easing;
  proposed.minimize_custom_bezier = defaults.minimize_custom_bezier;
  proposed.restore_custom_bezier = defaults.restore_custom_bezier;
  proposed.cancel_custom_bezier = defaults.cancel_custom_bezier;
  proposed.animation_style = defaults.animation_style;
  proposed.quality_mode = defaults.quality_mode;
  proposed.minimize_strength = defaults.minimize_strength;
  proposed.fade_strength = defaults.fade_strength;
  proposed.show_target_indicator = defaults.show_target_indicator;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetTargetIndicator(bool enabled) {
  auto proposed = settings_.Get();
  proposed.show_target_indicator = enabled;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetSmartSkipUnderLoad(bool enabled,
                                                    const std::function<void()>& applied) {
  auto proposed = settings_.Get();
  proposed.smart_skip_under_load = enabled;
  if (!settings_.Update(std::move(proposed))) return false;
  if (applied) applied();
  return true;
}

bool SettingsMutationService::SetCloseBehavior(const std::string& behavior) {
  if (behavior != "exit" && behavior != "tray") return false;
  auto proposed = settings_.Get();
  proposed.close_behavior = behavior;
  return settings_.Update(std::move(proposed));
}

bool SettingsMutationService::SetStartupOptions(bool run_at_startup, bool start_minimized) {
  const auto previous = settings_.Get();
  auto proposed = previous;
  proposed.run_at_startup = run_at_startup;
  proposed.start_minimized = start_minimized;
  if (!settings_.Update(std::move(proposed))) return false;
  if (run_at_startup != previous.run_at_startup &&
      !platform::windows::ConfigureRunAtStartup(run_at_startup)) {
    (void)settings_.Update(previous);
    return false;
  }
  return true;
}

bool SettingsMutationService::SetDisplayMinimizeExcluded(const std::string& device_name,
                                                         bool excluded,
                                                         const std::function<void()>& applied) {
  if (device_name.empty()) return false;
  auto proposed = settings_.Get();
  auto& displays = proposed.excluded_displays;
  const auto existing = std::find(displays.begin(), displays.end(), device_name);
  if (excluded) {
    if (existing == displays.end()) displays.push_back(device_name);
  } else if (existing != displays.end()) {
    displays.erase(existing);
  } else {
    return true;
  }
  if (!settings_.Update(std::move(proposed))) return false;
  if (applied) applied();
  return true;
}

bool SettingsMutationService::SetApplicationExcluded(const std::string& executable, bool excluded,
                                                     const std::function<void()>& applied) {
  std::optional<std::string> normalized = settings::NormalizeExecutableName(executable);
  if (!normalized.has_value()) return false;
  auto proposed = settings_.Get();
  auto& applications = proposed.excluded_applications;
  const auto existing = std::find_if(applications.begin(), applications.end(),
                                     [&normalized](const std::string& entry) {
                                       return settings::ExecutableNamesEqual(entry, *normalized);
                                     });
  if (excluded) {
    if (existing != applications.end()) return false;
    applications.push_back(std::move(*normalized));
  } else {
    if (existing == applications.end()) return false;
    applications.erase(existing);
  }
  if (!settings_.Update(std::move(proposed))) return false;
  if (applied) applied();
  return true;
}

bool SettingsMutationService::ImportSettingsFromFile(const std::wstring& path,
                                                     const std::function<void()>& applied,
                                                     bool* out_startup_registration_failed) {
  if (out_startup_registration_failed) *out_startup_registration_failed = false;
  if (path.empty()) return false;
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input) {
    core::LogDebug(L"Settings", L"Import failed: could not open file");
    return false;
  }
  std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (json.empty() || json.size() > 1024 * 1024) {
    core::LogDebug(L"Settings", L"Import failed: empty or oversized file");
    return false;
  }
  auto decoded = settings::SettingsSerializer::Deserialize(json);
  if (!decoded.has_value()) {
    core::LogDebug(L"Settings", L"Import failed: invalid JSON");
    return false;
  }
  auto proposed = settings::SettingsValidator::Normalize(std::move(*decoded));

  // Backup current settings next to the live file before overwriting.
  const std::wstring live = settings::SettingsRepository::Path();
  if (!live.empty()) {
    const std::filesystem::path backup = std::filesystem::path(live).wstring() + L".bak";
    std::error_code error;
    std::filesystem::copy_file(live, backup, std::filesystem::copy_options::overwrite_existing,
                               error);
  }

  const bool previous_startup = settings_.Get().run_at_startup;
  const bool desired_startup = proposed.run_at_startup;
  if (!settings_.Update(std::move(proposed))) return false;

  // Reconcile the Windows entry even when the imported value matches the previous setting.
  // The registry entry may have been removed or damaged outside the application.
  if (!platform::windows::ConfigureRunAtStartup(desired_startup)) {
    auto rolled_back = settings_.Get();
    rolled_back.run_at_startup = previous_startup;
    if (!settings_.Update(std::move(rolled_back))) {
      core::LogDebug(L"Settings", L"Import applied but failed to roll back runAtStartup");
    }
    if (out_startup_registration_failed) *out_startup_registration_failed = true;
    core::LogDebug(L"Settings",
                   L"Import applied but startup registration failed; previous preference restored");
  }
  if (applied) applied();
  return true;
}

bool SettingsMutationService::ExportSettingsToFile(const std::wstring& path) const {
  if (path.empty()) return false;
  std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output << settings::SettingsSerializer::Serialize(
      settings::SettingsValidator::Normalize(settings_.Get()));
  output.flush();
  return static_cast<bool>(output);
}

bool SettingsMutationService::SaveUiWindowState(const settings::UiWindowState& state) {
  auto proposed = settings_.Get();
  proposed.ui_window = state;
  return settings_.Update(std::move(proposed));
}

}  // namespace minimize::features
