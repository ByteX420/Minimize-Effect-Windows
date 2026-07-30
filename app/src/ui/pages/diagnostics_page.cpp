#include "pch.hpp"

#include "ui/pages/diagnostics_page.hpp"

#include <array>
#include <format>
#include <tuple>

#include "ui/components/controls.hpp"
#include "ui/settings_window.hpp"

namespace minimize::ui::pages {
namespace {

constexpr float kPageTitleTextSize = 22.0f;
constexpr float kPageSubtitleTextSize = 13.0f;
constexpr float kLabelTextSize = 15.0f;
constexpr float kValueTextSize = 13.0f;
constexpr float kHelperTextSize = 13.0f;
constexpr float kCaptionTextSize = 12.0f;
constexpr ImU32 kPrimaryTextColor = ::minimize::ui::theme::kText;
constexpr ImU32 kSecondaryTextColor = ::minimize::ui::theme::kMutedText;

}  // namespace

void DiagnosticsPage::Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                             const ::minimize::ui::motion::MotionContext& motion, float scale,
                             float alpha) {
  const ULONGLONG now = GetTickCount64();
  auto& diagnostics = window.controller_->view_model().diagnostics;
#ifdef _DEBUG
  if (diagnostics.stress_test.active || diagnostics.effect.empty() || now - window.last_diagnostics_refresh_ms_ >= 200) {
#else
  if (diagnostics.effect.empty() || now - window.last_diagnostics_refresh_ms_ >= 500) {
#endif
    diagnostics = window.controller_->actions().GetDiagnostics();
    window.last_diagnostics_refresh_ms_ = now;
  }

  layout.Title(window.font_title_, kPageTitleTextSize, "Repair", window.font_small_,
               kPageSubtitleTextSize, "Status, recovery tools, and system info");
  const auto status_row = [&](const char* title, const std::string& value) {
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeight);
    layout.ReserveControl(layout.content_width() * 0.55f);
    layout.RowTitle(window.font_body_, kLabelTextSize, title, kPrimaryTextColor);
    layout.RowValue(window.font_small_, kValueTextSize, value.c_str(), kSecondaryTextColor);
    layout.EndRow();
  };

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "STATUS");
  layout.BeginGroup();
  status_row("Effect", diagnostics.effect);
  status_row("Hook", diagnostics.hook);
  status_row("Renderer", diagnostics.renderer);
  status_row("D3D device", diagnostics.d3d_device);
  status_row("Animations", diagnostics.active_animations);
  status_row("Watchdog", diagnostics.watchdog);
  layout.EndGroup();

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "DISPLAY");
  layout.BeginGroup();
  status_row("Refresh rate", diagnostics.display_refresh);
  status_row("Monitor", diagnostics.window_monitor);
  status_row("Taskbar", diagnostics.taskbar);
  status_row("Startup repair", diagnostics.startup_repair);
  layout.EndGroup();

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "MACHINE");
  layout.BeginGroup();
  status_row("Windows", diagnostics.windows_version);
  status_row("GPU", diagnostics.graphics_adapter);
  status_row("Displays", diagnostics.monitor_configuration);
  status_row("Log folder", diagnostics.log_folder_size);
  layout.EndGroup();

  constexpr std::array actions = {
      std::tuple{"Copy report", "Copy a full diagnostics dump", features::DiagnosticsAction::kCopy},
      std::tuple{"Open logs", "Reveal the log folder in Explorer",
                 features::DiagnosticsAction::kOpenLogFolder},
      std::tuple{"Repair windows", "Recover stuck or orphaned windows",
                 features::DiagnosticsAction::kRepairWindows},
      std::tuple{"Restart renderer", "Rebuild the Direct3D overlay path",
                 features::DiagnosticsAction::kRestartRenderer},
  };
  const float button_width = 108.0f * scale;
  const float button_height = ::minimize::ui::theme::Metrics::kButtonHeight * scale;
  layout.SectionCaption(window.font_small_, kCaptionTextSize, "TOOLS");
  layout.BeginGroup();
  for (size_t index = 0; index < actions.size(); ++index) {
    const auto& [title, helper, action] = actions[index];
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
    layout.ReserveControl(button_width);
    layout.RowTitle(window.font_body_, kLabelTextSize, title, kPrimaryTextColor);
    layout.RowSubtitle(window.font_small_, kHelperTextSize, helper, kSecondaryTextColor);
    const ImVec2 cursor = layout.ControlCursor(button_width, button_height);
    layout.SetCursor(cursor.x, cursor.y);
    const std::string id = std::format("##diagnostics_action_{}", index);
    const char* label = action == features::DiagnosticsAction::kCopy            ? "Copy"
                        : action == features::DiagnosticsAction::kOpenLogFolder ? "Open"
                        : action == features::DiagnosticsAction::kRepairWindows ? "Repair"
                                                                                : "Restart";
    if (ui::components::CompactButton(motion, id.c_str(), label,
                                      ImVec2(button_width, button_height), window.font_body_, scale,
                                      alpha)) {
      const bool succeeded = window.controller_->actions().ExecuteDiagnosticsAction(action);
      window.diagnostics_feedback_ = succeeded ? "Done" : "Failed";
      window.last_diagnostics_refresh_ms_ = 0;
    }
    layout.EndRow();
  }
  layout.EndGroup();

