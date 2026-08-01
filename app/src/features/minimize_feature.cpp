#include "pch.hpp"

#include "features/minimize_feature.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>
#include <vector>

#include "animation/geometry.hpp"
#include "core/logger.hpp"
#include "features/animation_configuration.hpp"
#include "features/effect_policy.hpp"
#include "features/window_exclusion_service.hpp"
#include "features/window_recovery_service.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/native_animation_blocker.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/taskbar_target_provider.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "rendering/desktop_capture.hpp"
#include "runtime/animation_run_pool.hpp"
#include "runtime/snapshot_cache.hpp"

namespace minimize::features {
namespace {

animation::RectF ToRectF(const RECT& rect) {
  return animation::RectF{
      .left = static_cast<float>(rect.left),
      .top = static_cast<float>(rect.top),
      .right = static_cast<float>(rect.right),
      .bottom = static_cast<float>(rect.bottom),
  };
}

bool IsUsableRect(const RECT& rect) {
  return rect.right > rect.left && rect.bottom > rect.top && rect.left > -30000 &&
         rect.top > -30000;
}

bool IsCurrentlyMaximized(HWND window) {
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  return (GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED) ||
         IsZoomed(window) != FALSE;
}

std::optional<RECT> ResolveAnimationBounds(HWND window) {
  const std::optional<RECT> bounds = platform::GetExtendedFrameBounds(window);
  if (IsCurrentlyMaximized(window)) {
    const auto work_area = platform::GetMonitorWorkArea(window, bounds);
    if (work_area.has_value()) return work_area;
  }
  if (!bounds.has_value()) return std::nullopt;
  RECT clipped{};
  const RECT virtual_screen = platform::GetVirtualScreenRect();
  if (!IntersectRect(&clipped, &*bounds, &virtual_screen) || clipped.right <= clipped.left ||
      clipped.bottom <= clipped.top) {
    return std::nullopt;
  }
  return clipped;
}

}  // namespace

MinimizeFeature::Transaction::Transaction(MinimizeFeature& owner, HWND window)
    : owner_(&owner), window_(window) {}

MinimizeFeature::Transaction::~Transaction() {
  if (owner_ != nullptr) owner_->Cancel(window_);
}

MinimizeFeature::Transaction::Transaction(Transaction&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      window_(std::exchange(other.window_, nullptr)) {}

MinimizeFeature::Transaction& MinimizeFeature::Transaction::operator=(
    Transaction&& other) noexcept {
  if (this == &other) return *this;
  if (owner_ != nullptr) owner_->Cancel(window_);
  owner_ = std::exchange(other.owner_, nullptr);
  window_ = std::exchange(other.window_, nullptr);
  return *this;
}

void MinimizeFeature::Transaction::HandOff() {
  if (owner_ == nullptr) return;
  owner_->HandOff(window_);
  owner_ = nullptr;
  window_ = nullptr;
}

MinimizeFeature::MinimizeFeature(EffectPolicy& policy, WindowRecoveryService& recovery,
                                 runtime::AnimationRunPool& runs, runtime::SnapshotCache& snapshots,
                                 WindowExclusionService& window_exclusions)
    : policy_(policy),
      recovery_(recovery),
      runs_(runs),
      snapshots_(snapshots),
      window_exclusions_(window_exclusions) {}

bool MinimizeFeature::Execute(HWND window, const MinimizeExecutionContext& context) {
  if (!context.effect_active || context.renderer_recovering || context.shutting_down ||
      context.capture == nullptr || context.overlay == nullptr ||
      context.taskbar_targets == nullptr || context.animation_blocker == nullptr ||
      context.animation_configuration == nullptr || context.rendering_pressure == nullptr ||
      window == nullptr || !IsWindow(window) || window == context.overlay ||
      recovery_.restoring() || !platform::IsInterestingTopLevelWindow(window, context.overlay)) {
    return false;
  }
  // Per-window session exclusion (before any capture / cloak).
  if (window_exclusions_.IsExcluded(window)) {
    core::LogDebug(L"Minimize", L"Skipping Minimize: per-window exclusion");
    platform::SetDwmTransitionsDisabled(window, false);
    return false;
  }
  const auto executable = platform::GetWindowExecutableName(window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) {
    platform::SetDwmTransitionsDisabled(window, false);
    return false;
  }
  const ULONGLONG now_ms = GetTickCount64();
  if (!context.force_animation &&
      policy_.ShouldSkipAnimationForLoad(*context.rendering_pressure, now_ms)) {
    core::LogDebug(L"Minimize", L"Smart-skip: native minimize under load");
    platform::SetDwmTransitionsDisabled(window, false);
    return false;
  }

  int run_index = context.find_run(window);
  if (run_index != -1) {
    runtime::AnimationRun& run = runs_[run_index];
    if (run.pending_native_minimize_window == window ||
        platform::windows::properties::HasFlag(
            window, platform::windows::properties::WindowFlag::kAllowMinimize)) {
      return true;
    }
    if (context.complete_restore) context.complete_restore(window);
    run.animating_restore = false;
    if (!run.auto_hide_taskbar_revealed) {
      run.auto_hide_taskbar_revealed =
          context.taskbar_targets->RevealAutoHideTaskbarForWindow(run.live_animation_bounds);
    }
    run.overlay.ContinueMinimizeAnimation();
    run.direction_started_ms = GetTickCount64();
    context.set_state(run_index, runtime::RunState::kAnimating);
    run.live_animation_capture_enabled = false;
    return true;
  }

  run_index = context.find_available_run();
  if (run_index == -1) return false;
  runtime::AnimationRun& run = runs_[run_index];
  const bool minimizing = platform::windows::properties::HasFlag(
      window, platform::windows::properties::WindowFlag::kIsMinimizing);
  const auto restore_snapshot = snapshots_.Restore().find(window);
  if (minimizing) {
    return true;
  }
  if (restore_snapshot != snapshots_.Restore().end()) {
    const bool still_minimized = IsIconic(window) != FALSE ||
        platform::windows::properties::HasFlag(
            window, platform::windows::properties::WindowFlag::kMovedOffscreen);
    if (still_minimized) return true;

    // A visible window without Minimize flags is not in a live restore. Keeping its old capture
    // makes the next minimize look already handled and is exactly what left stress targets—and
    // normal windows after a raced restore—stuck visible.
    snapshots_.Restore().erase(restore_snapshot);
  }
  context.set_state(run_index, runtime::RunState::kCapturing);

  struct TopmostRestorer {
    HWND window = nullptr;
    bool was_topmost = false;
    bool activated = false;
    bool restored = false;
    void Activate() {
      if (activated || window == nullptr || !IsWindow(window)) return;
      was_topmost = platform::BringWindowToCaptureForeground(window);
      activated = true;
    }
    void RestoreNow() {
      if (restored) return;
      restored = true;
      if (activated && window != nullptr && IsWindow(window) && !was_topmost) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      }
    }
    ~TopmostRestorer() { RestoreNow(); }
  } topmost{.window = window};
  if (!context.force_animation) topmost.Activate();

  const std::optional<RECT> animation_bounds = ResolveAnimationBounds(window);
  if (!animation_bounds.has_value()) {
    context.set_state(run_index, runtime::RunState::kIdle);
    return false;
  }

  WINDOWPLACEMENT original_placement{};
  original_placement.length = sizeof(original_placement);
  RECT original_rect = GetWindowPlacement(window, &original_placement)
                           ? original_placement.rcNormalPosition
                           : *animation_bounds;
  if (!IsUsableRect(original_rect)) original_rect = *animation_bounds;
  const bool was_maximized = IsCurrentlyMaximized(window);

  rendering::CapturedTexture captured_texture;
  RECT source_bounds = *animation_bounds;
  const bool already_minimized = IsIconic(window) != FALSE;
  RECT captured_window_bounds{};
  snapshots_.Prune();
  auto pre_minimize = snapshots_.PreMinimize().find(window);
  const bool has_cached = pre_minimize != snapshots_.PreMinimize().end() &&
                          pre_minimize->second.texture.shader_resource_view != nullptr;
  const bool prefer_window_capture = rendering::QueryWindowVisualMetadata(window).is_layered;
  const auto capture_started = std::chrono::steady_clock::now();
  if (already_minimized && has_cached) {
    source_bounds = pre_minimize->second.bounds;
    captured_texture = pre_minimize->second.texture;
  } else if (!already_minimized && context.force_animation &&
             context.take_prepared_capture &&
             context.take_prepared_capture(window, &captured_texture, &captured_window_bounds)) {
    source_bounds = captured_window_bounds;
  } else if (!already_minimized && context.force_animation && !IsHungAppWindow(window) &&
             context.capture->CaptureWindow(window, *animation_bounds, &captured_texture,
                                            &captured_window_bounds)) {
    // Bulk capture does not need to focus or reorder the target. This avoids one DWM flush and
    // one fresh desktop-duplication frame per window while preserving the native-resolution image.
    source_bounds = captured_window_bounds;
  } else if (!already_minimized && !context.force_animation && prefer_window_capture &&
             context.capture->CaptureWindow(window, *animation_bounds, &captured_texture,
                                            &captured_window_bounds)) {
    source_bounds = captured_window_bounds;
  } else if (!already_minimized) {
    // Bulk hotkeys must never reorder every real window as capture fallbacks run.
    if (!context.force_animation) topmost.Activate();
    context.capture->ClearHistory();
    bool captured = context.capture->CaptureRegion(window, *animation_bounds, &captured_texture);
    if (!captured && !prefer_window_capture) {
      captured = context.capture->CaptureWindow(window, *animation_bounds, &captured_texture,
                                                &captured_window_bounds);
      if (captured) source_bounds = captured_window_bounds;
    }
    if (!captured && has_cached) {
      source_bounds = pre_minimize->second.bounds;
      captured_texture = pre_minimize->second.texture;
    }
  } else if (has_cached) {
    source_bounds = pre_minimize->second.bounds;
    captured_texture = pre_minimize->second.texture;
  }
  const float capture_duration =
      std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - capture_started)
          .count();
  if (context.record_capture_duration) context.record_capture_duration(capture_duration);
  topmost.RestoreNow();

