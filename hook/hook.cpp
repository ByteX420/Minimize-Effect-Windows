#include <format>
#include <iterator>
#include <string_view>
#include <windows.h>

#include "../app/src/core/logger.hpp"
#include "../app/src/platform/windows/window_properties.hpp"

namespace {

constexpr wchar_t kOverlayMessageName[] = L"MinimizeMinimizeAttempt";
constexpr wchar_t kRestoreMessageName[] = L"MinimizeRestoreAttempt";
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
  static const UINT msg = RegisterWindowMessageW(
      minimize::platform::windows::properties::kQueryWindowStateMessage);
  return msg;
}

[[nodiscard]] bool QueryWindowState(HWND overlay, HWND target,
                                    std::uint32_t* out_state) noexcept {
  if (overlay == nullptr || target == nullptr || out_state == nullptr) return false;
  DWORD_PTR raw_state = 0;
  constexpr UINT kStateQueryTimeoutMs = 50;
  const LRESULT result =
      SendMessageTimeoutW(overlay, GetQueryWindowStateMessage(), reinterpret_cast<WPARAM>(target), 0,
                          SMTO_ABORTIFHUNG | SMTO_BLOCK, kStateQueryTimeoutMs, &raw_state);
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

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int code, WPARAM w_param,
                                                          LPARAM l_param) noexcept {
  if (code < 0) {
    return CallNextHookEx(nullptr, code, w_param, l_param);
  }

  try {
    if (code == HCBT_MINMAX) {
      const auto show_cmd = static_cast<int>(l_param & 0xFFFF);
      const auto target_window = reinterpret_cast<HWND>(w_param);

      if (minimize::core::IsTraceLoggingEnabled()) {
        wchar_t class_name[256]{};
        GetClassNameW(target_window, class_name, static_cast<int>(std::size(class_name)));
        wchar_t title[256]{};
        GetWindowTextW(target_window, title, static_cast<int>(std::size(title)));

        std::wstring_view cmd_name = L"UNKNOWN";
        if (IsMinimizeCommand(show_cmd))
          cmd_name = L"MINIMIZE";
        else if (IsRestoreCommand(show_cmd))
          cmd_name = L"RESTORE";

        minimize::core::LogTrace(
            L"HookDLL",
            std::format(L"CBT HCBT_MINMAX: hwnd={} cmd={} show_cmd={} class=\"{}\" title=\"{}\"",
                        static_cast<const void*>(target_window), cmd_name, show_cmd, class_name,
                        title));
      }

      const HWND overlay_window = FindWindowW(kOverlayClassName, nullptr);
      std::uint32_t window_state = 0;
      if (!QueryWindowState(overlay_window, target_window, &window_state)) {
        return CallNextHookEx(nullptr, code, w_param, l_param);
      }
      if (IsMinimizeCommand(show_cmd)) {
        if ((window_state & minimize::platform::windows::properties::kHookStateExcluded) != 0) {
          minimize::core::LogTrace(L"HookDLL",
                                   L"Minimize allowed natively for excluded application");
        } else if ((window_state &
                    minimize::platform::windows::properties::kHookStateAllowMinimize) == 0) {
          const UINT message = GetMinimizeMessage();
          if (overlay_window != nullptr && message != 0) {
            if (PostMessageW(overlay_window, message, reinterpret_cast<WPARAM>(target_window),
                             l_param)) {
              minimize::core::LogTrace(L"HookDLL",
                                       L"PostMessage(MinimizeMinimizeAttempt) succeeded; blocking "
                                       L"native minimize");
              return 1;
            }
            const DWORD error = GetLastError();
            if (minimize::core::IsTraceLoggingEnabled()) {
              minimize::core::LogDebug(
                  L"HookDLL",
                  std::format(L"PostMessage(MinimizeMinimizeAttempt) failed error={}", error));
            }
          }
        } else {
          minimize::core::LogTrace(L"HookDLL", L"Minimize allowed by application window state");
        }
      }

      if (IsRestoreCommand(show_cmd)) {
        if ((window_state & minimize::platform::windows::properties::kHookStateExcluded) != 0) {
          minimize::core::LogTrace(L"HookDLL",
                                   L"Restore allowed natively for excluded "
                                   L"application");
        } else if ((window_state &
                    minimize::platform::windows::properties::kHookStateAllowRestore) == 0) {
          const UINT message = GetRestoreMessage();
          if (overlay_window != nullptr && message != 0) {
            DWORD_PTR handled = 0;
            constexpr UINT kRestoreMessageTimeoutMs = 75;
            const LRESULT send_result = SendMessageTimeoutW(
                overlay_window, message, reinterpret_cast<WPARAM>(target_window), l_param,
                SMTO_ABORTIFHUNG, kRestoreMessageTimeoutMs, &handled);
            if (send_result == 0) {
              const DWORD error = GetLastError();
              if (minimize::core::IsTraceLoggingEnabled()) {
                minimize::core::LogDebug(
                    L"HookDLL",
                    std::format(L"SendMessageTimeout(MinimizeRestoreAttempt) failed error={}",
                                error));
              }
            }
            if (handled != 0) {
              return 1;
            }
          }
        } else {
          minimize::core::LogTrace(L"HookDLL", L"Restore allowed by application window state");
        }
      }
    }
  } catch (...) {
    // Prevent any C++ exception from crossing exported extern "C" DLL boundary
  }

  return CallNextHookEx(nullptr, code, w_param, l_param);
}
