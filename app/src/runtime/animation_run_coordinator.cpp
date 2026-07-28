#include "pch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

int ApplicationRuntime::FindRunForWindow(HWND window) const {
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    if (runs_[i].animating_window == window) {
      return i;
    }
  }
  return -1;
}

int ApplicationRuntime::FindAvailableRun() {
  for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
    if (runs_[i].state == runtime::RunState::kIdle && runs_[i].animating_window == nullptr &&
        !runs_[i].overlay.active()) {
      return i;
    }
  }
  runtime::AnimationRun& first_run = runs_.Add();
  if (!InitializeRun(first_run)) {
    runs_.RemoveLast();
    return -1;
  }
  return static_cast<int>(runs_.size() - 1);
}

bool ApplicationRuntime::EnsureAnimationRunCapacity(std::size_t minimum_count) {
  while (runs_.size() < minimum_count) {
    runtime::AnimationRun& run = runs_.Add();
    if (!InitializeRun(run)) {
      runs_.RemoveLast();
      return false;
    }
  }
  return true;
}

bool ApplicationRuntime::IsOverlayWindow(HWND window) const {
  return std::any_of(runs_.begin(), runs_.end(), [window](const runtime::AnimationRun& slot) {
    return slot.overlay.window() == window;
  });
}

bool ApplicationRuntime::InitializeRun(runtime::AnimationRun& slot) {
  if (!slot.overlay.Initialize(
          instance_, d3d_device_.get(), [this](HWND window) { return OnMinimizeStart(window); },
          [this](HWND window) { return OnRestoreAttempt(window); })) {
    return false;
  }
  slot.overlay.SetAnimationDuration(settings_service_.Get().minimize_duration);
  slot.state = runtime::RunState::kIdle;
  slot.state_entered_ms = GetTickCount64();
  return true;
}

void ApplicationRuntime::SetRunState(int run_index, runtime::RunState state) {
  if (run_index < 0 || run_index >= static_cast<int>(runs_.size())) return;
  const runtime::RunState previous = runs_[run_index].state;
  if (!runtime::IsRunStateTransitionAllowed(previous, state)) {
    minimize::core::LogDebug(
        L"Runtime",
        L"Rejected run-state transition " +
            std::wstring(
                runtime::RunStateName(previous),
                runtime::RunStateName(previous) + std::strlen(runtime::RunStateName(previous))) +
            L" -> " +
            std::wstring(runtime::RunStateName(state),
                         runtime::RunStateName(state) + std::strlen(runtime::RunStateName(state))));
    return;
  }
  minimize::core::LogTrace(
      L"Runtime",
      L"Run " + std::to_wstring(run_index) + L" state " +
          std::wstring(
              runtime::RunStateName(previous),
              runtime::RunStateName(previous) + std::strlen(runtime::RunStateName(previous))) +
          L" -> " +
          std::wstring(runtime::RunStateName(state),
                       runtime::RunStateName(state) + std::strlen(runtime::RunStateName(state))));
  runs_[run_index].state = state;
  runs_[run_index].state_entered_ms = GetTickCount64();
}

