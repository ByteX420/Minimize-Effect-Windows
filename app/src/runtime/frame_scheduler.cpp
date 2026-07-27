#include "pch.hpp"

#include "runtime/frame_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <dwmapi.h>
#include <timeapi.h>

#include "core/logger.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/window_state.hpp"


namespace minimize::runtime {

FrameScheduler::~FrameScheduler() { Shutdown(); }

void FrameScheduler::Initialize() {
  if (timer_ != nullptr) return;
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
  high_resolution_timer_ = timer_ != nullptr;
  if (timer_ == nullptr) {
    timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
}

void FrameScheduler::Shutdown() {
  EndFallbackTimerResolution();
  if (timer_ != nullptr) {
    CloseHandle(timer_);
    timer_ = nullptr;
  }
  high_resolution_timer_ = false;
}

void FrameScheduler::Wake() {
  if (timer_ == nullptr) return;
  LARGE_INTEGER wake_now{};
  SetWaitableTimer(timer_, &wake_now, 0, nullptr, nullptr, FALSE);
}

void FrameScheduler::Reset(AnimationRun& run, HWND window, const RECT& animation_bounds) {
  BeginFallbackTimerResolution();
  run.live_animation_bounds = animation_bounds;
  if (window != nullptr && IsWindow(window)) {
    const auto current_bounds = platform::GetExtendedFrameBounds(window);
    if (current_bounds.has_value()) run.live_animation_bounds = *current_bounds;
  }
  run.animation_monitor = nullptr;
  run.animation_frame_interval = std::chrono::steady_clock::duration::zero();
  run.next_animation_frame_time = std::chrono::steady_clock::now();
  UpdateMonitor(run);
}

void FrameScheduler::UpdateMonitor(AnimationRun& run) {
  RECT monitor_bounds = run.live_animation_bounds;
  if (run.animating_window != nullptr && IsWindow(run.animating_window) &&
      !IsIconic(run.animating_window)) {
    const auto current_bounds = platform::GetExtendedFrameBounds(run.animating_window);
    if (current_bounds.has_value()) {
      monitor_bounds = *current_bounds;
      run.live_animation_bounds = *current_bounds;
    }
  }
  HMONITOR monitor = run.animating_window != nullptr && IsWindow(run.animating_window)
                         ? MonitorFromWindow(run.animating_window, MONITOR_DEFAULTTONEAREST)
                         : nullptr;
  if (monitor == nullptr) monitor = MonitorFromRect(&monitor_bounds, MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr || monitor == run.animation_monitor) return;

  run.animation_monitor = monitor;
  const auto refresh_rate = platform::GetMonitorRefreshRateHz(monitor);
  if (!refresh_rate.has_value() || *refresh_rate <= 0.0) {
    run.animation_frame_interval = std::chrono::steady_clock::duration::zero();
    run.next_animation_frame_time = std::chrono::steady_clock::now();
    core::LogDebug(L"FrameScheduler", L"Monitor refresh unavailable; limiter disabled");
    return;
  }
  run.animation_frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / *refresh_rate));
  run.next_animation_frame_time = std::chrono::steady_clock::now() + run.animation_frame_interval;
}

bool FrameScheduler::IsDue(const AnimationRun& run) const {
  return run.animation_frame_interval <= std::chrono::steady_clock::duration::zero() ||
         std::chrono::steady_clock::now() >= run.next_animation_frame_time;
}

unsigned int FrameScheduler::Advance(AnimationRun& run) {
  if (run.animation_frame_interval <= std::chrono::steady_clock::duration::zero()) return 0;
  const auto now = std::chrono::steady_clock::now();
  if (run.next_animation_frame_time == std::chrono::steady_clock::time_point{}) {
    run.next_animation_frame_time = now + run.animation_frame_interval;
    return 0;
  }
  if (run.next_animation_frame_time > now) return 0;
  const auto missed = (now - run.next_animation_frame_time) / run.animation_frame_interval;
  run.next_animation_frame_time += run.animation_frame_interval * (missed + 1);
  return static_cast<unsigned int>(std::min<std::int64_t>(static_cast<std::int64_t>(missed), 120));
}

void FrameScheduler::Wait(const AnimationRunPool& runs) {
  bool has_interval = false;
  auto earliest = std::chrono::steady_clock::time_point::max();
  for (const AnimationRun& run : runs) {
    if (!run.overlay.active()) continue;
    if (run.animation_frame_interval <= std::chrono::steady_clock::duration::zero()) {
      DwmFlush();
      return;
    }
    earliest = std::min(earliest, run.next_animation_frame_time);
    has_interval = true;
  }
  if (!has_interval) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= earliest) return;
  const auto wait_duration = earliest - now;
  if (timer_ != nullptr) {
    const auto hundred_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wait_duration).count() / 100;
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -std::max<std::int64_t>(1, hundred_ns);
    if (SetWaitableTimerEx(timer_, &due_time, 0, nullptr, nullptr, nullptr, 0)) {
      const HANDLE handles[] = {timer_};
      MsgWaitForMultipleObjectsEx(1, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      return;
    }
  }
  const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wait_duration).count();
  const DWORD timeout = static_cast<DWORD>(std::max<std::int64_t>(1, (wait_ns + 999999) / 1000000));
  MsgWaitForMultipleObjects(0, nullptr, FALSE, timeout, QS_ALLINPUT);
}

void FrameScheduler::BeginFallbackTimerResolution() {
  if (high_resolution_timer_ || fallback_resolution_active_) return;
  TIMECAPS capabilities{};
  if (timeGetDevCaps(&capabilities, sizeof(capabilities)) != TIMERR_NOERROR ||
      capabilities.wPeriodMin == 0) {
    return;
  }
  if (timeBeginPeriod(capabilities.wPeriodMin) == TIMERR_NOERROR) {
    fallback_period_ms_ = capabilities.wPeriodMin;
    fallback_resolution_active_ = true;
  }
}

void FrameScheduler::EndFallbackTimerResolution() {
  if (!fallback_resolution_active_) return;
  timeEndPeriod(fallback_period_ms_);
  fallback_period_ms_ = 0;
  fallback_resolution_active_ = false;
}

}  // namespace minimize::runtime
