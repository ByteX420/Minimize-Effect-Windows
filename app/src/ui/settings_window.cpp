#include "pch.hpp"

#include "ui/settings_window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <dwmapi.h>
#include <format>

#include "core/logger.hpp"
#include "imgui.h"
#include "settings/app_settings.hpp"
#include "ui/components/page_layout.hpp"
#include "ui/hotkey_presenter.hpp"
#include "ui/pages/about_page.hpp"
#include "ui/pages/animation_page.hpp"
#include "ui/pages/applications_page.hpp"
#include "ui/pages/diagnostics_page.hpp"
#include "ui/pages/general_page.hpp"
#include "ui/pages/hotkeys_page.hpp"
#include "ui/pages/windows_integration_page.hpp"
#include "ui/settings_shell.hpp"
#include "ui/theme/theme.hpp"
#include "ui/theme/theme_tokens.hpp"

namespace minimize::ui {
namespace {

constexpr wchar_t kSettingsWindowClass[] = L"MinimizeEffectImGuiSettings";
constexpr int kWindowWidth = static_cast<int>(theme::Metrics::kWindowWidth);
constexpr int kWindowHeight = static_cast<int>(theme::Metrics::kWindowHeight);
constexpr int kMinimumWindowWidth = kWindowWidth;
constexpr int kMinimumWindowHeight = kWindowHeight;
constexpr float kHeaderHeight = theme::Metrics::kTitlebarHeight;
constexpr UINT kShowSettingsMessage = WM_APP + 101;
constexpr int kHotkeyBaseId = 4100;
// Inter type scale (SIL OFL 1.1). Integer px sizes only — half-pixels blur glyphs.

bool SystemUiAnimationsEnabled() {
  BOOL enabled = TRUE;
  return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != FALSE &&
         enabled != FALSE;
}

bool SystemTransparencyEnabled() {
  DWORD enabled = 1;
  DWORD size = sizeof(enabled);
  const LSTATUS result = RegGetValueW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &enabled, &size);
  return result != ERROR_SUCCESS || enabled != 0;
}

using theme::WithAlpha;

}  // namespace

SettingsWindow::~SettingsWindow() { Shutdown(); }

bool SettingsWindow::Initialize(HINSTANCE instance, ui::SettingsActions& actions) {
  controller_ = std::make_unique<ui::SettingsController>(actions);
  if (!CreateRenderWindow(instance) || !renderer_.Initialize(hwnd_)) return false;
  current_dpi_ = renderer_.dpi();
  ui_scale_ = renderer_.scale();
  font_small_ = renderer_.small_font();
  font_body_ = renderer_.body_font();
  font_medium_ = renderer_.medium_font();
  font_title_ = renderer_.title_font();
  tray_icon_.Initialize();
  UpdateReducedMotion();
  update_service_.Start(hwnd_);
  return true;
}

void SettingsWindow::PrepareUpdateResume(int page, float page_scroll, bool maximized) {
  selected_page_ = static_cast<Page>(std::clamp(page, 0, 7));
  initial_page_scroll_ = std::max(0.0f, page_scroll);
  initial_maximized_ = maximized;
  update_workspace_engaged_ = true;
  update_resume_active_ = true;
  update_installer_started_ = true;
  motion_system_.Set(ui::motion::MotionKey("update", "workspace", "show"), 1.0f);
  motion_system_.Set(ui::motion::MotionKey("update", "workspace", "loader"), 1.0f);
  motion_system_.Set(ui::motion::MotionKey("update", "shell", "content"), 0.0f);
}

void SettingsWindow::CompleteUpdateHandover() {
  update_resume_active_ = false;
  update_workspace_engaged_ = false;
  update_installer_started_ = false;
  motion_system_.Set(ui::motion::MotionKey("window", "settings", "alpha"), 1.0f);
  motion_system_.Set(ui::motion::MotionKey("window", "settings", "offset"), ImVec2(0.0f, 0.0f));
  motion_system_.Set(ui::motion::MotionKey("page", "content", "alpha"), 0.0f);
  motion_system_.Set(ui::motion::MotionKey("page", "content", "offset"), ImVec2(0.0f, 10.0f));
  ForceRender();
}

void SettingsWindow::Shutdown() {
  update_service_.Stop();
  FlushPendingSpeedSave();
  animation_preview_.Close();
  tray_icon_.Remove(hwnd_);
  renderer_.Shutdown();
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  controller_.reset();
}

