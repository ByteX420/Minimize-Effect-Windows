#pragma once

#include <cstdint>
#include <string_view>

#include "settings/app_settings.hpp"

namespace minimize::features {

struct RenderingPressure {
  int active_animations = 0;
  // Exponential moving average of recent capture times (preferred over a single sample).
  float avg_capture_duration_ms = 0.0f;
  unsigned int recent_missed_frames = 0;
  unsigned int recent_device_failures = 0;
  bool renderer_recovering = false;
};

class EffectPolicy final {
public:
  void Configure(const settings::AppSettings& settings);

  void SetEnabled(bool enabled);
  [[nodiscard]] bool SetFullscreenSuppressed(bool suppressed);
  [[nodiscard]] bool SetPowerState(bool on_battery, bool battery_saver_active);

  [[nodiscard]] bool IsActive(bool temporarily_paused) const;
  [[nodiscard]] bool IsExcluded(std::string_view executable_name) const;
  // Hysteresis smart-skip: enter at score >= 5, exit at score <= 2, with post-skip cooldown.
  // now_ms drives cooldown / latch timing (GetTickCount64).
  [[nodiscard]] bool ShouldSkipAnimationForLoad(const RenderingPressure& pressure,
                                                std::uint64_t now_ms);
  // After a skip (including post-capture abort), keep native path for a short cooldown.
  void NoteSmartSkip(std::uint64_t now_ms);
  [[nodiscard]] int SelectMeshSegmentCount(int width, int height,
                                           const RenderingPressure& pressure) const;
  [[nodiscard]] bool on_battery() const { return on_battery_; }
  [[nodiscard]] bool battery_saver_active() const { return battery_saver_active_; }
  [[nodiscard]] bool smart_skip_enabled() const { return settings_.smart_skip_under_load; }
  [[nodiscard]] bool smart_skip_latched() const { return smart_skip_latched_; }
  [[nodiscard]] bool fullscreen_suppressed() const { return fullscreen_suppressed_; }

private:
  [[nodiscard]] int ScoreLoad(const RenderingPressure& pressure) const;

  settings::AppSettings settings_;
  bool fullscreen_suppressed_ = false;
  bool on_battery_ = false;
  bool battery_saver_active_ = false;
  bool smart_skip_latched_ = false;
  std::uint64_t last_smart_skip_ms_ = 0;
};

}  // namespace minimize::features
