#include "pch.hpp"

#include "platform/windows/single_instance_guard.hpp"

namespace minimize::platform::windows {
namespace {

constexpr wchar_t kMutexName[] = L"Local\\MinimizeEffect.Windows.SingleInstance";
constexpr wchar_t kSettingsWindowClass[] = L"MinimizeEffectImGuiSettings";
constexpr UINT kShowSettingsMessage = WM_APP + 101;

std::wstring MutexName() {
#ifdef _DEBUG
  // Keeps integration handover tests isolated from a developer's running installation.
  wchar_t suffix[96]{};
  const DWORD length =
      GetEnvironmentVariableW(L"MINIMIZE_TEST_INSTANCE_SUFFIX", suffix, std::size(suffix));
  if (length > 0 && length < std::size(suffix)) {
    return std::wstring(kMutexName) + L"." + suffix;
  }
#endif
  return kMutexName;
}

}  // namespace

SingleInstanceGuard::~SingleInstanceGuard() { Release(); }

bool SingleInstanceGuard::ActivateExistingInstance(DWORD timeout_ms) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  do {
    const HWND window = FindWindowW(kSettingsWindowClass, nullptr);
    if (window != nullptr) {
      DWORD_PTR ignored = 0;
      return SendMessageTimeoutW(window, kShowSettingsMessage, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                 1000, &ignored) != 0;
    }
    Sleep(50);
  } while (GetTickCount64() < deadline);
  return false;
}

SingleInstanceResult SingleInstanceGuard::Acquire() {
  if (mutex_) return SingleInstanceResult::kPrimary;
  SetLastError(ERROR_SUCCESS);
  const std::wstring mutex_name = MutexName();
  mutex_.reset(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
  error_ = GetLastError();
  if (!mutex_) {
    return error_ == ERROR_ACCESS_DENIED ? SingleInstanceResult::kAlreadyRunning
                                         : SingleInstanceResult::kError;
  }
  if (error_ == ERROR_ALREADY_EXISTS) {
    Release();
    return SingleInstanceResult::kAlreadyRunning;
  }
  owns_mutex_ = true;
  return SingleInstanceResult::kPrimary;
}

void SingleInstanceGuard::Release() {
  if (!mutex_) return;
  if (owns_mutex_) ReleaseMutex(mutex_.get());
  mutex_.reset();
  owns_mutex_ = false;
}

}  // namespace minimize::platform::windows