void ApplicationRuntime::CleanupRun(int run_index, RunCleanupOutcome outcome) {
  if (run_index < 0 || run_index >= static_cast<int>(runs_.size())) return;
  runtime::AnimationRun& slot = runs_[run_index];
  HWND window = slot.animating_window != nullptr ? slot.animating_window
                                                 : slot.pending_native_minimize_window;
  const bool was_restoring = slot.animating_restore;
  const bool native_minimize_pending = slot.pending_native_minimize_window == window;

  if (outcome == RunCleanupOutcome::kAborted && slot.state != runtime::RunState::kIdle) {
    SetRunState(run_index, runtime::RunState::kAborting);
  }
  if (slot.state != runtime::RunState::kIdle) {
    SetRunState(run_index, runtime::RunState::kCleaningUp);
  }

  // Detach first: restoring or showing the real window can synchronously re-enter event handlers.
  slot.animating_window = nullptr;
  slot.pending_native_minimize_window = nullptr;
  slot.animating_restore = false;
  slot.bulk_animation = false;
  slot.live_animation_capture_enabled = false;

  if (window != nullptr && IsWindow(window)) {
    if (outcome == RunCleanupOutcome::kAborted) {
      if (was_restoring) {
        restore_feature_.Cancel(window, true);
      } else {
        minimize_feature_.Cancel(window);
      }
      snapshot_cache_.Restore().erase(window);
      snapshot_cache_.PreMinimize().erase(window);
    } else if (was_restoring) {
      restore_feature_.Complete(window);
      constexpr ULONGLONG kPostRestoreMinimizeSuppressionMs = 150;
      const ULONGLONG now = GetTickCount64();
      for (auto iterator = minimize_suppressed_until_.begin();
           iterator != minimize_suppressed_until_.end();) {
        if (!IsWindow(iterator->first) || iterator->second <= now) {
          iterator = minimize_suppressed_until_.erase(iterator);
        } else {
          ++iterator;
        }
      }
      minimize_suppressed_until_[window] = now + kPostRestoreMinimizeSuppressionMs;
      RestoreWindowFromMinimizeState(window);
      slot.overlay.FinishRestoreAnimation();
      snapshot_cache_.Restore().erase(window);
      std::wcout << L"Restore animation completed.\n";
    } else {
      minimize_feature_.Complete(window);
      if (native_minimize_pending) {
        platform::windows::properties::SetFlag(
            window, platform::windows::properties::WindowFlag::kAllowMinimize);
        ShowWindow(window, SW_MINIMIZE);
        platform::windows::properties::SetFlag(
            window, platform::windows::properties::WindowFlag::kAllowMinimize, false);
      }
      platform::windows::properties::SetFlag(
          window, platform::windows::properties::WindowFlag::kAllowMinimize, false);
      HRGN hidden_region = CreateRectRgn(0, 0, 0, 0);
      (void)platform::SetOwnedWindowRegion(window, hidden_region, true);
      std::wcout << L"Minimize animation completed.\n";
    }
  } else {
    snapshot_cache_.Restore().erase(window);
    snapshot_cache_.PreMinimize().erase(window);
  }

  if (slot.overlay.active()) slot.overlay.CancelAnimation();
  if (slot.auto_hide_taskbar_revealed) {
    taskbar_target_provider_.ReleaseAutoHideTaskbar();
    slot.auto_hide_taskbar_revealed = false;
  }
  slot.animation_monitor = nullptr;
  slot.animation_frame_interval = std::chrono::steady_clock::duration::zero();
  SetRunState(run_index, runtime::RunState::kIdle);

  const bool any_active =
      std::any_of(runs_.begin(), runs_.end(),
                  [](const runtime::AnimationRun& run) { return run.overlay.active(); });
  if (!any_active) {
    frame_scheduler_.EndFallbackTimerResolution();
    frame_scheduler_.Wake();
  }
}

void ApplicationRuntime::CheckAnimationTimeouts() {
  const ULONGLONG now = GetTickCount64();
  for (int index = 0; index < static_cast<int>(runs_.size()); ++index) {
    runtime::AnimationRun& slot = runs_[index];
    if (slot.state == runtime::RunState::kIdle) continue;
    const ULONGLONG timeout_ms = runtime::RunStateTimeoutMs(slot.state);
    const ULONGLONG elapsed = now - slot.state_entered_ms;
    if (elapsed <= timeout_ms) continue;

    HWND window = slot.animating_window != nullptr ? slot.animating_window
                                                   : slot.pending_native_minimize_window;
    wchar_t title[128]{};
    if (window != nullptr) GetWindowTextW(window, title, 128);
    minimize::core::LogDebug(
        L"Watchdog", L"Aborting stuck animation=" + std::to_wstring(index) + L" state=" +
                         std::wstring(runtime::RunStateName(slot.state),
                                      runtime::RunStateName(slot.state) +
                                          std::strlen(runtime::RunStateName(slot.state))) +
                         L" elapsed_ms=" + std::to_wstring(elapsed) + L" hwnd=" +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(window)) + L" title=\"" +
                         title + L"\"");
    CleanupRun(index, RunCleanupOutcome::kAborted);
  }
}

HWND ApplicationRuntime::GetOverlayWindow() const {
  return runs_.empty() ? nullptr : runs_[0].overlay.window();
}

