#include "pch.hpp"

#include "app/application_runtime.hpp"

#include <atomic>
#include <iostream>
#include <string>
#include <string_view>
#include <wil/resource.h>

#include "animation/geometry.hpp"
#include "app/application.hpp"
#include "core/environment.hpp"
#include "core/logger.hpp"
#include "platform/windows/app_container_permissions.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/power_status.hpp"
#include "platform/windows/process_info.hpp"
#include "platform/windows/startup_manager.hpp"
#include "platform/windows/window_diagnostics.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "settings/exclusion_rules.hpp"
#include "settings/settings_service.hpp"

namespace minimize::app {

ApplicationRuntime::~ApplicationRuntime() { CleanupAndRestoreAll(); }

bool ApplicationRuntime::Initialize(HINSTANCE instance, const ApplicationLaunchOptions& options) {
  minimize::core::CleanupDebugLogs();
  instance_ = instance;
  main_thread_id_ = GetCurrentThreadId();
  elevation_handover_pending_ = options.IsElevationHandover();
#ifdef _DEBUG
  device_recovery_test_pending_ = core::EnvironmentFlagEnabled("MINIMIZE_TEST_DEVICE_RECOVERY");
#endif
  (void)settings_service_.Load();
  effect_policy_.Configure(settings_service_.Get());
  window_exclusion_service_.SetExcludedDisplays(settings_service_.Get().excluded_displays);
  if (settings_service_.Get().run_at_startup && !platform::windows::ConfigureRunAtStartup(true)) {
    minimize::core::LogDebug(L"Startup",
                             L"Could not repair the per-user startup entry; disabling the option");
    auto repaired_settings = settings_service_.Get();
    repaired_settings.run_at_startup = false;
    if (!settings_service_.Update(std::move(repaired_settings))) {
      minimize::core::LogDebug(L"Startup", L"Could not persist the repaired startup state");
    }
  }
#ifdef _DEBUG
  // Touch log file and grant permissions so AppContainers can write to it
  {
    const std::wstring& log_path = minimize::core::DebugLogPath();
    wil::unique_hfile file(CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file) {
      (void)platform::GrantAppContainerPermissions(log_path);
    }
  }
#endif

  minimize::core::LogDebug(L"App", L"ApplicationRuntime::Initialize started");
  minimize::core::LogTrace(L"App", L"ApplicationRuntime::Initialize started");

  frame_scheduler_.Initialize();

  if (!platform::IsCurrentProcessElevated()) {
    std::wcerr << L"WARNING: Not running as Administrator. Elevated windows (like Task Manager, "
                  L"cmd as Admin, etc.)\n"
               << L"         will NOT be hooked due to Windows UIPI security restrictions.\n"
               << L"         To hook all windows, please run MinimizeEffect.exe as "
                  L"Administrator.\n\n";
    minimize::core::LogDebug(L"App", L"Warning: Not running as Administrator");
  } else {
    minimize::core::LogDebug(L"App", L"Running as Administrator");
  }

  if (!settings_window_.Initialize(instance, *this)) {
    return false;
  }
  if (options.initial_window_bounds) {
    settings_window_.SetInitialBounds(*options.initial_window_bounds);
  }
  if (options.IsUpdateHandover()) {
    settings_window_.PrepareUpdateResume(options.initial_page, options.initial_page_scroll,
                                         options.initial_maximized);
  }
  settings_window_.UpdateState(settings_service_.Get());
  if (!options.IsUpdateHandover()) {
    settings_window_.RestoreUiState(settings_service_.Get().ui_window);
  }
  if (!options.IsHandover() && !StartRuntimeServices()) return false;

  // During a handover the replacement process paints this window first and starts hooks only
  // after the old process has acknowledged the frame and exited.
  if (!options.IsElevationHandover()) {
    settings_window_.Show(options.force_show_settings || !settings_service_.Get().start_minimized ||
                          settings_service_.Get().close_behavior != "tray");
  }

  std::wcout << L"Minimize minimize monitor is running.\n";
  minimize::core::LogTrace(L"App", L"ApplicationRuntime::Initialize completed");
  std::wcout << L"Set MINIMIZE_TASKBAR_RECT=left,top,right,bottom to aim at a "
                L"custom taskbar rectangle.\n";
  std::wcout << L"Close this console window to stop Minimize Effect and restore any active "
                L"window overrides.\n";
  return true;
}

bool ApplicationRuntime::StartRuntimeServices() {
  if (runtime_services_started_) return true;
  if (!power_status_monitor_.Start()) {
    minimize::core::LogDebug(L"Power", L"Power setting notifications could not be registered");
  }
  HealLeftoverWindows();
  if (!CreateAnimationRenderer()) return false;
  hotkey_controller_.SetWindow(settings_window_.hwnd());
  RegisterConfiguredHotkeys();
  settings_window_.UpdatePauseState(false, false);
  UpdateFullscreenSuppression(true);
  UpdatePowerState(true);
  RefreshEffectRuntimeState();

  if (!effect_controller_.Start([this](HWND window) { return OnMinimizeStart(window); },
                                [this](HWND window) { return OnRestoreAttempt(window); },
                                [this](HWND window, DWORD event) {
                                  effect_controller_.HandleWindowSeen(
                                      window, event, GetOverlayWindow(),
                                      renderer_recovery_.pending(), native_animation_blocker_,
                                      desktop_capture_.get(),
                                      [this](HWND target) { return OnRestoreAttempt(target); });
                                })) {
    return false;
  }
  runtime_services_started_ = true;
  seed_iconic_snapshots_pending_ = true;
  return true;
}

int ApplicationRuntime::Run() {
  return message_loop_.Run(MessageLoopCallbacks{
      .should_stop = [this] { return shutting_down_.load(std::memory_order_acquire); },
      .update = [this] { UpdateRuntime(); },
      .display_changed = [this] { HandleDisplayChange(); },
      .render_settings = [this] { settings_window_.Render(); },
      .tick_runtime = [this] { return TickRuntime(); },
      .wait_for_animation = [this] { WaitForAnimationFrameOrMessage(); },
      .wait_for_idle_frame = [this] { WaitForIdleFrame(); },
  });
}

void ApplicationRuntime::RenderUpdateHandoverFrame() { settings_window_.Render(); }

void ApplicationRuntime::PrepareForUpdateHandover() {
  if (update_handover_prepared_.exchange(true, std::memory_order_acq_rel)) return;
  UnregisterAllHotkeys();
  DisableEffectRuntime();
  frame_scheduler_.Wake();
}

void ApplicationRuntime::ResumeAfterUpdateHandoverFailure() {
  if (!update_handover_prepared_.exchange(false, std::memory_order_acq_rel)) return;
  RegisterConfiguredHotkeys();
  RefreshEffectRuntimeState();
  frame_scheduler_.Wake();
}

bool ApplicationRuntime::CompleteUpdateHandover() {
  if (!StartRuntimeServices()) {
    minimize::core::LogDebug(L"Update", L"Could not start runtime services after handover");
    return false;
  }
  if (elevation_handover_pending_) {
    elevation_handover_pending_ = false;
    settings_window_.Show(true);
  } else {
    settings_window_.CompleteUpdateHandover();
  }
  update_handover_prepared_.store(false, std::memory_order_release);
  frame_scheduler_.Wake();
  return true;
}

void ApplicationRuntime::RequestShutdown() {
  shutting_down_.store(true, std::memory_order_release);
  frame_scheduler_.Wake();
  // PostQuitMessage works when called from the main thread (settings UI click path).
  // PostThreadMessage covers the same queue if we are already on it, and is the
  // wake path for console-control / other-thread shutdown requests.
  PostQuitMessage(0);
  if (main_thread_id_ != 0) {
    PostThreadMessageW(main_thread_id_, WM_QUIT, 0, 0);
  }
}

}  // namespace minimize::app
