#include "pch.hpp"

#include "features/window_recovery_service.hpp"

#include <cstdint>
#include <utility>

#include "core/logger.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"
#include "runtime/snapshot_cache.hpp"

namespace minimize::features {

WindowRecoveryService::WindowRecoveryService(runtime::SnapshotCache& snapshots)
    : snapshots_(snapshots) {}

void WindowRecoveryService::Restore(HWND window, bool force_show_if_iconic, bool activate) {
  if (!IsWindow(window)) return;
  restoring_ = true;

  (void)platform::SetOwnedWindowRegion(window, nullptr, true);
  const runtime::CachedSnapshot* snapshot = snapshots_.FindBest(window);
  const bool was_maximized =
      snapshot != nullptr
          ? snapshot->was_maximized
          : platform::windows::properties::WasMaximized(window);

  if (IsIconic(window) == FALSE || force_show_if_iconic) {
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowRestore);
    if (activate) {
      ShowWindow(window, was_maximized ? SW_SHOWMAXIMIZED : SW_RESTORE);
    } else if (was_maximized) {
      WINDOWPLACEMENT placement{};
      placement.length = sizeof(placement);
      bool placed = false;
      if (GetWindowPlacement(window, &placement)) {
        placement.showCmd = SW_SHOWMAXIMIZED;
        placed = SetWindowPlacement(window, &placement) != FALSE;
      }
      if (!placed) ShowWindow(window, SW_SHOWNOACTIVATE);
    } else {
      ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowRestore, false);
  }

  // Keep the real window alpha-hidden while its restored placement and first composed frame
  // settle. Layered Chromium windows can otherwise expose an incomplete fullscreen frame before
  // the Minimize overlay is removed.
  platform::SetWindowCloaked(window, false);
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
  DwmFlush();
  platform::windows::properties::ClearMinimizeState(window);
  DwmFlush();
  restoring_ = false;
}

void WindowRecoveryService::ReleaseWithoutShowing(HWND window, bool finish_as_minimized) {
  if (!IsWindow(window)) return;
  restoring_ = true;

  (void)platform::SetOwnedWindowRegion(window, nullptr, true);

  // Mid-minimize: Minimize has cloaked/transparent the window but native minimize may not have
  // finished. On shutdown we must finish as minimized — never SW_RESTORE those windows.
  if (finish_as_minimized && IsIconic(window) == FALSE &&
      platform::windows::properties::HasMinimizeState(window)) {
    platform::SetDwmTransitionsDisabled(window, true);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowMinimize);
    ShowWindow(window, SW_SHOWMINNOACTIVE);
    platform::windows::properties::SetFlag(
        window, platform::windows::properties::WindowFlag::kAllowMinimize, false);
    platform::SetDwmTransitionsDisabled(window, false);
  }

  platform::SetWindowCloaked(window, false);
  platform::windows::properties::RestoreTransparency(window);
  platform::windows::properties::ClearMinimizeState(window);
  restoring_ = false;
}

std::size_t WindowRecoveryService::HealLeftovers() {
  core::LogDebug(L"Recovery", L"Checking for leftover Minimize windows");
  std::size_t repaired_count = 0;
  std::pair<WindowRecoveryService*, std::size_t*> context{this, &repaired_count};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* context =
            reinterpret_cast<std::pair<WindowRecoveryService*, std::size_t*>*>(parameter);
        if (platform::windows::properties::HasMinimizeState(window)) {
          core::LogDebug(L"Recovery",
                         L"Restoring leftover window hwnd=0x" +
                             std::to_wstring(reinterpret_cast<std::uintptr_t>(window)));
          context->first->Restore(window, false);
          ++*context->second;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  return repaired_count;
}

void WindowRecoveryService::HealUntrackedWindows() {
  EnumWindows(
      [](HWND window, LPARAM) -> BOOL {
        if (platform::windows::properties::HasMinimizeState(window)) {
          platform::SetWindowCloaked(window, false);
          platform::windows::properties::RestoreTransparency(window);
          (void)platform::SetOwnedWindowRegion(window, nullptr, true);
          platform::windows::properties::ClearMinimizeState(window);
        }
        return TRUE;
      },
      0);
}

}  // namespace minimize::features
