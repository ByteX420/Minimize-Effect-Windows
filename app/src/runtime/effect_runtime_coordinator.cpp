#include "pch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string_view>

#include "app/application_runtime.hpp"
#include "core/environment.hpp"
#include "core/logger.hpp"
#include "platform/windows/app_container_permissions.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/power_status.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/startup_manager.hpp"
#include "platform/windows/window_diagnostics.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "settings/exclusion_rules.hpp"

namespace minimize::app {

bool ApplicationRuntime::CreateAnimationRenderer() {
  constexpr std::size_t kInitialRunCount = 8;
  for (runtime::AnimationRun& slot : runs_) slot.overlay.Shutdown();
  runs_.Clear();
  desktop_capture_.reset();
  d3d_device_.reset();

  d3d_device_ = rendering::D3dDevice::Create();
  if (d3d_device_ == nullptr) {
    return false;
  }
  runtime::AnimationRun& retry_run = runs_.Add();
  if (!InitializeRun(retry_run)) {
    runs_.Clear();
    d3d_device_.reset();
    return false;
  }
  desktop_capture_ = std::make_unique<rendering::DesktopCapture>(d3d_device_.get());
  if (!EnsureAnimationRunCapacity(kInitialRunCount)) {
    minimize::core::LogDebug(
        L"Renderer",
        L"Could not pre-initialize every animation run; idle prewarm will retry later");
  }
  return true;
}

bool ApplicationRuntime::AnimationRendererDeviceLost() const {
  const bool overlay_lost =
      std::any_of(runs_.begin(), runs_.end(),
                  [](const runtime::AnimationRun& slot) { return slot.overlay.device_lost(); });
  return overlay_lost || (desktop_capture_ != nullptr && desktop_capture_->device_lost()) ||
         (d3d_device_ != nullptr && d3d_device_->IsDeviceLost());
}

void ApplicationRuntime::BeginAnimationRendererRecovery() {
  if (renderer_recovery_.pending()) {
    return;
  }

  recent_device_failures_ = std::min(recent_device_failures_ + 1u, 8u);
  last_device_failure_ms_ = GetTickCount64();
  minimize::core::LogDebug(L"App", L"Animation renderer device lost; rebuilding D3D resources");
  native_animation_blocker_.Disable();
  bulk_window_action_ = BulkWindowAction::kNone;
  bulk_hotkey_locked_ = false;
  bulk_window_queue_.clear();
  prepared_bulk_captures_.clear();
  bulk_window_in_flight_ = nullptr;
  bulk_window_request_started_ms_ = 0;

  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    auto& slot = runs_[i];
    HWND interrupted_window = slot.animating_window;
    if (interrupted_window != nullptr || slot.pending_native_minimize_window != nullptr ||
        slot.overlay.active()) {
      CleanupRun(i, RunCleanupOutcome::kAborted);
    }
    slot.overlay.Shutdown();
  }

  frame_scheduler_.EndFallbackTimerResolution();

  desktop_capture_.reset();
  snapshot_cache_.Restore().clear();
  snapshot_cache_.PreMinimize().clear();
  d3d_device_.reset();

  renderer_recovery_.Begin();
  TryRecoverAnimationRenderer();
}

bool ApplicationRuntime::TryRecoverAnimationRenderer() {
  if (!renderer_recovery_.pending()) {
    return true;
  }
  const ULONGLONG now = GetTickCount64();
  if (!renderer_recovery_.ShouldAttempt(now)) return false;

  if (CreateAnimationRenderer()) {
    renderer_recovery_.MarkSucceeded();
    if (IsEffectActive()) {
      native_animation_blocker_.Enable(GetOverlayWindow());
    }
    minimize::core::LogDebug(L"App", L"Animation renderer recovery completed");
    return true;
  }

  renderer_recovery_.ScheduleRetry(now);
  minimize::core::LogDebug(L"App", L"Animation renderer recovery retry scheduled");
  return false;
}

void ApplicationRuntime::PrewarmAnimationRuns() {
  constexpr std::size_t kWarmRunCount = 8;
  constexpr ULONGLONG kWarmIntervalMs = 100;
  if (runs_.size() >= kWarmRunCount || d3d_device_ == nullptr || renderer_recovery_.pending() ||
      bulk_window_action_ != BulkWindowAction::kNone ||
      minimize_feature_.SeedSnapshotsInProgress() ||
      settings_window_.IsStartupEnterMotionActive()) {
    return;
  }
  if (std::any_of(runs_.begin(), runs_.end(), [](const runtime::AnimationRun& run) {
        return run.state != runtime::RunState::kIdle || run.overlay.active();
      })) {
    return;
  }

  const ULONGLONG now = GetTickCount64();
  if (now - last_run_prewarm_ms_ < kWarmIntervalMs) return;
  last_run_prewarm_ms_ = now;
  if (!EnsureAnimationRunCapacity(runs_.size() + 1)) {
    minimize::core::LogDebug(L"Renderer", L"Idle animation-run prewarm failed");
  }
}

