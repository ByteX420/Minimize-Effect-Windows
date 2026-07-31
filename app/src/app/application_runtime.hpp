#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "app/message_loop.hpp"
#include "features/animation_configuration.hpp"
#include "features/diagnostics_service.hpp"
#include "features/effect_controller.hpp"
#include "features/effect_policy.hpp"
#include "features/hotkey_controller.hpp"
#include "features/minimize_feature.hpp"
#include "features/open_windows_service.hpp"
#include "features/pause_controller.hpp"
#include "features/restore_feature.hpp"
#include "features/settings_mutation_service.hpp"
#include "features/window_exclusion_service.hpp"
#include "features/window_recovery_service.hpp"
#include "platform/windows/cbt_hook_manager.hpp"
#include "platform/windows/global_hotkey_manager.hpp"
#include "platform/windows/native_animation_blocker.hpp"
#include "platform/windows/power_status.hpp"
#include "platform/windows/taskbar_target_provider.hpp"
#include "rendering/d3d_device.hpp"
#include "rendering/desktop_capture.hpp"
#include "rendering/overlay_window.hpp"
#include "runtime/animation_run.hpp"
#include "runtime/animation_run_pool.hpp"
#include "runtime/frame_scheduler.hpp"
#include "runtime/renderer_recovery.hpp"
#include "runtime/run_state.hpp"
#include "runtime/snapshot_cache.hpp"
#include "settings/settings_service.hpp"
#include "ui/settings_window.hpp"

namespace minimize::app {

struct ApplicationLaunchOptions;

class ApplicationRuntime : public ui::SettingsActions {
public:
  ApplicationRuntime() = default;
  ~ApplicationRuntime();

  ApplicationRuntime(const ApplicationRuntime&) = delete;
  ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

  bool Initialize(HINSTANCE instance, const ApplicationLaunchOptions& options);
  int Run();
  void RenderUpdateHandoverFrame();
  void CompleteUpdateHandover();
  void PrepareForUpdateHandover() override;
  void ResumeAfterUpdateHandoverFailure() override;
  void RequestShutdown();
  void CleanupAndRestoreAll();
  void HealLeftoverWindows();
  bool SetEnabled(bool enabled) override;
  bool SetAnimationDurations(float minimize_duration, float restore_duration, float cancel_duration,
                             bool save) override;
  bool SetLinkSpeeds(bool linked) override;
  bool SetDisableAnimationsFullscreen(bool enabled) override;
  bool SetDisableEffectsBatterySaver(bool enabled) override;
  bool SetEasing(const std::string& minimize_easing, const std::string& restore_easing) override;
  bool SetCustomEasingBezier(bool is_minimize, animation::CubicBezier bezier, bool save) override;
  bool SetCancelEasing(const std::string& easing) override;
  bool SetCancelCustomBezier(animation::CubicBezier bezier, bool save) override;
  bool SetAnimationStyle(const std::string& style) override;
  bool SetQualityMode(const std::string& mode) override;
  bool SetMinimizeStrength(float strength, bool save) override;
  bool SetFadeStrength(const std::string& strength) override;
  bool ResetMotionSettings() override;
  bool SetTargetIndicator(bool enabled) override;
  bool SetSmartSkipUnderLoad(bool enabled) override;
  bool SetCloseBehavior(const std::string& close_behavior) override;
  bool SetStartupOptions(bool run_at_startup, bool start_minimized) override;
  bool SetApplicationExcluded(const std::string& executable_name, bool excluded) override;
  bool SetWindowMinimizeExcluded(HWND window, bool excluded) override;
  bool SetDisplayMinimizeExcluded(const std::string& device_name, bool excluded) override;
  [[nodiscard]] features::OpenWindowsSnapshot GetOpenWindowsSnapshot() override;
  bool FocusOpenWindow(HWND window) override;
  ui::SettingsFileOperationResult ExportSettings() override;
  ui::SettingsFileOperationResult ImportSettings() override;
  void SetTemporaryPause(ui::TemporaryPauseAction action) override;
  ui::HotkeyUpdateResult SetHotkey(settings::HotkeyAction action,
                                   settings::HotkeyBinding binding) override;
  void ExecuteHotkeyAction(settings::HotkeyAction action) override;
  [[nodiscard]] features::DiagnosticsSnapshot GetDiagnostics() const override;
  bool ExecuteDiagnosticsAction(features::DiagnosticsAction action) override;
  void HealWindows() override { HealLeftoverWindows(); }
  void RequestExit() override { RequestShutdown(); }
#ifdef _DEBUG
  bool RunStressTest();
  void UpdateStressTest();
  void DestroyStressTestWindows();
  void SetStressTestWindowState(HWND window, bool minimized);
  [[nodiscard]] bool HasActiveAnimationRuns() const;
#endif

private:
  enum class RunCleanupOutcome {
    kCompleted,
    kAborted,
  };

