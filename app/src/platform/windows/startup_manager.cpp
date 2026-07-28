#include "pch.hpp"

#include "platform/windows/startup_manager.hpp"

#include <vector>
#include <wil/registry.h>

namespace minimize::platform::windows {
namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MinimizeEffect";

std::wstring CurrentExecutablePath() {
  std::vector<wchar_t> buffer(512);
  while (buffer.size() <= 32768) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size() - 1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return std::wstring(buffer.data(), length);
    }
    buffer.resize(buffer.size() * 2);
  }
  return {};
}

}  // namespace

bool ConfigureRunAtStartup(bool enabled) {
  if (enabled) {
    const std::wstring executable_path = CurrentExecutablePath();
    if (executable_path.empty() || executable_path.find(L'"') != std::wstring::npos) {
      return false;
    }
    const std::wstring command = L"\"" + executable_path + L"\"";
    wil::unique_hkey run_key;
    if (FAILED(wil::reg::create_unique_key_nothrow(
            HKEY_CURRENT_USER, kRunKeyPath, run_key, wil::reg::key_access::readwrite))) {
      return false;
    }
    return SUCCEEDED(
        wil::reg::set_value_string_nothrow(run_key.get(), kRunValueName, command.c_str()));
  }

  wil::unique_hkey run_key;
  const HRESULT open_result = wil::reg::open_unique_key_nothrow(
      HKEY_CURRENT_USER, kRunKeyPath, run_key, wil::reg::key_access::readwrite);
  if (wil::reg::is_registry_not_found(open_result)) return true;
  if (FAILED(open_result)) return false;
  const LSTATUS delete_result = RegDeleteValueW(run_key.get(), kRunValueName);
  return delete_result == ERROR_SUCCESS || delete_result == ERROR_FILE_NOT_FOUND;
}

}  // namespace minimize::platform::windows
