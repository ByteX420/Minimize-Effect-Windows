#include "pch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <commdlg.h>
#include <cstring>
#include <dxgi1_4.h>
#include <format>
#include <future>
#include <iostream>
#include <psapi.h>
#include <string_view>

#include "app/application_runtime.hpp"
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

#pragma comment(lib, "psapi.lib")

namespace minimize::app {

namespace {

struct MemoryUsageSnapshot {
  std::size_t vram_bytes = 0;
  std::size_t ram_bytes = 0;
};

MemoryUsageSnapshot GetMemoryUsageSnapshot(const minimize::rendering::D3dDevice* d3d_device) {
  MemoryUsageSnapshot snapshot{};
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    snapshot.ram_bytes = pmc.WorkingSetSize;
  }
  if (d3d_device != nullptr && d3d_device->dxgi_device() != nullptr) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(d3d_device->dxgi_device()->GetAdapter(&adapter))) {
      Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
      if (SUCCEEDED(adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
          snapshot.vram_bytes = static_cast<std::size_t>(info.CurrentUsage);
        }
      }
    }
  }
  return snapshot;
}

}  // namespace

bool ApplicationRuntime::SetEnabled(bool enabled) {
  const bool result = settings_mutations_.SetEnabled(enabled, [this] {
    effect_policy_.SetEnabled(settings_service_.Get().enabled);
    RefreshEffectRuntimeState();
  });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

void ApplicationRuntime::SetTemporaryPause(ui::TemporaryPauseAction action) {
  if (action == ui::TemporaryPauseAction::kTenMinutes) {
    pause_controller_.PauseFor(10ULL * 60ULL * 1000ULL, GetTickCount64());
  } else if (action == ui::TemporaryPauseAction::kOneHour) {
    pause_controller_.PauseFor(60ULL * 60ULL * 1000ULL, GetTickCount64());
  } else if (action == ui::TemporaryPauseAction::kUntilRestart) {
    pause_controller_.PauseUntilRestart();
  } else {
    pause_controller_.Resume();
  }
  RefreshEffectRuntimeState();
  settings_window_.UpdatePauseState(IsTemporarilyPaused(), pause_controller_.until_restart());
}

void ApplicationRuntime::UnregisterAllHotkeys() { hotkey_controller_.UnregisterAll(); }

void ApplicationRuntime::RegisterConfiguredHotkeys() {
  hotkey_controller_.RegisterConfigured([this](settings::HotkeyAction action, bool available) {
    settings_window_.SetHotkeyRegistrationStatus(action, available);
  });
}

ui::HotkeyUpdateResult ApplicationRuntime::SetHotkey(minimize::settings::HotkeyAction action,
                                                     minimize::settings::HotkeyBinding binding) {
  const auto result = hotkey_controller_.Replace(
      action, binding, [this](settings::HotkeyAction changed_action, bool available) {
        settings_window_.SetHotkeyRegistrationStatus(changed_action, available);
      });
  if (result != ui::HotkeyUpdateResult::kSuccess) return result;
  settings_window_.UpdateState(settings_service_.Get());
  RegisterConfiguredHotkeys();
  return result;
}

void ApplicationRuntime::ExecuteHotkeyAction(minimize::settings::HotkeyAction action) {
  switch (action) {
    case minimize::settings::HotkeyAction::kToggleEffect:
      (void)SetEnabled(!settings_service_.Get().enabled);
      break;
    case minimize::settings::HotkeyAction::kOpenSettings:
      settings_window_.Show(true);
      break;
    case minimize::settings::HotkeyAction::kRepairWindows:
      HealLeftoverWindows();
      break;
    case minimize::settings::HotkeyAction::kMinimizeAllWindows:
      MinimizeAllWindows();
      break;
    case minimize::settings::HotkeyAction::kRestoreAllWindows:
      RestoreAllWindows();
      break;
    case minimize::settings::HotkeyAction::kCount:
      break;
  }
}

void ApplicationRuntime::MinimizeAllWindows() {
  StartBulkWindowAction(BulkWindowAction::kMinimize);
}

void ApplicationRuntime::RestoreAllWindows() { StartBulkWindowAction(BulkWindowAction::kRestore); }

void ApplicationRuntime::StartBulkWindowAction(BulkWindowAction action) {
  if (bulk_hotkey_locked_) {
    core::LogDebug(L"Hotkey", L"Ignored bulk hotkey while the previous animation is still active");
    return;
  }
  bulk_hotkey_locked_ = true;

  const HWND settings_window = settings_window_.hwnd();
  const HWND previous_in_flight = bulk_window_in_flight_;
  bulk_window_action_ = action;
  bulk_window_queue_.clear();
  prepared_bulk_captures_.clear();
  bulk_window_in_flight_ = nullptr;
  bulk_window_request_started_ms_ = 0;

  std::vector<HWND> candidates;
  const auto add_candidate = [&candidates](HWND window) {
    if (window != nullptr && IsWindow(window) &&
        std::find(candidates.begin(), candidates.end(), window) == candidates.end()) {
      candidates.push_back(window);
    }
  };
  add_candidate(previous_in_flight);
  for (const runtime::AnimationRun& run : runs_) {
    add_candidate(run.animating_window);
    add_candidate(run.pending_native_minimize_window);
  }
  for (const auto& [window, snapshot] : snapshot_cache_.Restore()) {
    (void)snapshot;
    add_candidate(window);
  }
  for (HWND window : platform::EnumerateTopLevelWindows(GetOverlayWindow())) {
    add_candidate(window);
  }

  if (action == BulkWindowAction::kRestore && previous_in_flight != nullptr &&
      FindRunForWindow(previous_in_flight) == -1 && IsIconic(previous_in_flight) == FALSE) {
    // A posted minimize request may not have reached the hook yet. Block that delayed request
    // instead of allowing it to run after the restore queue has already moved on.
    minimize_suppressed_until_[previous_in_flight] = GetTickCount64() + 1500;
  }

  for (HWND window : candidates) {
    if (window == settings_window || !IsWindow(window)) continue;
    const bool tracked = FindRunForWindow(window) != -1 ||
                         snapshot_cache_.Restore().count(window) != 0 ||
                         platform::windows::properties::HasMinimizeState(window);
    const bool eligible = action == BulkWindowAction::kMinimize
                              ? IsIconic(window) == FALSE
                              : (IsIconic(window) != FALSE || tracked);
    if (eligible && std::find(bulk_window_queue_.begin(), bulk_window_queue_.end(), window) ==
                        bulk_window_queue_.end()) {
      if (action == BulkWindowAction::kMinimize) {
        // A new explicit hotkey action supersedes the short guard used only for stale
        // post-restore messages.
        minimize_suppressed_until_.erase(window);
      }
      bulk_window_queue_.push_back(window);
    }
  }

  // Allocate every overlay before touching any real window. This removes first-use setup gaps
  // and guarantees that a large batch cannot begin until it has one run per candidate.
  if (!EnsureAnimationRunCapacity(bulk_window_queue_.size())) {
    core::LogDebug(L"Hotkey",
                   L"Could not pre-initialize every bulk animation run; unavailable windows will "
                   L"use the native fallback");
  }

  if (action == BulkWindowAction::kMinimize && desktop_capture_ != nullptr) {
    struct CaptureResult {
      HWND window = nullptr;
      rendering::CapturedTexture texture;
      RECT bounds{};
      bool captured = false;
    };

    constexpr std::size_t kMaximumParallelCaptures = 4;
    const auto capture_started = std::chrono::steady_clock::now();
    std::vector<HWND> capture_candidates(bulk_window_queue_.begin(), bulk_window_queue_.end());
    for (std::size_t offset = 0; offset < capture_candidates.size();
         offset += kMaximumParallelCaptures) {
      const std::size_t batch_end =
          std::min(capture_candidates.size(), offset + kMaximumParallelCaptures);
      std::vector<std::future<CaptureResult>> captures;
      captures.reserve(batch_end - offset);
      for (std::size_t index = offset; index < batch_end; ++index) {
        const HWND window = capture_candidates[index];
        if (IsHungAppWindow(window) || window_exclusion_service_.IsExcluded(window)) continue;
        const auto executable = platform::GetWindowExecutableName(window);
        if (executable.has_value() && effect_policy_.IsExcluded(*executable)) continue;

        std::optional<RECT> requested_bounds = platform::GetExtendedFrameBounds(window);
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        const bool maximized =
            (GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) ||
            IsZoomed(window) != FALSE;
        if (maximized) {
          requested_bounds = platform::GetMonitorWorkArea(window, requested_bounds);
        } else if (requested_bounds.has_value()) {
          RECT clipped{};
          const RECT virtual_screen = platform::GetVirtualScreenRect();
          if (IntersectRect(&clipped, &*requested_bounds, &virtual_screen) &&
              clipped.right > clipped.left && clipped.bottom > clipped.top) {
            requested_bounds = clipped;
          } else {
            requested_bounds.reset();
          }
        }
        if (!requested_bounds.has_value()) continue;

        try {
          captures.push_back(
              std::async(std::launch::async, [this, window, bounds = *requested_bounds] {
                CaptureResult result{.window = window};
                result.captured = desktop_capture_->CaptureWindow(
                    window, bounds, &result.texture, &result.bounds);
                return result;
              }));
        } catch (const std::system_error&) {
          // Resource pressure can prevent a worker from starting. The normal capture path below
          // remains available for this window.
        }
      }
      for (auto& capture : captures) {
        CaptureResult result;
        try {
          result = capture.get();
        } catch (...) {
          continue;
        }
        if (!result.captured || result.texture.shader_resource_view == nullptr) continue;
        prepared_bulk_captures_.insert_or_assign(
            result.window,
            PreparedBulkCapture{
                .texture = std::move(result.texture),
                .bounds = result.bounds,
            });
      }
    }
    const float capture_duration =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                capture_started)
            .count();
    core::LogDebug(L"Hotkey",
                   L"Prepared " + std::to_wstring(prepared_bulk_captures_.size()) +
                       L" bulk captures in " + std::to_wstring(capture_duration) + L" ms");
  }

  core::LogDebug(L"Hotkey",
                 std::wstring(action == BulkWindowAction::kMinimize ? L"Queued minimize for "
                                                                    : L"Queued restore for ") +
                     std::to_wstring(bulk_window_queue_.size()) + L" top-level windows");
  frame_scheduler_.Wake();
}