bool ApplicationRuntime::OnMinimizeStart(HWND window) {
  const ULONGLONG now = GetTickCount64();
  const auto suppressed = minimize_suppressed_until_.find(window);
  if (suppressed != minimize_suppressed_until_.end()) {
    if (now < suppressed->second) {
      minimize::core::LogDebug(L"Minimize", L"Ignored delayed minimize immediately after restore");
      return true;
    }
    minimize_suppressed_until_.erase(suppressed);
  }

  const features::RenderingPressure pressure = GetRenderingPressure();
  const bool force_animation =
      bulk_window_action_ == BulkWindowAction::kMinimize && bulk_window_in_flight_ == window;
  const bool handled = minimize_feature_.Execute(
      window,
      features::MinimizeExecutionContext{
          .overlay = GetOverlayWindow(),
          .effect_active = IsEffectActive(),
          .force_animation = force_animation,
          .renderer_recovering = renderer_recovery_.pending(),
          .shutting_down = shutting_down_.load(std::memory_order_acquire),
          .capture = desktop_capture_.get(),
          .taskbar_targets = &taskbar_target_provider_,
          .animation_blocker = &native_animation_blocker_,
          .animation_configuration = &animation_configuration_,
          .rendering_pressure = &pressure,
          .find_run = [this](HWND target) { return FindRunForWindow(target); },
          .find_available_run = [this] { return FindAvailableRun(); },
          .set_state = [this](int index, runtime::RunState state) { SetRunState(index, state); },
          .reset_frame_pacing =
              [this](int index, HWND target, const RECT& bounds) {
                ResetAnimationFramePacing(index, target, bounds);
              },
          .abort_run = [this](int index) { CleanupRun(index, RunCleanupOutcome::kAborted); },
          .complete_restore = [this](HWND target) { restore_feature_.Complete(target); },
          .record_capture_duration =
              [this](float duration_ms) { NoteCaptureDuration(duration_ms); },
          .take_prepared_capture =
              [this](HWND target, rendering::CapturedTexture* texture, RECT* bounds) {
                if (texture == nullptr || bounds == nullptr) return false;
                const auto prepared = prepared_bulk_captures_.find(target);
                if (prepared == prepared_bulk_captures_.end()) return false;
                *texture = std::move(prepared->second.texture);
                *bounds = prepared->second.bounds;
                prepared_bulk_captures_.erase(prepared);
                return texture->shader_resource_view != nullptr;
              },
      });
  const int run_index = FindRunForWindow(window);
  if (force_animation && run_index != -1) runs_[run_index].bulk_animation = true;
  return handled;
}

void ApplicationRuntime::FinishActiveAnimation(int run_index) {
  if (run_index < 0 || run_index >= static_cast<int>(runs_.size())) return;
  CleanupRun(run_index, RunCleanupOutcome::kCompleted);
  DwmFlush();
}

bool ApplicationRuntime::OnRestoreAttempt(HWND window) {
  const features::RenderingPressure pressure = GetRenderingPressure();
  const bool bulk_restore = bulk_window_action_ == BulkWindowAction::kRestore &&
                            bulk_window_in_flight_ == window;
  const bool handled = restore_feature_.Execute(
      window,
      features::RestoreExecutionContext{
          .overlay = GetOverlayWindow(),
          .effect_active = IsEffectActive(),
          .defer_first_frame_wait = bulk_restore,
          .renderer_recovering = renderer_recovery_.pending(),
          .shutting_down = shutting_down_.load(std::memory_order_acquire),
          .animation_blocker = &native_animation_blocker_,
          .animation_configuration = &animation_configuration_,
          .rendering_pressure = &pressure,
          .find_run = [this](HWND target) { return FindRunForWindow(target); },
          .find_available_run = [this] { return FindAvailableRun(); },
          .set_state = [this](int index, runtime::RunState state) { SetRunState(index, state); },
          .reset_frame_pacing =
              [this](int index, HWND target, const RECT& bounds) {
                ResetAnimationFramePacing(index, target, bounds);
              },
          .finish_run = [this](int index) { FinishActiveAnimation(index); },
          .abort_run = [this](int index) { CleanupRun(index, RunCleanupOutcome::kAborted); },
      });
  const int run_index = FindRunForWindow(window);
  if (bulk_restore && run_index != -1) runs_[run_index].bulk_animation = true;
  return handled;
}

void ApplicationRuntime::RestoreWindowFromMinimizeState(HWND window, bool force_show_if_iconic) {
  window_recovery_service_.Restore(window, force_show_if_iconic);
}

