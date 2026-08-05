#pragma once

#include <windows.h>
#include <wil/resource.h>

#include "runtime/animation_run.hpp"
#include "runtime/animation_run_pool.hpp"

namespace minimize::runtime {

class FrameScheduler final {
public:
  FrameScheduler() = default;
  ~FrameScheduler();

  FrameScheduler(const FrameScheduler&) = delete;
  FrameScheduler& operator=(const FrameScheduler&) = delete;

  void Initialize();
  void Shutdown();
  void Wake();
  void Reset(AnimationRun& run, HWND window, const RECT& animation_bounds);
  void UpdateMonitor(AnimationRun& run);
  void ValidateMonitorIfDue(AnimationRun& run, ULONGLONG now_ms);
  [[nodiscard]] bool IsDue(const AnimationRun& run) const;
  [[nodiscard]] unsigned int Advance(AnimationRun& run);
  void Wait(const AnimationRunPool& runs, HANDLE settings_frame_waitable_object = nullptr);
  void WaitIdle(ULONGLONG deadline_ms, HANDLE settings_frame_waitable_object = nullptr);
  void EndFallbackTimerResolution();

private:
  void BeginFallbackTimerResolution();

  wil::unique_handle timer_;
  wil::unique_handle wake_event_;
  bool high_resolution_timer_ = false;
  bool fallback_resolution_active_ = false;
  UINT fallback_period_ms_ = 0;
};

}  // namespace minimize::runtime
