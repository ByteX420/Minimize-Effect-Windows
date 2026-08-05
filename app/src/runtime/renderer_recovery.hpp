#pragma once

#include <windows.h>

namespace minimize::runtime {

class RendererRecovery final {
public:
  void Begin();
  [[nodiscard]] bool pending() const { return pending_; }
  [[nodiscard]] bool ShouldAttempt(ULONGLONG now) const;
  void MarkSucceeded();
  void ScheduleRetry(ULONGLONG now);
  [[nodiscard]] ULONGLONG next_attempt_ms() const { return next_attempt_ms_; }

private:
  static constexpr DWORD kInitialDelayMilliseconds = 250;
  static constexpr DWORD kMaximumDelayMilliseconds = 4000;
  bool pending_ = false;
  ULONGLONG next_attempt_ms_ = 0;
  DWORD delay_ms_ = 0;
};

}  // namespace minimize::runtime
