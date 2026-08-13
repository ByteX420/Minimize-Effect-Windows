#include "pch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <commdlg.h>
#include <cstring>
#include <dxgi1_4.h>
#include <format>
#include <iostream>
#include <psapi.h>
#include <shellapi.h>
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
  if (d3d_device == nullptr || d3d_device->dxgi_device() == nullptr) return snapshot;

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(d3d_device->dxgi_device()->GetAdapter(&adapter))) return snapshot;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(adapter.As(&adapter3))) return snapshot;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    snapshot.vram_bytes = static_cast<std::size_t>(info.CurrentUsage);
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

void ApplicationRuntime::PauseForMinutes(unsigned int minutes) {
  const std::uint64_t bounded_minutes = std::clamp<std::uint64_t>(minutes, 1, 24 * 60);
  pause_controller_.PauseFor(bounded_minutes * 60ULL * 1000ULL, GetTickCount64());
  RefreshEffectRuntimeState();
  settings_window_.UpdatePauseState(true, false);
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

namespace {

std::optional<RECT> CalculateBulkCaptureBounds(HWND window) {
  std::optional<RECT> requested_bounds = platform::GetExtendedFrameBounds(window);
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const bool maximized =
      (GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) ||
      IsZoomed(window) != FALSE;
  if (maximized) {
    return platform::GetMonitorWorkArea(window, requested_bounds);
  }
  if (requested_bounds.has_value()) {
    RECT clipped{};
    const RECT virtual_screen = platform::GetVirtualScreenRect();
    if (IntersectRect(&clipped, &*requested_bounds, &virtual_screen) &&
        clipped.right > clipped.left && clipped.bottom > clipped.top) {
      return clipped;
    }
  }
  return std::nullopt;
}

struct PreparedBulkRun {
  int index = -1;
  HWND overlay = nullptr;
  std::size_t z_order = 0;
};

void RevealPreparedBulkOverlays(const std::vector<PreparedBulkRun>& prepared_runs) {
  bool revealed_together = false;
  HDWP positions = BeginDeferWindowPos(static_cast<int>(prepared_runs.size()));
  if (positions != nullptr) {
    HWND insert_after = HWND_TOPMOST;
    for (const PreparedBulkRun& prepared : prepared_runs) {
      positions = DeferWindowPos(
          positions, prepared.overlay, insert_after, 0, 0, 0, 0,
          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
      if (positions == nullptr) break;
      insert_after = prepared.overlay;
    }
    if (positions != nullptr) revealed_together = EndDeferWindowPos(positions) != FALSE;
  }
  if (revealed_together) return;

  HWND insert_after = HWND_TOPMOST;
  for (const PreparedBulkRun& prepared : prepared_runs) {
    SetWindowPos(prepared.overlay, insert_after, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    insert_after = prepared.overlay;
  }
}

}  // namespace

std::vector<HWND> ApplicationRuntime::CollectBulkWindowCandidates(HWND previous_in_flight) const {
  std::vector<HWND> candidates;
  const auto append_unique = [&candidates](HWND window) {
    if (window == nullptr || !IsWindow(window)) return;
    if (std::find(candidates.begin(), candidates.end(), window) != candidates.end()) return;
    candidates.push_back(window);
  };

  append_unique(previous_in_flight);
  for (const runtime::AnimationRun& run : runs_) {
    append_unique(run.animating_window);
    append_unique(run.pending_native_minimize_window);
  }
  for (const auto& [window, snapshot] : snapshot_cache_.Restore()) {
    (void)snapshot;
    append_unique(window);
  }
  for (HWND window : platform::EnumerateTopLevelWindows(GetOverlayWindow())) {
    append_unique(window);
  }
  return candidates;
}

void ApplicationRuntime::QueueBulkWindowCandidates(BulkWindowAction action, HMONITOR target_monitor,
                                                   const std::vector<HWND>& candidates) {
  const HWND settings_window = settings_window_.hwnd();
  for (HWND window : candidates) {
    if (window == settings_window || !IsWindow(window)) continue;
    if (MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != target_monitor) continue;

    // A display-level Minimize disable must stop the bulk action before it reaches the native
    // fallback path.
    if (action == BulkWindowAction::kMinimize && window_exclusion_service_.IsExcluded(window)) {
      continue;
    }

    const bool tracked = FindRunForWindow(window) != -1 ||
                         snapshot_cache_.Restore().count(window) != 0 ||
                         platform::windows::properties::HasMinimizeState(window);
    const bool eligible = action == BulkWindowAction::kMinimize
                              ? IsIconic(window) == FALSE
                              : (IsIconic(window) != FALSE || tracked);
    if (!eligible || std::find(bulk_window_queue_.begin(), bulk_window_queue_.end(), window) !=
                         bulk_window_queue_.end()) {
      continue;
    }

    if (action == BulkWindowAction::kMinimize) {
      // An explicit hotkey action supersedes the short guard for stale post-restore messages.
      minimize_suppressed_until_.erase(window);
    }
    bulk_window_queue_.push_back(window);
  }
}

void ApplicationRuntime::PrepareBulkWindowCapture(HWND window) {
  if (IsHungAppWindow(window) || window_exclusion_service_.IsExcluded(window)) return;
  const auto executable = platform::GetWindowExecutableName(window);
  if (executable.has_value() && effect_policy_.IsExcluded(*executable)) return;

  const std::optional<RECT> requested_bounds = CalculateBulkCaptureBounds(window);
  if (!requested_bounds.has_value()) return;

  rendering::CapturedTexture texture;
  RECT bounds{};
  if (desktop_capture_ == nullptr ||
      !desktop_capture_->CaptureWindow(window, *requested_bounds, &texture, &bounds) ||
      texture.shader_resource_view == nullptr) {
    return;
  }
  prepared_bulk_captures_.insert_or_assign(
      window, PreparedBulkCapture{.texture = std::move(texture), .bounds = bounds});
}

void ApplicationRuntime::PrepareBulkWindowCaptures() {
  if (bulk_window_action_ != BulkWindowAction::kMinimize || desktop_capture_ == nullptr) return;

  const auto capture_started = std::chrono::steady_clock::now();
  const std::vector<HWND> capture_candidates(bulk_window_queue_.begin(), bulk_window_queue_.end());
  for (HWND window : capture_candidates) {
    PrepareBulkWindowCapture(window);
  }
  const float capture_duration =
      std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - capture_started)
          .count();
  core::LogDebug(L"Hotkey", L"Prepared " + std::to_wstring(prepared_bulk_captures_.size()) +
                                L" bulk captures in " + std::to_wstring(capture_duration) + L" ms");
}

void ApplicationRuntime::StartBulkWindowAction(BulkWindowAction action) {
  if (bulk_hotkey_locked_) {
    core::LogDebug(L"Hotkey", L"Ignored bulk hotkey while the previous animation is still active");
    return;
  }

  POINT cursor{};
  if (!GetCursorPos(&cursor)) {
    core::LogDebug(L"Hotkey", L"Could not determine the cursor display for the bulk action");
    return;
  }
  const HMONITOR target_monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  if (target_monitor == nullptr) {
    core::LogDebug(L"Hotkey", L"Could not resolve the cursor display for the bulk action");
    return;
  }

  const HWND previous_in_flight = bulk_window_in_flight_;
  bulk_window_queue_.clear();
  prepared_bulk_captures_.clear();
  bulk_window_in_flight_ = nullptr;
  bulk_window_request_started_ms_ = 0;

  if (action == BulkWindowAction::kRestore && previous_in_flight != nullptr &&
      FindRunForWindow(previous_in_flight) == -1 && IsIconic(previous_in_flight) == FALSE) {
    // A posted minimize request may not have reached the hook yet. Block that delayed request
    // instead of allowing it to run after the restore queue has already moved on.
    minimize_suppressed_until_[previous_in_flight] = GetTickCount64() + 1500;
  }

  const std::vector<HWND> candidates = CollectBulkWindowCandidates(previous_in_flight);
  QueueBulkWindowCandidates(action, target_monitor, candidates);
  if (bulk_window_queue_.empty()) {
    core::LogDebug(L"Hotkey", L"Bulk hotkey found no eligible windows on the cursor display");
    return;
  }

  bulk_hotkey_locked_ = true;
  bulk_window_action_ = action;
  bulk_window_request_started_ms_ = GetTickCount64();

  // Allocate every overlay before touching any real window. This removes first-use setup gaps
  // and guarantees that a large batch cannot begin until it has one run per candidate.
  if (!EnsureAnimationRunCapacity(bulk_window_queue_.size())) {
    core::LogDebug(L"Hotkey",
                   L"Could not pre-initialize every bulk animation run; unavailable windows will "
                   L"use the native fallback");
  }

  PrepareBulkWindowCaptures();

  core::LogDebug(L"Hotkey",
                 std::wstring(action == BulkWindowAction::kMinimize ? L"Queued minimize for "
                                                                    : L"Queued restore for ") +
                     std::to_wstring(bulk_window_queue_.size()) +
                     L" top-level windows on the cursor display");
  frame_scheduler_.Wake();
}

void ApplicationRuntime::CommitPreparedBulkMinimizeRuns() {
  std::vector<PreparedBulkRun> prepared_runs;
  const std::vector<HWND> z_order = platform::EnumerateTopLevelWindows(GetOverlayWindow());
  for (int index = 0; index < static_cast<int>(runs_.size()); ++index) {
    runtime::AnimationRun& run = runs_[index];
    if (!run.bulk_animation || run.state != runtime::RunState::kCapturing ||
        !run.overlay.active() || run.animating_window == nullptr) {
      continue;
    }
    const auto position = std::find(z_order.begin(), z_order.end(), run.animating_window);
    prepared_runs.push_back(PreparedBulkRun{
        .index = index,
        .overlay = run.overlay.window(),
        .z_order = position == z_order.end()
                       ? z_order.size()
                       : static_cast<std::size_t>(std::distance(z_order.begin(), position)),
    });
  }

  if (prepared_runs.empty()) return;
  std::stable_sort(prepared_runs.begin(), prepared_runs.end(),
                   [](const PreparedBulkRun& left, const PreparedBulkRun& right) {
                     return left.z_order < right.z_order;
                   });

  // The real windows are still visible and unchanged here. Reveal every captured clone in
  // the original desktop Z-order as one transaction, then make/minimize the originals behind
  // those clones. The user therefore never observes an intermediate promoted or missing window.
  RevealPreparedBulkOverlays(prepared_runs);

  for (const PreparedBulkRun& prepared : prepared_runs) {
    (void)minimize_feature_.CommitPreparedBulkMinimize(
        prepared.index, &native_animation_blocker_,
        [this](int index, runtime::RunState state) { SetRunState(index, state); },
        [this](int index) { CleanupRun(index, RunCleanupOutcome::kAborted); });
  }

  // Commit overlay visibility and original-window transparency in the same compositor frame,
  // then release every animation clock without waiting for sequential native minimize events.
  DwmFlush();
  for (const PreparedBulkRun& prepared : prepared_runs) {
    runtime::AnimationRun& run = runs_[prepared.index];
    if (!run.bulk_animation || !run.overlay.active() || run.overlay.clock_started()) continue;
    run.overlay.StartAnimationClock();
    SetRunState(prepared.index, runtime::RunState::kAnimating);
  }
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
    CommitPreparedBulkMinimizeRuns();
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
      .elevated = platform::IsCurrentProcessElevated(),
      .reference_window = effect_controller_.last_foreground_window(),
      .taskbar_targets = &taskbar_target_provider_,
  });
#ifdef _DEBUG
  snapshot.stress_test = stress_test_report_;
#endif
  return snapshot;
}

