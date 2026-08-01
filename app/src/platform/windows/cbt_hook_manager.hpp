#pragma once

#include <windows.h>

namespace minimize::platform::windows {

class CbtHookManager final {
public:
  CbtHookManager() = default;
  ~CbtHookManager();

  CbtHookManager(const CbtHookManager&) = delete;
  CbtHookManager& operator=(const CbtHookManager&) = delete;

  [[nodiscard]] bool Install();
  void Uninstall();
  [[nodiscard]] bool IsInstalled() const {
    return hook_ != nullptr && call_window_hook_ != nullptr;
  }

private:
  HMODULE library_ = nullptr;
  HHOOK hook_ = nullptr;
  HHOOK call_window_hook_ = nullptr;
};

}  // namespace minimize::platform::windows
