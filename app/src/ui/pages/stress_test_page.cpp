#include "pch.hpp"

#include "ui/pages/stress_test_page.hpp"

#include <algorithm>
#include <format>

#include "ui/components/controls.hpp"
#include "ui/settings_window.hpp"

#ifdef _DEBUG
namespace minimize::ui::pages {
namespace {
constexpr float kTitle = 22.0f;
constexpr float kSubtitle = 13.0f;
constexpr float kLabel = 15.0f;
constexpr float kValue = 13.0f;
constexpr float kCaption = 12.0f;
constexpr ImU32 kPrimary = ::minimize::ui::theme::kText;
constexpr ImU32 kMuted = ::minimize::ui::theme::kMutedText;
}  // namespace

void StressTestPage::Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                            const ::minimize::ui::motion::MotionContext& motion, float scale,
                            float alpha) {
  const ULONGLONG now = GetTickCount64();
  auto& diagnostics = window.controller_->view_model().diagnostics;
  if (diagnostics.effect.empty() || diagnostics.stress_test.active ||
      now - window.last_diagnostics_refresh_ms_ >= 150) {
    diagnostics = window.controller_->actions().GetDiagnostics();
    window.last_diagnostics_refresh_ms_ = now;
  }
  const auto& test = diagnostics.stress_test;
  const float button_width = 124.0f * scale;
  const float button_height = ::minimize::ui::theme::Metrics::kButtonHeight * scale;

  layout.Title(window.font_title_, kTitle, "Stress Test", window.font_small_, kSubtitle,
               "Debug-only end-to-end verification of minimize, restore and cleanup state");
  layout.SectionCaption(window.font_small_, kCaption, "RUNNER");
  layout.BeginGroup();
  layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
  layout.ReserveControl(button_width);
  layout.RowTitle(window.font_body_, kLabel, "60-cycle stress suite", kPrimary);
  layout.RowSubtitle(window.font_small_, kSubtitle,
      test.active ? "Running against six generated windows; each requested state is verified after settling."
                  : "Covers resize, bursts, all-window runs, cross-phase reversals and final cleanup.", kMuted);
  const ImVec2 cursor = layout.ControlCursor(button_width, button_height);
  layout.SetCursor(cursor.x, cursor.y);
  if (ui::components::CompactButton(motion, "##run_stress_suite", test.active ? "Running..." : "Run suite",
                                    ImVec2(button_width, button_height), window.font_body_, scale,
                                    alpha, test.active) && !test.active) {
    const bool started = window.controller_->actions().ExecuteDiagnosticsAction(features::DiagnosticsAction::kStressTest);
    window.diagnostics_feedback_ = started ? "Stress suite started" : "Could not start stress suite";
    window.last_diagnostics_refresh_ms_ = 0;
  }
  layout.EndRow();

  const auto row = [&](const char* label, const std::string& value) {
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeight);
    layout.ReserveControl(layout.content_width() * 0.56f);
    layout.RowTitle(window.font_body_, kLabel, label, kPrimary);
    layout.RowValue(window.font_small_, kValue, value.c_str(), kMuted);
    layout.EndRow();
  };
  row("Phase", test.active ? (test.finalizing ? "Final cleanup verification" : "Executing and settling") : "Idle");
  row("Progress", std::format("{} / {} cycles ({} actions requested)", test.current_cycle,
                              test.target_cycles, test.actions_requested));
  row("Assertions", std::format("{} passed, {} failed", test.assertions_passed, test.assertions_failed));
  row("Failure counters", std::format("{} state mismatch, {} timeout, {} invalid HWND, {} deadlock",
      test.state_mismatches, test.animation_timeouts, test.invalid_windows, test.deadlocks_detected));
  row("Windows verified", std::to_string(test.windows_processed));
  if (!test.last_log.empty()) row("Latest operation", test.last_log);
  layout.EndGroup();

  if (!test.summary.empty()) {
    layout.SectionCaption(window.font_small_, kCaption, "RESULT");
    layout.BeginGroup();
    const int lines = static_cast<int>(std::count(test.summary.begin(), test.summary.end(), '\n')) + 1;
    layout.BeginRow(std::max(78.0f, 28.0f + static_cast<float>(lines) * 17.0f));
    layout.ReserveControl(button_width);
    layout.RowTitle(window.font_body_, kLabel, "Final report", kPrimary);
    layout.RowSubtitle(window.font_small_, kSubtitle, test.summary.c_str(), kMuted);
    const ImVec2 copy = layout.ControlCursor(button_width, button_height);
    layout.SetCursor(copy.x, copy.y);
    if (ui::components::CompactButton(motion, "##copy_stress_report", "Copy report",
        ImVec2(button_width, button_height), window.font_body_, scale, alpha)) {
      std::string report = test.summary;
      if (!test.findings.empty()) {
        report += "\n\nFINDINGS:\n";
        for (const std::string& item : test.findings) report += item + "\n";
      }
      ImGui::SetClipboardText(report.c_str());
      window.diagnostics_feedback_ = "Stress report copied";
    }
    layout.EndRow();
    layout.EndGroup();
  }

  layout.SectionCaption(window.font_small_, kCaption, "FINDINGS (ONLY FAILURES)");
  layout.BeginGroup();
  if (test.findings.empty()) {
    row("Result", test.active ? "No failures recorded so far" : "No recorded failures");
  } else {
    for (const std::string& item : test.findings) {
      layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
      layout.ReserveControl(0.0f);
      layout.RowSubtitle(window.font_small_, kSubtitle, item.c_str(), kMuted);
      layout.EndRow();
    }
  }
  layout.EndGroup();
}

}  // namespace minimize::ui::pages
#endif
