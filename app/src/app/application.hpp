#pragma once

#include <memory>
#include <optional>
#include <string>
#include <windows.h>

namespace minimize::app {

class ApplicationRuntime;

struct ApplicationLaunchOptions {
  bool force_show_settings = false;
  std::optional<RECT> initial_window_bounds;
  DWORD update_parent_process_id = 0;
  std::wstring update_ready_event_name;
  DWORD elevation_parent_process_id = 0;
  int initial_page = 0;
  float initial_page_scroll = 0.0f;
  bool initial_maximized = false;

  [[nodiscard]] bool IsUpdateHandover() const {
    return update_parent_process_id != 0 && !update_ready_event_name.empty();
  }
  [[nodiscard]] bool IsElevationHandover() const { return elevation_parent_process_id != 0; }
  [[nodiscard]] bool IsHandover() const { return IsUpdateHandover() || IsElevationHandover(); }
};

class Application final {
public:
  Application();
  ~Application();
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  [[nodiscard]] bool Initialize(HINSTANCE instance, const ApplicationLaunchOptions& options = {});
  [[nodiscard]] int Run();
  void RenderUpdateHandoverFrame();
  [[nodiscard]] bool CompleteUpdateHandover();
  void RequestShutdown();

private:
  std::unique_ptr<ApplicationRuntime> runtime_;
};

}  // namespace minimize::app