void SettingsWindow::Show(bool show) {
  if (hwnd_ == nullptr) return;
  if (!show) FlushPendingSpeedSave();
  if (show) {
    motion_system_.Set(ui::motion::MotionKey("window", "settings", "alpha"), 0.0f);
    motion_system_.Set(ui::motion::MotionKey("window", "settings", "offset"), ImVec2(0.0f, 6.0f));
    // Block deferred startup work until shell/sidebar/page enter tracks settle.
    startup_enter_motion_active_ = true;
    startup_enter_motion_seen_ = false;
    if (IsIconic(hwnd_)) {
      ShowWindow(hwnd_, SW_RESTORE);
    }
    POINT cursor_pos{};
    GetCursorPos(&cursor_pos);
    HMONITOR monitor = MonitorFromPoint(cursor_pos, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
      RECT rect{};
      GetWindowRect(hwnd_, &rect);
      const int w = rect.right - rect.left;
      const int h = rect.bottom - rect.top;
      const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - w) / 2;
      const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - h) / 2;
      SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(hwnd_, SW_SHOW);
    tray_icon_.Remove(hwnd_);
  } else {
    ShowWindow(hwnd_, SW_HIDE);
    if (!tray_icon_.Add(hwnd_, controller_->view_model())) {
      // Never leave the settings inaccessible if Explorer rejects the icon.
      ShowWindow(hwnd_, SW_SHOW);
      ForceRender();
      SetForegroundWindow(hwnd_);
      return;
    }
  }
  if (!show) {
    render_requested_ = false;
    return;
  }

  shown_at_ms_ = GetTickCount64();
  ForceRender();
  SetForegroundWindow(hwnd_);
  Render();
}

void SettingsWindow::UpdateState(const minimize::settings::AppSettings& settings) {
  const bool enabled_changed = controller_->view_model().enabled != settings.enabled;
  const bool changed =
      enabled_changed ||
      std::abs(controller_->view_model().minimize_duration - settings.minimize_duration) >
          0.0001f ||
       std::abs(controller_->view_model().restore_duration - settings.restore_duration) > 0.0001f ||
       std::abs(controller_->view_model().cancel_duration - settings.cancel_duration) > 0.0001f ||
      controller_->view_model().link_speeds != settings.link_speeds ||
      controller_->view_model().disable_animations_fullscreen !=
          settings.disable_animations_fullscreen ||
      controller_->view_model().disable_effects_battery_saver !=
          settings.disable_effects_battery_saver ||
       controller_->view_model().minimize_easing != settings.minimize_easing ||
       controller_->view_model().restore_easing != settings.restore_easing ||
       controller_->view_model().cancel_easing != settings.cancel_easing ||
       controller_->view_model().minimize_custom_bezier != settings.minimize_custom_bezier ||
       controller_->view_model().restore_custom_bezier != settings.restore_custom_bezier ||
       controller_->view_model().cancel_custom_bezier != settings.cancel_custom_bezier ||
      controller_->view_model().animation_style != settings.animation_style ||
      controller_->view_model().quality_mode != settings.quality_mode ||
      std::abs(controller_->view_model().minimize_strength - settings.minimize_strength) > 0.0001f ||
      controller_->view_model().fade_strength != settings.fade_strength ||
      controller_->view_model().show_target_indicator != settings.show_target_indicator ||
      controller_->view_model().close_behavior != settings.close_behavior ||
      controller_->view_model().run_at_startup != settings.run_at_startup ||
      controller_->view_model().start_minimized != settings.start_minimized ||
      controller_->view_model().excluded_applications != settings.excluded_applications ||
      controller_->view_model().hotkeys != settings.hotkeys;
  controller_->view_model().Apply(settings);
  if (enabled_changed) tray_icon_.UpdateTooltip(hwnd_, controller_->view_model());
  if (changed) ForceRender();
}

void SettingsWindow::UpdatePauseState(bool paused, bool until_restart) {
  if (controller_->view_model().temporarily_paused == paused &&
      controller_->view_model().paused_until_restart == until_restart)
    return;
  controller_->view_model().SetPause(paused, until_restart);
  tray_icon_.UpdateTooltip(hwnd_, controller_->view_model());
  ForceRender();
}

void SettingsWindow::SetHotkeyRegistrationStatus(minimize::settings::HotkeyAction action,
                                                 bool available) {
  const size_t index = static_cast<size_t>(action);
  if (index >= controller_->view_model().hotkey_available.size() ||
      controller_->view_model().hotkey_available[index] == available)
    return;
  controller_->view_model().SetHotkeyAvailability(action, available);
  ForceRender();
}

