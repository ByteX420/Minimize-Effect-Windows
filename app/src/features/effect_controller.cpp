#include "pch.hpp"

#include "features/effect_controller.hpp"

#include "core/logger.hpp"
#include "features/effect_policy.hpp"
#include "features/minimize_feature.hpp"
#include "features/pause_controller.hpp"
#include "features/restore_feature.hpp"
#include "platform/windows/native_animation_blocker.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "rendering/desktop_capture.hpp"

namespace minimize::features {

EffectController::EffectController(EffectPolicy& policy, PauseController& pause,
                                   MinimizeFeature& minimize, RestoreFeature& restore)
    : policy_(policy), pause_(pause), minimize_(minimize), restore_(restore) {}

bool EffectController::Start(WindowAction minimize, WindowAction restore,
                             WindowSeenAction window_seen) {
  return event_monitor_.Start(std::move(minimize), std::move(restore), std::move(window_seen));
}

void EffectController::Stop() { event_monitor_.Stop(); }

bool EffectController::IsActive() const {
  return policy_.IsActive(pause_.IsPaused(GetTickCount64()));
}

bool EffectController::IsWindowExcluded(HWND window) const {
  const auto executable = platform::GetWindowExecutableName(window);
  return executable.has_value() && policy_.IsExcluded(*executable);
}

void EffectController::ApplyExclusionTransitionOverrides(HWND overlay) const {
  for (HWND window : platform::EnumerateTopLevelWindows(overlay)) {
    const bool excluded = IsActive() && IsWindowExcluded(window);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kExcludedApplication, excluded);
    platform::SetDwmTransitionsDisabled(window, IsActive() && !excluded);
  }
}

void EffectController::HandleWindowSeen(HWND window, DWORD event, HWND overlay,
                                        bool renderer_recovering,
                                        platform::NativeAnimationBlocker& animation_blocker,
                                        rendering::DesktopCapture* capture,
                                        const WindowAction& restore) {
  if (!IsActive() || renderer_recovering || window == nullptr || window == overlay) return;

  if (event == EVENT_SYSTEM_FOREGROUND &&
      platform::WindowProcessId(window) != GetCurrentProcessId() &&
      platform::IsInterestingTopLevelWindow(window, overlay)) {
    last_foreground_window_ = window;
  }

  const auto executable = platform::GetWindowExecutableName(window);
  if (executable.has_value() && policy_.IsExcluded(*executable)) {
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kExcludedApplication);
    platform::SetDwmTransitionsDisabled(window, false);
    return;
  }
  platform::windows::properties::SetFlag(
      window, platform::windows::properties::WindowFlag::kExcludedApplication, false);
  if (minimize_.IsAnimating(window)) return;
  animation_blocker.SetTransitionsDisabledForWindow(window, true);

  if (IsWindowVisible(window) && restore_.IsWindowRestored(window)) {
    core::LogDebug(L"Effect", L"Surprise restore detected");
    if (restore) restore(window);
    return;
  }
  if (event == EVENT_SYSTEM_FOREGROUND) {
    minimize_.UpdatePreMinimizeSnapshot(window, overlay, capture, renderer_recovering);
  }
}

}  // namespace minimize::features