  // Capture cost is known only after this sample — if it was already too slow, abort Minimize
  // for this minimize and latch smart-skip so subsequent events stay native while pressure cools.
  constexpr float kAbortCaptureMs = 28.0f;
  if (!context.force_animation && policy_.smart_skip_enabled() && !already_minimized &&
      capture_duration >= kAbortCaptureMs) {
    core::LogDebug(L"Minimize", L"Smart-skip: abort after slow capture (" +
                                    std::to_wstring(capture_duration) + L" ms)");
    policy_.NoteSmartSkip(now_ms);
    context.set_state(run_index, runtime::RunState::kIdle);
    platform::SetDwmTransitionsDisabled(window, false);
    return false;
  }

  if (captured_texture.shader_resource_view == nullptr) {
    context.set_state(run_index, runtime::RunState::kIdle);
    return false;
  }
  run.auto_hide_taskbar_revealed =
      context.taskbar_targets->RevealAutoHideTaskbarForWindow(source_bounds);
  const platform::TaskbarTarget target =
      context.taskbar_targets->GetTargetForWindow(window, source_bounds);
  runtime::CachedSnapshot snapshot;
  snapshot.window = window;
  snapshot.bounds = source_bounds;
  snapshot.texture = captured_texture;
  snapshot.target = target;
  snapshot.original_placement = original_rect;
  snapshot.was_maximized = was_maximized;
  snapshot.process_id = platform::WindowProcessId(window);
  snapshot.captured_at_ms = GetTickCount64();