  enum class BulkWindowAction {
    kNone,
    kMinimize,
    kRestore,
  };

  int FindRunForWindow(HWND window) const;
  [[nodiscard]] bool IsOverlayWindow(HWND window) const;
  int FindAvailableRun();
  bool EnsureAnimationRunCapacity(std::size_t minimum_count);
  bool InitializeRun(runtime::AnimationRun& slot);
  void SetRunState(int run_index, runtime::RunState state);
  void CleanupRun(int run_index, RunCleanupOutcome outcome);
  void CheckAnimationTimeouts();
  void UpdateRuntime();
  void HandleDisplayChange();
  [[nodiscard]] MessageLoopWait TickRuntime();

  bool OnMinimizeStart(HWND window);
  bool OnRestoreAttempt(HWND window);
  void FinishActiveAnimation(int run_index);
  void RestoreWindowFromMinimizeState(HWND window, bool force_show_if_iconic = true,
                                      bool activate = true);
  void UpdateTemporaryPause();
  void MinimizeAllWindows();
  void RestoreAllWindows();
  void StartBulkWindowAction(BulkWindowAction action);
  void ProcessBulkWindowAction();
  void RegisterConfiguredHotkeys();
  void UnregisterAllHotkeys();
  [[nodiscard]] bool StartRuntimeServices();
  void PrewarmAnimationRuns();
  [[nodiscard]] features::DiagnosticsSnapshot BuildDiagnosticsSnapshot() const;
  [[nodiscard]] bool IsTemporarilyPaused() const;
  [[nodiscard]] bool IsEffectActive() const;
  void UpdateFullscreenSuppression(bool force = false);
  void UpdatePowerState(bool force = false);
  void EnableEffectRuntime();
  void DisableEffectRuntime();
  void RefreshEffectRuntimeState();
  [[nodiscard]] features::RenderingPressure GetRenderingPressure() const;
  [[nodiscard]] HWND GetOverlayWindow() const;
  void ResetAnimationFramePacing(int run_index, HWND window, const RECT& animation_bounds);
  void UpdateAnimationFramePacingMonitor(int run_index);
  [[nodiscard]] bool IsAnimationFrameDue(int run_index) const;
  void AdvanceAnimationFrameDeadline(int run_index);
  void WaitForAnimationFrameOrMessage();
  bool CreateAnimationRenderer();
  void BeginAnimationRendererRecovery();
  bool TryRecoverAnimationRenderer();
  [[nodiscard]] bool AnimationRendererDeviceLost() const;

