#pragma once

#include <windows.h>
#include <wil/resource.h>

namespace minimize::platform::windows {

enum class SingleInstanceResult {
  kPrimary,
  kAlreadyRunning,
  kError,
};

class SingleInstanceGuard final {
public:
  SingleInstanceGuard() = default;
  ~SingleInstanceGuard();

  SingleInstanceGuard(const SingleInstanceGuard&) = delete;
  SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

  [[nodiscard]] SingleInstanceResult Acquire();
  [[nodiscard]] static bool ActivateExistingInstance(DWORD timeout_ms);
  void Release();
  [[nodiscard]] DWORD error() const { return error_; }

private:
  wil::unique_handle mutex_;
  bool owns_mutex_ = false;
  DWORD error_ = ERROR_SUCCESS;
};

}  // namespace minimize::platform::windows