  auto transaction = Begin(MinimizeRequest{
      .window = window,
      .shutting_down = context.shutting_down,
      .renderer_available =
          !context.renderer_recovering && context.capture != nullptr && context.overlay != nullptr,
  });
  if (!transaction.has_value()) {
    if (run.auto_hide_taskbar_revealed) {
      context.taskbar_targets->ReleaseAutoHideTaskbar();
      run.auto_hide_taskbar_revealed = false;
    }
    context.set_state(run_index, runtime::RunState::kIdle);
    return false;
  }
  snapshots_.Restore()[window] = std::move(snapshot);
  run.animating_window = window;
  run.animating_restore = false;
  run.live_animation_bounds = source_bounds;
  context.reset_frame_pacing(run_index, window, source_bounds);
  run.last_animation_texture_refresh_ms = 0;
  run.live_animation_capture_enabled = false;
  const float duration = context.animation_configuration->Apply(run.overlay, source_bounds, false,
                                                                *context.rendering_pressure);
  core::LogTrace(L"Minimize", L"Configured minimize duration=" + std::to_wstring(duration));
  if (context.force_animation) {
    // Bulk minimize uses a two-phase transaction. Keep the real window untouched until every
    // captured overlay is ready, then the controller reveals all overlays before committing the
    // native minimizes behind them.
    if (!run.overlay.PrepareHiddenAnimation(captured_texture, ToRectF(source_bounds), target.rect,
                                            target.edge, 0.0f, 1.0f)) {
      transaction->HandOff();
      context.abort_run(run_index);
      return false;
    }
    snapshots_.PreMinimize().erase(window);
    transaction->HandOff();
    return true;
  }

