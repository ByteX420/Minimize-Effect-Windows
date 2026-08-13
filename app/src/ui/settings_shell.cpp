#include "pch.hpp"

#include "ui/settings_shell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "imgui.h"
#include "ui/components/page_layout.hpp"
#include "ui/pages/about_page.hpp"
#include "ui/pages/animation_page.hpp"
#include "ui/pages/applications_page.hpp"
#include "ui/pages/diagnostics_page.hpp"
#ifdef _DEBUG
#include "ui/pages/stress_test_page.hpp"
#endif
#include "ui/pages/displays_page.hpp"
#include "ui/pages/general_page.hpp"
#include "ui/pages/hotkeys_page.hpp"
#include "ui/pages/windows_integration_page.hpp"
#include "ui/settings_window.hpp"
#include "ui/theme/theme.hpp"
#include "ui/theme/theme_tokens.hpp"
#include "ui/update_presenter.hpp"

namespace minimize::ui {
namespace settings_ui = ::minimize::ui::theme;
using theme::WithAlpha;

void SettingsShell::Render(SettingsWindow& window) {
  const ImVec2 window_size = ImGui::GetIO().DisplaySize;
  const float scale = window.ui_scale_;
  const auto px = [scale](float value) { return value * scale; };
  motion::MotionContext widget_motion{window.motion_system_, window.motion_tokens_};
  const float window_alpha =
      window.motion_system_.AnimateValue(ui::motion::MotionKey("window", "settings", "alpha"), 1.0f,
                                         window.motion_tokens_.panel_enter_fade, 0.0f);
  const bool update_workspace_active =
      window.update_workspace_engaged_ || window.update_resume_active_;
  const float shell_content = window.motion_system_.AnimateValue(
      ui::motion::MotionKey("update", "shell", "content"),
      update_workspace_active ? 0.0f : 1.0f,
      update_workspace_active
          ? ui::motion::MotionSpec::Timed(0.54f, ui::motion::MotionEasing::kSmootherStep)
          : ui::motion::MotionSpec::Timed(0.50f, ui::motion::MotionEasing::kSmootherStep),
      update_workspace_active ? 1.0f : 0.0f);
  float content_alpha = window_alpha * shell_content;
  const ImVec2 window_offset = window.motion_system_.AnimateVector(
      ui::motion::MotionKey("window", "settings", "offset"), ImVec2(0.0f, 0.0f),
      window.motion_tokens_.panel_enter_offset, ImVec2(0.0f, 6.0f));
  float y_offset = px(window_offset.y);

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(window_size);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                                     ImGuiWindowFlags_NoBackground;
  ImGui::Begin("MinimizeEffectRoot", nullptr, flags);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 window_origin = ImGui::GetWindowPos();
  const auto window_point = [&window_origin](float x, float y) {
    return ImVec2(window_origin.x + x, window_origin.y + y);
  };

  const float sidebar_width = px(theme::Metrics::kSidebarWidth);

  // Shell: compact rail + solid content pane (outer radius matches DWM round corners).
  const float shell_round = px(theme::Metrics::kWindowRounding);
  ImVec4 main_background = ui::theme::kMainColor;
  // DirectComposition uses the render-target alpha as real desktop transparency. The shell
  // itself must therefore stay opaque; only the pixels outside these rounded fills are clear.
  main_background.w = 1.0f;
  theme::DrawSmoothRoundRectFilled(draw, window_point(sidebar_width, 0.0f),
                                  window_point(window_size.x, window_size.y),
                                  ImGui::GetColorU32(main_background), shell_round,
                                  ImDrawFlags_RoundCornersRight, 16);
  ImVec4 sidebar_background = ui::theme::kSidebarColor;
  sidebar_background.w = 1.0f;
  theme::DrawSmoothRoundRectFilled(draw, window_origin, window_point(sidebar_width, window_size.y),
                                  ImGui::GetColorU32(sidebar_background), shell_round,
                                  ImDrawFlags_RoundCornersLeft, 16);
  draw->AddLine(window_point(sidebar_width, 0.0f), window_point(sidebar_width, window_size.y),
                WithAlpha(theme::kBorder, content_alpha * 0.45f));
  if (update_workspace_active || shell_content < 0.999f) {
    const float workspace_base_alpha = window_alpha * (1.0f - shell_content);
    theme::DrawSmoothRoundRectFilled(draw, window_origin, window_point(window_size.x, window_size.y),
                                    IM_COL32(20, 20, 22,
                                             static_cast<int>(255.0f * workspace_base_alpha)),
                                    shell_round, ImDrawFlags_RoundCornersAll, 16);
  }

  // Brand under traffic lights — same left inset as nav.
  {
    const float brand_reveal =
        window.motion_system_.AnimateValue(ui::motion::MotionKey("shell", "brand", "reveal"), 1.0f,
                                           window.motion_tokens_.panel_enter_fade, 0.0f);
    const float brand_shift = (1.0f - brand_reveal) * px(6.0f);
    const ImVec2 brand = window_point(px(theme::Metrics::kSidebarMargin),
                                      px(theme::Metrics::kSidebarBrandY) + brand_shift);
    draw->AddText(window.font_small_, window.font_small_->FontSize,
                  ImVec2(std::floor(brand.x + 0.5f), std::floor(brand.y + 0.5f)),
                  WithAlpha(theme::kMutedText, content_alpha * brand_reveal * 0.85f),
                  "MINIMIZE EFFECT");
  }

  struct PageEntry {
    SettingsWindow::Page page;
    const char* label;
    bool section_gap;
  };
  constexpr std::array pages = {
      PageEntry{SettingsWindow::Page::kGeneral, "Effect", false},
      PageEntry{SettingsWindow::Page::kAnimation, "Motion", false},
      PageEntry{SettingsWindow::Page::kApplications, "Apps", false},
      PageEntry{SettingsWindow::Page::kDisplays, "Displays", false},
      PageEntry{SettingsWindow::Page::kWindowsIntegration, "System", true},
      PageEntry{SettingsWindow::Page::kHotkeys, "Hotkeys", false},
      PageEntry{SettingsWindow::Page::kDiagnostics, "Repair", false},
#ifdef _DEBUG
      PageEntry{SettingsWindow::Page::kStressTest, "Test", false},
#endif
      PageEntry{SettingsWindow::Page::kAbout, "About", false},
  };
  // Sidebar tabs — same motion vocabulary as SegmentSelector / buttons:
  // staggered enter, sliding selection pill (spring), per-item hover/press/text.
  const float nav_x = px(theme::Metrics::kSidebarMargin);
  const float nav_w = px(theme::Metrics::kSidebarContentWidth);
  const float nav_h = px(theme::Metrics::kNavigationRowHeight);
  const float nav_step =
      px(theme::Metrics::kNavigationRowHeight + theme::Metrics::kNavigationSpacing);
  const float nav_base_y = px(theme::Metrics::kNavigationY);

  std::array<float, pages.size()> item_offsets{};
  float layout_y = nav_base_y;
  size_t selected_index = 0;
  for (size_t index = 0; index < pages.size(); ++index) {
    const PageEntry& entry = pages[index];
    if (entry.section_gap) {
      layout_y += px(10.0f);
      const float div_y = layout_y - px(5.0f);
      draw->AddLine(window_point(nav_x + px(4.0f), div_y),
                    window_point(nav_x + nav_w - px(4.0f), div_y),
                    WithAlpha(theme::kSeparator, content_alpha * 0.55f));
    }
    item_offsets[index] = layout_y - nav_base_y;
    if (window.selected_page_ == entry.page) selected_index = index;
    layout_y += nav_step;
  }

  // Relative Y so the spring survives DPI/window moves (same idea as segment slide-x).
  const float select_target = item_offsets[selected_index];
  const float select_y = window.motion_system_.AnimateValue(
      ui::motion::MotionKey("sidebar", "nav", "select-y"), select_target,
      window.motion_tokens_.spring_soft, select_target);
  {
    const float pill_rounding = 8.0f * scale;
    const ImVec2 pill_min = window_point(nav_x, nav_base_y + select_y);
    const ImVec2 pill_max(pill_min.x + nav_w, pill_min.y + nav_h);
    theme::DrawSmoothRoundRectFilled(
        draw, pill_min, pill_max,
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f * content_alpha)), pill_rounding,
        ImDrawFlags_RoundCornersAll, 16);
  }

  for (size_t index = 0; index < pages.size(); ++index) {
    const PageEntry& entry = pages[index];
    const std::string id = std::format("##settings_page_{}", index);
    // Staggered enter like page layout Reveal / brand chip.
    const float item_reveal = window.motion_system_.AnimateValue(
        ui::motion::MotionKey("sidebar", id, "reveal"), 1.0f,
        ui::motion::MotionSpec::Timed(0.34f, ui::motion::MotionEasing::kSmootherStep,
                                      0.03f * static_cast<float>(index)),
        0.0f);
    const float item_alpha = content_alpha * std::clamp(item_reveal, 0.0f, 1.0f);
    const float item_shift = (1.0f - item_reveal) * px(6.0f);
    const ImVec2 item_position = window_point(nav_x - item_shift, nav_base_y + item_offsets[index]);
    if (theme::SidebarItem(widget_motion, id.c_str(), entry.label,
                           window.selected_page_ == entry.page, item_position, ImVec2(nav_w, nav_h),
                           window.font_body_, window.font_medium_, scale, item_alpha) &&
        window.selected_page_ != entry.page) {
      window.FlushPendingSpeedSave();
      window.selected_page_ = entry.page;
      window.reset_page_scroll_ = true;
      window.motion_system_.Set(ui::motion::MotionKey("page", "content", "alpha"), 0.0f);
      window.motion_system_.Set(ui::motion::MotionKey("page", "content", "offset"),
                                ImVec2(0.0f, 14.0f));
    }
  }

  // Status chip: same left column as nav/ampel; bottom inset mirrors traffic-light top edge.
  {
    const char* status = window.controller_->view_model().temporarily_paused
                             ? "Paused"
                             : (window.controller_->view_model().enabled ? "On" : "Off");
    const ImU32 status_color =
        window.controller_->view_model().temporarily_paused ? IM_COL32(220, 170, 90, 255)
        : window.controller_->view_model().enabled          ? IM_COL32(120, 200, 140, 255)
                                                            : theme::kMutedText;
    const float status_reveal =
        window.motion_system_.AnimateValue(ui::motion::MotionKey("shell", "status", "reveal"), 1.0f,
                                           window.motion_tokens_.fade_medium, 0.0f);
    ImFont* status_font = window.font_small_ ? window.font_small_ : ImGui::GetFont();
    const float status_sz = status_font->FontSize;
    const ImVec2 status_text_size = status_font->CalcTextSizeA(status_sz, FLT_MAX, 0.0f, status);
    const float chip_pad_x = px(8.0f);
    const float chip_pad_y = px(4.0f);
    const float chip_h = status_sz + chip_pad_y * 2.0f;
    const float chip_w = status_text_size.x + chip_pad_x * 2.0f;
    // Ampel top-edge inset ≈ kSidebarMargin; pin chip to the matching bottom inset.
    const float chip_y = window_size.y - px(theme::Metrics::kSidebarStatusBottom) - chip_h +
                         (1.0f - status_reveal) * px(6.0f);
    const ImVec2 chip_min = window_point(nav_x, chip_y);
    const ImVec2 chip_max(chip_min.x + chip_w, chip_min.y + chip_h);
    const float chip_round = chip_h * 0.5f;
    const float a = content_alpha * status_reveal;
    theme::DrawSmoothRoundRectFilled(
        draw, chip_min, chip_max, IM_COL32(255, 255, 255, static_cast<int>(12.0f * a)),
        chip_round, ImDrawFlags_RoundCornersAll, 16);
    draw->AddText(status_font, status_sz,
                  ImVec2(std::floor(chip_min.x + chip_pad_x + 0.5f),
                         theme::CenteredTextTop(status_font, chip_min.y, chip_h)),
                  WithAlpha(status_color, a), status);
  }

  ImGui::SetCursorPos(ImVec2(sidebar_width, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  // Full remaining width — custom overlay scrollbar (not ImGui's edge strip) so the grab
  // stays inside the DWM-rounded shell and does not square off the window corners.
  const float page_width = window_size.x - sidebar_width;
  ImGui::BeginChild("##settings_page", ImVec2(page_width, window_size.y), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar);
  content_alpha *=
      window.motion_system_.AnimateValue(ui::motion::MotionKey("page", "content", "alpha"), 1.0f,
                                         window.motion_tokens_.tab_fade, 0.0f);
  const ImVec2 page_offset = window.motion_system_.AnimateVector(
      ui::motion::MotionKey("page", "content", "offset"), ImVec2(0.0f, 0.0f),
      window.motion_tokens_.tab_slide, ImVec2(0.0f, 12.0f));
  y_offset += px(page_offset.y);
  if (window.reset_page_scroll_) {
    ImGui::SetScrollY(0.0f);
    window.reset_page_scroll_ = false;
  }
  if (window.initial_page_scroll_) {
    ImGui::SetScrollY(*window.initial_page_scroll_);
    window.initial_page_scroll_.reset();
  }
  draw = ImGui::GetWindowDrawList();
  const ImVec2 size = ImGui::GetWindowSize();
  const ImVec2 origin = ImGui::GetWindowPos();
  // Full child width so left/right page insets stay equal. Reserving ScrollbarSize
  // permanently made the right margin larger than the left (visible on Apps).
  // Scrollbar overlays the right edge over the page inset.
  const float layout_width = size.x;

  const char* page_scope = "general";
  switch (window.selected_page_) {
    case SettingsWindow::Page::kGeneral:
      page_scope = "general";
      break;
    case SettingsWindow::Page::kAnimation:
      page_scope = "motion";
      break;
    case SettingsWindow::Page::kApplications:
      page_scope = "apps";
      break;
    case SettingsWindow::Page::kDisplays:
      page_scope = "displays";
      break;
    case SettingsWindow::Page::kWindowsIntegration:
      page_scope = "system";
      break;
    case SettingsWindow::Page::kHotkeys:
      page_scope = "hotkeys";
      break;
    case SettingsWindow::Page::kDiagnostics:
      page_scope = "repair";
      break;
#ifdef _DEBUG
    case SettingsWindow::Page::kStressTest:
      page_scope = "stress-test";
      break;
#endif
    case SettingsWindow::Page::kAbout:
      page_scope = "about";
      break;
  }
  ui::components::PageLayout layout(draw, origin, layout_width, scale, content_alpha,
                                    px(16.0f) + y_offset, &widget_motion, page_scope);
  // ── Effect ──────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kGeneral) {
    ui::pages::GeneralPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  // ── Motion ──────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kAnimation) {
    ui::pages::AnimationPage::Render(window, layout, widget_motion, scale, content_alpha, y_offset);
  }

  // ── Apps ────────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kApplications) {
    ui::pages::ApplicationsPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  // ── Displays ────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kDisplays) {
    ui::pages::DisplaysPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  // ── System ──────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kWindowsIntegration) {
    ui::pages::WindowsIntegrationPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  // ── Hotkeys ─────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kHotkeys) {
    ui::pages::HotkeysPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  // ── Repair ──────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kDiagnostics) {
    ui::pages::DiagnosticsPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

#ifdef _DEBUG
  if (window.selected_page_ == SettingsWindow::Page::kStressTest) {
    ui::pages::StressTestPage::Render(window, layout, widget_motion, scale, content_alpha);
  }
#endif

  // ── About ───────────────────────────────────────────────────────────────
  if (window.selected_page_ == SettingsWindow::Page::kAbout) {
    ui::pages::AboutPage::Render(window, layout, widget_motion, scale, content_alpha);
  }

  ImGui::SetCursorPos(
      ImVec2(0.0f, layout.content_bottom() + px(theme::Metrics::kScrollBottomPadding)));
  ImGui::Dummy(ImVec2(1.0f, 1.0f));

  const float scroll_y = ImGui::GetScrollY();
  window.current_page_scroll_ = scroll_y;
  const float scroll_max = ImGui::GetScrollMaxY();
  const float fade_height = px(theme::Metrics::kScrollFadeHeight);
  // Keep fades clear of the overlay scrollbar strip on the right edge.
  const float scrollbar_reserve = px(10.0f);
  const float fade_right = origin.x + size.x - scrollbar_reserve;
  if (scroll_y > 0.5f) {
    draw->AddRectFilledMultiColor(origin, ImVec2(fade_right, origin.y + fade_height),
                                  WithAlpha(theme::kMainBackground, content_alpha),
                                  WithAlpha(theme::kMainBackground, content_alpha),
                                  WithAlpha(theme::kMainBackground, 0.0f),
                                  WithAlpha(theme::kMainBackground, 0.0f));
  }
  if (scroll_y + 0.5f < scroll_max) {
    draw->AddRectFilledMultiColor(
        ImVec2(origin.x, origin.y + size.y - fade_height), ImVec2(fade_right, origin.y + size.y),
        WithAlpha(theme::kMainBackground, 0.0f), WithAlpha(theme::kMainBackground, 0.0f),
        WithAlpha(theme::kMainBackground, content_alpha),
        WithAlpha(theme::kMainBackground, content_alpha));
  }

  // Overlay scrollbar: inset by shell rounding so it follows the rounded window, not a
  // full-height square strip that fights DWM corner radius.
  if (scroll_max > 0.5f && content_alpha > 0.02f) {
    const float bar_w = px(5.0f);
    const float edge_pad = px(4.0f);
    // Match outer shell radius so the thumb never enters the curved corner zone.
    const float y_inset = shell_round + px(2.0f);
    const float track_x = origin.x + size.x - edge_pad - bar_w;
    const float track_top = origin.y + y_inset;
    const float track_bot = origin.y + size.y - y_inset;
    const float track_h = std::max(1.0f, track_bot - track_top);
    const float view_h = size.y;
    const float content_h = view_h + scroll_max;
    // Min grab must never exceed track_h — MSVC debug std::clamp asserts if lo > hi.
    const float grab_h_floor = std::min(px(28.0f), track_h);
    const float grab_h =
        std::clamp(track_h * (view_h / std::max(content_h, 1.0f)), grab_h_floor, track_h);
    const float grab_travel = std::max(0.0f, track_h - grab_h);
    const float grab_t = scroll_max > 0.0f ? std::clamp(scroll_y / scroll_max, 0.0f, 1.0f) : 0.0f;
    const float grab_y = track_top + grab_travel * grab_t;

    const ImVec2 track_min(track_x, track_top);
    const ImVec2 track_max(track_x + bar_w, track_bot);
    const ImVec2 grab_min(track_x, grab_y);
    const ImVec2 grab_max(track_x + bar_w, grab_y + grab_h);

    // Hit target slightly wider than the visual thumb for easier grabbing.
    const ImVec2 hit_min(track_x - px(4.0f), track_top);
    const ImVec2 hit_max(track_x + bar_w + px(2.0f), track_bot);
    const bool track_hovered = ImGui::IsMouseHoveringRect(hit_min, hit_max, false);
    const bool grab_hovered = ImGui::IsMouseHoveringRect(grab_min, grab_max, false);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID drag_id = ImGui::GetID("##page_scrollbar_drag");
    const ImGuiID grab_off_id = ImGui::GetID("##page_scrollbar_grab_off");
    bool dragging = storage->GetBool(drag_id, false);
    float grab_off = storage->GetFloat(grab_off_id, 0.0f);
    const ImGuiIO& io = ImGui::GetIO();

    if (track_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (!grab_hovered && grab_travel > 0.0f) {
        const float click_t =
            std::clamp((io.MousePos.y - track_top - grab_h * 0.5f) / grab_travel, 0.0f, 1.0f);
        ImGui::SetScrollY(click_t * scroll_max);
      }
      // Recompute grab after possible jump so drag offset stays correct.
      const float gy =
          track_top +
          grab_travel *
              (scroll_max > 0.0f ? std::clamp(ImGui::GetScrollY() / scroll_max, 0.0f, 1.0f) : 0.0f);
      grab_off = io.MousePos.y - gy;
      dragging = true;
    }
    if (dragging) {
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging = false;
      } else if (grab_travel > 0.0f) {
        const float new_t =
            std::clamp((io.MousePos.y - grab_off - track_top) / grab_travel, 0.0f, 1.0f);
        ImGui::SetScrollY(new_t * scroll_max);
      }
    }
    storage->SetBool(drag_id, dragging);
    storage->SetFloat(grab_off_id, grab_off);

    if (track_hovered || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Fresh grab rect after possible SetScrollY this frame.
    const float live_scroll = ImGui::GetScrollY();
    const float live_t =
        scroll_max > 0.0f ? std::clamp(live_scroll / scroll_max, 0.0f, 1.0f) : 0.0f;
    const float live_grab_y = track_top + grab_travel * live_t;
    const ImVec2 live_grab_min(track_x, live_grab_y);
    const ImVec2 live_grab_max(track_x + bar_w, live_grab_y + grab_h);

    const float hover_amt = (grab_hovered || dragging || track_hovered) ? 1.0f : 0.0f;
    const float grab_alpha = (0.16f + 0.14f * hover_amt) * content_alpha;
    // FG list: above page content + scroll fades; still clipped by the HWND/DWM round.
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    theme::DrawSmoothRoundRectFilled(
        fg, live_grab_min, live_grab_max,
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, grab_alpha)), bar_w * 0.5f,
        ImDrawFlags_RoundCornersAll, 16);
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Save toast — foreground draw list so it sits above page, popups, and scroll fades.
  // Position: top-center of the content pane (right of sidebar), not bottom-right.
  {
    const ULONGLONG now = GetTickCount64();
    const bool toast_live = !window.save_feedback_.empty() && now < window.save_feedback_until_ms_;
    auto& motion = window.motion_system_;
    const auto& tokens = window.motion_tokens_;
    const auto show_key = ui::motion::MotionKey("toast", "save", "show");
    const float show =
        motion.AnimateValue(show_key, toast_live ? 1.0f : 0.0f,
                            toast_live ? tokens.fade_fast : tokens.popup_close, 0.0f);

    if (!toast_live && show <= 0.02f) {
      if (!window.save_feedback_.empty()) {
        window.save_feedback_.clear();
        motion.Forget(show_key);
      }
    } else if (show > 0.01f && !window.save_feedback_.empty()) {
      ImFont* toast_font = window.font_small_ ? window.font_small_ : ImGui::GetFont();
      const float font_sz = toast_font->FontSize;
      const ImVec2 text_size =
          toast_font->CalcTextSizeA(font_sz, FLT_MAX, 0.0f, window.save_feedback_.c_str());
      const bool is_error = window.save_feedback_error_;
      const float pad_x = px(14.0f);
      const float pad_y = px(8.0f);
      const float icon_slot = is_error ? 0.0f : px(16.0f);
      const float toast_h = text_size.y + pad_y * 2.0f;
      const float toast_w = text_size.x + pad_x * 2.0f + icon_slot;
      // Content pane (second half): horizontally centered, near the top.
      const float content_left = sidebar_width;
      const float content_center_x = content_left + (window_size.x - content_left) * 0.5f;
      const float top_margin = px(14.0f);
      const float slide = (1.0f - show) * px(-10.0f);  // enters from above
      const float toast_x = content_center_x - toast_w * 0.5f;
      const float toast_y = top_margin + slide;
      const ImVec2 toast_min = window_point(toast_x, toast_y);
      const ImVec2 toast_max(toast_min.x + toast_w, toast_min.y + toast_h);
      const float rounding = toast_h * 0.5f;
      const float a = content_alpha * show;

      ImDrawList* fg = ImGui::GetForegroundDrawList();
      theme::DrawSmoothRoundRectFilled(
          fg, ImVec2(toast_min.x, toast_min.y + px(1.5f)),
          ImVec2(toast_max.x, toast_max.y + px(1.5f)),
          IM_COL32(0, 0, 0, static_cast<int>(70.0f * a)), rounding, ImDrawFlags_RoundCornersAll, 16);
      theme::DrawSmoothRoundRectFilled(
          fg, toast_min, toast_max, IM_COL32(28, 28, 30, static_cast<int>(250.0f * a)),
          rounding, ImDrawFlags_RoundCornersAll, 16);
      theme::DrawSmoothRoundRectOutline(
          fg, toast_min, toast_max,
          IM_COL32(is_error ? 90 : 52, is_error ? 40 : 52, is_error ? 42 : 55,
                   static_cast<int>(255.0f * a)),
          rounding, std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);

      float text_x = toast_min.x + pad_x;
      if (!is_error) {
        const ImVec2 cc(toast_min.x + pad_x + px(5.0f), toast_min.y + toast_h * 0.5f);
        const float s = px(3.2f);
        fg->PathLineTo(ImVec2(cc.x - s, cc.y));
        fg->PathLineTo(ImVec2(cc.x - s * 0.2f, cc.y + s * 0.85f));
        fg->PathLineTo(ImVec2(cc.x + s * 1.15f, cc.y - s * 0.85f));
        fg->PathStroke(IM_COL32(150, 210, 165, static_cast<int>(255.0f * a)), 0,
                       std::max(1.2f, 1.4f * scale));
        text_x += icon_slot;
      }

      const ImU32 text_col = is_error ? IM_COL32(235, 140, 140, static_cast<int>(255.0f * a))
                                      : IM_COL32(220, 220, 224, static_cast<int>(255.0f * a));
      fg->AddText(toast_font, font_sz,
                  ImVec2(std::floor(text_x + 0.5f),
                         theme::CenteredTextTop(toast_font, toast_min.y, toast_h)),
                  text_col, window.save_feedback_.c_str());
    }
  }

  UpdatePresenter::Render(window);

  // Window chrome is intentionally last. During an update the complete application content
  // can dissolve into the updater workspace while the traffic lights remain pixel-stable.
  switch (theme::DrawTrafficLights(widget_motion, window_origin, scale, window_alpha)) {
    case theme::TrafficLightAction::kClose:
      window.HandleCloseRequest();
      break;
    case theme::TrafficLightAction::kMinimize:
      ShowWindow(window.hwnd_, SW_MINIMIZE);
      break;
    case theme::TrafficLightAction::kZoom:
      ShowWindow(window.hwnd_, IsZoomed(window.hwnd_) != FALSE ? SW_RESTORE : SW_MAXIMIZE);
      window.ForceRender();
      break;
    case theme::TrafficLightAction::kNone:
      break;
  }

  // Apple macOS native window outline: 1px frosted glass perimeter stroke with top specular highlight.
  theme::DrawWindowOutline(ImGui::GetForegroundDrawList(), window_origin,
                           window_point(window_size.x, window_size.y), shell_round, scale,
                           window_alpha);

  ImGui::End();
}

}  // namespace minimize::ui
