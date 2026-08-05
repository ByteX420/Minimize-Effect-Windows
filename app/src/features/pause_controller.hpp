#pragma once

#include <cstdint>

namespace minimize::features {

class PauseController final {
public:
  void PauseFor(std::uint64_t duration_ms, std::uint64_t now_ms);
  void PauseUntilRestart();
  void Resume();
  [[nodiscard]] bool Update(std::uint64_t now_ms);
  [[nodiscard]] bool IsPaused(std::uint64_t now_ms) const;
  [[nodiscard]] bool until_restart() const { return until_restart_; }
  [[nodiscard]] std::uint64_t until_ms() const { return until_ms_; }

private:
  bool until_restart_ = false;
  std::uint64_t until_ms_ = 0;
};

}  // namespace minimize::features
