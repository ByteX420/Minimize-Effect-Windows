#include "pch.hpp"

#include "ui/hotkey_presenter.hpp"
#include "ui/settings_window.hpp"
#include "ui/theme/theme.hpp"

namespace minimize::ui {
namespace {

constexpr int kMinimumWindowWidth = static_cast<int>(theme::Metrics::kWindowWidth);
constexpr int kMinimumWindowHeight = static_cast<int>(theme::Metrics::kWindowHeight);
constexpr float kHeaderHeight = theme::Metrics::kTitlebarHeight;
constexpr UINT kShowSettingsMessage = WM_APP + 101;
constexpr int kHotkeyBaseId = 4100;

bool IsTitlebarDragRegion(HWND window, POINT point, float scale) {
  RECT client{};
  GetClientRect(window, &client);
  const LONG traffic_lights_end = static_cast<LONG>(140.0f * scale);
  const LONG header_actions_start = client.right - static_cast<LONG>(220.0f * scale);
  return point.y >= 0 && point.y < static_cast<LONG>(kHeaderHeight * scale) &&
         point.x >= traffic_lights_end && point.x < header_actions_start;
}

}  // namespace

LRESULT CALLBACK SettingsWindow::WindowProc(HWND hwnd, UINT message, WPARAM w_param,
                                            LPARAM l_param) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* settings = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  const float scale = settings == nullptr ? 1.0f : settings->ui_scale_;

  if (settings != nullptr && settings->tray_icon_.IsTaskbarCreatedMessage(message)) {
    settings->tray_icon_.OnTaskbarCreated();
    if (!IsWindowVisible(hwnd)) {
      (void)settings->tray_icon_.Add(hwnd, settings->controller_->view_model());
      settings->HandleUpdateStateChanged();
    }
    return 0;
  }

  if (settings != nullptr && message == features::UpdateService::kStateChangedMessage) {
    settings->HandleUpdateStateChanged();
    return 0;
  }

  // Do not return HTCAPTION for the custom titlebar: the system's modal move loop would block
  // the runtime loop and freeze active minimize/restore overlays. A captured client drag keeps
  // normal message pumping and animation ticks alive while the menu is moved.
  if (settings != nullptr) {
    switch (message) {
      case WM_LBUTTONDOWN: {
        const POINT point{static_cast<short>(LOWORD(l_param)),
                          static_cast<short>(HIWORD(l_param))};
        if (!IsTitlebarDragRegion(hwnd, point, scale)) break;
        POINT cursor{};
        RECT bounds{};
        if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &bounds)) {
          settings->titlebar_dragging_ = true;
          settings->titlebar_drag_offset_ = {cursor.x - bounds.left, cursor.y - bounds.top};
          SetCapture(hwnd);
          settings->ForceRender();
          return 0;
        }
        break;
      }
      case WM_MOUSEMOVE: {
        if (!settings->titlebar_dragging_) break;
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
          SetWindowPos(hwnd, nullptr, cursor.x - settings->titlebar_drag_offset_.x,
                       cursor.y - settings->titlebar_drag_offset_.y, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        settings->ForceRender();
        return 0;
      }
      case WM_LBUTTONUP:
        if (!settings->titlebar_dragging_) break;
        settings->titlebar_dragging_ = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
      case WM_CAPTURECHANGED:
      case WM_CANCELMODE:
        if (!settings->titlebar_dragging_) break;
        settings->titlebar_dragging_ = false;
        return 0;
      case WM_LBUTTONDBLCLK: {
        const POINT point{static_cast<short>(LOWORD(l_param)),
                          static_cast<short>(HIWORD(l_param))};
        if (!IsTitlebarDragRegion(hwnd, point, scale)) break;
        ShowWindow(hwnd, IsZoomed(hwnd) != FALSE ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
      }
    }
  }

  if (settings != nullptr && settings->editing_hotkey_ >= 0 &&
      (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
    const UINT virtual_key = static_cast<UINT>(w_param);
    if (virtual_key == VK_ESCAPE) {
      settings->editing_hotkey_ = -1;
      settings->hotkey_feedback_ = "Hotkey edit cancelled";
      settings->ForceRender();
      return 0;
    }
    if (virtual_key == VK_CONTROL || virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL ||
        virtual_key == VK_MENU || virtual_key == VK_LMENU || virtual_key == VK_RMENU ||
        virtual_key == VK_SHIFT || virtual_key == VK_LSHIFT || virtual_key == VK_RSHIFT ||
        virtual_key == VK_LWIN || virtual_key == VK_RWIN) {
      return 0;
    }
    std::uint32_t modifiers = 0;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= MOD_CONTROL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= MOD_ALT;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) {
      modifiers |= MOD_WIN;
    }
    const minimize::settings::HotkeyAction action =
        static_cast<minimize::settings::HotkeyAction>(settings->editing_hotkey_);
    const ui::HotkeyUpdateResult result =
        settings->controller_ != nullptr
            ? settings->controller_->actions().SetHotkey(
                  action, minimize::settings::HotkeyBinding{.modifiers = modifiers,
                                                            .virtual_key = virtual_key})
            : ui::HotkeyUpdateResult::kInvalid;
    settings->hotkey_feedback_ = ui::HotkeyUpdateMessage(result);
    if (result == ui::HotkeyUpdateResult::kSuccess) settings->editing_hotkey_ = -1;
    settings->ForceRender();
    return 0;
  }