void ApplicationRuntime::HealLeftoverWindows() {
  const std::size_t repaired_count = window_recovery_service_.HealLeftovers();
  startup_repair_status_ = repaired_count == 0
                               ? "No issues found"
                               : std::to_string(repaired_count) + " window(s) repaired";
  minimize::core::LogDebug(L"App", L"Startup repair result: " + std::to_wstring(repaired_count) +
                                       L" suspicious window(s) repaired");
}

void ApplicationRuntime::CleanupAndRestoreAll() {
  if (cleaned_up_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  // Signal the main loop and all event handlers to stop immediately.
  shutting_down_.store(true, std::memory_order_release);

  minimize::core::LogDebug(L"App", L"CleanupAndRestoreAll starting");
  seed_iconic_snapshots_pending_ = false;
  minimize_feature_.CancelSeedSnapshotsForIconicWindows();
  power_status_monitor_.Stop();
  UnregisterAllHotkeys();
  effect_controller_.Stop();
  cbt_hook_manager_.Uninstall();
  native_animation_blocker_.Disable();

  // Post WM_QUIT so the main message loop exits if it's still running.
  if (GetOverlayWindow() != nullptr) {
    PostMessageW(GetOverlayWindow(), WM_CLOSE, 0, 0);
  }

  // Tear down in-flight animations without unminimizing windows that Minimize already
  // put in the taskbar. Mid-minimize finishes as minimized; mid-restore only clears
  // cloak/transparency and leaves iconic windows iconic.
  for (int index = 0; index < static_cast<int>(runs_.size()); ++index) {
    runtime::AnimationRun& run = runs_[index];
    if (run.animating_window == nullptr && run.pending_native_minimize_window == nullptr &&
        !run.overlay.active()) {
      continue;
    }
    HWND window =
        run.animating_window != nullptr ? run.animating_window : run.pending_native_minimize_window;
    const bool was_restoring = run.animating_restore;
    if (run.state != runtime::RunState::kIdle) {
      SetRunState(index, runtime::RunState::kCleaningUp);
    }
    run.animating_window = nullptr;
    run.pending_native_minimize_window = nullptr;
    run.animating_restore = false;
    run.bulk_animation = false;
    run.live_animation_capture_enabled = false;
    if (window != nullptr && IsWindow(window)) {
      if (was_restoring) {
        window_recovery_service_.ReleaseWithoutShowing(window, false);
      } else {
        window_recovery_service_.ReleaseWithoutShowing(window, true);
      }
      snapshot_cache_.Restore().erase(window);
      snapshot_cache_.PreMinimize().erase(window);
    }
    if (run.overlay.active()) run.overlay.CancelAnimation();
    run.animation_monitor = nullptr;
    run.animation_frame_interval = std::chrono::steady_clock::duration::zero();
    SetRunState(index, runtime::RunState::kIdle);
  }
  minimize_feature_.ReleaseAll();
  restore_feature_.ReleaseAll();
  runtime::SnapshotCache::Contents snapshots = snapshot_cache_.TakeAll();

  // Snapshots of effect-minimized windows first (finish_as_minimized), while effect state still
  // exist.
  for (const auto& [hwnd, snapshot] : snapshots.restore) {
    (void)snapshot;
    window_recovery_service_.ReleaseWithoutShowing(hwnd, true);
  }
  // Pre-minimize cache is for still-visible windows — only clear state, never force minimize.
  for (const auto& [hwnd, snapshot] : snapshots.pre_minimize) {
    (void)snapshot;
    window_recovery_service_.ReleaseWithoutShowing(hwnd, false);
  }

  // Safety net: any remaining tracked Minimize windows, still without SW_RESTORE.
  window_recovery_service_.HealUntrackedWindows();
  platform::windows::properties::ClearAllState();
  taskbar_target_provider_.RestoreAutoHideTaskbar();

  runs_.ShutdownOverlays();
  settings_window_.Shutdown();
  desktop_capture_.reset();
  d3d_device_.reset();
  frame_scheduler_.EndFallbackTimerResolution();
  frame_scheduler_.Wake();
  minimize::core::LogDebug(L"App", L"CleanupAndRestoreAll completed");
}

}  // namespace minimize::app
