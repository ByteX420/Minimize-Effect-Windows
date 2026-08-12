#include <dwmapi.h>
#include <format>
#include <iterator>
#include <string_view>
#include <windows.h>

#include "../app/src/core/logger.hpp"
#include "../app/src/platform/windows/window_properties.hpp"

namespace {

constexpr wchar_t kOverlayMessageName[] = L"MinimizeMinimizeAttempt";
constexpr wchar_t kRestoreMessageName[] = L"MinimizeRestoreAttempt";
constexpr wchar_t kSetWindowCloakMessageName[] = L"MinimizeSetWindowCloak";
constexpr wchar_t kOverlayClassName[] = L"MinimizeEffectOverlayWindow";

[[nodiscard]] constexpr bool IsMinimizeCommand(int show_cmd) noexcept {
  return show_cmd == SW_MINIMIZE || show_cmd == SW_SHOWMINIMIZED || show_cmd == SW_FORCEMINIMIZE ||
         show_cmd == SW_SHOWMINNOACTIVE;
}

[[nodiscard]] constexpr bool IsRestoreCommand(int show_cmd) noexcept {
  return show_cmd == SW_RESTORE || show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW ||
         show_cmd == SW_SHOWDEFAULT || show_cmd == SW_SHOWNA || show_cmd == SW_SHOWNOACTIVATE ||
         show_cmd == SW_SHOWMAXIMIZED || show_cmd == SW_MAXIMIZE;
}

[[nodiscard]] UINT GetMinimizeMessage() noexcept {
  static const UINT msg = RegisterWindowMessageW(kOverlayMessageName);
  return msg;
}

[[nodiscard]] UINT GetRestoreMessage() noexcept {
  static const UINT msg = RegisterWindowMessageW(kRestoreMessageName);
  return msg;
}

[[nodiscard]] UINT GetQueryWindowStateMessage() noexcept {
  static const UINT msg =
      RegisterWindowMessageW(minimize::platform::windows::properties::kQueryWindowStateMessage);
  return msg;
}

[[nodiscard]] bool QueryWindowState(HWND overlay, HWND target, std::uint32_t* out_state) noexcept {
  if (overlay == nullptr || target == nullptr || out_state == nullptr) return false;
  DWORD_PTR raw_state = 0;
  constexpr UINT kStateQueryTimeoutMs = 50;
  const LRESULT result =
      SendMessageTimeoutW(overlay, GetQueryWindowStateMessage(), reinterpret_cast<WPARAM>(target),
                          0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kStateQueryTimeoutMs, &raw_state);
  if (result == 0) return false;
  *out_state = static_cast<std::uint32_t>(raw_state);
  return true;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) noexcept {
  (void)lpReserved;
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
  }
  return TRUE;
}