void ApplicationRuntime::NoteCaptureDuration(float duration_ms) {
  duration_ms = std::max(0.0f, duration_ms);
  // EMA so a single slow capture raises pressure without permanent sticky peaks.
  constexpr float kAlpha = 0.35f;
  if (avg_capture_duration_ms_ <= 0.0f) {
    avg_capture_duration_ms_ = duration_ms;
  } else {
    avg_capture_duration_ms_ = avg_capture_duration_ms_ * (1.0f - kAlpha) + duration_ms * kAlpha;
  }
}

void ApplicationRuntime::DecayRenderingPressure(ULONGLONG now_ms) {
  constexpr ULONGLONG kMissedDecayIntervalMs = 250;
  constexpr ULONGLONG kDeviceFailureHoldMs = 30000;
  constexpr float kCapturePressureHalfLifeMs = 1000.0f;
  const int active_animations = static_cast<int>(
      std::count_if(runs_.begin(), runs_.end(),
                    [](const runtime::AnimationRun& run) { return run.overlay.active(); }));

  // Missed-frame debt must cool even when smart-skip prevents further animations.
  if (last_missed_frame_decay_ms_ == 0 || active_animations != 0 || recent_missed_frames_ == 0) {
    last_missed_frame_decay_ms_ = now_ms;
  } else {
    const ULONGLONG elapsed_ms = now_ms - last_missed_frame_decay_ms_;
    const ULONGLONG decay_steps = elapsed_ms / kMissedDecayIntervalMs;
    if (decay_steps > 0) {
      const unsigned int decrement =
          static_cast<unsigned int>(std::min<ULONGLONG>(decay_steps, recent_missed_frames_));
      recent_missed_frames_ -= decrement;
      last_missed_frame_decay_ms_ += decay_steps * kMissedDecayIntervalMs;
    }
  }

  // Device failures stop counting after 30s without a new loss.
  if (recent_device_failures_ > 0 && last_device_failure_ms_ != 0 &&
      now_ms - last_device_failure_ms_ >= kDeviceFailureHoldMs) {
    recent_device_failures_ = 0;
  }

  // Time-based half-life keeps capture pressure independent of message-loop frequency.
  if (last_capture_decay_ms_ == 0) last_capture_decay_ms_ = now_ms;
  const ULONGLONG capture_elapsed_ms = now_ms - last_capture_decay_ms_;
  last_capture_decay_ms_ = now_ms;
  if (active_animations == 0 && avg_capture_duration_ms_ > 0.0f && capture_elapsed_ms > 0) {
    const float decay =
        std::exp2(-static_cast<float>(capture_elapsed_ms) / kCapturePressureHalfLifeMs);
    avg_capture_duration_ms_ *= decay;
    if (avg_capture_duration_ms_ < 1.0f) avg_capture_duration_ms_ = 0.0f;
  }
}

void ApplicationRuntime::UpdateRuntime() {
  taskbar_target_provider_.UpdateAutoHideTaskbarRestore();
  UpdateTemporaryPause();
  UpdateFullscreenSuppression();
  UpdatePowerState();
  CheckAnimationTimeouts();
  DecayRenderingPressure(GetTickCount64());

  // After shell/sidebar/page enter motion settles, seed iconic windows one-per-tick.
  if (!shutting_down_.load(std::memory_order_acquire)) {
    const bool settings_enter_busy = settings_window_.hwnd() != nullptr &&
                                     IsWindowVisible(settings_window_.hwnd()) &&
                                     settings_window_.IsStartupEnterMotionActive();
    if (seed_iconic_snapshots_pending_ && !settings_enter_busy) {
      seed_iconic_snapshots_pending_ = false;
      minimize_feature_.BeginSeedSnapshotsForIconicWindows(
          GetOverlayWindow(), desktop_capture_.get(), &taskbar_target_provider_,
          renderer_recovery_.pending());
    }
    if (minimize_feature_.SeedSnapshotsInProgress()) {
      (void)minimize_feature_.TickSeedSnapshotsForIconicWindows();
    }
    PrewarmAnimationRuns();
  }
}