#ifdef _DEBUG
  const auto& stress = diagnostics.stress_test;
  layout.SectionCaption(window.font_small_, kCaptionTextSize, "AUTOMATED STRESS TEST MODE (DEBUG ONLY)");
  layout.BeginGroup();
  layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
  layout.ReserveControl(button_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "50-Cycle Load Test", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize,
                     stress.active ? "Automated 50-cycle minimize/restore load test in progress..."
                                   : "Rapidly minimizes/restores windows 50x to verify 0 VRAM leaks & 0 deadlocks",
                     kSecondaryTextColor);
  const ImVec2 debug_cursor = layout.ControlCursor(button_width, button_height);
  layout.SetCursor(debug_cursor.x, debug_cursor.y);
  if (ui::components::CompactButton(motion, "##diagnostics_action_stress",
                                    stress.active ? "Testing..." : "Run Test",
                                    ImVec2(button_width, button_height), window.font_body_, scale,
                                    alpha, stress.active)) {
    if (!stress.active) {
      window.controller_->actions().ExecuteDiagnosticsAction(features::DiagnosticsAction::kStressTest);
      window.last_diagnostics_refresh_ms_ = 0;
    }
  }
  layout.EndRow();

  const double cur_vram_mb = static_cast<double>(stress.current_vram_bytes) / (1024.0 * 1024.0);
  const double vram_leak_mb = static_cast<double>(static_cast<long long>(stress.current_vram_bytes) - static_cast<long long>(stress.start_vram_bytes)) / (1024.0 * 1024.0);
  const double peak_vram_mb = static_cast<double>(stress.peak_vram_bytes) / (1024.0 * 1024.0);

  const std::string cycle_text = std::format("{} / 50 cycles", stress.current_cycle);
  const std::string vram_text = std::format("{:.2f} MB (Delta: {:+.2f} MB, Peak: {:.2f} MB)", cur_vram_mb, vram_leak_mb, peak_vram_mb);
  const std::string deadlock_text = std::format("{} detected", stress.deadlocks_detected);

  const auto stress_row = [&](const char* title, const std::string& value) {
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeight);
    layout.ReserveControl(layout.content_width() * 0.55f);
    layout.RowTitle(window.font_body_, kLabelTextSize, title, kPrimaryTextColor);
    layout.RowValue(window.font_small_, kValueTextSize, value.c_str(), kSecondaryTextColor);
    layout.EndRow();
  };

  stress_row("Test Progress", cycle_text);
  stress_row("VRAM Metrics", vram_text);
  stress_row("Deadlocks", deadlock_text);

  if (!stress.last_log.empty()) {
    stress_row("Live Console Log", stress.last_log);
  }

  if (!stress.summary.empty()) {
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
    layout.ReserveControl(layout.content_width() * 0.55f);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Diagnostic Result", kPrimaryTextColor);
    layout.RowSubtitle(window.font_small_, kHelperTextSize, stress.summary.c_str(), kSecondaryTextColor);
    layout.EndRow();
  }

  layout.EndGroup();
#endif

  if (window.diagnostics_feedback_.empty()) return;
  const ImVec2 position = layout.ToScreen(layout.content_left(), layout.y());
  ImGui::GetWindowDrawList()->AddText(
      window.font_small_, window.font_small_->FontSize,
      ImVec2(std::floor(position.x + 0.5f), std::floor(position.y + 0.5f)),
      ::minimize::ui::theme::WithAlpha(kSecondaryTextColor, alpha),
      window.diagnostics_feedback_.c_str());
  layout.Gap(18.0f);
}

}  // namespace minimize::ui::pages
