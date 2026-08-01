#include "pch.hpp"

#include "features/restore_feature.hpp"

#include <iostream>
#include <utility>

#include "animation/geometry.hpp"
#include "core/logger.hpp"
#include "features/animation_configuration.hpp"
#include "features/effect_policy.hpp"
#include "features/minimize_feature.hpp"
#include "features/window_exclusion_service.hpp"
#include "features/window_recovery_service.hpp"
#include "platform/windows/native_animation_blocker.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "runtime/animation_run.hpp"
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

}  // namespace

RestoreFeature::Transaction::Transaction(RestoreFeature& owner, HWND window)
    : owner_(&owner), window_(window) {}

RestoreFeature::Transaction::~Transaction() {
  if (owner_ != nullptr) owner_->Cancel(window_);
}

RestoreFeature::Transaction::Transaction(Transaction&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      window_(std::exchange(other.window_, nullptr)) {}

RestoreFeature::Transaction& RestoreFeature::Transaction::operator=(Transaction&& other) noexcept {
  if (this == &other) return *this;
  if (owner_ != nullptr) owner_->Cancel(window_);
  owner_ = std::exchange(other.owner_, nullptr);
  window_ = std::exchange(other.window_, nullptr);
  return *this;
}

void RestoreFeature::Transaction::HandOff() {
  if (owner_ == nullptr) return;
  owner_->HandOff(window_);
  owner_ = nullptr;
  window_ = nullptr;
}

RestoreFeature::RestoreFeature(EffectPolicy& policy, WindowRecoveryService& recovery,
                               runtime::SnapshotCache& snapshots, runtime::AnimationRunPool& runs,
                               MinimizeFeature& minimize, WindowExclusionService& window_exclusions)
    : policy_(policy),
      recovery_(recovery),
      snapshots_(snapshots),
      runs_(runs),
      minimize_(minimize),
      window_exclusions_(window_exclusions) {}

bool RestoreFeature::Execute(HWND window, const RestoreExecutionContext& context) {
  if (!context.effect_active || context.renderer_recovering || context.shutting_down ||
      context.overlay == nullptr || window == nullptr || !IsWindow(window) ||
      context.animation_blocker == nullptr || context.animation_configuration == nullptr ||
      context.rendering_pressure == nullptr) {
    return false;
  }

  auto clean_excluded = [&] {
    platform::SetDwmTransitionsDisabled(window, false);
    const int run_index = context.find_run(window);
    if (run_index != -1) context.finish_run(run_index);
    const bool has_state =
        snapshots_.Restore().count(window) != 0 ||
        platform::windows::properties::HasFlag(
            window, platform::windows::properties::WindowFlag::kIsMinimizing) ||
        platform::windows::properties::HasFlag(
            window, platform::windows::properties::WindowFlag::kMovedOffscreen);
    if (has_state) {
      recovery_.Restore(window, false);
      snapshots_.Restore().erase(window);
      snapshots_.PreMinimize().erase(window);
    }
  };

  if (window_exclusions_.IsExcluded(window)) {
    clean_excluded();
    return false;
  }

  const auto executable = platform::GetWindowExecutableName(window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) {
    clean_excluded();
    return false;
  }
  if (recovery_.restoring() || window == context.overlay ||
      !platform::IsInterestingTopLevelWindow(window, context.overlay)) {
    return false;
  }

  auto snapshot_iterator = snapshots_.Restore().find(window);
  const bool has_snapshot = snapshot_iterator != snapshots_.Restore().end();
  const bool moved_offscreen =
      (has_snapshot && snapshot_iterator->second.moved_offscreen) ||
      platform::windows::properties::HasFlag(
          window, platform::windows::properties::WindowFlag::kMovedOffscreen);
  const bool minimize_minimized =
      has_snapshot || platform::windows::properties::HasFlag(
                          window, platform::windows::properties::WindowFlag::kIsMinimizing);

  int run_index = context.find_run(window);
  if (run_index != -1) {
    auto transaction = Begin(RestoreRequest{
        .window = window,
        .shutting_down = context.shutting_down,
        .renderer_available = !context.renderer_recovering && context.overlay != nullptr,
    });
    if (!transaction.has_value()) return false;
    minimize_.Complete(window);
    runtime::AnimationRun& run = runs_[run_index];
    if (run.pending_native_minimize_window == window) {
      run.pending_native_minimize_window = nullptr;
    }
    run.animating_restore = true;
    run.overlay.ReverseAnimation(!context.defer_first_frame_wait);
    run.direction_started_ms = GetTickCount64();
    context.set_state(run_index, runtime::RunState::kRestoring);
    run.live_animation_capture_enabled = false;
    if (IsIconic(window) == FALSE) {
      (void)platform::SetWindowCloaked(window, true);
      (void)platform::windows::properties::MakeTransparent(window);
      if (!moved_offscreen) {
        runtime::CachedSnapshot* snapshot = has_snapshot ? &snapshot_iterator->second : nullptr;
        if (!PreservePlacementAndMarkOffscreen(window, snapshot)) {
          transaction->HandOff();
          context.abort_run(run_index);
          return false;
        }
      }
    }
    transaction->HandOff();
    return true;
  }

  const bool window_is_iconic = IsIconic(window) != FALSE;
  if (!minimize_minimized && !window_is_iconic) return false;
  auto transaction = Begin(RestoreRequest{
      .window = window,
      .shutting_down = context.shutting_down,
      .renderer_available = !context.renderer_recovering && context.overlay != nullptr,
  });
  if (!transaction.has_value()) return false;

  if (!window_is_iconic && !moved_offscreen) {
    (void)platform::SetWindowCloaked(window, true);
    (void)platform::windows::properties::MakeTransparent(window);
    runtime::CachedSnapshot* snapshot = has_snapshot ? &snapshot_iterator->second : nullptr;
    if (!PreservePlacementAndMarkOffscreen(window, snapshot)) {
      snapshots_.Restore().erase(window);
      return false;
    }
  } else if (!window_is_iconic && moved_offscreen) {
    (void)platform::SetWindowCloaked(window, true);
    (void)platform::windows::properties::MakeTransparent(window);
  }

  auto current = snapshots_.Restore().find(window);
  if (current == snapshots_.Restore().end() ||
      current->second.texture.shader_resource_view == nullptr) {
    return false;
  }

  run_index = context.find_available_run();
  if (run_index == -1) {
    snapshots_.Restore().erase(window);
    return false;
  }
  context.set_state(run_index, runtime::RunState::kRestoring);
  runtime::AnimationRun& run = runs_[run_index];
  run.animating_window = window;
  run.animating_restore = true;
  run.live_animation_capture_enabled = false;
  context.reset_frame_pacing(run_index, window, current->second.bounds);
  context.animation_blocker->SetTransitionsDisabledForWindow(window, true);

  const float duration = context.animation_configuration->Apply(run.overlay, current->second.bounds,
                                                                true, *context.rendering_pressure);
  core::LogTrace(L"Restore", L"Configured restore duration=" + std::to_wstring(duration));
  if (!run.overlay.StartAnimation(current->second.texture, ToRectF(current->second.bounds),
                                  current->second.target.rect, current->second.target.edge, 1.0f,
                                  0.0f, !context.defer_first_frame_wait,
                                  !context.defer_first_frame_wait)) {
    transaction->HandOff();
    context.abort_run(run_index);
    return false;
  }
  if (!context.defer_first_frame_wait) run.overlay.StartAnimationClock();
  transaction->HandOff();
  return true;
}

std::optional<RestoreFeature::Transaction> RestoreFeature::Begin(const RestoreRequest& request) {
  if (request.shutting_down || !request.renderer_available || request.window == nullptr ||
      !IsWindow(request.window)) {
    return std::nullopt;
  }
  const auto executable = platform::GetWindowExecutableName(request.window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) return std::nullopt;
  return Transaction(*this, request.window);
}

void RestoreFeature::HandOff(HWND window) { active_.insert(window); }

void RestoreFeature::Complete(HWND window) { active_.erase(window); }

void RestoreFeature::Cancel(HWND window, bool force_show_if_iconic, bool activate) {
  active_.erase(window);
  recovery_.Restore(window, force_show_if_iconic, activate);
}

void RestoreFeature::CancelAll(bool force_show_if_iconic) {
  const auto active = std::move(active_);
  active_.clear();
  for (HWND window : active) recovery_.Restore(window, force_show_if_iconic);
}

void RestoreFeature::ReleaseAll() { active_.clear(); }

bool RestoreFeature::PreservePlacementAndMarkOffscreen(HWND window,
                                                       runtime::CachedSnapshot* snapshot) const {
  const auto usable = [](const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top && rect.left > -30000 &&
           rect.top > -30000;
  };
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(window, &placement)) return false;

  RECT original_rect = placement.rcNormalPosition;
  if (!usable(original_rect) && snapshot != nullptr) {
    original_rect =
        usable(snapshot->original_placement) ? snapshot->original_placement : snapshot->bounds;
  }
  if (!usable(original_rect)) {
    const auto bounds = platform::GetExtendedFrameBounds(window);
    if (bounds.has_value() && usable(*bounds)) original_rect = *bounds;
  }
  if (!usable(original_rect)) return false;

  const bool was_maximized =
      IsZoomed(window) != FALSE || (snapshot != nullptr && snapshot->was_maximized);
  if (snapshot != nullptr) {
    if (!usable(snapshot->original_placement)) snapshot->original_placement = original_rect;
    snapshot->was_maximized = was_maximized;
    snapshot->moved_offscreen = true;
  }
  platform::windows::properties::StoreOriginalPlacement(window, original_rect);
  platform::windows::properties::SetFlag(
      window, platform::windows::properties::WindowFlag::kMovedOffscreen);
  platform::windows::properties::StoreWasMaximized(window, was_maximized);
  return true;
}

bool RestoreFeature::IsWindowRestored(HWND window) const {
  if (snapshots_.Restore().count(window) == 0 &&
      !platform::windows::properties::HasFlag(
          window, platform::windows::properties::WindowFlag::kIsMinimizing)) {
    return false;
  }
  return IsIconic(window) == FALSE;
}

}  // namespace minimize::features