  if (!run.overlay.StartAnimation(captured_texture, ToRectF(source_bounds), target.rect, target.edge,
                                  0.0f, 1.0f, !context.force_animation,
                                  !context.force_animation)) {
    transaction->HandOff();
    context.abort_run(run_index);
    return false;
  }
  snapshots_.PreMinimize().erase(window);

  if (!platform::SetWindowCloaked(window, true)) {
    core::LogDebug(L"Minimize", L"Cloaking failed; falling back to native minimize");
    transaction->HandOff();
    context.abort_run(run_index);
    return false;
  }
  (void)platform::windows::properties::MakeTransparent(window);
  context.animation_blocker->SetTransitionsDisabledForWindow(window, true);
  platform::windows::properties::StoreOriginalPlacement(window, original_rect);
  platform::windows::properties::StoreWasMaximized(window, was_maximized);
  platform::windows::properties::SetFlag(
      window, platform::windows::properties::WindowFlag::kIsMinimizing);

  if (IsIconic(window) == FALSE) {
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowMinimize);
    const int minimize_command = context.force_animation ? SW_SHOWMINNOACTIVE : SW_MINIMIZE;
    if (!ShowWindowAsync(window, minimize_command)) {
      context.animation_blocker->SetTransitionsDisabledForWindow(window, false);
      transaction->HandOff();
      context.abort_run(run_index);
      return false;
    }
    run.pending_native_minimize_window = window;
    context.set_state(run_index, runtime::RunState::kWaitingForNativeMinimize);
  } else {
    run.pending_native_minimize_window = nullptr;
    run.overlay.StartAnimationClock();
    context.set_state(run_index, runtime::RunState::kAnimating);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kMovedOffscreen);
    auto stored = snapshots_.Restore().find(window);
    if (stored != snapshots_.Restore().end()) stored->second.moved_offscreen = true;
  }
  run.direction_started_ms = GetTickCount64();
  transaction->HandOff();
  return true;
}