void ApplicationRuntime::HandleDisplayChange() {
  for (auto& run : runs_) run.animation_monitor = nullptr;
  settings_window_.InvalidateOpenWindowsSnapshot();
  settings_window_.ForceRender();
}

MessageLoopWait ApplicationRuntime::TickRuntime() {
#ifdef _DEBUG
  if (device_recovery_test_pending_) {
    device_recovery_test_pending_ = false;
    BeginAnimationRendererRecovery();
  }
#endif

  if (AnimationRendererDeviceLost()) {
    BeginAnimationRendererRecovery();
  }
  if (renderer_recovery_.pending() && !TryRecoverAnimationRenderer()) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    return MessageLoopWait::kFrame;
  }

  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    auto& slot = runs_[i];
    if (slot.pending_native_minimize_window != nullptr) {
      platform::windows::TraceWindowEvent(
          L"Run pending_native_minimize before CompletePendingNativeMinimize",
          slot.pending_native_minimize_window);
      const bool start_animation_clock =
          !slot.bulk_animation || bulk_window_action_ == BulkWindowAction::kNone;
      minimize_feature_.CompletePendingNativeMinimize(
          i, start_animation_clock,
          [this](int index, runtime::RunState state) { SetRunState(index, state); },
          [this](int index) { CleanupRun(index, RunCleanupOutcome::kAborted); });
      platform::windows::TraceWindowEvent(
          L"Run pending_native_minimize after CompletePendingNativeMinimize",
          slot.pending_native_minimize_window);
    }

    if (slot.bulk_animation && bulk_window_action_ == BulkWindowAction::kNone &&
        slot.overlay.active() && !slot.overlay.clock_started()) {
      slot.overlay.StartAnimationClock();
      SetRunState(i, slot.animating_restore ? runtime::RunState::kRestoring
                                            : runtime::RunState::kAnimating);
    }

    if (slot.overlay.active() && !slot.overlay.restoring() && slot.animating_window != nullptr) {
      if (!slot.overlay.clock_started()) {
        const bool is_iconic = IsIconic(slot.animating_window) != FALSE;
        const bool is_moved = platform::windows::properties::HasFlag(
            slot.animating_window, platform::windows::properties::WindowFlag::kMovedOffscreen);
        if (is_iconic || is_moved) {
          const bool wait_for_bulk_start =
              slot.bulk_animation && bulk_window_action_ != BulkWindowAction::kNone;
          if (!wait_for_bulk_start) {
            slot.overlay.StartAnimationClock();
            SetRunState(i, slot.animating_restore ? runtime::RunState::kRestoring
                                                  : runtime::RunState::kAnimating);
            if (slot.pending_native_minimize_window == slot.animating_window) {
              slot.pending_native_minimize_window = nullptr;
            }
            std::wcout << L"Target is minimized, starting animation clock.\n";
          }
        } else {
          const ULONGLONG now = GetTickCount64();
          if (now - slot.direction_started_ms >= 800) {
            HWND stalled_window = slot.animating_window;
            platform::windows::TraceWindowEvent(L"Run minimize timeout aborting stalled animation",
                                                stalled_window);
            std::wcerr << L"Minimize minimize event timeout before native minimize completed; "
                          L"aborting animation.\n";
            if (stalled_window != nullptr && IsWindow(stalled_window)) {
              (void)platform::SetWindowCloaked(stalled_window, false);
              platform::windows::properties::RestoreTransparency(stalled_window);
              platform::windows::properties::ClearMinimizeState(stalled_window);
              native_animation_blocker_.SetTransitionsDisabledForWindow(stalled_window, false);
            }
            CleanupRun(i, RunCleanupOutcome::kAborted);
          }
        }
      }
    }

    const bool was_active = slot.overlay.active();
    if (was_active && slot.live_animation_capture_enabled) {
      if (slot.animating_window == nullptr || !IsWindow(slot.animating_window) ||
          IsIconic(slot.animating_window) || !IsWindowVisible(slot.animating_window)) {
        slot.live_animation_capture_enabled = false;
      } else {
        const ULONGLONG now_ms = GetTickCount64();
        constexpr ULONGLONG refresh_interval_ms = 16;
        if (now_ms - slot.last_animation_texture_refresh_ms >= refresh_interval_ms) {
          slot.last_animation_texture_refresh_ms = now_ms;
          if (!desktop_capture_->RefreshCapturedTexture(slot.live_animation_bounds,
                                                        slot.overlay.mutable_captured_texture())) {
            slot.live_animation_capture_enabled = false;
          }
        }
      }
    }

    bool animation_active = false;
    if (was_active) {
      UpdateAnimationFramePacingMonitor(i);
      if (IsAnimationFrameDue(i)) {
        animation_active = slot.overlay.Tick();
        AdvanceAnimationFrameDeadline(i);
      } else {
        animation_active = true;
      }
    }

    if (AnimationRendererDeviceLost()) {
      BeginAnimationRendererRecovery();
      break;
    }

    if (was_active && !animation_active && slot.animating_window != nullptr) {
      CleanupRun(i, RunCleanupOutcome::kCompleted);
    }
  }

  bool any_active = false;
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    if (runs_[i].overlay.active()) {
      any_active = true;
    }
  }

  const ULONGLONG now_ms = GetTickCount64();
  if (IsEffectActive() && now_ms - last_snapshot_refresh_ms_ >= 120) {
    last_snapshot_refresh_ms_ = now_ms;
    minimize_feature_.UpdatePreMinimizeSnapshot(GetForegroundWindow(), GetOverlayWindow(),
                                                desktop_capture_.get(),
                                                renderer_recovery_.pending());
  }

  if (IsEffectActive() && FindAvailableRun() != -1) {
    for (auto& [hwnd, snapshot] : snapshot_cache_.Restore()) {
      (void)snapshot;
      if (FindRunForWindow(hwnd) != -1) {
        continue;
      }
      if (IsWindow(hwnd) && IsWindowVisible(hwnd) && restore_feature_.IsWindowRestored(hwnd)) {
        std::wcout << L"Poll: detected restore for hwnd=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << std::endl;
        OnRestoreAttempt(hwnd);
        break;  // Only handle one at a time
      }
    }
  }

  any_active = false;
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    if (runs_[i].overlay.active()) {
      any_active = true;
    }
  }

  // Start at most one new bulk target per runtime tick. Its capture and run setup complete
  // before the next target is posted, while all established overlays continue concurrently.
  ProcessBulkWindowAction();
