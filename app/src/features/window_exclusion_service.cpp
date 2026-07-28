#include "pch.hpp"

#include "features/window_exclusion_service.hpp"

#include <algorithm>

#include "core/logger.hpp"
#include "platform/windows/process_info.hpp"

namespace minimize::features {
namespace {

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

}  // namespace

WindowExclusionService::~WindowExclusionService() { Clear(); }

std::string WindowExclusionService::MonitorDeviceName(HMONITOR monitor) {
  if (monitor == nullptr) return {};
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return {};
  return WideToUtf8(info.szDevice);
}

bool WindowExclusionService::SetExcluded(HWND window, bool excluded) {
  if (window == nullptr || !IsWindow(window)) return false;
  const DWORD process_id = platform::WindowProcessId(window);
  if (process_id == 0) return false;

  if (!excluded) {
    Remove(window);
    return true;
  }

  excluded_windows_[window] = Entry{.window = window, .process_id = process_id};
  core::LogDebug(L"WindowExclude",
                 L"Excluded hwnd=0x" +
                     std::to_wstring(reinterpret_cast<std::uintptr_t>(window)) + L" pid=" +
                     std::to_wstring(process_id));
  return true;
}

bool WindowExclusionService::IsExcluded(HWND window) const {
  if (window == nullptr || !IsWindow(window)) {
    if (window != nullptr) excluded_windows_.erase(window);
    return false;
  }

  // Persisted display exclusion wins for any window currently on that monitor.
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (IsDisplayExcluded(monitor)) return true;

  const auto it = excluded_windows_.find(window);
  if (it == excluded_windows_.end()) return false;

  const DWORD live_pid = platform::WindowProcessId(window);
  if (live_pid == 0 || live_pid != it->second.process_id) {
    excluded_windows_.erase(it);
    core::LogDebug(L"WindowExclude", L"Dropped stale exclusion (window instance changed)");
    return false;
  }
  return true;
}

void WindowExclusionService::Remove(HWND window) {
  if (window == nullptr) return;
  excluded_windows_.erase(window);
}

void WindowExclusionService::PruneInvalidWindows() {
  for (auto it = excluded_windows_.begin(); it != excluded_windows_.end();) {
    const HWND window = it->first;
    if (!IsWindow(window) || platform::WindowProcessId(window) != it->second.process_id) {
      it = excluded_windows_.erase(it);
    } else {
      ++it;
    }
  }
}

void WindowExclusionService::Clear() {
  excluded_windows_.clear();
  excluded_display_devices_.clear();
  excluded_display_lookup_.clear();
}

void WindowExclusionService::SetExcludedDisplays(std::vector<std::string> device_names) {
  excluded_display_devices_ = std::move(device_names);
  excluded_display_lookup_.clear();
  for (const std::string& name : excluded_display_devices_) {
    if (!name.empty()) excluded_display_lookup_.insert(name);
  }
}

bool WindowExclusionService::IsDisplayExcluded(const std::string& device_name) const {
  if (device_name.empty()) return false;
  return excluded_display_lookup_.count(device_name) > 0;
}

bool WindowExclusionService::IsDisplayExcluded(HMONITOR monitor) const {
  return IsDisplayExcluded(MonitorDeviceName(monitor));
}

bool WindowExclusionService::SetDisplayExcluded(const std::string& device_name, bool excluded) {
  if (device_name.empty()) return false;
  const auto it =
      std::find(excluded_display_devices_.begin(), excluded_display_devices_.end(), device_name);
  if (excluded) {
    if (it == excluded_display_devices_.end()) {
      excluded_display_devices_.push_back(device_name);
      excluded_display_lookup_.insert(device_name);
    }
  } else if (it != excluded_display_devices_.end()) {
    excluded_display_devices_.erase(it);
    excluded_display_lookup_.erase(device_name);
  }
  return true;
}

}  // namespace minimize::features