bool ApplicationRuntime::RestartElevated() {
  if (platform::IsCurrentProcessElevated()) return true;
  settings_window_.Show(false);
  std::wstring executable(MAX_PATH, L'\0');
  DWORD length =
      GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  while (length == executable.size()) {
    executable.resize(executable.size() * 2);
    length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  }
  if (length == 0) {
    settings_window_.Show(true);
    return false;
  }
  executable.resize(length);
  const std::wstring parameters = L"--elevated-resume " + std::to_wstring(GetCurrentProcessId());
  const INT_PTR result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(settings_window_.hwnd(), L"runas", executable.c_str(), parameters.c_str(),
                    platform::ExecutableDirectory().c_str(), SW_SHOWNORMAL));
  if (result <= 32) {
    settings_window_.Show(true);
    return false;
  }
  RequestShutdown();
  return true;
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
                  .restart_elevated = [this] { return RestartElevated(); },
#ifdef _DEBUG
                  .run_stress_test = [this] { return RunStressTest(); },
#endif
              });
}

#ifdef _DEBUG
namespace {

constexpr wchar_t kStressTestWindowClass[] = L"MinimizeEffectStressTestWindow";

LRESULT CALLBACK StressTestWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);

      // Clean white background (#FFFFFF)
      HBRUSH bg_brush = CreateSolidBrush(RGB(255, 255, 255));
      FillRect(dc, &client, bg_brush);
      DeleteObject(bg_brush);

      // Soft WinUI light header bar (#F3F3F3)
      RECT header_rect = {client.left, client.top, client.right, client.top + 48};
      HBRUSH header_brush = CreateSolidBrush(RGB(243, 243, 243));
      FillRect(dc, &header_rect, header_brush);
      DeleteObject(header_brush);

      // Header divider line (#E5E5E5)
      RECT divider = {client.left, client.top + 47, client.right, client.top + 48};
      HBRUSH divider_brush = CreateSolidBrush(RGB(229, 229, 229));
      FillRect(dc, &divider, divider_brush);
      DeleteObject(divider_brush);

      SetBkMode(dc, TRANSPARENT);

      HFONT header_font = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HFONT old_font = static_cast<HFONT>(SelectObject(dc, header_font));

      SetTextColor(dc, RGB(27, 27, 27));
      wchar_t title[256]{};
      GetWindowTextW(window, title, 256);
      RECT header_text_rect = {client.left + 20, client.top + 10, client.right - 20,
                               client.top + 38};
      DrawTextW(dc, title, -1, &header_text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

      SelectObject(dc, old_font);
      DeleteObject(header_font);

      HFONT body_font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      SelectObject(dc, body_font);
      SetTextColor(dc, RGB(90, 90, 90));

      const int width = client.right - client.left;
      const int height = client.bottom - client.top;
      std::wstring text = std::format(
          L"WinUI 3 Test Canvas\n\nDimensions: {} \u00d7 {} px\nStatus: Active Stress Test Target",
          width, height);
      RECT body_rect = {client.left + 24, client.top + 70, client.right - 24, client.bottom - 24};
      DrawTextW(dc, text.c_str(), -1, &body_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);

      SelectObject(dc, old_font);
      DeleteObject(body_font);

      EndPaint(window, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

void RegisterStressTestWindowClass() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = StressTestWindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kStressTestWindowClass;
  RegisterClassExW(&wc);
}

}  // namespace