bool MinimizeFeature::CommitPreparedBulkMinimize(
    int run_index, platform::NativeAnimationBlocker* animation_blocker,
    const std::function<void(int, runtime::RunState)>& set_state,
    const std::function<void(int)>& abort) {
  if (run_index < 0 || run_index >= static_cast<int>(runs_.size())) return false;
  runtime::AnimationRun& run = runs_[run_index];
  const HWND window = run.animating_window;
  auto snapshot = snapshots_.Restore().find(window);
  if (!run.bulk_animation || run.state != runtime::RunState::kCapturing ||
      animation_blocker == nullptr || window == nullptr || !IsWindow(window) ||
      snapshot == snapshots_.Restore().end() || !run.overlay.active()) {
    abort(run_index);
    return false;
  }

  if (!platform::SetWindowCloaked(window, true)) {
    core::LogDebug(L"Minimize", L"Bulk cloaking failed; falling back to native minimize");
    abort(run_index);
    return false;
  }
  (void)platform::windows::properties::MakeTransparent(window);
  animation_blocker->SetTransitionsDisabledForWindow(window, true);
  platform::windows::properties::StoreOriginalPlacement(window,
                                                        snapshot->second.original_placement);
  platform::windows::properties::StoreWasMaximized(window, snapshot->second.was_maximized);
  platform::windows::properties::SetFlag(
      window, platform::windows::properties::WindowFlag::kIsMinimizing);

  if (IsIconic(window) == FALSE) {
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowMinimize);
    if (!ShowWindowAsync(window, SW_SHOWMINNOACTIVE)) {
      animation_blocker->SetTransitionsDisabledForWindow(window, false);
      abort(run_index);
      return false;
    }
    run.pending_native_minimize_window = window;
    set_state(run_index, runtime::RunState::kWaitingForNativeMinimize);
  } else {
    run.pending_native_minimize_window = nullptr;
    set_state(run_index, runtime::RunState::kAnimating);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kMovedOffscreen);
    snapshot->second.moved_offscreen = true;
  }
  run.direction_started_ms = GetTickCount64();
  return true;
}

std::optional<MinimizeFeature::Transaction> MinimizeFeature::Begin(const MinimizeRequest& request) {
  if (request.shutting_down || !request.renderer_available || request.window == nullptr ||
      !IsWindow(request.window)) {
    return std::nullopt;
  }
  const auto executable = platform::GetWindowExecutableName(request.window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) return std::nullopt;
  return Transaction(*this, request.window);
}

void MinimizeFeature::HandOff(HWND window) { active_.insert(window); }

void MinimizeFeature::Complete(HWND window) { active_.erase(window); }

void MinimizeFeature::Cancel(HWND window, bool force_show_if_iconic, bool activate) {
  active_.erase(window);
  recovery_.Restore(window, force_show_if_iconic, activate);
}

void MinimizeFeature::CancelAll(bool force_show_if_iconic) {
  const auto active = std::move(active_);
  active_.clear();
  for (HWND window : active) recovery_.Restore(window, force_show_if_iconic);
}

void MinimizeFeature::ReleaseAll() { active_.clear(); }

bool MinimizeFeature::IsAnimating(HWND window) const {
  return std::any_of(runs_.begin(), runs_.end(), [window](const runtime::AnimationRun& run) {
    return run.animating_window == window;
  });
}

