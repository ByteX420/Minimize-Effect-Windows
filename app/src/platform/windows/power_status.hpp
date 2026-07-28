#pragma once

#include <memory>
#include <optional>

namespace minimize::platform {

struct PowerStatus {
  bool on_battery = false;
  bool battery_saver_active = false;
};

class PowerStatusMonitor final {
public:
  PowerStatusMonitor();
  ~PowerStatusMonitor();

  PowerStatusMonitor(const PowerStatusMonitor&) = delete;
  PowerStatusMonitor& operator=(const PowerStatusMonitor&) = delete;

  [[nodiscard]] bool Start();
  void Stop();
  [[nodiscard]] std::optional<PowerStatus> Current() const;
  [[nodiscard]] std::optional<PowerStatus> ConsumeChange();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace minimize::platform