void ApplicationRuntime::DestroyStressTestWindows() {
  for (HWND window : stress_test_windows_) {
    if (window != nullptr && IsWindow(window)) {
      snapshot_cache_.Restore().erase(window);
      snapshot_cache_.PreMinimize().erase(window);
      DestroyWindow(window);
    }
  }
  stress_test_windows_.clear();
  stress_test_expected_iconic_.clear();
}

void ApplicationRuntime::SetStressTestWindowState(HWND window, bool minimized) {
  if (!IsWindow(window)) return;
  if (minimized) {
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    // This is a new explicit test command, not the delayed duplicate callback which the
    // post-restore guard is designed to absorb.
    minimize_suppressed_until_.erase(window);
    // Stress windows belong to this process, so WINEVENT_SKIPOWNPROCESS deliberately excludes
    // them. Drive the exact production feature path directly; it captures, animates and issues
    // the native minimize itself instead of testing only a raw ShowWindow call.
    if (!OnMinimizeStart(window)) ShowWindow(window, SW_MINIMIZE);
  } else if (!OnRestoreAttempt(window)) {
    ShowWindow(window, SW_RESTORE);
  }
}

void ApplicationRuntime::AbortActiveAnimationRuns() {
  for (int index = 0; index < static_cast<int>(runs_.size()); ++index) {
    if (runs_[index].overlay.active() || runs_[index].animating_window != nullptr) {
      CleanupRun(index, RunCleanupOutcome::kAborted);
    }
  }
}