void MinimizeFeature::UpdatePreMinimizeSnapshot(HWND window, HWND overlay,
                                                rendering::DesktopCapture* capture,
                                                bool renderer_recovering) {
  if (capture == nullptr || renderer_recovering || window == nullptr || window == overlay ||
      !IsWindow(window) || IsIconic(window) || !IsWindowVisible(window)) {
    return;
  }
  if (window_exclusions_.IsExcluded(window)) return;
  const auto executable = platform::GetWindowExecutableName(window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) return;
  for (const runtime::AnimationRun& run : runs_) {
    if (run.animating_window == window) return;
  }

  const std::optional<RECT> animation_bounds = ResolveAnimationBounds(window);
  if (!animation_bounds.has_value()) {
    core::LogTrace(L"Minimize", L"Pre-minimize snapshot skipped: bounds unavailable");
    return;
  }

  rendering::CapturedTexture captured_texture;
  RECT snapshot_bounds = *animation_bounds;
  bool captured = false;
  snapshots_.Prune();
  const bool prefer_window_capture = rendering::QueryWindowVisualMetadata(window).is_layered;
  auto existing = snapshots_.PreMinimize().find(window);
  if (!prefer_window_capture && existing != snapshots_.PreMinimize().end() &&
      EqualRect(&existing->second.bounds, &snapshot_bounds) &&
      existing->second.texture.texture != nullptr) {
    captured_texture = existing->second.texture;
    captured = capture->RefreshCapturedTexture(*animation_bounds, &captured_texture);
  }
  if (!captured && prefer_window_capture) {
    RECT captured_window_bounds{};
    captured = capture->CaptureWindow(window, *animation_bounds, &captured_texture,
                                      &captured_window_bounds);
    if (captured) snapshot_bounds = captured_window_bounds;
  }
  if (!captured) {
    captured = capture->CaptureRegion(window, *animation_bounds, &captured_texture);
  }
  if (!captured) {
    RECT captured_window_bounds{};
    if (!capture->CaptureWindow(window, *animation_bounds, &captured_texture,
                                &captured_window_bounds)) {
      core::LogTrace(L"Minimize", L"Pre-minimize snapshot capture failed");
      return;
    }
    snapshot_bounds = captured_window_bounds;
  }

  runtime::CachedSnapshot snapshot;
  snapshot.window = window;
  snapshot.bounds = snapshot_bounds;
  snapshot.texture = captured_texture;
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  snapshot.original_placement =
      GetWindowPlacement(window, &placement) ? placement.rcNormalPosition : *animation_bounds;
  if (!IsUsableRect(snapshot.original_placement)) snapshot.original_placement = snapshot_bounds;
  snapshot.was_maximized = IsCurrentlyMaximized(window);
  snapshot.process_id = platform::WindowProcessId(window);
  snapshot.captured_at_ms = GetTickCount64();
  snapshots_.PreMinimize()[window] = std::move(snapshot);
  snapshots_.Prune();
  core::LogTrace(L"Minimize", L"Pre-minimize snapshot updated");
}

bool MinimizeFeature::SeedSnapshotsInProgress() const { return seed_phase_ != SeedPhase::kIdle; }

void MinimizeFeature::CancelSeedSnapshotsForIconicWindows() {
  if (seed_phase_ == SeedPhase::kIdle) return;
  // Only the current window may be restored; re-minimize it if needed.
  if (seed_index_ < seed_candidates_.size()) {
    SeedCandidate& candidate = seed_candidates_[seed_index_];
    if (candidate.window != nullptr && IsWindow(candidate.window)) {
      if (IsIconic(candidate.window) == FALSE) {
        platform::windows::properties::SetFlag(
            candidate.window, platform::windows::properties::WindowFlag::kAllowMinimize);
        WINDOWPLACEMENT placement = candidate.placement;
        placement.showCmd = SW_SHOWMINNOACTIVE;
        SetWindowPlacement(candidate.window, &placement);
      }
      // Always clear temporary state. Restore may have failed and left the window iconic.
      platform::windows::properties::SetFlag(
          candidate.window, platform::windows::properties::WindowFlag::kAllowMinimize, false);
      platform::windows::properties::SetFlag(
          candidate.window, platform::windows::properties::WindowFlag::kAllowRestore, false);
      platform::SetDwmTransitionsDisabled(candidate.window, false);
    }
  }
  seed_candidates_.clear();
  seed_index_ = 0;
  seed_capture_ = nullptr;
  seed_targets_ = nullptr;
  seed_phase_ = SeedPhase::kIdle;
}

