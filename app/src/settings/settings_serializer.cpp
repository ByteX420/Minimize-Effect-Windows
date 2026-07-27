#include "pch.hpp"

#include "settings/settings_serializer.hpp"

#include <cmath>
#include <string_view>

#include "nlohmann/json.hpp"
#include "settings/exclusion_rules.hpp"

namespace minimize::settings {
namespace {

using Json = nlohmann::json;

constexpr float kMinimumDuration = 0.10f;
constexpr float kMaximumDuration = 2.00f;
constexpr std::uint32_t kSupportedHotkeyModifiers = 0x000fu;

constexpr std::array kHotkeyNames = {
    std::string_view{"toggleEffectHotkey"},  std::string_view{"openSettingsHotkey"},
    std::string_view{"repairWindowsHotkey"}, std::string_view{"minimizeAllWindowsHotkey"},
    std::string_view{"restoreAllWindowsHotkey"},
};

bool IsValidEasingName(std::string_view value) {
  constexpr std::array names = {
      std::string_view{"Linear"},      std::string_view{"Ease In"}, std::string_view{"Ease Out"},
      std::string_view{"Ease In Out"}, std::string_view{"Cubic"},   std::string_view{"Back"},
      std::string_view{"Elastic"},     std::string_view{"Custom"},
  };
  return std::find(names.begin(), names.end(), value) != names.end();
}

bool IsValidAnimationStyle(std::string_view value) {
  return value == "Genie classic" || value == "Genie curvy" || value == "Squash" ||
         value == "Classic Minimize";
}

template <typename T, typename Predicate>
void ReadIf(const Json& object, std::string_view key, T& destination, Predicate&& predicate) {
  const auto value = object.find(key);
  if (value == object.end()) return;
  try {
    T candidate = value->get<T>();
    if (predicate(candidate)) destination = std::move(candidate);
  } catch (const Json::exception&) {
    // Invalid values retain the field default, matching the previous tolerant loader.
  }
}

template <typename T>
void ReadIf(const Json& object, std::string_view key, T& destination) {
  ReadIf(object, key, destination, [](const T&) { return true; });
}

void ReadBezier(const Json& object, std::string_view key, animation::CubicBezier& destination) {
  const auto value = object.find(key);
  if (value == object.end() || !value->is_array() || value->size() != 4) return;
  try {
    animation::CubicBezier candidate{
        value->at(0).get<float>(), value->at(1).get<float>(), value->at(2).get<float>(),
        value->at(3).get<float>()};
    if (!std::isfinite(candidate.x1) || !std::isfinite(candidate.y1) ||
        !std::isfinite(candidate.x2) || !std::isfinite(candidate.y2)) {
      return;
    }
    candidate.ClampHandles();
    destination = candidate;
  } catch (const Json::exception&) {
  }
}

void ReadHotkeys(const Json& object, AppSettings& settings) {
  for (std::size_t index = 0; index < kHotkeyNames.size(); ++index) {
    const std::string prefix(kHotkeyNames[index]);
    ReadIf<std::uint32_t>(
        object, prefix + "Modifiers", settings.hotkeys[index].modifiers,
        [](std::uint32_t value) { return value <= kSupportedHotkeyModifiers; });
    ReadIf<std::uint32_t>(object, prefix + "Key", settings.hotkeys[index].virtual_key,
                          [](std::uint32_t value) { return value <= 254; });
  }
}

Json SerializeHotkeys(const AppSettings& settings) {
  Json hotkeys = Json::object();
  for (std::size_t index = 0; index < kHotkeyNames.size(); ++index) {
    const std::string prefix(kHotkeyNames[index]);
    hotkeys[prefix + "Modifiers"] = settings.hotkeys[index].modifiers;
    hotkeys[prefix + "Key"] = settings.hotkeys[index].virtual_key;
  }
  return hotkeys;
}

}  // namespace

std::optional<AppSettings> SettingsSerializer::Deserialize(std::string_view json) {
  const Json document = Json::parse(json, nullptr, false);
  if (document.is_discarded() || !document.is_object()) return std::nullopt;

  AppSettings loaded;
  ReadIf(document, "enabled", loaded.enabled);
  ReadIf<float>(document, "minimizeDuration", loaded.minimize_duration,
                [](float value) {
                  return std::isfinite(value) && value >= kMinimumDuration &&
                         value <= kMaximumDuration;
                });
  ReadIf<float>(document, "restoreDuration", loaded.restore_duration,
                [](float value) {
                  return std::isfinite(value) && value >= kMinimumDuration &&
                         value <= kMaximumDuration;
                });
  ReadIf(document, "linkSpeeds", loaded.link_speeds);
  ReadIf(document, "disableAnimationsFullscreen", loaded.disable_animations_fullscreen);
  ReadIf(document, "disableEffectsBatterySaver", loaded.disable_effects_battery_saver);
  ReadIf<std::string>(document, "minimizeEasing", loaded.minimize_easing, IsValidEasingName);
  ReadIf<std::string>(document, "restoreEasing", loaded.restore_easing, IsValidEasingName);
  ReadBezier(document, "minimizeCustomBezier", loaded.minimize_custom_bezier);
  ReadBezier(document, "restoreCustomBezier", loaded.restore_custom_bezier);
  ReadIf<std::string>(document, "animationStyle", loaded.animation_style, IsValidAnimationStyle);
  ReadIf<std::string>(
      document, "qualityMode", loaded.quality_mode,
      [](std::string_view value) {
        return value == "automatic" || value == "best_quality" || value == "power_saving";
      });
  ReadIf<float>(document, "minimizeStrength", loaded.minimize_strength,
                [](float value) { return std::isfinite(value) && value >= 0.25f && value <= 1.0f; });
  ReadIf<std::string>(document, "fadeStrength", loaded.fade_strength,
                      [](std::string_view value) {
                        return value == "No fade" || value == "Subtle" || value == "Strong";
                      });
  ReadIf(document, "showTargetIndicator", loaded.show_target_indicator);
  ReadIf(document, "smartSkipUnderLoad", loaded.smart_skip_under_load);
  ReadIf<std::string>(document, "closeBehavior", loaded.close_behavior,
                      [](std::string_view value) { return value == "exit" || value == "tray"; });
  ReadIf(document, "startMinimized", loaded.start_minimized);
  ReadIf(document, "runAtStartup", loaded.run_at_startup);
  ReadIf(document, "excludedApplications", loaded.excluded_applications);
  ReadIf(document, "excludedDisplays", loaded.excluded_displays);
  ReadHotkeys(document, loaded);

  NormalizeExcludedApplications(&loaded.excluded_applications);
  for (HotkeyBinding& binding : loaded.hotkeys) {
    binding.modifiers &= kSupportedHotkeyModifiers;
    if (binding.virtual_key == 0) binding.modifiers = 0;
  }
  if (loaded.animation_style == "Classic Minimize") loaded.animation_style = "Genie classic";
  loaded.minimize_custom_bezier.ClampHandles();
  loaded.restore_custom_bezier.ClampHandles();
  return loaded;
}

std::string SettingsSerializer::Serialize(const AppSettings& settings) {
  std::vector<std::string> excluded_applications = settings.excluded_applications;
  NormalizeExcludedApplications(&excluded_applications);

  Json document = {
      {"enabled", settings.enabled},
      {"minimizeDuration", settings.minimize_duration},
      {"restoreDuration", settings.restore_duration},
      {"linkSpeeds", settings.link_speeds},
      {"disableAnimationsFullscreen", settings.disable_animations_fullscreen},
      {"disableEffectsBatterySaver", settings.disable_effects_battery_saver},
      {"minimizeEasing", settings.minimize_easing},
      {"restoreEasing", settings.restore_easing},
      {"minimizeCustomBezier",
       {settings.minimize_custom_bezier.x1, settings.minimize_custom_bezier.y1,
        settings.minimize_custom_bezier.x2, settings.minimize_custom_bezier.y2}},
      {"restoreCustomBezier",
       {settings.restore_custom_bezier.x1, settings.restore_custom_bezier.y1,
        settings.restore_custom_bezier.x2, settings.restore_custom_bezier.y2}},
      {"animationStyle", settings.animation_style},
      {"qualityMode", settings.quality_mode},
      {"minimizeStrength", settings.minimize_strength},
      {"fadeStrength", settings.fade_strength},
      {"showTargetIndicator", settings.show_target_indicator},
      {"smartSkipUnderLoad", settings.smart_skip_under_load},
      {"closeBehavior", settings.close_behavior},
      {"startMinimized", settings.start_minimized},
      {"runAtStartup", settings.run_at_startup},
      {"excludedApplications", excluded_applications},
      {"excludedDisplays", settings.excluded_displays},
  };
  document.update(SerializeHotkeys(settings));
  return document.dump(2) + '\n';
}

}  // namespace minimize::settings