void ApplicationRuntime::RestoreStressTestWindowsForFinalization() {
  for (HWND window : stress_test_windows_) {
    if (!IsWindow(window)) continue;
    ShowWindow(window, SW_RESTORE);
    stress_test_expected_iconic_[window] = false;
    ++stress_test_report_.actions_requested;
  }
}

bool ApplicationRuntime::RunStressTest() {
  if (stress_test_report_.active) return false;

  DestroyStressTestWindows();
  RegisterStressTestWindowClass();

  HMONITOR monitor = MonitorFromWindow(settings_window_.hwnd(), MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    info.rcWork = RECT{100, 100, 1800, 1000};
  }
  const RECT& work = info.rcWork;
  const int work_w = std::max(800, static_cast<int>(work.right - work.left));
  const int work_h = std::max(600, static_cast<int>(work.bottom - work.top));

  struct WindowSpec {
    std::wstring title;
    int width;
    int height;
    float rel_x;
    float rel_y;
  };

  const WindowSpec specs[] = {
      {L"WinUI Test Window #1", 640, 420, 0.04f, 0.06f},
      {L"WinUI Test Window #2", 820, 520, 0.30f, 0.08f},
      {L"WinUI Test Window #3", 540, 360, 0.08f, 0.46f},
      {L"WinUI Test Window #4", 760, 460, 0.40f, 0.34f},
      {L"WinUI Test Window #5", 680, 480, 0.18f, 0.22f},
      {L"WinUI Test Window #6", 580, 400, 0.52f, 0.16f},
  };

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  for (std::size_t i = 0; i < std::size(specs); ++i) {
    const auto& spec = specs[i];
    const int x = work.left + static_cast<int>(work_w * spec.rel_x);
    const int y = work.top + static_cast<int>(work_h * spec.rel_y);
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kStressTestWindowClass, spec.title.c_str(),
                                  WS_OVERLAPPEDWINDOW, x, y, spec.width, spec.height, nullptr,
                                  nullptr, instance, nullptr);
    if (window != nullptr) {
      ShowWindow(window, SW_SHOWNORMAL);
      UpdateWindow(window);
      stress_test_windows_.push_back(window);
    }
  }

  if (stress_test_windows_.empty()) {
    core::LogDebug(L"Diagnostics", L"Failed to create custom stress test windows.");
    return false;
  }

  // Establish the leak baseline after first-use capture allocations. D3D/DXGI commonly retain
  // their initial texture heaps for reuse; counting that one-time warm-up as a leak produced the
  // repeatable +40..+60 MB false failure even though process RAM stayed flat.
  if (desktop_capture_ != nullptr) {
    for (HWND window : stress_test_windows_) {
      const auto bounds = platform::GetExtendedFrameBounds(window);
      if (!bounds.has_value()) continue;
      rendering::CapturedTexture warmed_texture;
      RECT warmed_bounds{};
      (void)desktop_capture_->CaptureWindow(window, *bounds, &warmed_texture, &warmed_bounds);
    }
    DwmFlush();
  }
  const auto mem = GetMemoryUsageSnapshot(d3d_device_.get());
  stress_test_report_ = features::StressTestReport{};
  stress_test_report_.active = true;
  stress_test_report_.target_cycles = 60;
  stress_test_report_.current_cycle = 0;
  stress_test_report_.current_step = 0;
  stress_test_report_.start_vram_bytes = mem.vram_bytes;
  stress_test_report_.current_vram_bytes = mem.vram_bytes;
  stress_test_report_.peak_vram_bytes = mem.vram_bytes;
  stress_test_report_.start_ram_bytes = mem.ram_bytes;
  stress_test_report_.current_ram_bytes = mem.ram_bytes;
  stress_test_report_.deadlocks_detected = 0;
  stress_test_report_.last_log =
      std::format("Automated Stress Test initiated: Created {} custom WinUI test windows...",
                  stress_test_windows_.size());
  stress_test_report_.summary =
      "Running 60-cycle suite: resize, burst, all-window, cross-phase and focus patterns.";

  stress_test_last_step_ms_ = GetTickCount64();
  stress_test_step_started_ms_ = 0;
  stress_test_settle_started_ms_ = 0;
  stress_test_expected_iconic_.clear();
  core::LogDebug(L"Diagnostics", L"60-cycle stress suite started on custom test windows.");
  return true;
}

