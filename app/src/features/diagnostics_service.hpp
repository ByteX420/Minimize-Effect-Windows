#pragma once

#include <functional>
#include <string>
#include <windows.h>

namespace minimize::platform {
class TaskbarTargetProvider;
}
namespace minimize::rendering {
class D3dDevice;
}

namespace minimize::features {

#ifdef _DEBUG
struct StressTestReport {
  bool active = false;
  int current_cycle = 0;
  int target_cycles = 50;
  int current_step = 0;
  std::size_t start_vram_bytes = 0;
  std::size_t current_vram_bytes = 0;
  std::size_t peak_vram_bytes = 0;
  std::size_t start_ram_bytes = 0;
  std::size_t current_ram_bytes = 0;
  int deadlocks_detected = 0;
  int windows_processed = 0;
  std::string last_log;
  std::string summary;
};
#endif

struct DiagnosticsSnapshot {
  std::string effect;
  std::string hook;
  std::string renderer;
  std::string d3d_device;
  std::string active_animations;
  std::string watchdog;
  std::string display_refresh;
  std::string window_monitor;
  std::string taskbar;
  std::string startup_repair;
  std::string version;
  std::string windows_version;
  std::string graphics_adapter;
  std::string monitor_configuration;
  std::string log_folder_size;
  std::string report;
#ifdef _DEBUG
  StressTestReport stress_test;
#endif
};

struct DiagnosticsContext {
  bool effect_active = false;
  bool hook_installed = false;
  bool renderer_recovering = false;
  const rendering::D3dDevice* d3d_device = nullptr;
  int active_animations = 0;
  std::string startup_repair;
  HWND reference_window = nullptr;
  const platform::TaskbarTargetProvider* taskbar_targets = nullptr;
};

enum class DiagnosticsAction {
  kCopy,
  kOpenLogFolder,
  kRepairWindows,
  kRestartRenderer,
#ifdef _DEBUG
  kStressTest,
#endif
};

struct DiagnosticsActions {
  HWND owner = nullptr;
  std::function<std::string()> build_report;
  std::function<bool()> repair_windows;
  std::function<bool()> restart_renderer;
#ifdef _DEBUG
  std::function<bool()> run_stress_test;
#endif
};

class DiagnosticsService final {
public:
  [[nodiscard]] DiagnosticsSnapshot Build(const DiagnosticsContext& context) const;
  [[nodiscard]] bool Execute(DiagnosticsAction action, const DiagnosticsActions& actions) const;
  [[nodiscard]] bool CopyReport(HWND owner, const std::string& report) const;
  [[nodiscard]] bool OpenLogFolder(HWND owner) const;
};

}  // namespace minimize::features
