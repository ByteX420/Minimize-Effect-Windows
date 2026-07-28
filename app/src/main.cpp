#include "pch.hpp"

#include <array>
#include <limits>
#include <wil/resource.h>

#include "app/application.hpp"
#include "platform/windows/process_runtime.hpp"
#include "platform/windows/single_instance_guard.hpp"
#include "platform/windows/update_installer.hpp"

#ifndef _DEBUG
#pragma comment(linker, "/subsystem:windows /ENTRY:wmainCRTStartup")
#endif

namespace {

std::optional<minimize::app::ApplicationLaunchOptions> ParseLaunchOptions(int argument_count,
                                                                          wchar_t* arguments[]) {
  minimize::app::ApplicationLaunchOptions options{};
  if (argument_count == 1) return options;
  if ((argument_count != 6 && argument_count != 11) ||
      std::wstring_view(arguments[1]) != L"--update-resume") {
    return std::nullopt;
  }
  std::array<long, 4> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    wchar_t* end = nullptr;
    values[index] = wcstol(arguments[index + 2], &end, 10);
    if (end == arguments[index + 2] || *end != L'\0') return std::nullopt;
  }
  if (values[2] <= values[0] || values[3] <= values[1]) {
    values[0] = 100;
    values[1] = 100;
    values[2] = 1100;
    values[3] = 750;
  }
  options.force_show_settings = true;
  options.initial_window_bounds = RECT{values[0], values[1], values[2], values[3]};
  if (argument_count == 11) {
    wchar_t* end = nullptr;
    const unsigned long parent = wcstoul(arguments[6], &end, 10);
    if (end == arguments[6] || *end != L'\0' || parent == 0 ||
        parent > std::numeric_limits<DWORD>::max() || arguments[7][0] == L'\0') {
      return std::nullopt;
    }
    options.update_parent_process_id = static_cast<DWORD>(parent);
    options.update_ready_event_name = arguments[7];
    const long page = wcstol(arguments[8], &end, 10);
    if (end == arguments[8] || *end != L'\0' || page < 0 || page > 7) return std::nullopt;
    const long scroll_milli = wcstol(arguments[9], &end, 10);
    if (end == arguments[9] || *end != L'\0' || scroll_milli < 0) return std::nullopt;
    const long maximized = wcstol(arguments[10], &end, 10);
    if (end == arguments[10] || *end != L'\0' || (maximized != 0 && maximized != 1)) {
      return std::nullopt;
    }
    options.initial_page = static_cast<int>(page);
    options.initial_page_scroll = static_cast<float>(scroll_milli) / 1000.0f;
    options.initial_maximized = maximized != 0;
  }
  return options;
}

}  // namespace

int wmain(int argument_count, wchar_t* arguments[]) {
  if (const auto installer_result =
          minimize::platform::windows::TryRunUpdateInstaller(argument_count, arguments)) {
    return *installer_result;
  }
  const auto launch_options = ParseLaunchOptions(argument_count, arguments);
  if (!launch_options) return ERROR_INVALID_PARAMETER;

  minimize::platform::windows::SingleInstanceGuard instance_guard;
  if (!launch_options->IsUpdateHandover()) {
    const auto instance_result = instance_guard.Acquire();
    if (instance_result == minimize::platform::windows::SingleInstanceResult::kAlreadyRunning) {
      (void)minimize::platform::windows::SingleInstanceGuard::ActivateExistingInstance(5000);
      return 0;
    }
    if (instance_result == minimize::platform::windows::SingleInstanceResult::kError) {
      std::wcerr << L"Could not create the single-instance guard: " << instance_guard.error()
                 << L"\n";
      return 1;
    }
  }

  minimize::platform::windows::ProcessRuntime process_runtime;
  if (!process_runtime.Initialize()) {
    std::wcerr << L"Process initialization failed: 0x" << std::hex << process_runtime.error()
               << L"\n";
    return 1;
  }

  minimize::app::Application application;
  process_runtime.SetShutdownHandler([&application] { application.RequestShutdown(); });

  if (!application.Initialize(GetModuleHandleW(nullptr), *launch_options)) {
    return 1;
  }

  if (launch_options->IsUpdateHandover()) {
    wil::unique_handle ready_event(
        OpenEventW(EVENT_MODIFY_STATE, FALSE, launch_options->update_ready_event_name.c_str()));
    if (ready_event) SetEvent(ready_event.get());
    wil::unique_handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                          launch_options->update_parent_process_id));
    if (parent) {
      const ULONGLONG handover_deadline = GetTickCount64() + 30000;
      DWORD parent_wait = WAIT_TIMEOUT;
      while (GetTickCount64() < handover_deadline) {
        const HANDLE parent_handle = parent.get();
        parent_wait = MsgWaitForMultipleObjects(1, &parent_handle, FALSE, 16, QS_ALLINPUT);
        if (parent_wait == WAIT_OBJECT_0 || parent_wait == WAIT_FAILED) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          if (message.message == WM_QUIT) {
            return 0;
          }
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        application.RenderUpdateHandoverFrame();
      }
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto instance_result = instance_guard.Acquire();
      if (instance_result == minimize::platform::windows::SingleInstanceResult::kPrimary) break;
      Sleep(50);
    }
    application.CompleteUpdateHandover();
  }

  return application.Run();
}
