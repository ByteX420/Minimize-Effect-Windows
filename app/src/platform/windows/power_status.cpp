#include "pch.hpp"

#include "platform/windows/power_status.hpp"

#include <mutex>
#include <powersetting.h>

namespace minimize::platform {
namespace {

constexpr wchar_t kPowerMonitorWindowClass[] = L"MinimizeEffectPowerStatusMonitor";

std::optional<PowerStatus> QueryInitialStatus() {
  SYSTEM_POWER_STATUS status{};
  if (!GetSystemPowerStatus(&status)) return std::nullopt;
  return PowerStatus{
      .on_battery = status.ACLineStatus == 0,
      .battery_saver_active = status.SystemStatusFlag != 0,
  };
}

}  // namespace

struct PowerStatusMonitor::Impl {
  bool Start() {
    if (window != nullptr) return true;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = &WindowProc;
    window_class.lpszClassName = kPowerMonitorWindowClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
    window = CreateWindowExW(0, kPowerMonitorWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, window_class.hInstance, this);
    if (window == nullptr) return false;
    ac_notification = RegisterPowerSettingNotification(window, &GUID_ACDC_POWER_SOURCE,
                                                       DEVICE_NOTIFY_WINDOW_HANDLE);
    saver_notification = RegisterPowerSettingNotification(
        window, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (ac_notification == nullptr || saver_notification == nullptr) {
      Stop();
      return false;
    }
    {
      std::scoped_lock lock(mutex);
      status = QueryInitialStatus();
      changed = status.has_value();
    }
    return true;
  }

  void Stop() {
    if (ac_notification != nullptr) {
      UnregisterPowerSettingNotification(ac_notification);
      ac_notification = nullptr;
    }
    if (saver_notification != nullptr) {
      UnregisterPowerSettingNotification(saver_notification);
      saver_notification = nullptr;
    }
    if (window != nullptr) {
      DestroyWindow(window);
      window = nullptr;
    }
  }

  void Update(const POWERBROADCAST_SETTING& setting) {
    if (setting.DataLength < sizeof(DWORD)) return;
    const DWORD value = *reinterpret_cast<const DWORD*>(setting.Data);
    std::scoped_lock lock(mutex);
    if (!status) status = PowerStatus{};
    if (setting.PowerSetting == GUID_ACDC_POWER_SOURCE) {
      status->on_battery = value != 0;
    } else if (setting.PowerSetting == GUID_POWER_SAVING_STATUS) {
      status->battery_saver_active = value != 0;
    } else {
      return;
    }
    changed = true;
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && message == WM_POWERBROADCAST &&
        w_param == PBT_POWERSETTINGCHANGE && l_param != 0) {
      self->Update(*reinterpret_cast<const POWERBROADCAST_SETTING*>(l_param));
      return TRUE;
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }

  mutable std::mutex mutex;
  std::optional<PowerStatus> status;
  bool changed = false;
  HWND window = nullptr;
  HPOWERNOTIFY ac_notification = nullptr;
  HPOWERNOTIFY saver_notification = nullptr;
};

PowerStatusMonitor::PowerStatusMonitor() : impl_(std::make_unique<Impl>()) {}

PowerStatusMonitor::~PowerStatusMonitor() { Stop(); }

bool PowerStatusMonitor::Start() { return impl_->Start(); }

void PowerStatusMonitor::Stop() { impl_->Stop(); }

std::optional<PowerStatus> PowerStatusMonitor::Current() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

std::optional<PowerStatus> PowerStatusMonitor::ConsumeChange() {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->changed) return std::nullopt;
  impl_->changed = false;
  return impl_->status;
}

}  // namespace minimize::platform
