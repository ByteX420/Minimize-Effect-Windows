#include "pch.hpp"

#include "features/effect_policy.hpp"

#include <algorithm>

#include "settings/exclusion_rules.hpp"

namespace minimize::features {
namespace {

constexpr int kSmartSkipEnterScore = 12;
constexpr int kSmartSkipExitScore = 4;
constexpr std::uint64_t kSmartSkipCooldownMs = 200;

}  // namespace

void EffectPolicy::Configure(const settings::AppSettings& settings) { settings_ = settings; }

void EffectPolicy::SetEnabled(bool enabled) { settings_.enabled = enabled; }

bool EffectPolicy::SetFullscreenSuppressed(bool suppressed) {
  if (fullscreen_suppressed_ == suppressed) return false;
  fullscreen_suppressed_ = suppressed;
  return true;
}

bool EffectPolicy::SetPowerState(bool on_battery, bool battery_saver_active) {
  const bool previous_suppression =
      settings_.disable_effects_battery_saver && battery_saver_active_;
  on_battery_ = on_battery;
  battery_saver_active_ = battery_saver_active;
  const bool current_suppression = settings_.disable_effects_battery_saver && battery_saver_active_;
  return previous_suppression != current_suppression;
}

bool EffectPolicy::IsActive(bool temporarily_paused) const {
  return settings_.enabled && !temporarily_paused && !fullscreen_suppressed_ &&
         !(settings_.disable_effects_battery_saver && battery_saver_active_);
}

bool EffectPolicy::IsExcluded(std::string_view executable_name) const {
  return settings::ContainsExcludedApplication(settings_.excluded_applications, executable_name);
}

int EffectPolicy::ScoreLoad(const RenderingPressure& pressure) const {
  int score = 0;
  if (pressure.renderer_recovering || pressure.recent_device_failures > 0) score += 5;
  if (pressure.active_animations >= 8) {
    score += 4;
  } else if (pressure.active_animations >= 5) {
    score += 2;
  }
  if (pressure.avg_capture_duration_ms >= 60.0f) {
    score += 4;
  } else if (pressure.avg_capture_duration_ms >= 40.0f) {
    score += 2;
  }
  if (pressure.recent_missed_frames >= 30) {
    score += 4;
  } else if (pressure.recent_missed_frames >= 15) {
    score += 2;
  }
  return score;
}

void EffectPolicy::NoteSmartSkip(std::uint64_t now_ms) {
  smart_skip_latched_ = true;
  last_smart_skip_ms_ = now_ms;
}

bool EffectPolicy::ShouldSkipAnimationForLoad(const RenderingPressure& pressure,
                                              std::uint64_t now_ms) {
  if (!settings_.smart_skip_under_load) {
    smart_skip_latched_ = false;
    return false;
  }

  const int score = ScoreLoad(pressure);

  if (smart_skip_latched_) {
    // Cooldown keeps us on native for a beat after a skip so pressure can cool.
    if (now_ms - last_smart_skip_ms_ < kSmartSkipCooldownMs) return true;
    if (score <= kSmartSkipExitScore) {
      smart_skip_latched_ = false;
      return false;
    }
    last_smart_skip_ms_ = now_ms;
    return true;
  }

  if (score >= kSmartSkipEnterScore) {
    NoteSmartSkip(now_ms);
    return true;
  }
  return false;
}

int EffectPolicy::SelectMeshSegmentCount(int width, int height,
                                         const RenderingPressure& rendering) const {
  if (settings_.quality_mode == "best_quality") return 50;
  if (settings_.quality_mode == "power_saving") return 20;

  int pressure = on_battery_ ? 1 : 0;
  if (battery_saver_active_) pressure += 2;
  const std::int64_t pixels = static_cast<std::int64_t>(std::max(0, width)) * std::max(0, height);
  if (pixels >= 3840ll * 2160ll) {
    pressure += 2;
  } else if (pixels >= 2560ll * 1440ll) {
    ++pressure;
  }
  if (rendering.active_animations >= 2) {
    pressure += 2;
  } else if (rendering.active_animations == 1) {
    ++pressure;
  }
  if (rendering.avg_capture_duration_ms >= 25.0f) {
    pressure += 2;
  } else if (rendering.avg_capture_duration_ms >= 12.0f) {
    ++pressure;
  }
  if (rendering.recent_missed_frames >= 10) {
    pressure += 2;
  } else if (rendering.recent_missed_frames >= 3) {
    ++pressure;
  }
  if (rendering.recent_device_failures > 0 || rendering.renderer_recovering) pressure += 2;

  if (pressure >= 3) return 20;
  if (pressure >= 1) return 35;
  return 50;
}

}  // namespace minimize::features