void SettingsWindow::FlushPendingSpeedSave() {
  const bool speeds_pending = minimize_slider_dirty_ || restore_slider_dirty_ ||
                              cancel_slider_dirty_ || minimize_slider_active_ ||
                              restore_slider_active_ || cancel_slider_active_;
  if (speeds_pending) {
    const bool saved =
        controller_ == nullptr || controller_->actions().SetAnimationDurations(
                                       controller_->view_model().minimize_duration,
                                       controller_->view_model().restore_duration,
                                       controller_->view_model().cancel_duration, true);
    RecordSaveResult(saved);
    if (saved) {
      minimize_slider_dirty_ = false;
      restore_slider_dirty_ = false;
      cancel_slider_dirty_ = false;
    }
  }
  const bool strength_pending = strength_slider_dirty_ || strength_slider_active_;
  if (strength_pending) {
    const bool saved =
        controller_ == nullptr ||
        controller_->actions().SetMinimizeStrength(controller_->view_model().minimize_strength, true);
    RecordSaveResult(saved);
    if (saved) strength_slider_dirty_ = false;
  }
  minimize_slider_active_ = false;
  restore_slider_active_ = false;
  cancel_slider_active_ = false;
  strength_slider_active_ = false;
}

void SettingsWindow::RecordSaveResult(bool saved) {
  // Restart toast enter animation from 0 each time so rapid saves re-trigger cleanly.
  auto& motion = motion_system_;
  motion.Set(ui::motion::MotionKey("toast", "save", "show"), 0.0f);
  if (saved) {
    persistence_error_.clear();
    save_feedback_ = "Saved";
    save_feedback_until_ms_ = GetTickCount64() + 2200;
    save_feedback_error_ = false;
  } else {
    persistence_error_ = "Could not save settings. Check folder permissions or disk space.";
    save_feedback_ = "Could not save settings";
    save_feedback_until_ms_ = GetTickCount64() + 5500;
    save_feedback_error_ = true;
    minimize::core::LogDebug(L"Settings", L"Settings window could not persist the requested change");
  }
  ForceRender();
}

void SettingsWindow::RecordFileOperationResult(const SettingsFileOperationResult& result) {
  if (result.result == SettingsFileResult::kCancelled) {
    // User closed the picker — silent, not an error toast.
    return;
  }
  auto& motion = motion_system_;
  motion.Set(ui::motion::MotionKey("toast", "save", "show"), 0.0f);
  if (result.result == SettingsFileResult::kSuccess) {
    persistence_error_.clear();
    save_feedback_ = result.message.empty() ? "Saved" : result.message;
    save_feedback_until_ms_ = GetTickCount64() + (result.is_error ? 5500 : 2200);
    save_feedback_error_ = result.is_error;
  } else {
    persistence_error_ = result.message.empty()
                             ? "Could not save settings. Check folder permissions or disk space."
                             : result.message;
    save_feedback_ = result.message.empty() ? "Could not save settings" : result.message;
    save_feedback_until_ms_ = GetTickCount64() + 5500;
    save_feedback_error_ = true;
    minimize::core::LogDebug(L"Settings", L"Settings file operation failed");
  }
  ForceRender();
}

void SettingsWindow::HandleCloseRequest() {
  // close_behavior "tray" only hides the window. Everything else must exit the
  // process (same contract as 795f55b2 — close button exits the app).
  if (controller_->view_model().close_behavior == "tray") {
    Show(false);
    return;
  }

  // Hide immediately so the click feels responsive while shutdown runs.
  if (hwnd_ != nullptr && IsWindow(hwnd_)) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (controller_ != nullptr) {
    controller_->actions().RequestExit();
  } else {
    // Last resort if the host did not wire ExitCallback.
    PostQuitMessage(0);
  }
}

bool SettingsWindow::CreateRenderWindow(HINSTANCE instance) {
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  window_class.lpszClassName = kSettingsWindowClass;
  RegisterClassExW(&window_class);

  const DPI_AWARENESS_CONTEXT old_context =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const UINT dpi = GetDpiForSystem();
  const int width = MulDiv(kWindowWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  const int height = MulDiv(kWindowHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kSettingsWindowClass, L"Minimize Effect",
                          WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU, CW_USEDEFAULT,
                          CW_USEDEFAULT, width, height, nullptr, nullptr, instance, this);
  if (old_context != nullptr) SetThreadDpiAwarenessContext(old_context);
  if (hwnd_ == nullptr) return false;
  ChangeWindowMessageFilterEx(hwnd_, kShowSettingsMessage, MSGFLT_ALLOW, nullptr);

  current_dpi_ = GetDpiForWindow(hwnd_);
  ui_scale_ = static_cast<float>(current_dpi_) / USER_DEFAULT_SCREEN_DPI;
  ApplyWindowShape(width, height);
  const DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                        sizeof(corner_preference));
  const MARGINS margins{-1};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
  const BOOL dark_mode = TRUE;
  DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark_mode,
                        sizeof(dark_mode));
  const DWORD mica_backdrop = 2;  // DWMSBT_MAINWINDOW, matching ImGuiBase.
  if (FAILED(DwmSetWindowAttribute(hwnd_, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &mica_backdrop,
                                   sizeof(mica_backdrop)))) {
    const DWORD legacy_mica = 1;
    DwmSetWindowAttribute(hwnd_, 1029 /* DWMWA_MICA_EFFECT */, &legacy_mica, sizeof(legacy_mica));
  }
  return true;
}

