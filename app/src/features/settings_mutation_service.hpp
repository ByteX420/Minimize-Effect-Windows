#pragma once

#include <functional>
#include <string>

#include "animation/easing.hpp"
#include "settings/settings_service.hpp"

namespace minimize::features {

class SettingsMutationService final {
public:
  explicit SettingsMutationService(settings::SettingsService& settings) : settings_(settings) {}

  bool SetEnabled(bool enabled, const std::function<void()>& applied);
  bool SetAnimationDurations(float minimize, float restore, float cancel, bool save);
  bool SetLinkSpeeds(bool linked);
  bool SetDisableAnimationsFullscreen(bool enabled, const std::function<void()>& applied);
  bool SetDisableEffectsBatterySaver(bool enabled, const std::function<void()>& applied);
  bool SetEasing(const std::string& minimize, const std::string& restore);
  bool SetCustomEasingBezier(bool minimize, animation::CubicBezier bezier, bool save);
  bool SetCancelEasing(const std::string& easing);
  bool SetCancelCustomBezier(animation::CubicBezier bezier, bool save);
  bool SetAnimationStyle(const std::string& style);
  bool SetQualityMode(const std::string& mode);
  bool SetMinimizeStrength(float strength, bool save);
  bool SetFadeStrength(const std::string& strength);
  // Atomically restore every Motion-page field to AppSettings defaults.
  bool ResetMotionSettings();
  bool SetTargetIndicator(bool enabled);
  bool SetSmartSkipUnderLoad(bool enabled, const std::function<void()>& applied);
  bool SaveMotionProfile(const std::string& name);
  bool ApplyMotionProfile(const std::string& name, const std::function<void()>& applied);
  bool DeleteMotionProfile(const std::string& name);
  [[nodiscard]] bool CanUndo() const { return settings_.CanUndo(); }
  bool Undo(const std::function<void()>& applied);
  bool RestoreBackup(const std::function<void()>& applied,
                     bool* out_startup_registration_failed = nullptr);
  bool SetCloseBehavior(const std::string& behavior);
  bool SetStartupOptions(bool run_at_startup, bool start_minimized);
  bool SetApplicationExcluded(const std::string& executable, bool excluded,
                              const std::function<void()>& applied);
  bool SetDisplayMinimizeExcluded(const std::string& device_name, bool excluded,
                                  const std::function<void()>& applied);
  // applied runs after a successful settings write. When startup registration fails,
  // applied still runs but out_startup_registration_failed is set so the host can warn.
  bool ImportSettingsFromFile(const std::wstring& path, const std::function<void()>& applied,
                              bool* out_startup_registration_failed = nullptr);
  bool ExportSettingsToFile(const std::wstring& path) const;
  bool SaveUiWindowState(const settings::UiWindowState& state);

private:
  settings::SettingsService& settings_;
};

}  // namespace minimize::features