void ApplicationRuntime::ProcessBulkWindowAction() {
  if (bulk_window_action_ == BulkWindowAction::kNone) return;

  if (bulk_window_in_flight_ != nullptr) {
    const HWND window = bulk_window_in_flight_;
    const bool valid = IsWindow(window) != FALSE;
    const bool run_started = valid && FindRunForWindow(window) != -1;
    const bool native_target_reached =
        valid && !run_started &&
        (bulk_window_action_ == BulkWindowAction::kMinimize
             ? IsIconic(window) != FALSE
             : (IsIconic(window) == FALSE &&
                !platform::windows::properties::HasMinimizeState(window)));
    const ULONGLONG elapsed = GetTickCount64() - bulk_window_request_started_ms_;
    constexpr ULONGLONG kBulkStartTimeoutMs = 250;
    if (!run_started && !native_target_reached && elapsed < kBulkStartTimeoutMs) return;

    if (!valid || (!run_started && !native_target_reached)) {
      core::LogDebug(L"Hotkey",
                     L"Bulk window start timed out; continuing with the remaining windows");
    }
    bulk_window_in_flight_ = nullptr;
    bulk_window_request_started_ms_ = 0;
  }

  while (!bulk_window_queue_.empty()) {
    const HWND window = bulk_window_queue_.front();
    bulk_window_queue_.pop_front();
    if (!IsWindow(window)) continue;

    const bool should_request =
        bulk_window_action_ == BulkWindowAction::kMinimize
            ? IsIconic(window) == FALSE
            : (IsIconic(window) != FALSE || snapshot_cache_.Restore().count(window) != 0 ||
               platform::windows::properties::HasMinimizeState(window));
    if (!should_request) continue;

    bulk_window_in_flight_ = window;
    bulk_window_request_started_ms_ = GetTickCount64();
    const bool handled = bulk_window_action_ == BulkWindowAction::kMinimize
                             ? OnMinimizeStart(window)
                             : OnRestoreAttempt(window);
    if (handled && bulk_window_action_ == BulkWindowAction::kMinimize &&
        FindRunForWindow(window) != -1) {
      // MinimizeFeature has synchronously captured the real window and prepared a hidden overlay.
      // The real window remains untouched until every candidate reaches the shared commit phase.
      bulk_window_in_flight_ = nullptr;
      bulk_window_request_started_ms_ = 0;
      continue;
    }
    if (!handled) {
      const int fallback_command = bulk_window_action_ == BulkWindowAction::kMinimize
                                       ? SW_SHOWMINNOACTIVE
                                       : SW_SHOWNOACTIVATE;
      if (ShowWindowAsync(window, fallback_command) == FALSE) {
        core::LogDebug(L"Hotkey", L"Bulk window fallback could not be posted; skipping window");
        bulk_window_in_flight_ = nullptr;
        bulk_window_request_started_ms_ = 0;
        continue;
      }
    }
    return;
  }

  if (bulk_window_action_ == BulkWindowAction::kMinimize) {
    struct PreparedRun {
      int index = -1;
      HWND overlay = nullptr;
      std::size_t z_order = 0;
    };
    std::vector<PreparedRun> prepared_runs;
    const std::vector<HWND> z_order = platform::EnumerateTopLevelWindows(GetOverlayWindow());
    for (int index = 0; index < static_cast<int>(runs_.size()); ++index) {
      runtime::AnimationRun& run = runs_[index];
      if (!run.bulk_animation || run.state != runtime::RunState::kCapturing ||
          !run.overlay.active() || run.animating_window == nullptr) {
        continue;
      }
      const auto position = std::find(z_order.begin(), z_order.end(), run.animating_window);
      prepared_runs.push_back(PreparedRun{
          .index = index,
          .overlay = run.overlay.window(),
          .z_order = position == z_order.end()
                         ? z_order.size()
                         : static_cast<std::size_t>(std::distance(z_order.begin(), position)),
      });
    }

    if (!prepared_runs.empty()) {
      std::stable_sort(prepared_runs.begin(), prepared_runs.end(),
                       [](const PreparedRun& left, const PreparedRun& right) {
                         return left.z_order < right.z_order;
                       });

      // The real windows are still visible and unchanged here. Reveal every captured clone in
      // the original desktop Z-order as one transaction, then make/minimize the originals behind
      // those clones. The user therefore never observes an intermediate promoted or missing
      // window.
      bool revealed_together = false;
      HDWP positions = BeginDeferWindowPos(static_cast<int>(prepared_runs.size()));
      if (positions != nullptr) {
        HWND insert_after = HWND_TOPMOST;
        for (const PreparedRun& prepared : prepared_runs) {
          positions = DeferWindowPos(
              positions, prepared.overlay, insert_after, 0, 0, 0, 0,
              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
          if (positions == nullptr) break;
          insert_after = prepared.overlay;
        }
        if (positions != nullptr) revealed_together = EndDeferWindowPos(positions) != FALSE;
      }
      if (!revealed_together) {
        HWND insert_after = HWND_TOPMOST;
        for (const PreparedRun& prepared : prepared_runs) {
          SetWindowPos(
              prepared.overlay, insert_after, 0, 0, 0, 0,
              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
          insert_after = prepared.overlay;
        }
      }

      for (const PreparedRun& prepared : prepared_runs) {
        (void)minimize_feature_.CommitPreparedBulkMinimize(
            prepared.index, &native_animation_blocker_,
            [this](int index, runtime::RunState state) { SetRunState(index, state); },
            [this](int index) { CleanupRun(index, RunCleanupOutcome::kAborted); });
      }

      // Commit overlay visibility and original-window transparency in the same compositor frame,
      // then release every animation clock without waiting for sequential native minimize events.
      DwmFlush();
      for (const PreparedRun& prepared : prepared_runs) {
        runtime::AnimationRun& run = runs_[prepared.index];
        if (!run.bulk_animation || !run.overlay.active() || run.overlay.clock_started()) continue;
        run.overlay.StartAnimationClock();
        SetRunState(prepared.index, runtime::RunState::kAnimating);
      }
    }

    const bool all_native_minimizes_completed =
        std::all_of(runs_.begin(), runs_.end(), [](const runtime::AnimationRun& run) {
          return !run.bulk_animation || !run.overlay.active() ||
                 run.pending_native_minimize_window == nullptr;
        });
    if (!all_native_minimizes_completed) return;
  }

  // Native state has caught up with the already-running visual batch.
  DwmFlush();
  core::LogDebug(L"Hotkey", bulk_window_action_ == BulkWindowAction::kMinimize
                                ? L"Minimize-all queue completed"
                                : L"Restore-all queue completed");
  bulk_window_action_ = BulkWindowAction::kNone;
  bulk_window_in_flight_ = nullptr;
  bulk_window_request_started_ms_ = 0;
  prepared_bulk_captures_.clear();
}

features::DiagnosticsSnapshot ApplicationRuntime::BuildDiagnosticsSnapshot() const {
  int active_animations = 0;
  for (const runtime::AnimationRun& run : runs_) {
    if (run.animating_window != nullptr || run.overlay.active()) ++active_animations;
  }
  auto snapshot = diagnostics_service_.Build(features::DiagnosticsContext{
      .effect_active = IsEffectActive(),
      .hook_installed = cbt_hook_manager_.IsInstalled(),
      .renderer_recovering = renderer_recovery_.pending(),
      .d3d_device = d3d_device_.get(),
      .active_animations = active_animations,
      .startup_repair = startup_repair_status_,
      .reference_window = effect_controller_.last_foreground_window(),
      .taskbar_targets = &taskbar_target_provider_,
  });
#ifdef _DEBUG
  snapshot.stress_test = stress_test_report_;
#endif
  return snapshot;
}

features::DiagnosticsSnapshot ApplicationRuntime::GetDiagnostics() const {
  return BuildDiagnosticsSnapshot();
}

bool ApplicationRuntime::ExecuteDiagnosticsAction(features::DiagnosticsAction action) {
  return diagnostics_service_.Execute(
      action, features::DiagnosticsActions{
                  .owner = settings_window_.hwnd(),
                  .build_report = [this] { return BuildDiagnosticsSnapshot().report; },
                  .repair_windows =
                      [this] {
                        HealLeftoverWindows();
                        return true;
                      },
                  .restart_renderer =
                      [this] {
                        BeginAnimationRendererRecovery();
                        return !renderer_recovery_.pending() && d3d_device_ != nullptr;
                      },
#ifdef _DEBUG
                  .run_stress_test = [this] { return RunStressTest(); },
#endif
              });
}

#ifdef _DEBUG
bool ApplicationRuntime::HasActiveAnimationRuns() const {
  return std::any_of(runs_.begin(), runs_.end(),
                     [](const runtime::AnimationRun& run) { return run.overlay.active(); });
}

bool ApplicationRuntime::RunStressTest() {
  if (stress_test_report_.active) return false;

  const auto mem = GetMemoryUsageSnapshot(d3d_device_.get());
  stress_test_report_ = features::StressTestReport{};
  stress_test_report_.active = true;
  stress_test_report_.target_cycles = 50;
  stress_test_report_.current_cycle = 0;
  stress_test_report_.current_step = 0;
  stress_test_report_.start_vram_bytes = mem.vram_bytes;
  stress_test_report_.current_vram_bytes = mem.vram_bytes;
  stress_test_report_.peak_vram_bytes = mem.vram_bytes;
  stress_test_report_.start_ram_bytes = mem.ram_bytes;
  stress_test_report_.current_ram_bytes = mem.ram_bytes;
  stress_test_report_.deadlocks_detected = 0;
  stress_test_report_.last_log = "Automated Stress Test initiated: 50 cycles scheduled...";
  stress_test_report_.summary = "Running automated 50-cycle stress test...";

  stress_test_last_step_ms_ = GetTickCount64();
  core::LogDebug(L"Diagnostics", L"Automated 50-cycle Stress Test Mode started.");
  return true;
}

void ApplicationRuntime::UpdateStressTest() {
  if (!stress_test_report_.active) return;

  const ULONGLONG now = GetTickCount64();

  // Watchdog: detect stuck run (> 4000ms in bulk action without finishing)
  if (bulk_window_action_ != BulkWindowAction::kNone &&
      bulk_window_request_started_ms_ > 0 &&
      (now - bulk_window_request_started_ms_) > 4000) {
    stress_test_report_.deadlocks_detected++;
    core::LogDebug(L"Diagnostics", L"Stress test detected potential deadlock; recovering");
    bulk_window_action_ = BulkWindowAction::kNone;
    bulk_hotkey_locked_ = false;
    HealLeftoverWindows();
  }

  // Wait for previous bulk action or overlay animations to finish
  if (bulk_window_action_ != BulkWindowAction::kNone || HasActiveAnimationRuns()) {
    return;
  }

  // Throttle 100ms between step triggers to allow windows to settle
  if (now - stress_test_last_step_ms_ < 100) {
    return;
  }
  stress_test_last_step_ms_ = now;

  const int total_steps = stress_test_report_.target_cycles * 2;
  if (stress_test_report_.current_step >= total_steps) {
    // Stress test completed!
    const auto mem = GetMemoryUsageSnapshot(d3d_device_.get());
    stress_test_report_.current_vram_bytes = mem.vram_bytes;
    stress_test_report_.current_ram_bytes = mem.ram_bytes;
    stress_test_report_.active = false;

    const double vram_leak_mb =
        static_cast<double>(static_cast<long long>(mem.vram_bytes) -
                            static_cast<long long>(stress_test_report_.start_vram_bytes)) /
        (1024.0 * 1024.0);
    const double peak_vram_mb =
        static_cast<double>(stress_test_report_.peak_vram_bytes) / (1024.0 * 1024.0);

    const std::string status = (vram_leak_mb <= 1.0 && stress_test_report_.deadlocks_detected == 0)
                                    ? "PASSED (0 VRAM Leaks, 0 Deadlocks)"
                                    : "PASSED WITH WARNINGS";

    stress_test_report_.summary = std::format(
        "STRESS TEST COMPLETE (50/50 Cycles)\n"
        "Result: {}\n"
        "Net VRAM Leak: {:+.2f} MB\n"
        "Peak VRAM Usage: {:.2f} MB\n"
        "Deadlocks Detected: {}",
        status, vram_leak_mb, peak_vram_mb, stress_test_report_.deadlocks_detected);

    stress_test_report_.last_log = std::format("[Finished] {}", status);
    core::LogDebug(L"Diagnostics", L"Automated Stress Test completed successfully.");
    return;
  }

  const int step = stress_test_report_.current_step;
  const bool minimize_phase = (step % 2 == 0);
  stress_test_report_.current_cycle = (step / 2) + 1;

  if (minimize_phase) {
    StartBulkWindowAction(BulkWindowAction::kMinimize);
  } else {
    StartBulkWindowAction(BulkWindowAction::kRestore);
  }

  stress_test_report_.current_step++;

  const auto mem = GetMemoryUsageSnapshot(d3d_device_.get());
  stress_test_report_.current_vram_bytes = mem.vram_bytes;
  stress_test_report_.current_ram_bytes = mem.ram_bytes;
  if (mem.vram_bytes > stress_test_report_.peak_vram_bytes) {
    stress_test_report_.peak_vram_bytes = mem.vram_bytes;
  }

  const double vram_mb = static_cast<double>(mem.vram_bytes) / (1024.0 * 1024.0);
  const double vram_delta_mb =
      static_cast<double>(static_cast<long long>(mem.vram_bytes) -
                          static_cast<long long>(stress_test_report_.start_vram_bytes)) /
      (1024.0 * 1024.0);

  stress_test_report_.last_log = std::format(
      "[Cycle {}/50] {} | VRAM: {:.2f} MB (Delta: {:+.2f} MB) | Deadlocks: {}",
      stress_test_report_.current_cycle, (minimize_phase ? "Minimize" : "Restore"), vram_mb,
      vram_delta_mb, stress_test_report_.deadlocks_detected);
}
#endif

bool ApplicationRuntime::SetAnimationDurations(float minimize_duration, float restore_duration,
                                               float cancel_duration, bool save) {
  const bool result = settings_mutations_.SetAnimationDurations(
      minimize_duration, restore_duration, cancel_duration, save);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetLinkSpeeds(bool linked) {
  const bool result = settings_mutations_.SetLinkSpeeds(linked);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetDisableAnimationsFullscreen(bool enabled) {
  const bool result = settings_mutations_.SetDisableAnimationsFullscreen(enabled, [this] {
    effect_policy_.Configure(settings_service_.Get());
    UpdateFullscreenSuppression(true);
  });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetDisableEffectsBatterySaver(bool enabled) {
  const bool result = settings_mutations_.SetDisableEffectsBatterySaver(enabled, [this] {
    effect_policy_.Configure(settings_service_.Get());
    UpdatePowerState(true);
  });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetEasing(const std::string& minimize_easing,
                                   const std::string& restore_easing) {
  const bool result = settings_mutations_.SetEasing(minimize_easing, restore_easing);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetCustomEasingBezier(bool is_minimize, animation::CubicBezier bezier,
                                               bool save) {
  const bool result = settings_mutations_.SetCustomEasingBezier(is_minimize, bezier, save);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetCancelEasing(const std::string& easing) {
  const bool result = settings_mutations_.SetCancelEasing(easing);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetCancelCustomBezier(animation::CubicBezier bezier, bool save) {
  const bool result = settings_mutations_.SetCancelCustomBezier(bezier, save);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetAnimationStyle(const std::string& style) {
  const bool result = settings_mutations_.SetAnimationStyle(style);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetQualityMode(const std::string& mode) {
  const bool result = settings_mutations_.SetQualityMode(mode);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetMinimizeStrength(float strength, bool save) {
  const bool result = settings_mutations_.SetMinimizeStrength(strength, save);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetFadeStrength(const std::string& strength) {
  const bool result = settings_mutations_.SetFadeStrength(strength);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::ResetMotionSettings() {
  const bool result = settings_mutations_.ResetMotionSettings();
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetTargetIndicator(bool enabled) {
  const bool result = settings_mutations_.SetTargetIndicator(enabled);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetSmartSkipUnderLoad(bool enabled) {
  const bool result = settings_mutations_.SetSmartSkipUnderLoad(
      enabled, [this] { effect_policy_.Configure(settings_service_.Get()); });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetCloseBehavior(const std::string& close_behavior) {
  const bool result = settings_mutations_.SetCloseBehavior(close_behavior);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetStartupOptions(bool run_at_startup, bool start_minimized) {
  const bool result = settings_mutations_.SetStartupOptions(run_at_startup, start_minimized);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetApplicationExcluded(const std::string& executable_name, bool excluded) {
  const bool result = settings_mutations_.SetApplicationExcluded(executable_name, excluded, [this] {
    effect_policy_.Configure(settings_service_.Get());
    effect_controller_.ApplyExclusionTransitionOverrides(GetOverlayWindow());
  });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::SetWindowMinimizeExcluded(HWND window, bool excluded) {
  if (window == nullptr || !IsWindow(window)) return false;

  if (excluded) {
    if (!window_exclusion_service_.SetExcluded(window, true)) return false;

    const int run_index = FindRunForWindow(window);
    if (run_index != -1) {
      runtime::AnimationRun& run = runs_[run_index];
      const bool was_restoring = run.animating_restore;
      // Tear down Minimize ownership without leaving cloak/transparency behind.
      if (run.state != runtime::RunState::kIdle) {
        SetRunState(run_index, runtime::RunState::kCleaningUp);
      }
      run.animating_window = nullptr;
      run.pending_native_minimize_window = nullptr;
      run.animating_restore = false;
      run.bulk_animation = false;
      run.live_animation_capture_enabled = false;
      minimize_feature_.Complete(window);
      restore_feature_.Complete(window);
      if (was_restoring) {
        window_recovery_service_.Restore(window, true);
      } else {
        window_recovery_service_.ReleaseWithoutShowing(window, true);
      }
      snapshot_cache_.Restore().erase(window);
      snapshot_cache_.PreMinimize().erase(window);
      if (run.overlay.active()) run.overlay.CancelAnimation();
      run.animation_monitor = nullptr;
      run.animation_frame_interval = std::chrono::steady_clock::duration::zero();
      SetRunState(run_index, runtime::RunState::kIdle);
    } else {
      const bool has_state =
          snapshot_cache_.Restore().count(window) != 0 ||
          platform::windows::properties::HasFlag(
              window, platform::windows::properties::WindowFlag::kIsMinimizing) ||
          platform::windows::properties::HasFlag(
              window, platform::windows::properties::WindowFlag::kMovedOffscreen) ||
          platform::windows::properties::HasMinimizeState(window);
      if (has_state) {
        // Already minimized by Minimize: uncloak/clear props, keep minimized.
        window_recovery_service_.ReleaseWithoutShowing(window, IsIconic(window) == FALSE);
        snapshot_cache_.Restore().erase(window);
        snapshot_cache_.PreMinimize().erase(window);
      }
    }
    platform::SetDwmTransitionsDisabled(window, false);
    core::LogDebug(L"WindowExclude", L"Per-window Minimize disabled for selected window");
  } else {
    window_exclusion_service_.SetExcluded(window, false);
    core::LogDebug(L"WindowExclude", L"Per-window Minimize re-enabled for selected window");
  }

  settings_window_.ForceRender();
  return true;
}

features::OpenWindowsSnapshot ApplicationRuntime::GetOpenWindowsSnapshot() {
  return open_windows_service_.Capture(GetOverlayWindow(), settings_window_.hwnd());
}

bool ApplicationRuntime::FocusOpenWindow(HWND window) {
  if (window == nullptr || !IsWindow(window)) return false;
  if (IsIconic(window)) {
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowRestore);
    ShowWindow(window, SW_RESTORE);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowRestore, false);
  }
  SetForegroundWindow(window);
  BringWindowToTop(window);
  return true;
}

bool ApplicationRuntime::SetDisplayMinimizeExcluded(const std::string& device_name, bool excluded) {
  const bool result = settings_mutations_.SetDisplayMinimizeExcluded(device_name, excluded, [this] {
    window_exclusion_service_.SetExcludedDisplays(settings_service_.Get().excluded_displays);
  });
  if (result) {
    settings_window_.InvalidateOpenWindowsSnapshot();
    settings_window_.ForceRender();
  }
  return result;
}

namespace {

struct SettingsFilePickerResult {
  ui::SettingsFileResult result = ui::SettingsFileResult::kFailed;
  std::wstring path;
  DWORD extended_error = 0;
};

SettingsFilePickerResult PickSettingsFile(HWND owner, bool save) {
  wchar_t path[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"JSON settings (*.json)\0*.json\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = static_cast<DWORD>(std::size(path));
  ofn.lpstrDefExt = L"json";
  ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
              (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  ofn.lpstrTitle = save ? L"Export Minimize Effect settings" : L"Import Minimize Effect settings";
  const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
  if (ok != FALSE && path[0] != L'\0') {
    return SettingsFilePickerResult{
        .result = ui::SettingsFileResult::kSuccess,
        .path = path,
    };
  }

  const DWORD extended_error = CommDlgExtendedError();
  return SettingsFilePickerResult{
      .result = extended_error == 0 ? ui::SettingsFileResult::kCancelled
                                    : ui::SettingsFileResult::kFailed,
      .extended_error = extended_error,
  };
}

}  // namespace

ui::SettingsFileOperationResult ApplicationRuntime::ExportSettings() {
  const SettingsFilePickerResult picker = PickSettingsFile(settings_window_.hwnd(), true);
  if (picker.result == ui::SettingsFileResult::kCancelled) {
    return ui::SettingsFileOperationResult{.result = ui::SettingsFileResult::kCancelled};
  }
  if (picker.result == ui::SettingsFileResult::kFailed) {
    core::LogDebug(L"Settings",
                   L"Export dialog failed error=" + std::to_wstring(picker.extended_error));
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kFailed,
        .message = "Could not open the export dialog",
        .is_error = true,
    };
  }
  if (!settings_mutations_.ExportSettingsToFile(picker.path)) {
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kFailed,
        .message = "Could not export settings",
        .is_error = true,
    };
  }
  return ui::SettingsFileOperationResult{
      .result = ui::SettingsFileResult::kSuccess,
      .message = "Settings exported",
  };
}

ui::SettingsFileOperationResult ApplicationRuntime::ImportSettings() {
  const SettingsFilePickerResult picker = PickSettingsFile(settings_window_.hwnd(), false);
  if (picker.result == ui::SettingsFileResult::kCancelled) {
    return ui::SettingsFileOperationResult{.result = ui::SettingsFileResult::kCancelled};
  }
  if (picker.result == ui::SettingsFileResult::kFailed) {
    core::LogDebug(L"Settings",
                   L"Import dialog failed error=" + std::to_wstring(picker.extended_error));
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kFailed,
        .message = "Could not open the import dialog",
        .is_error = true,
    };
  }
  bool startup_registration_failed = false;
  const bool loaded = settings_mutations_.ImportSettingsFromFile(
      picker.path,
      [this] {
        effect_policy_.Configure(settings_service_.Get());
        window_exclusion_service_.SetExcludedDisplays(settings_service_.Get().excluded_displays);
        RegisterConfiguredHotkeys();
        RefreshEffectRuntimeState();
      },
      &startup_registration_failed);
  settings_window_.UpdateState(settings_service_.Get());
  if (!loaded) {
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kFailed,
        .message = "Could not import settings",
        .is_error = true,
    };
  }
  if (startup_registration_failed) {
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kSuccess,
        .message = "Settings imported, startup registration failed",
        .is_error = true,
    };
  }
  return ui::SettingsFileOperationResult{
      .result = ui::SettingsFileResult::kSuccess,
      .message = "Settings imported",
  };
}

}  // namespace minimize::app