  if (message == ui::TrayIcon::kCallbackMessage && settings != nullptr) {
    const ui::TrayCommand command =
        settings->tray_icon_.HandleCallback(hwnd, l_param, settings->controller_->view_model());
    switch (command) {
      case ui::TrayCommand::kShowSettings:
        settings->Show(true);
        break;
      case ui::TrayCommand::kToggleEnabled:
        if (settings->controller_ != nullptr)
          settings->controller_->actions().SetEnabled(!settings->controller_->view_model().enabled);
        break;
      case ui::TrayCommand::kResume:
        if (settings->controller_ != nullptr)
          settings->controller_->actions().SetTemporaryPause(ui::TemporaryPauseAction::kResume);
        break;
      case ui::TrayCommand::kPauseTenMinutes:
        if (settings->controller_ != nullptr)
          settings->controller_->actions().SetTemporaryPause(ui::TemporaryPauseAction::kTenMinutes);
        break;
      case ui::TrayCommand::kPauseOneHour:
        if (settings->controller_ != nullptr)
          settings->controller_->actions().SetTemporaryPause(ui::TemporaryPauseAction::kOneHour);
        break;
      case ui::TrayCommand::kPauseUntilRestart:
        if (settings->controller_ != nullptr)
          settings->controller_->actions().SetTemporaryPause(
              ui::TemporaryPauseAction::kUntilRestart);
        break;
      case ui::TrayCommand::kRepairWindows:
        if (settings->controller_ != nullptr) settings->controller_->actions().HealWindows();
        break;
      case ui::TrayCommand::kExit:
        if (settings->controller_ != nullptr) settings->controller_->actions().RequestExit();
        break;
      case ui::TrayCommand::kNone:
        break;
    }
    return 0;
  }

  if (settings != nullptr && settings->renderer_.ready()) {
    bool needs_render = false;
    switch (message) {
      case WM_MOUSEMOVE:
      case WM_MOUSELEAVE:
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_CHAR:
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
      case WM_CAPTURECHANGED:
      case WM_CANCELMODE:
      case WM_PAINT:
        needs_render = true;
        break;
    }
    const bool imgui_handled =
        settings->renderer_.HandleWin32Message(hwnd, message, w_param, l_param);
    if (needs_render) settings->ForceRender();
    if (imgui_handled) {
      return TRUE;
    }
  }

  switch (message) {
    case WM_HOTKEY: {
      if (settings != nullptr && settings->editing_hotkey_ >= 0) return 0;
      const int index = static_cast<int>(w_param) - kHotkeyBaseId;
      if (settings != nullptr && index >= 0 &&
          index < static_cast<int>(minimize::settings::HotkeyAction::kCount) &&
          settings->controller_ != nullptr) {
        settings->controller_->actions().ExecuteHotkeyAction(
            static_cast<minimize::settings::HotkeyAction>(index));
      }
      return 0;
    }
    case kShowSettingsMessage:
      if (settings != nullptr) settings->Show(true);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
      const UINT dpi = settings == nullptr ? USER_DEFAULT_SCREEN_DPI : settings->current_dpi_;
      limits->ptMinTrackSize.x =
          MulDiv(kMinimumWindowWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
      limits->ptMinTrackSize.y =
          MulDiv(kMinimumWindowHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
      return 0;
    }
    case WM_TIMER:
      if (settings != nullptr && w_param == ui::TrayIcon::kRetryTimerId) {
        (void)settings->tray_icon_.Add(hwnd, settings->controller_->view_model());
        settings->HandleUpdateStateChanged();
        return 0;
      }
      return DefWindowProcW(hwnd, message, w_param, l_param);
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      if (settings != nullptr) settings->ForceRender();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(l_param);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      if (settings != nullptr) {
        settings->UpdateDpi(LOWORD(w_param));
        settings->ForceRender();
      }
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (settings != nullptr) {
        settings->UpdateReducedMotion();
        settings->ForceRender();
      }
      return 0;
    case WM_SIZE:
      if (settings != nullptr && w_param != SIZE_MINIMIZED) {
        const UINT width = LOWORD(l_param);
        const UINT height = HIWORD(l_param);
        settings->renderer_.Resize(width, height);
        settings->ApplyWindowShape(static_cast<int>(width), static_cast<int>(height));
        settings->ForceRender();
      }
      return 0;
    case WM_CLOSE:
      if (settings != nullptr) {
        settings->HandleCloseRequest();
      }
      return 0;
    case WM_NCHITTEST:
      return HTCLIENT;
    default:
      return DefWindowProcW(hwnd, message, w_param, l_param);
  }
}

}  // namespace minimize::ui
