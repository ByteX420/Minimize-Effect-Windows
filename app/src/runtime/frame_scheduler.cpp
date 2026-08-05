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
  if (timer_) return;
  timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_MODIFY_STATE | SYNCHRONIZE));
  high_resolution_timer_ = static_cast<bool>(timer_);
  if (!timer_) {
    timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
  }
  wake_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

void FrameScheduler::Shutdown() {
  EndFallbackTimerResolution();
  timer_.reset();
  wake_event_.reset();
  high_resolution_timer_ = false;
}

void FrameScheduler::Wake() {
  if (wake_event_ != nullptr) SetEvent(wake_event_.get());
  if (!timer_) return;
  LARGE_INTEGER wake_now{};
  SetWaitableTimer(timer_.get(), &wake_now, 0, nullptr, nullptr, FALSE);
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
  HMONITOR monitor = run.animating_window != nullptr && IsWindow(run.animating_window) &&
                             !IsIconic(run.animating_window)
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

void FrameScheduler::Wait(const AnimationRunPool& runs, HANDLE settings_frame_waitable_object) {
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
  if (!has_interval && settings_frame_waitable_object == nullptr) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (has_interval && now >= earliest) return;

  DWORD timeout = INFINITE;
  HANDLE handles[2]{};
  DWORD handle_count = 0;
  bool timer_armed = false;
  std::chrono::steady_clock::duration wait_duration{};
  if (has_interval) {
    wait_duration = earliest - now;
  }
  if (has_interval && timer_) {
    const auto hundred_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wait_duration).count() / 100;
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -std::max<std::int64_t>(1, hundred_ns);
    if (SetWaitableTimerEx(timer_.get(), &due_time, 0, nullptr, nullptr, nullptr, 0)) {
      handles[handle_count++] = timer_.get();
      timer_armed = true;
    }
  }
  if (settings_frame_waitable_object != nullptr) {
    handles[handle_count++] = settings_frame_waitable_object;
  }
  if (has_interval && !timer_armed) {
    const auto wait_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wait_duration).count();
    timeout =
        static_cast<DWORD>(std::max<std::int64_t>(1, (wait_ns + 999999) / 1000000));
  }
  MsgWaitForMultipleObjectsEx(handle_count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void FrameScheduler::WaitIdle(ULONGLONG deadline_ms, HANDLE settings_frame_waitable_object) {
  // Idle waits are event-driven: wake on the wake event, on a settings render request, or on
  // an incoming message; otherwise sleep until the given periodic-work deadline (0 = forever).
  HANDLE handles[2]{};
  DWORD handle_count = 0;
  if (wake_event_ != nullptr) handles[handle_count++] = wake_event_.get();
  if (settings_frame_waitable_object != nullptr) {
    handles[handle_count++] = settings_frame_waitable_object;
  }
  DWORD timeout_ms = INFINITE;
  const ULONGLONG now_ms = GetTickCount64();
  if (deadline_ms != 0) {
    if (deadline_ms > now_ms) {
      const ULONGLONG remaining_ms = deadline_ms - now_ms;
      timeout_ms = static_cast<DWORD>(std::min<ULONGLONG>(remaining_ms, 1000));
    } else {
      timeout_ms = 0;
    }
  }
  MsgWaitForMultipleObjectsEx(handle_count, handles, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void FrameScheduler::BeginFallbackTimerResolution() {
  if (fallback_resolution_active_) return;
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