void MinimizeFeature::BeginSeedSnapshotsForIconicWindows(
    HWND overlay, rendering::DesktopCapture* capture,
    platform::TaskbarTargetProvider* taskbar_targets, bool renderer_recovering) {
  // Collect only — restore happens one window at a time so others never flash open together.
  CancelSeedSnapshotsForIconicWindows();
  if (capture == nullptr || renderer_recovering || taskbar_targets == nullptr) return;

  seed_candidates_.clear();
  seed_candidates_.reserve(16);
  for (HWND window : platform::EnumerateTopLevelWindows(overlay)) {
    if (window == nullptr || !IsWindow(window) || IsIconic(window) == FALSE) continue;
    if (!platform::IsInterestingTopLevelWindow(window, overlay)) continue;
    if (window_exclusions_.IsExcluded(window)) continue;
    const auto executable = platform::GetWindowExecutableName(window);
    if (executable.has_value() && policy_.IsExcluded(*executable)) continue;
    if (snapshots_.Restore().count(window) > 0 &&
        snapshots_.Restore()[window].texture.shader_resource_view != nullptr) {
      continue;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement)) continue;
    RECT normal = placement.rcNormalPosition;
    if (!IsUsableRect(normal)) continue;

    seed_candidates_.push_back(SeedCandidate{
        .window = window,
        .placement = placement,
        .normal = normal,
        .was_maximized = (placement.flags & WPF_RESTORETOMAXIMIZED) != 0,
    });
  }
  if (seed_candidates_.empty()) return;

  seed_capture_ = capture;
  seed_targets_ = taskbar_targets;
  seed_index_ = 0;
  seed_phase_ = SeedPhase::kRestoreCurrent;
}

bool MinimizeFeature::TickSeedSnapshotsForIconicWindows() {
  if (seed_phase_ == SeedPhase::kIdle) return false;
  if (seed_capture_ == nullptr || seed_targets_ == nullptr) {
    CancelSeedSnapshotsForIconicWindows();
    return false;
  }

  auto finish_all = [this] {
    snapshots_.Prune();
    seed_candidates_.clear();
    seed_index_ = 0;
    seed_capture_ = nullptr;
    seed_targets_ = nullptr;
    seed_phase_ = SeedPhase::kIdle;
  };

  // Skip holes left by destroyed windows.
  while (seed_index_ < seed_candidates_.size() &&
         (seed_candidates_[seed_index_].window == nullptr ||
          !IsWindow(seed_candidates_[seed_index_].window))) {
    ++seed_index_;
  }
  if (seed_index_ >= seed_candidates_.size()) {
    finish_all();
    return false;
  }

  SeedCandidate& candidate = seed_candidates_[seed_index_];

  if (seed_phase_ == SeedPhase::kRestoreCurrent) {
    platform::SetDwmTransitionsDisabled(candidate.window, true);
    platform::windows::properties::SetFlag(
        candidate.window, platform::windows::properties::WindowFlag::kAllowRestore);
    WINDOWPLACEMENT placement = candidate.placement;
    placement.showCmd = SW_SHOWNOACTIVATE;
    SetWindowPlacement(candidate.window, &placement);
    DwmFlush();
    seed_phase_ = SeedPhase::kWaitPaint;
    return true;
  }

  if (seed_phase_ == SeedPhase::kWaitPaint) {
    // One message-loop frame so this single restored window can paint.
    DwmFlush();
    seed_phase_ = SeedPhase::kCaptureCurrent;
  }

  // kCaptureCurrent — capture then immediately re-minimize this window only.
  RECT live{};
  if (!GetWindowRect(candidate.window, &live) || !IsUsableRect(live)) live = candidate.normal;
  const std::optional<RECT> frame = platform::GetExtendedFrameBounds(candidate.window);
  RECT bounds = (frame.has_value() && IsUsableRect(*frame)) ? *frame : live;

  rendering::CapturedTexture texture;
  RECT captured_window_bounds{};
  bool ok =
      seed_capture_->CaptureWindow(candidate.window, bounds, &texture, &captured_window_bounds);
  if (ok && IsUsableRect(captured_window_bounds)) {
    bounds = captured_window_bounds;
  } else if (!ok) {
    ok = seed_capture_->CaptureRegion(candidate.window, bounds, &texture);
  }
  if (!ok || texture.shader_resource_view == nullptr) {
    core::LogDebug(L"Minimize", L"Startup seed capture failed for iconic window");
    ok = false;
  }

  platform::windows::properties::SetFlag(
      candidate.window, platform::windows::properties::WindowFlag::kAllowMinimize);
  WINDOWPLACEMENT placement = candidate.placement;
  placement.showCmd = SW_SHOWMINNOACTIVE;
  SetWindowPlacement(candidate.window, &placement);
  platform::windows::properties::SetFlag(
      candidate.window, platform::windows::properties::WindowFlag::kAllowMinimize, false);
  platform::windows::properties::SetFlag(
      candidate.window, platform::windows::properties::WindowFlag::kAllowRestore, false);
  platform::SetDwmTransitionsDisabled(candidate.window, false);

  if (ok) {
    runtime::CachedSnapshot snapshot;
    snapshot.window = candidate.window;
    snapshot.bounds = bounds;
    snapshot.texture = std::move(texture);
    snapshot.target = seed_targets_->GetTargetForWindow(candidate.window, bounds);
    snapshot.original_placement = candidate.normal;
    snapshot.was_maximized = candidate.was_maximized;
    snapshot.process_id = platform::WindowProcessId(candidate.window);
    snapshot.captured_at_ms = GetTickCount64();
    snapshots_.PreMinimize()[candidate.window] = snapshot;
    snapshots_.Restore()[candidate.window] = std::move(snapshot);
    core::LogDebug(L"Minimize", L"Seeded snapshot for already-minimized window");
  }

  ++seed_index_;
  if (seed_index_ >= seed_candidates_.size()) {
    finish_all();
    return false;
  }
  seed_phase_ = SeedPhase::kRestoreCurrent;
  return true;
}