void SettingsWindow::ApplyWindowShape(int width, int height) {
  (void)width;
  (void)height;
  SetWindowRgn(hwnd_, nullptr, TRUE);
}

void SettingsWindow::UpdateDpi(UINT dpi) {
  renderer_.UpdateDpi(dpi);
  current_dpi_ = renderer_.dpi();
  ui_scale_ = renderer_.scale();
  font_small_ = renderer_.small_font();
  font_body_ = renderer_.body_font();
  font_medium_ = renderer_.medium_font();
  font_title_ = renderer_.title_font();
}

void SettingsWindow::UpdateReducedMotion() {
  const bool reduced = !SystemUiAnimationsEnabled();
  motion_system_.SetReducedMotion(reduced);
  motion_tokens_ =
      reduced ? ui::motion::MotionTokens::Reduced() : ui::motion::MotionTokens::Default();
}

void SettingsWindow::HandleUpdateStateChanged() {
  const features::UpdateSnapshot update = update_service_.GetSnapshot();
  if (update.phase == features::UpdatePhase::kAvailable &&
      update_notified_version_ != update.latest_version) {
    update_card_dismissed_ = false;
    update_notified_version_ = update.latest_version;
  }
  if (update.phase == features::UpdatePhase::kError && update_workspace_engaged_ &&
      !update_resume_active_ && !update_installer_started_) {
    update_workspace_engaged_ = false;
  }
  ForceRender();
}

bool SettingsWindow::DetectStartupEnterMotionActive() const {
  const auto& motion = motion_system_;
  // Window shell enter, first page content enter, brand/status, sidebar item reveals,
  // and staggered page-layout reveals — not hover/press (sidebar-main / controls).
  if (motion.IsActive(ui::motion::MotionKey("window", "settings", "alpha"))) return true;
  if (motion.IsActive(ui::motion::MotionKey("window", "settings", "offset"))) return true;
  if (motion.IsActive(ui::motion::MotionKey("page", "content", "alpha"))) return true;
  if (motion.IsActive(ui::motion::MotionKey("page", "content", "offset"))) return true;
  if (motion.AnyActiveWithPrefix("shell/")) return true;
  if (motion.AnyActiveWithPrefix("layout/")) return true;
  // Sidebar tab reveals use "sidebar/##settings_page_N/reveal" (not "sidebar-main/...").
  if (motion.AnyActiveWithPrefix("sidebar/")) return true;
  return false;
}

void SettingsWindow::UpdateStartupEnterMotionGate() {
  if (!startup_enter_motion_active_) return;
  if (DetectStartupEnterMotionActive()) {
    startup_enter_motion_seen_ = true;
    return;
  }
  // Wait until enter tracks have started at least once, then fully settled.
  if (startup_enter_motion_seen_) {
    startup_enter_motion_active_ = false;
  }
}

void SettingsWindow::Render() {
  animation_preview_.Update(hwnd_, controller_->view_model().minimize_duration,
                            controller_->view_model().restore_duration);
  if (!IsWindowVisible(hwnd_)) return;
  const ULONGLONG now_ms = GetTickCount64();
  const bool is_animating = startup_enter_motion_active_ || (now_ms - shown_at_ms_ < 500);
  const bool feedback_active = !save_feedback_.empty() && now_ms < save_feedback_until_ms_;
  const bool update_active = update_workspace_engaged_ || update_resume_active_;
  if (!is_animating && !titlebar_dragging_ && !animation_preview_.active() && !feedback_active &&
      !update_active && !motion_system_.HasActiveTracks() && !render_requested_)
    return;
  render_requested_ = false;
  if (!renderer_.BeginFrame()) {
    render_requested_ = true;
    return;
  }
  motion_system_.BeginFrame(ImGui::GetIO().DeltaTime);
  SettingsShell::Render(*this);
  UpdateStartupEnterMotionGate();
  if (!renderer_.EndFrame()) render_requested_ = true;
}

void SettingsWindow::ForceRender() { render_requested_ = true; }

bool SettingsWindow::WantsContinuousRendering() const {
  if (!renderer_.ready() || hwnd_ == nullptr || !IsWindowVisible(hwnd_)) {
    return false;
  }
  return startup_enter_motion_active_ || animation_preview_.active() ||
         update_workspace_engaged_ || update_resume_active_ ||
         titlebar_dragging_ || motion_system_.HasActiveTracks() || render_requested_;
}

}  // namespace minimize::ui