void ApplicationRuntime::UpdateStressTest() {
  if (!stress_test_report_.active) return;

  const ULONGLONG now = GetTickCount64();
  const auto finding = [this](std::string text) {
    if (stress_test_report_.findings.size() < 48)
      stress_test_report_.findings.push_back(std::move(text));
  };

  // A posted minimize/restore is not a pass. Once animations settle, verify every requested
  // HWND reached the target state and that restored windows no longer carry Minimize state.
  if (!stress_test_expected_iconic_.empty() && !HasActiveAnimationRuns() &&
      bulk_window_action_ == BulkWindowAction::kNone) {
    if (now - stress_test_settle_started_ms_ < 350) return;
    for (const auto& [window, expected_iconic] : stress_test_expected_iconic_) {
      ++stress_test_report_.windows_processed;
      if (!IsWindow(window)) {
        ++stress_test_report_.invalid_windows;
        ++stress_test_report_.assertions_failed;
        finding("FAIL: test window was destroyed before it could be verified");
        continue;
      }
      const bool iconic = IsIconic(window) != FALSE;
      const bool stale_state =
          !expected_iconic && platform::windows::properties::HasMinimizeState(window);
      if (iconic != expected_iconic || stale_state) {
        ++stress_test_report_.state_mismatches;
        ++stress_test_report_.assertions_failed;
        finding(std::format(
            "FAIL: HWND {} expected {}, got {}{}", reinterpret_cast<std::uintptr_t>(window),
            expected_iconic ? "minimized" : "restored", iconic ? "minimized" : "restored",
            stale_state ? " with leaked Minimize state" : ""));
      } else {
        ++stress_test_report_.assertions_passed;
      }
    }
    stress_test_expected_iconic_.clear();
    stress_test_settle_started_ms_ = 0;
    stress_test_step_started_ms_ = 0;
    // Start the inter-phase delay when the previous animations actually completed. Measuring
    // from the request time launched the next minimize inside the 150 ms restore-suppression
    // window and caused the exact repeated "expected minimized, got restored" failures.
    stress_test_last_step_ms_ = now;
  }

  // Watchdog: detect stuck run (> 4000ms without finishing)
  if (bulk_window_action_ != BulkWindowAction::kNone && bulk_window_request_started_ms_ > 0 &&
      (now - bulk_window_request_started_ms_) > 4000) {
    stress_test_report_.deadlocks_detected++;
    core::LogDebug(L"Diagnostics", L"Stress test detected potential deadlock; recovering");
    bulk_window_action_ = BulkWindowAction::kNone;
    bulk_hotkey_locked_ = false;
    HealLeftoverWindows();
  }

  // Wait for previous bulk action or overlay animations to finish
  if (bulk_window_action_ != BulkWindowAction::kNone || HasActiveAnimationRuns()) {
    if (stress_test_step_started_ms_ != 0 && now - stress_test_step_started_ms_ > 7000) {
      ++stress_test_report_.animation_timeouts;
      ++stress_test_report_.assertions_failed;
      finding("FAIL: animation did not settle within 7 seconds");
      AbortActiveAnimationRuns();
      bulk_window_action_ = BulkWindowAction::kNone;
      bulk_window_queue_.clear();
      prepared_bulk_captures_.clear();
      bulk_window_in_flight_ = nullptr;
      bulk_window_request_started_ms_ = 0;
      HealLeftoverWindows();
      stress_test_step_started_ms_ = 0;
      core::LogDebug(L"Diagnostics", L"Stress test animation timeout detected.");
    }
    return;
  }

  // Leave a margin above the 150 ms delayed-minimize suppression window.
  if (now - stress_test_last_step_ms_ < 250) {
    return;
  }
  stress_test_last_step_ms_ = now;

  const int total_steps = stress_test_report_.target_cycles * 2;
  if (stress_test_report_.current_step >= total_steps) {
    if (!stress_test_report_.finalizing) {
      stress_test_report_.finalizing = true;
      RestoreStressTestWindowsForFinalization();
      stress_test_settle_started_ms_ = now;
      stress_test_report_.last_log =
          "Final cleanup: restoring and verifying every stress window...";
      return;
    }
    // Stress test completed! Clean up custom test windows
    stress_test_report_.active = false;
    DestroyStressTestWindows();
    DwmFlush();
    const auto mem = GetMemoryUsageSnapshot(d3d_device_.get());
    stress_test_report_.current_vram_bytes = mem.vram_bytes;
    stress_test_report_.current_ram_bytes = mem.ram_bytes;

    const double vram_leak_mb =
        static_cast<double>(static_cast<long long>(mem.vram_bytes) -
                            static_cast<long long>(stress_test_report_.start_vram_bytes)) /
        (1024.0 * 1024.0);
    const double peak_vram_mb =
        static_cast<double>(stress_test_report_.peak_vram_bytes) / (1024.0 * 1024.0);

    const double ram_delta_mb =
        static_cast<double>(static_cast<long long>(mem.ram_bytes) -
                            static_cast<long long>(stress_test_report_.start_ram_bytes)) /
        (1024.0 * 1024.0);
    if (vram_leak_mb > 8.0) {
      ++stress_test_report_.assertions_failed;
      finding(std::format("FAIL: VRAM grew by {:+.2f} MB (limit: +8.00 MB)", vram_leak_mb));
    } else {
      ++stress_test_report_.assertions_passed;
    }
    if (ram_delta_mb > 64.0) {
      ++stress_test_report_.assertions_failed;
      finding(std::format("FAIL: RAM grew by {:+.2f} MB (limit: +64.00 MB)", ram_delta_mb));
    } else {
      ++stress_test_report_.assertions_passed;
    }
    const std::string status = stress_test_report_.assertions_failed == 0 ? "PASSED" : "FAILED";

    stress_test_report_.summary = std::format(
        "STRESS TEST COMPLETE (60/60 cycles)\n"
        "Result: {}\n"
        "Actions requested: {} | windows verified: {}\n"
        "Assertions: {} passed, {} failed\n"
        "State mismatches: {} | animation timeouts: {} | invalid windows: {}\n"
        "Net VRAM: {:+.2f} MB | Net RAM: {:+.2f} MB\n"
        "Peak VRAM Usage: {:.2f} MB\n"
        "Deadlocks Detected: {}",
        status, stress_test_report_.actions_requested, stress_test_report_.windows_processed,
        stress_test_report_.assertions_passed, stress_test_report_.assertions_failed,
        stress_test_report_.state_mismatches, stress_test_report_.animation_timeouts,
        stress_test_report_.invalid_windows, vram_leak_mb, ram_delta_mb, peak_vram_mb,
        stress_test_report_.deadlocks_detected);

    stress_test_report_.last_log = std::format("[Finished] {}", status);
    core::LogDebug(L"Diagnostics", L"Automated Stress Test completed successfully.");
    return;
  }

  if (stress_test_windows_.empty()) {
    stress_test_report_.active = false;
    return;
  }

  const int step = stress_test_report_.current_step;
  stress_test_report_.current_cycle = (step / 2) + 1;
  const bool minimize_phase = (step % 2 == 0);
  const std::size_t num_windows = stress_test_windows_.size();
  const int cycle = step / 2;

  HMONITOR monitor = MonitorFromWindow(settings_window_.hwnd(), MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    info.rcWork = RECT{100, 100, 1800, 1000};
  }
  const RECT& work = info.rcWork;
  const int work_w = std::max(600, static_cast<int>(work.right - work.left));
  const int work_h = std::max(400, static_cast<int>(work.bottom - work.top));

  std::string pattern_description;

  const int mode = cycle % 5;
  if (!minimize_phase) {
    // Each cycle ends with a full recovery sweep. This makes the next minimize pattern start
    // from a known state instead of treating a delayed restore as a failed minimize command.
    for (HWND hwnd : stress_test_windows_) SetStressTestWindowState(hwnd, false);
    pattern_description = "Full Recovery Sweep (all stress windows)";
  } else if (mode == 0) {
    // Mode 0: Position & Size Shuffle + Action on 2 windows
    pattern_description = "Position/Size Shuffle & ";
    for (std::size_t i = 0; i < 2; ++i) {
      const std::size_t idx = (cycle + i) % num_windows;
      HWND hwnd = stress_test_windows_[idx];
      if (hwnd != nullptr && IsWindow(hwnd)) {
        const int seed = cycle * 17 + static_cast<int>(i) * 31;
        const int new_w = 480 + ((seed * 73 + 17) % 360);
        const int new_h = 340 + ((seed * 37 + 23) % 260);
        const int new_x = work.left + ((seed * 113 + 41) % std::max(100, work_w - new_w));
        const int new_y = work.top + ((seed * 97 + 59) % std::max(100, work_h - new_h));
        SetWindowPos(hwnd, nullptr, new_x, new_y, new_w, new_h, SWP_NOZORDER | SWP_NOACTIVATE);

        SetStressTestWindowState(hwnd, minimize_phase);
      }
    }
    pattern_description += minimize_phase ? "Minimize 2 Windows" : "Restore 2 Windows";
  } else if (mode == 1) {
    // Mode 1: Simultaneous Multi-Window Burst (3 windows at once)
    const std::size_t start_idx = (cycle * 2) % num_windows;
    for (std::size_t i = 0; i < 3 && i < num_windows; ++i) {
      const std::size_t idx = (start_idx + i) % num_windows;
      HWND hwnd = stress_test_windows_[idx];
      if (hwnd != nullptr && IsWindow(hwnd)) {
        SetStressTestWindowState(hwnd, minimize_phase);
      }
    }
    pattern_description =
        std::format("Simultaneous Burst {} (3 Windows)", minimize_phase ? "Minimize" : "Restore");
  } else if (mode == 2) {
    // Mode 2: ALL Windows Simultaneously (Bulk)
    for (HWND hwnd : stress_test_windows_) {
      if (hwnd != nullptr && IsWindow(hwnd)) {
        SetStressTestWindowState(hwnd, minimize_phase);
      }
    }
    pattern_description =
        std::format("ALL Windows {} Simultaneously", minimize_phase ? "Minimize" : "Restore");
  } else if (mode == 3) {
    // Mode 3: Mixed Cross-Phase (Restore half while minimizing half)
    for (std::size_t i = 0; i < num_windows; ++i) {
      HWND hwnd = stress_test_windows_[i];
      if (hwnd != nullptr && IsWindow(hwnd)) {
        SetStressTestWindowState(hwnd, i % 2 == static_cast<std::size_t>(step % 2));
      }
    }
    pattern_description = "Cross-Phase Mixed Multi-Window Swap";
  } else {
    // Mode 4: Single Window Rapid Focus
    const std::size_t target_idx = cycle % num_windows;
    HWND hwnd = stress_test_windows_[target_idx];
    if (hwnd != nullptr && IsWindow(hwnd)) {
      SetStressTestWindowState(hwnd, minimize_phase);
      if (!minimize_phase) SetForegroundWindow(hwnd);
    }
    pattern_description = std::format("Single Window #{} Focus {}", target_idx + 1,
                                      minimize_phase ? "Minimize" : "Restore");
  }

  const auto expect = [this](HWND window, bool iconic) {
    if (window != nullptr && IsWindow(window)) {
      stress_test_expected_iconic_[window] = iconic;
      ++stress_test_report_.actions_requested;
    }
  };
  if (!minimize_phase) {
    for (HWND window : stress_test_windows_) expect(window, false);
  } else if (mode == 0) {
    for (std::size_t i = 0; i < 2; ++i)
      expect(stress_test_windows_[(cycle + i) % num_windows], minimize_phase);
  } else if (mode == 1) {
    const std::size_t start = (cycle * 2) % num_windows;
    for (std::size_t i = 0; i < 3 && i < num_windows; ++i)
      expect(stress_test_windows_[(start + i) % num_windows], minimize_phase);
  } else if (mode == 2) {
    for (HWND window : stress_test_windows_) expect(window, minimize_phase);
  } else if (mode == 3) {
    for (std::size_t i = 0; i < num_windows; ++i)
      expect(stress_test_windows_[i], i % 2 == static_cast<std::size_t>(step % 2));
  } else {
    expect(stress_test_windows_[cycle % num_windows], minimize_phase);
  }

  stress_test_report_.current_step++;
  stress_test_step_started_ms_ = now;
  stress_test_settle_started_ms_ = now;

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
      "[Cycle {}/60] {} | {} state assertions queued | VRAM: {:.2f} MB (Delta: {:+.2f} MB)",
      stress_test_report_.current_cycle, pattern_description, stress_test_expected_iconic_.size(),
      vram_mb, vram_delta_mb);
}
#endif