#ifdef _DEBUG
  UpdateStressTest();
#endif

  any_active = false;
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    if (runs_[i].overlay.active()) {
      any_active = true;
    }
  }

  if (bulk_hotkey_locked_ && bulk_window_action_ == BulkWindowAction::kNone) {
    const bool bulk_animation_active =
        std::any_of(runs_.begin(), runs_.end(), [](const runtime::AnimationRun& run) {
          return run.bulk_animation &&
                 (run.state != runtime::RunState::kIdle || run.overlay.active() ||
                  run.animating_window != nullptr || run.pending_native_minimize_window != nullptr);
        });
    if (!bulk_animation_active) {
      bulk_hotkey_locked_ = false;
      minimize::core::LogDebug(L"Hotkey", L"Bulk animation finished; hotkeys unlocked");
    }
  }

  if (!any_active) {
    frame_scheduler_.EndFallbackTimerResolution();
  }
  // Bulk preparation is a short capture/setup phase, not an animation frame. Do not add one
  // monitor refresh of latency between windows; queued hook messages still get dispatched by the
  // normal message pump before the next pass.
  if (bulk_window_action_ != BulkWindowAction::kNone) return MessageLoopWait::kImmediate;
  if (any_active || settings_window_.WantsContinuousRendering()) {
    return MessageLoopWait::kAnimation;
  }
  return MessageLoopWait::kFrame;
}

void ApplicationRuntime::ResetAnimationFramePacing(int run_index, HWND window,
                                                   const RECT& animation_bounds) {
  frame_scheduler_.Reset(runs_[run_index], window, animation_bounds);
}

void ApplicationRuntime::UpdateAnimationFramePacingMonitor(int run_index) {
  frame_scheduler_.UpdateMonitor(runs_[run_index]);
}

bool ApplicationRuntime::IsAnimationFrameDue(int run_index) const {
  return frame_scheduler_.IsDue(runs_[run_index]);
}

void ApplicationRuntime::AdvanceAnimationFrameDeadline(int run_index) {
  const unsigned int missed = frame_scheduler_.Advance(runs_[run_index]);
  if (missed > 0) {
    recent_missed_frames_ = std::min(recent_missed_frames_ + missed, 120u);
  } else if (recent_missed_frames_ > 0) {
    --recent_missed_frames_;
  }
}

void ApplicationRuntime::WaitForAnimationFrameOrMessage() {
  const HANDLE settings_frame = settings_window_.WantsContinuousRendering()
                                    ? settings_window_.RenderWaitHandle()
                                    : nullptr;
  frame_scheduler_.Wait(runs_, settings_frame);
}

