#pragma once

#include <string>
#include <windows.h>

#include "animation/easing.hpp"
#include "features/diagnostics_service.hpp"
#include "features/open_windows_service.hpp"
#include "settings/app_settings.hpp"
#include "settings/hotkey_binding.hpp"

namespace minimize::ui {

enum class TemporaryPauseAction {
  kResume,
  kTenMinutes,
  kOneHour,
  kUntilRestart,
};

enum class HotkeyUpdateResult {
  kSuccess,
  kInvalid,
  kDuplicate,
  kUnavailable,
  kSaveFailed,
};

enum class SettingsFileResult {
  kSuccess,
  kCancelled,
  kFailed,
};

struct SettingsFileOperationResult {
  SettingsFileResult result = SettingsFileResult::kFailed;
  // Optional toast override (e.g. import succeeded with a non-fatal warning).
  std::string message;
  bool is_error = false;
};

class SettingsActions {
public:
  virtual ~SettingsActions() = default;

  virtual bool SetEnabled(bool enabled) = 0;
  virtual bool SetAnimationDurations(float minimize, float restore, float cancel, bool save) = 0;
  virtual bool SetLinkSpeeds(bool linked) = 0;
  virtual bool SetDisableAnimationsFullscreen(bool enabled) = 0;
  virtual bool SetDisableEffectsBatterySaver(bool enabled) = 0;
  virtual bool SetEasing(const std::string& minimize, const std::string& restore) = 0;
  virtual bool SetCustomEasingBezier(bool minimize, animation::CubicBezier bezier, bool save) = 0;
  virtual bool SetCancelEasing(const std::string& easing) = 0;
  virtual bool SetCancelCustomBezier(animation::CubicBezier bezier, bool save) = 0;
  virtual bool SetAnimationStyle(const std::string& style) = 0;
  virtual bool SetQualityMode(const std::string& mode) = 0;
  virtual bool SetMinimizeStrength(float strength, bool save) = 0;
  virtual bool SetFadeStrength(const std::string& strength) = 0;
  virtual bool ResetMotionSettings() = 0;
  virtual bool SetTargetIndicator(bool enabled) = 0;
  virtual bool SetSmartSkipUnderLoad(bool enabled) = 0;
  virtual bool SetCloseBehavior(const std::string& behavior) = 0;
  virtual bool SetStartupOptions(bool run_at_startup, bool start_minimized) = 0;
  virtual bool SetApplicationExcluded(const std::string& executable, bool excluded) = 0;
  // Session per-window Minimize disable (HWND + PID; not exe-wide).
  virtual bool SetWindowMinimizeExcluded(HWND window, bool excluded) = 0;
  // Persisted display-device Minimize disable (settings.json excludedDisplays).
  virtual bool SetDisplayMinimizeExcluded(const std::string& device_name, bool excluded) = 0;
  [[nodiscard]] virtual features::OpenWindowsSnapshot GetOpenWindowsSnapshot() = 0;
  virtual bool FocusOpenWindow(HWND window) = 0;
  virtual SettingsFileOperationResult ExportSettings() = 0;
  virtual SettingsFileOperationResult ImportSettings() = 0;
  virtual bool SaveUiWindowState(const settings::UiWindowState& state) = 0;
  virtual void SetTemporaryPause(TemporaryPauseAction action) = 0;
  virtual HotkeyUpdateResult SetHotkey(settings::HotkeyAction action,
                                       settings::HotkeyBinding binding) = 0;
  virtual void ExecuteHotkeyAction(settings::HotkeyAction action) = 0;
  [[nodiscard]] virtual features::DiagnosticsSnapshot GetDiagnostics() const = 0;
  virtual bool ExecuteDiagnosticsAction(features::DiagnosticsAction action) = 0;
  virtual void HealWindows() = 0;
  // Quiesces hooks and hotkeys while keeping the settings HWND alive for a seamless
  // updater process handover.
  virtual void PrepareForUpdateHandover() = 0;
  virtual void ResumeAfterUpdateHandoverFailure() = 0;
  virtual void RequestExit() = 0;
};

}  // namespace minimize::ui