bool ApplicationRuntime::SetAnimationDurations(float minimize_duration, float restore_duration,
                                               float cancel_duration, bool save) {
  const bool result = settings_mutations_.SetAnimationDurations(minimize_duration, restore_duration,
                                                                cancel_duration, save);
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

bool ApplicationRuntime::SaveMotionProfile(const std::string& name) {
  const bool result = settings_mutations_.SaveMotionProfile(name);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::ApplyMotionProfile(const std::string& name) {
  const bool result = settings_mutations_.ApplyMotionProfile(
      name, [this] { effect_policy_.Configure(settings_service_.Get()); });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::DeleteMotionProfile(const std::string& name) {
  const bool result = settings_mutations_.DeleteMotionProfile(name);
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

bool ApplicationRuntime::CanUndoSettings() const { return settings_mutations_.CanUndo(); }

void ApplicationRuntime::ApplyRestoredSettings() {
  effect_policy_.Configure(settings_service_.Get());
  window_exclusion_service_.SetExcludedDisplays(settings_service_.Get().excluded_displays);
  effect_controller_.ApplyExclusionTransitionOverrides(GetOverlayWindow());
  RegisterConfiguredHotkeys();
  RefreshEffectRuntimeState();
}

bool ApplicationRuntime::UndoSettings() {
  const bool result = settings_mutations_.Undo([this] { ApplyRestoredSettings(); });
  settings_window_.UpdateState(settings_service_.Get());
  return result;
}

ui::SettingsFileOperationResult ApplicationRuntime::RestoreSettingsBackup() {
  bool startup_registration_failed = false;
  const bool restored = settings_mutations_.RestoreBackup([this] { ApplyRestoredSettings(); },
                                                          &startup_registration_failed);
  settings_window_.UpdateState(settings_service_.Get());
  if (!restored) {
    return ui::SettingsFileOperationResult{
        .result = ui::SettingsFileResult::kFailed,
        .message = "No valid settings backup found",
        .is_error = true,
    };
  }
  return ui::SettingsFileOperationResult{
      .result = ui::SettingsFileResult::kSuccess,
      .message = startup_registration_failed ? "Backup restored, startup registration failed"
                                             : "Settings backup restored",
      .is_error = startup_registration_failed,
  };
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
  return open_windows_service_.Capture(GetOverlayWindow(), settings_window_.hwnd(),
                                       effect_controller_.last_foreground_window());
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
      picker.path, [this] { ApplyRestoredSettings(); }, &startup_registration_failed);
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

bool ApplicationRuntime::SaveUiWindowState(const settings::UiWindowState& state) {
  return settings_mutations_.SaveUiWindowState(state);
}

}  // namespace minimize::app