  std::unique_ptr<rendering::D3dDevice> d3d_device_;
  std::unique_ptr<rendering::DesktopCapture> desktop_capture_;
  runtime::AnimationRunPool runs_;
  runtime::FrameScheduler frame_scheduler_;
  platform::NativeAnimationBlocker native_animation_blocker_;
  platform::TaskbarTargetProvider taskbar_target_provider_;
  HINSTANCE instance_ = nullptr;
  DWORD main_thread_id_ = 0;
  platform::windows::CbtHookManager cbt_hook_manager_;
  platform::windows::GlobalHotkeyManager hotkey_manager_;
  platform::PowerStatusMonitor power_status_monitor_;
  runtime::SnapshotCache snapshot_cache_;
  BulkWindowAction bulk_window_action_ = BulkWindowAction::kNone;
  bool bulk_hotkey_locked_ = false;
  std::deque<HWND> bulk_window_queue_;
  struct PreparedBulkCapture {
    rendering::CapturedTexture texture;
    RECT bounds{};
  };
  std::unordered_map<HWND, PreparedBulkCapture> prepared_bulk_captures_;
  HWND bulk_window_in_flight_ = nullptr;
  ULONGLONG bulk_window_request_started_ms_ = 0;
  std::unordered_map<HWND, ULONGLONG> minimize_suppressed_until_;
  ULONGLONG last_snapshot_refresh_ms_ = 0;
  ULONGLONG last_run_prewarm_ms_ = 0;
  runtime::RendererRecovery renderer_recovery_;
  bool effect_runtime_active_ = false;
  ULONGLONG last_fullscreen_check_ms_ = 0;
  unsigned int recent_missed_frames_ = 0;
  unsigned int recent_device_failures_ = 0;
  float avg_capture_duration_ms_ = 0.0f;
  ULONGLONG last_device_failure_ms_ = 0;
  ULONGLONG last_missed_frame_decay_ms_ = 0;
  ULONGLONG last_capture_decay_ms_ = 0;
  void NoteCaptureDuration(float duration_ms);
  void DecayRenderingPressure(ULONGLONG now_ms);
  bool device_recovery_test_pending_ = false;
  std::string startup_repair_status_ = "Not checked";
  // Defer startup iconic capture until settings enter motion (shell/sidebar/page) finishes.
  bool seed_iconic_snapshots_pending_ = false;
  minimize::settings::SettingsService settings_service_;
  minimize::features::HotkeyController hotkey_controller_{settings_service_, hotkey_manager_};
  minimize::features::SettingsMutationService settings_mutations_{settings_service_};
  minimize::features::EffectPolicy effect_policy_;
  minimize::features::AnimationConfiguration animation_configuration_{settings_service_,
                                                                      effect_policy_};
  minimize::features::DiagnosticsService diagnostics_service_;
  minimize::features::PauseController pause_controller_;
  minimize::features::WindowRecoveryService window_recovery_service_{snapshot_cache_};
  minimize::features::WindowExclusionService window_exclusion_service_;
  minimize::features::OpenWindowsService open_windows_service_{window_exclusion_service_};
  minimize::features::MinimizeFeature minimize_feature_{
      effect_policy_, window_recovery_service_, runs_, snapshot_cache_, window_exclusion_service_};
  minimize::features::RestoreFeature restore_feature_{effect_policy_,    window_recovery_service_,
                                                      snapshot_cache_,   runs_,
                                                      minimize_feature_, window_exclusion_service_};
  minimize::features::EffectController effect_controller_{effect_policy_, pause_controller_,
                                                          minimize_feature_, restore_feature_};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> cleaned_up_{false};
  std::atomic<bool> update_handover_prepared_{false};
  bool runtime_services_started_ = false;
#ifdef _DEBUG
  features::StressTestReport stress_test_report_;
  ULONGLONG stress_test_last_step_ms_ = 0;
  ULONGLONG stress_test_step_started_ms_ = 0;
  ULONGLONG stress_test_settle_started_ms_ = 0;
  std::vector<HWND> stress_test_windows_;
  std::unordered_map<HWND, bool> stress_test_expected_iconic_;
#endif
  ui::SettingsWindow settings_window_;
  MessageLoop message_loop_;
};

}  // namespace minimize::app
