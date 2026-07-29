#include "pch.hpp"

#include "ui/update_presenter.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "features/update_service.hpp"
#include "imgui.h"
#include "ui/settings_window.hpp"
#include "ui/theme/theme.hpp"
#include "ui/theme/theme_tokens.hpp"

namespace minimize::ui {
namespace {

using features::UpdatePhase;
using features::UpdateSnapshot;
using theme::WithAlpha;

constexpr float kGridDuration = 1.5f;
constexpr float kGridStagger = 0.2f;

std::string FormatBytes(std::uint64_t bytes) {
  if (bytes < 1024) return std::format("{} B", bytes);
  const double kibibytes = static_cast<double>(bytes) / 1024.0;
  if (kibibytes < 1024.0) return std::format("{:.0f} KB", kibibytes);
  return std::format("{:.1f} MB", kibibytes / 1024.0);
}

float CubicBezier(float progress) {
  // Framer Motion's "easeInOut": cubic-bezier(0.42, 0, 0.58, 1).
  constexpr float x1 = 0.42f;
  constexpr float x2 = 0.58f;
  float parameter = std::clamp(progress, 0.0f, 1.0f);
  for (int iteration = 0; iteration < 5; ++iteration) {
    const float inverse = 1.0f - parameter;
    const float x =
        3.0f * inverse * inverse * parameter * x1 +
        3.0f * inverse * parameter * parameter * x2 + parameter * parameter * parameter;
    const float derivative =
        3.0f * inverse * inverse * x1 +
        6.0f * inverse * parameter * (x2 - x1) +
        3.0f * parameter * parameter * (1.0f - x2);
    if (std::abs(derivative) < 0.0001f) break;
    parameter = std::clamp(parameter - (x - progress) / derivative, 0.0f, 1.0f);
  }
  const float inverse = 1.0f - parameter;
  return 3.0f * inverse * parameter * parameter + parameter * parameter * parameter;
}

float GridKeyframeValue(float elapsed, float delay, float middle) {
  if (elapsed < delay) return 1.0f;
  const float phase = std::fmod(elapsed - delay, kGridDuration) / kGridDuration;
  const bool returning = phase >= 0.5f;
  const float segment = returning ? (phase - 0.5f) * 2.0f : phase * 2.0f;
  const float eased = CubicBezier(segment);
  return returning ? std::lerp(middle, 1.0f, eased) : std::lerp(1.0f, middle, eased);
}

void DrawGridDots(ImDrawList* draw, const ImVec2& center, float elapsed, float scale, float alpha) {
  constexpr float dot_size = 10.0f;
  constexpr float gap = 6.0f;
  const float step = (dot_size + gap) * scale;
  const float grid_size = (dot_size * 3.0f + gap * 2.0f) * scale;
  const ImVec2 origin(center.x - grid_size * 0.5f, center.y - grid_size * 0.5f);

  for (int index = 0; index < 9; ++index) {
    const int column = index % 3;
    const int row = index / 3;
    const float delay = static_cast<float>(column + row) * kGridStagger;
    const float dot_scale = GridKeyframeValue(elapsed, delay, 0.5f);
    const float opacity = GridKeyframeValue(elapsed, delay, 0.3f) * alpha;
    const float radius = dot_size * 0.5f * scale * dot_scale;
    const ImVec2 dot_center(origin.x + dot_size * 0.5f * scale + column * step,
                            origin.y + dot_size * 0.5f * scale + row * step);
    draw->AddCircleFilled(dot_center, radius, WithAlpha(theme::kText, opacity), 24);
  }
}

void DrawProgress(motion::MotionSystem& motion_system, const motion::MotionTokens& tokens,
                  float scale, ImDrawList* draw, const ImVec2& minimum, float width, float progress,
                  float alpha) {
  const float height = 5.0f * scale;
  const ImVec2 maximum(minimum.x + width, minimum.y + height);
  draw->AddRectFilled(minimum, maximum, IM_COL32(255, 255, 255, static_cast<int>(22.0f * alpha)),
                      height * 0.5f);
  const float displayed =
      motion_system.AnimateValue(motion::MotionKey("update", "download", "progress"),
                                 std::clamp(progress, 0.0f, 1.0f), tokens.spring_soft, 0.0f);
  if (displayed > 0.001f) {
    const ImVec2 fill_max(minimum.x + width * displayed, maximum.y);
    draw->AddRectFilled(minimum, fill_max,
                        IM_COL32(233, 233, 239, static_cast<int>(255.0f * alpha)), height * 0.5f);
  }
}

}  // namespace

void UpdatePresenter::DrawUpdateWorkspace(SettingsWindow& window) {
  const UpdateSnapshot snapshot = window.update_service_.GetSnapshot();
  const bool active = window.update_workspace_engaged_ || window.update_resume_active_;
  const float show = window.motion_system_.AnimateValue(
      motion::MotionKey("update", "workspace", "show"), active ? 1.0f : 0.0f,
      active ? motion::MotionSpec::Timed(0.54f, motion::MotionEasing::kSmootherStep)
             : motion::MotionSpec::Timed(0.50f, motion::MotionEasing::kSmootherStep),
      0.0f);
  if (active && window.update_grid_started_at_ < 0.0) {
    window.update_grid_started_at_ = ImGui::GetTime();
  }
  if (show <= 0.005f) {
    if (!active) window.update_grid_started_at_ = -1.0;
    return;
  }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const float scale = window.ui_scale_;
  const float chrome_width = 92.0f * scale;
  const ImU32 workspace_background =
      IM_COL32(20, 20, 22, static_cast<int>(255.0f * show));
  // Foreground overlays always sort above the root draw list. Keep a precise transparent
  // chrome pocket so the original traffic lights remain visible and interactive.
  draw->AddRectFilled(ImVec2(chrome_width, 0.0f), display, workspace_background);
  // Draw interactive top-left traffic lights (Close, Minimize, Restore) directly over the update workspace.
  const motion::MotionContext widget_motion{window.motion_system_, window.motion_tokens_};
  const theme::TrafficLightAction action =
      theme::DrawTrafficLights(widget_motion, ImVec2(0.0f, 0.0f), scale, show);
  if (action == theme::TrafficLightAction::kClose) {
    window.controller_->actions().RequestExit();
  } else if (action == theme::TrafficLightAction::kMinimize) {
    ShowWindow(window.hwnd(), SW_MINIMIZE);
  } else if (action == theme::TrafficLightAction::kZoom) {
    ShowWindow(window.hwnd(), IsZoomed(window.hwnd()) ? SW_RESTORE : SW_MAXIMIZE);
  }

  // Keep the update workspace intentionally minimal: loader centered, progress directly below.
  const float vertical_shift = (1.0f - show) * 16.0f * scale;
  const ImVec2 center(display.x * 0.5f, display.y * 0.5f - 12.0f * scale + vertical_shift);
  const float alpha = show;
  const float elapsed =
      window.update_grid_started_at_ >= 0.0
          ? static_cast<float>(ImGui::GetTime() - window.update_grid_started_at_)
          : 0.0f;
  DrawGridDots(draw, center, elapsed, scale, alpha);

  const float progress_width = 190.0f * scale;
  const ImVec2 progress_min(center.x - progress_width * 0.5f, center.y + 46.0f * scale);
  float progress = snapshot.progress;
  if (window.update_resume_active_ || snapshot.phase == UpdatePhase::kReadyToInstall ||
      snapshot.phase == UpdatePhase::kInstalling) {
    progress = 1.0f;
  }
  DrawProgress(window.motion_system_, window.motion_tokens_, scale, draw, progress_min,
               progress_width, progress, alpha);
  if (snapshot.phase == UpdatePhase::kDownloading && snapshot.total_bytes > 0) {
    const std::string amount =
        FormatBytes(snapshot.downloaded_bytes) + " / " + FormatBytes(snapshot.total_bytes);
    ImFont* font = window.font_small_ ? window.font_small_ : ImGui::GetFont();
    const ImVec2 text_size =
        font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, amount.c_str());
    draw->AddText(font, font->FontSize,
                  ImVec2(std::floor(center.x - text_size.x * 0.5f),
                         std::floor(progress_min.y + 13.0f * scale)),
                  WithAlpha(theme::kMutedText, alpha), amount.c_str());
  }

  if (snapshot.phase == UpdatePhase::kReadyToInstall && show >= 0.985f &&
      !window.update_installer_started_) {
    RECT bounds{};
    GetWindowRect(window.hwnd_, &bounds);
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    const bool maximized =
        GetWindowPlacement(window.hwnd_, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
    window.controller_->actions().PrepareForUpdateHandover();
    if (window.update_service_.LaunchInstaller(
            bounds, static_cast<int>(window.selected_page_), window.current_page_scroll_,
            maximized)) {
      window.update_installer_started_ = true;
    } else {
      window.controller_->actions().ResumeAfterUpdateHandoverFailure();
    }
  }
  if (window.update_installer_started_ && window.update_service_.InstallerHandoverReady()) {
    window.controller_->actions().RequestExit();
  } else if (window.update_installer_started_ &&
             window.update_service_.InstallerHandoverFailed()) {
    window.update_installer_started_ = false;
    window.controller_->actions().ResumeAfterUpdateHandoverFailure();
  }
}

void UpdatePresenter::Render(SettingsWindow& window) {
  DrawUpdateWorkspace(window);
}

}  // namespace minimize::ui