bool ApplicationRuntime::IsTemporarilyPaused() const {
  return pause_controller_.IsPaused(GetTickCount64());
}

bool ApplicationRuntime::IsEffectActive() const { return effect_controller_.IsActive(); }

void ApplicationRuntime::UpdateFullscreenSuppression(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now - last_fullscreen_check_ms_ < 500) return;
  last_fullscreen_check_ms_ = now;
  const bool suppressed =
      settings_service_.Get().disable_animations_fullscreen &&
      platform::IsFullscreenForegroundWindow(GetOverlayWindow(), settings_window_.hwnd());
  if (!effect_policy_.SetFullscreenSuppressed(suppressed)) return;
  minimize::core::LogDebug(L"Fullscreen", suppressed
                                              ? L"Fullscreen application detected; effect suspended"
                                              : L"Fullscreen application ended; effect resumed");
  RefreshEffectRuntimeState();
}

void ApplicationRuntime::UpdatePowerState(bool force) {
  const auto power_status =
      force ? power_status_monitor_.Current() : power_status_monitor_.ConsumeChange();
  if (!power_status.has_value()) return;
  const bool on_battery = power_status->on_battery;
  const bool saver_active = power_status->battery_saver_active;
  const bool power_changed = effect_policy_.on_battery() != on_battery ||
                             effect_policy_.battery_saver_active() != saver_active;
  const bool suppression_changed = effect_policy_.SetPowerState(on_battery, saver_active);
  if (power_changed) {
    minimize::core::LogDebug(L"Power", L"Power state changed: battery=" +
                                           std::to_wstring(on_battery) + L" saver=" +
                                           std::to_wstring(saver_active));
  }
  if (suppression_changed) RefreshEffectRuntimeState();
}

void ApplicationRuntime::DisableEffectRuntime() {
  bulk_window_action_ = BulkWindowAction::kNone;
  bulk_hotkey_locked_ = false;
  bulk_window_queue_.clear();
  prepared_bulk_captures_.clear();
  bulk_window_in_flight_ = nullptr;
  bulk_window_request_started_ms_ = 0;
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    FinishActiveAnimation(i);
  }
  native_animation_blocker_.Disable();

  std::vector<HWND> tracked_windows;
  for (const auto& [window, snapshot] : snapshot_cache_.Restore()) {
    (void)snapshot;
    tracked_windows.push_back(window);
  }
  for (HWND window : tracked_windows) {
    RestoreWindowFromMinimizeState(window, false);
  }
  snapshot_cache_.Restore().clear();
  snapshot_cache_.PreMinimize().clear();
  if (desktop_capture_ != nullptr) desktop_capture_->ClearHistory();
  cbt_hook_manager_.Uninstall();
  effect_runtime_active_ = false;
}

void ApplicationRuntime::EnableEffectRuntime() {
  const bool cbt_hook_installed = cbt_hook_manager_.Install();
  if (!cbt_hook_installed) {
    minimize::core::LogDebug(L"Pause",
                             L"Global CBT hook unavailable; WinEvent fallback remains active");
  }
  if (!renderer_recovery_.pending() && GetOverlayWindow() != nullptr) {
    native_animation_blocker_.Enable(GetOverlayWindow());
  }
  effect_runtime_active_ = true;
}

void ApplicationRuntime::RefreshEffectRuntimeState() {
  effect_policy_.Configure(settings_service_.Get());
  const bool should_be_active = IsEffectActive();
  if (should_be_active != effect_runtime_active_) {
    if (should_be_active) {
      EnableEffectRuntime();
    } else {
      DisableEffectRuntime();
    }
  }
  effect_controller_.ApplyExclusionTransitionOverrides(GetOverlayWindow());
}

void ApplicationRuntime::UpdateTemporaryPause() {
  if (!pause_controller_.Update(GetTickCount64())) return;
  minimize::core::LogDebug(L"Pause", L"Temporary pause expired; resuming Minimize Effect");
  RefreshEffectRuntimeState();
  settings_window_.UpdatePauseState(false, false);
}

features::RenderingPressure ApplicationRuntime::GetRenderingPressure() const {
  const int active_animations = static_cast<int>(
      std::count_if(runs_.begin(), runs_.end(),
                    [](const runtime::AnimationRun& run) { return run.overlay.active(); }));
  return features::RenderingPressure{
      .active_animations = active_animations,
      .avg_capture_duration_ms = avg_capture_duration_ms_,
      .recent_missed_frames = recent_missed_frames_,
      .recent_device_failures = recent_device_failures_,
      .renderer_recovering = renderer_recovery_.pending(),
  };
}

}  // namespace minimize::app