void MinimizeFeature::CompletePendingNativeMinimize(
    int run_index, bool start_animation_clock,
    const std::function<void(int, runtime::RunState)>& set_state,
    const std::function<void(int)>& abort) {
  if (run_index < 0 || run_index >= static_cast<int>(runs_.size())) return;
  runtime::AnimationRun& run = runs_[run_index];
  HWND window = run.pending_native_minimize_window;
  if (window == nullptr || !IsWindow(window) || run.animating_window != window ||
      !run.overlay.active() || run.overlay.restoring()) {
    if (window != nullptr && run.animating_window == window && !run.overlay.restoring()) {
      abort(run_index);
    }
    run.pending_native_minimize_window = nullptr;
    return;
  }

  auto snapshot = snapshots_.Restore().find(window);
  if (snapshot == snapshots_.Restore().end()) {
    abort(run_index);
    run.pending_native_minimize_window = nullptr;
    return;
  }
  if (IsIconic(window) == FALSE) return;

  run.pending_native_minimize_window = nullptr;
  run.live_animation_capture_enabled = false;
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(window, &placement)) {
    core::LogDebug(L"Minimize", L"Native minimize placement unavailable; aborting");
    abort(run_index);
    return;
  }

  const bool was_maximized =
      snapshot->second.was_maximized || (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
  platform::windows::properties::StoreWasMaximized(window, was_maximized);
  (void)platform::SetWindowCloaked(window, true);
  (void)platform::windows::properties::MakeTransparent(window);
  platform::windows::properties::SetFlag(
      window, platform::windows::properties::WindowFlag::kMovedOffscreen);
  snapshot->second.was_maximized = was_maximized;
  snapshot->second.moved_offscreen = true;
  if (start_animation_clock) run.overlay.StartAnimationClock();
  set_state(run_index, runtime::RunState::kAnimating);
  core::LogTrace(L"Minimize", start_animation_clock
                                  ? L"Native minimize completed; animation clock started"
                                  : L"Native minimize completed; waiting at bulk start barrier");
}

}  // namespace minimize::features
