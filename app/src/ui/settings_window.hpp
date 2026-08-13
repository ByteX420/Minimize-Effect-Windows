#pragma once

#include <array>
#include <memory>
#include <optional>
#include <windows.h>

#include "features/diagnostics_service.hpp"
#include "features/open_windows_service.hpp"
#include "features/update_service.hpp"
#include "settings/app_settings.hpp"
#include "ui/animation_preview.hpp"
#include "ui/application_list_provider.hpp"
#include "ui/motion/motion.hpp"
#include "ui/motion/motion_tokens.hpp"
#include "ui/rendering/imgui_renderer.hpp"
#include "ui/settings_actions.hpp"
#include "ui/settings_controller.hpp"
#include "ui/settings_view_model.hpp"
#include "ui/tray_icon.hpp"

struct ImFont;

namespace minimize::ui::pages {
class AboutPage;
class AnimationPage;
class ApplicationsPage;
class DiagnosticsPage;
#ifdef _DEBUG
class StressTestPage;
#endif
class GeneralPage;
class HotkeysPage;
class DisplaysPage;
class WindowsIntegrationPage;
}  // namespace minimize::ui::pages

namespace minimize::ui {
class SettingsShell;
class UpdatePresenter;

class SettingsWindow {
public:
  SettingsWindow() = default;
  ~SettingsWindow();
  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  bool Initialize(HINSTANCE instance, ui::SettingsActions& actions);
  void Shutdown();
  void Show(bool show);
  void SetInitialBounds(const RECT& bounds);
  void RestoreUiState(const settings::UiWindowState& state);
  void PrepareUpdateResume(int page, float page_scroll, bool maximized);
  void CompleteUpdateHandover();
  void UpdateState(const minimize::settings::AppSettings& settings);
  void UpdatePauseState(bool paused, bool until_restart);
  void SetHotkeyRegistrationStatus(minimize::settings::HotkeyAction action, bool available);
  void Render();
  void ForceRender();
  [[nodiscard]] HWND hwnd() const { return hwnd_; }
  [[nodiscard]] bool WantsContinuousRendering() const;
  [[nodiscard]] HANDLE RenderWaitHandle() const {
    return renderer_.frame_latency_waitable_object();
  }
  // True while the open/enter motion for shell + sidebar + first page content is still running.
  // Used to defer heavy startup work (e.g. iconic seed) until the UI is fully settled.
  [[nodiscard]] bool IsStartupEnterMotionActive() const { return startup_enter_motion_active_; }
  void InvalidateOpenWindowsSnapshot() { open_windows_snapshot_valid_ = false; }

private:
  friend class SettingsShell;
  friend class UpdatePresenter;
  friend class ui::pages::AboutPage;
  friend class ui::pages::AnimationPage;
  friend class ui::pages::ApplicationsPage;
  friend class ui::pages::DiagnosticsPage;
#ifdef _DEBUG
  friend class ui::pages::StressTestPage;
#endif
  friend class ui::pages::GeneralPage;
  friend class ui::pages::HotkeysPage;
  friend class ui::pages::DisplaysPage;
  friend class ui::pages::WindowsIntegrationPage;
  enum class Page {
    kGeneral,
    kAnimation,
    kApplications,
    kDisplays,
    kWindowsIntegration,
    kHotkeys,
    kDiagnostics,
#ifdef _DEBUG
    kAbout,
    kStressTest,
#else
    kAbout,
#endif
  };

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
  void HandleTrayCommand(ui::TrayCommand command);
  bool CreateRenderWindow(HINSTANCE instance);
  void ApplyWindowShape(int width, int height);
  void UpdateDpi(UINT dpi);
  void UpdateReducedMotion();
  void FlushPendingSpeedSave();
  void RecordSaveResult(bool saved);
  void RecordFileOperationResult(const SettingsFileOperationResult& result);
  void HandleCloseRequest();
  void HandleUpdateStateChanged();
  [[nodiscard]] std::optional<LRESULT> HandleTitlebarMessage(HWND hwnd, UINT message,
                                                             LPARAM l_param, float scale);
  void UpdateStartupEnterMotionGate();
  void PersistUiState();
  [[nodiscard]] bool DetectStartupEnterMotionActive() const;

  HWND hwnd_ = nullptr;
  ui::rendering::ImguiRenderer renderer_;
  std::unique_ptr<ui::SettingsController> controller_;
  int editing_hotkey_ = -1;
  std::string hotkey_feedback_;
  ULONGLONG last_diagnostics_refresh_ms_ = 0;
  std::string diagnostics_feedback_;
  bool minimize_bezier_dirty_ = false;
  bool restore_bezier_dirty_ = false;
  bool cancel_bezier_dirty_ = false;
  bool minimize_bezier_active_ = false;
  bool restore_bezier_active_ = false;
  bool cancel_bezier_active_ = false;
  bool strength_slider_active_ = false;
  bool strength_slider_dirty_ = false;
  std::array<char, 49> motion_profile_name_{};
  int selected_motion_profile_ = 0;
  std::array<char, 260> exclusion_input_{};
  std::string exclusion_error_;
  ULONGLONG last_active_apps_refresh_ms_ = 0;
  std::vector<std::string> cached_active_apps_;
  features::OpenWindowsSnapshot cached_open_windows_{};
  ULONGLONG last_open_windows_refresh_ms_ = 0;
  bool open_windows_snapshot_valid_ = false;
  int selected_display_index_ = 0;  // Display 1 (primary) by default
  std::string persistence_error_;
  std::string save_feedback_;
  ULONGLONG save_feedback_until_ms_ = 0;
  bool save_feedback_error_ = false;
  Page selected_page_ = Page::kGeneral;
  bool reset_page_scroll_ = false;
  bool minimize_slider_active_ = false;
  bool minimize_slider_dirty_ = false;
  bool restore_slider_active_ = false;
  bool restore_slider_dirty_ = false;
  bool cancel_slider_active_ = false;
  bool cancel_slider_dirty_ = false;
  ui::AnimationPreview animation_preview_;
  ui::ApplicationListProvider application_list_provider_;
  ui::TrayIcon tray_icon_;
  bool titlebar_dragging_ = false;
  POINT titlebar_drag_offset_{};
  bool render_requested_ = false;
  ULONGLONG shown_at_ms_ = 0;
  // Gates deferred startup work until window/sidebar/page enter animations finish.
  bool startup_enter_motion_active_ = false;
  bool startup_enter_motion_seen_ = false;
  UINT current_dpi_ = USER_DEFAULT_SCREEN_DPI;
  float ui_scale_ = 1.0f;
  ImFont* font_small_ = nullptr;
  ImFont* font_body_ = nullptr;
  ImFont* font_medium_ = nullptr;
  ImFont* font_title_ = nullptr;
  ui::motion::MotionSystem motion_system_;
  ui::motion::MotionTokens motion_tokens_ = ui::motion::MotionTokens::Default();
  features::UpdateService update_service_;
  std::string update_notified_version_;
  bool update_card_dismissed_ = false;
  bool update_workspace_engaged_ = false;
  bool update_resume_active_ = false;
  bool update_installer_started_ = false;
  float current_page_scroll_ = 0.0f;
  std::optional<float> initial_page_scroll_;
  bool initial_maximized_ = false;
  double update_grid_started_at_ = -1.0;
  std::optional<RECT> initial_bounds_;
  bool position_initialized_ = false;
};

}  // namespace minimize::ui