namespace {

void LogMinMaxTrace(HWND target_window, int show_cmd) {
  if (!minimize::core::IsTraceLoggingEnabled()) return;
  wchar_t class_name[256]{};
  GetClassNameW(target_window, class_name, static_cast<int>(std::size(class_name)));
  wchar_t title[256]{};
  GetWindowTextW(target_window, title, static_cast<int>(std::size(title)));

  std::wstring_view cmd_name = L"UNKNOWN";
  if (IsMinimizeCommand(show_cmd)) {
    cmd_name = L"MINIMIZE";
  } else if (IsRestoreCommand(show_cmd)) {
    cmd_name = L"RESTORE";
  }

  minimize::core::LogTrace(L"HookDLL", std::format(L"CBT HCBT_MINMAX: hwnd={} cmd={} show_cmd={} "
                                                   L"class=\"{}\" title=\"{}\"",
                                                   static_cast<const void*>(target_window),
                                                   cmd_name, show_cmd, class_name, title));
}

bool TryHandleMinimize(HWND overlay_window, HWND target_window, LPARAM l_param,
                       std::uint32_t window_state) {
  const auto show_cmd = static_cast<int>(l_param & 0xFFFF);
  if (!IsMinimizeCommand(show_cmd)) return false;
  if ((window_state & minimize::platform::windows::properties::kHookStateExcluded) != 0) {
    minimize::core::LogTrace(L"HookDLL", L"Minimize allowed natively for excluded application");
    return false;
  }
  if ((window_state & minimize::platform::windows::properties::kHookStateAllowMinimize) != 0) {
    minimize::core::LogTrace(L"HookDLL", L"Minimize allowed by application window state");
    return false;
  }
  const UINT message = GetMinimizeMessage();
  if (overlay_window == nullptr || message == 0) return false;

  if (PostMessageW(overlay_window, message, reinterpret_cast<WPARAM>(target_window), l_param)) {
    minimize::core::LogTrace(
        L"HookDLL", L"PostMessage(MinimizeMinimizeAttempt) succeeded; blocking native minimize");
    return true;
  }
  const DWORD error = GetLastError();
  if (minimize::core::IsTraceLoggingEnabled()) {
    minimize::core::LogDebug(
        L"HookDLL", std::format(L"PostMessage(MinimizeMinimizeAttempt) failed error={}", error));
  }
  return false;
}

bool TryHandleRestore(HWND overlay_window, HWND target_window, LPARAM l_param,
                      std::uint32_t window_state) {
  const auto show_cmd = static_cast<int>(l_param & 0xFFFF);
  if (!IsRestoreCommand(show_cmd)) return false;
  if ((window_state & minimize::platform::windows::properties::kHookStateExcluded) != 0) {
    minimize::core::LogTrace(L"HookDLL", L"Restore allowed natively for excluded application");
    return false;
  }
  if ((window_state & minimize::platform::windows::properties::kHookStateAllowRestore) != 0) {
    minimize::core::LogTrace(L"HookDLL", L"Restore allowed by application window state");
    return false;
  }
  const UINT message = GetRestoreMessage();
  if (overlay_window == nullptr || message == 0) return false;

  DWORD_PTR handled = 0;
  constexpr UINT kRestoreMessageTimeoutMs = 75;
  const LRESULT send_result =
      SendMessageTimeoutW(overlay_window, message, reinterpret_cast<WPARAM>(target_window), l_param,
                          SMTO_ABORTIFHUNG, kRestoreMessageTimeoutMs, &handled);
  if (send_result == 0) {
    const DWORD error = GetLastError();
    if (minimize::core::IsTraceLoggingEnabled()) {
      minimize::core::LogDebug(
          L"HookDLL",
          std::format(L"SendMessageTimeout(MinimizeRestoreAttempt) failed error={}", error));
    }
  }
  return handled != 0;
}

}  // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int code, WPARAM w_param,
                                                          LPARAM l_param) noexcept {
  if (code != HCBT_MINMAX) {
    return CallNextHookEx(nullptr, code, w_param, l_param);
  }

  try {
    const auto show_cmd = static_cast<int>(l_param & 0xFFFF);
    const auto target_window = reinterpret_cast<HWND>(w_param);

    LogMinMaxTrace(target_window, show_cmd);

    const HWND overlay_window = FindWindowW(kOverlayClassName, nullptr);
    std::uint32_t window_state = 0;
    if (!QueryWindowState(overlay_window, target_window, &window_state)) {
      return CallNextHookEx(nullptr, code, w_param, l_param);
    }

    if (TryHandleMinimize(overlay_window, target_window, l_param, window_state)) {
      return 1;
    }
    if (TryHandleRestore(overlay_window, target_window, l_param, window_state)) {
      return 1;
    }
  } catch (...) {
    // Prevent any C++ exception from crossing the exported extern "C" DLL boundary.
  }

  return CallNextHookEx(nullptr, code, w_param, l_param);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK CallWndProc(int code, WPARAM w_param,
                                                              LPARAM l_param) noexcept {
  (void)w_param;
  if (code < 0 || l_param == 0) {
    return CallNextHookEx(nullptr, code, w_param, l_param);
  }

  const auto* message = reinterpret_cast<const CWPSTRUCT*>(l_param);
  const UINT set_window_cloak_message = RegisterWindowMessageW(kSetWindowCloakMessageName);
  if (set_window_cloak_message != 0 && message->message == set_window_cloak_message &&
      message->hwnd != nullptr) {
    const BOOL cloaked = message->wParam != 0 ? TRUE : FALSE;
    const HRESULT result =
        DwmSetWindowAttribute(message->hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
    if (FAILED(result)) {
      minimize::core::LogDebug(L"HookDLL",
                               L"DwmSetWindowAttribute(DWMWA_CLOAK) failed in target process");
    }
  }

  return CallNextHookEx(nullptr, code, w_param, l_param);
}
