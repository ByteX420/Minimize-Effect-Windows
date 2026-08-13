#include "pch.hpp"

#include "ui/pages/displays_page.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "features/open_windows_service.hpp"
#include "imgui.h"
#include "ui/components/controls.hpp"
#include "ui/settings_window.hpp"
#include "ui/theme/theme.hpp"

namespace minimize::ui::pages {
namespace {

constexpr float kPageTitleTextSize = 22.0f;
constexpr float kPageSubtitleTextSize = 13.0f;
constexpr float kLabelTextSize = 15.0f;
constexpr float kHelperTextSize = 13.0f;
constexpr float kCaptionTextSize = 12.0f;
constexpr ImU32 kPrimaryTextColor = ::minimize::ui::theme::kText;
constexpr ImU32 kSecondaryTextColor = ::minimize::ui::theme::kMutedText;
constexpr ULONGLONG kRefreshIntervalMs = 750;

}  // namespace

void DisplaysPage::Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                          const ::minimize::ui::motion::MotionContext& motion, float scale,
                          float alpha) {
  using namespace ::minimize::ui::theme;
  auto px = [scale](float value) { return value * scale; };
  auto& actions = window.controller_->actions();
  const float button_height = Metrics::kButtonHeight * scale;
  const float action_width = px(92.0f);

  // Same placement as Motion "Reset": top-right of the page title row.
  const float title_top = layout.y();
  layout.Title(window.font_title_, kPageTitleTextSize, "Displays", window.font_small_,
               kPageSubtitleTextSize, "Select a display, then choose Minimize for its windows",
               action_width + px(8.0f));
  layout.SetCursor(layout.group_right() - action_width, title_top + px(2.0f));
  if (ui::components::CompactButton(motion, "##refresh_displays", "Refresh",
                                    ImVec2(action_width, button_height), window.font_body_, scale,
                                    alpha)) {
    window.InvalidateOpenWindowsSnapshot();
    window.ForceRender();
  }

  const ULONGLONG now = GetTickCount64();
  if (now - window.last_open_windows_refresh_ms_ >= kRefreshIntervalMs ||
      !window.open_windows_snapshot_valid_) {
    window.cached_open_windows_ = actions.GetOpenWindowsSnapshot();
    window.last_open_windows_refresh_ms_ = now;
    window.open_windows_snapshot_valid_ = true;
  }

  const auto& snapshot = window.cached_open_windows_;

  if (!snapshot.monitors.empty()) {
    if (window.selected_display_index_ < 0 ||
        window.selected_display_index_ >= static_cast<int>(snapshot.monitors.size())) {
      window.selected_display_index_ = 0;
    }
  } else {
    window.selected_display_index_ = -1;
  }

  const features::OpenMonitorInfo* selected = nullptr;
  if (window.selected_display_index_ >= 0 &&
      window.selected_display_index_ < static_cast<int>(snapshot.monitors.size())) {
    selected = &snapshot.monitors[static_cast<std::size_t>(window.selected_display_index_)];
  }

  int windows_on_display = 0;
  int excluded_on_display = 0;
  if (selected != nullptr) {
    for (const features::OpenWindowInfo& info : snapshot.windows) {
      if (info.monitor == selected->monitor || info.monitor_index == selected->index) {
        ++windows_on_display;
        if (info.minimize_excluded) ++excluded_on_display;
      }
    }
  }

  const float content_w = layout.content_width();
  const float map_h = px(200.0f);
  const float toggle_width = Metrics::kToggleWidth * scale;
  const float toggle_height = (Metrics::kToggleHeight + 4.0f) * scale;

  // One card: arrangement map + disable toggle underneath.
  layout.SectionCaption(window.font_small_, kCaptionTextSize, "ARRANGEMENT");
  layout.BeginGroup();

  // Map row (stack without title band so the card padding is even).
  layout.BeginStackRow(0.0f, map_h / scale);
  const ImVec2 map_min = layout.ToScreen(layout.content_left(), layout.StackControlY());
  const ImVec2 map_max(map_min.x + content_w, map_min.y + map_h);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float map_round = px(10.0f);
  theme::DrawSmoothRoundRectFilled(
      draw, map_min, map_max, IM_COL32(16, 16, 18, static_cast<int>(230 * alpha)), map_round,
      ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectOutline(
      draw, map_min, map_max, IM_COL32(42, 42, 46, static_cast<int>(255 * alpha)), map_round,
      std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);

  const float pad = px(14.0f);
  const float inner_x = map_min.x + pad;
  const float inner_y = map_min.y + pad;
  const float inner_w = std::max(1.0f, content_w - pad * 2.0f);
  const float inner_h = std::max(1.0f, map_h - pad * 2.0f);
  const RECT& virtual_bounds = snapshot.virtual_bounds;
  // One rounded inline outline only (no outer stroke). Mapped monitor rects can overlap by a
  // pixel, so draw selected fill + outline last so nothing covers the selection ring.
  const float outline_thickness = std::max(1.0f, scale);
  const float corner = px(8.0f);

  struct MonitorDraw {
    ImVec2 box0{};
    ImVec2 box1{};
    ImU32 fill = 0;
    ImU32 border = 0;
    std::string label;
    bool selected = false;
  };
  std::vector<MonitorDraw> monitor_draws;
  monitor_draws.reserve(snapshot.monitors.size());

  // Pass 1: hit-test + collect draw data (no painting yet).
  for (const features::OpenMonitorInfo& monitor : snapshot.monitors) {
    const RECT view = features::MapDesktopRectToView(monitor.bounds, virtual_bounds, inner_x,
                                                     inner_y, inner_w, inner_h);
    const ImVec2 m0(static_cast<float>(view.left), static_cast<float>(view.top));
    const float mw = std::max(px(40.0f), static_cast<float>(view.right - view.left));
    const float mh = std::max(px(28.0f), static_cast<float>(view.bottom - view.top));
    const ImVec2 box0 = m0;
    const ImVec2 box1(m0.x + mw, m0.y + mh);

    const std::string hit_id = std::format("##display_hit_{}", monitor.index);
    ImGui::SetCursorScreenPos(box0);
    ImGui::InvisibleButton(hit_id.c_str(), ImVec2(mw, mh));
    const bool hovered = ImGui::IsItemHovered();
    const bool is_selected = monitor.index == window.selected_display_index_;
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) window.selected_display_index_ = monitor.index;

    const float hover =
        motion.system.AnimateValue(ui::motion::MotionKey("displays", hit_id, "hover"),
                                   hovered ? 1.0f : 0.0f, motion.tokens.hover_soft, 0.0f);
    const float select =
        motion.system.AnimateValue(ui::motion::MotionKey("displays", hit_id, "select"),
                                   is_selected ? 1.0f : 0.0f, motion.tokens.select_sharp, 0.0f);

    ImU32 fill =
        is_selected ? IM_COL32(55, 105, 130, static_cast<int>((200 + 40 * select) * alpha))
                    : IM_COL32(32 + static_cast<int>(14 * hover), 32 + static_cast<int>(14 * hover),
                               36 + static_cast<int>(14 * hover), static_cast<int>(230 * alpha));
    if (monitor.minimize_excluded && !is_selected) {
      fill = IM_COL32(48 + static_cast<int>(10 * hover), 34 + static_cast<int>(8 * hover),
                      34 + static_cast<int>(8 * hover), static_cast<int>(230 * alpha));
    }
    // Color only for selection — thickness stays constant.
    const ImU32 border =
        is_selected ? IM_COL32(140, 190, 210, static_cast<int>(255 * alpha))
        : monitor.minimize_excluded
            ? IM_COL32(160 + static_cast<int>(20 * hover), 90 + static_cast<int>(20 * hover),
                       90 + static_cast<int>(20 * hover), static_cast<int>(255 * alpha))
            : IM_COL32(70 + static_cast<int>(40 * hover), 70 + static_cast<int>(40 * hover),
                       78 + static_cast<int>(40 * hover), static_cast<int>(255 * alpha));

    monitor_draws.push_back(MonitorDraw{box0, box1, fill, border, monitor.label, is_selected});
  }

  auto draw_fill = [&](const MonitorDraw& item) {
    theme::DrawSmoothRoundRectFilled(draw, item.box0, item.box1, item.fill, corner,
                                     ImDrawFlags_RoundCornersAll, 16);
  };
  auto draw_inline_outline = [&](const MonitorDraw& item) {
    draw->PushClipRect(item.box0, item.box1, true);
    theme::DrawSmoothRoundRectOutline(draw, item.box0, item.box1, item.border, corner,
                                      outline_thickness, ImDrawFlags_RoundCornersAll, 16);
    draw->PopClipRect();
  };

  // Pass 2: unselected fills, then selected fill (on top at shared/overlapping edges).
  for (const MonitorDraw& item : monitor_draws) {
    if (!item.selected) draw_fill(item);
  }
  for (const MonitorDraw& item : monitor_draws) {
    if (item.selected) draw_fill(item);
  }

  // Pass 3: unselected outlines, then selected outline last so the ring is never covered.
  for (const MonitorDraw& item : monitor_draws) {
    if (!item.selected) draw_inline_outline(item);
  }
  for (const MonitorDraw& item : monitor_draws) {
    if (item.selected) draw_inline_outline(item);
  }

  // Pass 4: labels above everything.
  if (window.font_body_) {
    for (const MonitorDraw& item : monitor_draws) {
      const char* text = item.label.c_str();
      const ImVec2 label_size =
          window.font_body_->CalcTextSizeA(window.font_body_->FontSize, FLT_MAX, 0.0f, text);
      const float mw = item.box1.x - item.box0.x;
      const float mh = item.box1.y - item.box0.y;
      const float lx = item.box0.x + (mw - label_size.x) * 0.5f;
      const float ly = item.box0.y + (mh - label_size.y) * 0.5f;
      draw->AddText(window.font_body_, window.font_body_->FontSize,
                    ImVec2(std::floor(lx + 0.5f), std::floor(ly + 0.5f)),
                    IM_COL32(230, 230, 234, static_cast<int>(255 * alpha)), text);
    }
  }

  layout.SetCursor(layout.content_left(), layout.StackControlY());
  ImGui::Dummy(ImVec2(content_w, map_h));
  layout.EndRow();

  // Disable Minimize — directly under the arrangement map, same card. Persisted to settings.
  layout.BeginRow(Metrics::kRowHeight);
  layout.ReserveControl(toggle_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Disable Minimize on this display",
                  kPrimaryTextColor);
  bool disable_display = selected != nullptr && selected->minimize_excluded;
  const ImVec2 toggle_cursor = layout.ControlCursor(toggle_width, toggle_height);
  layout.SetCursor(toggle_cursor.x, toggle_cursor.y);
  if (selected != nullptr &&
      ui::components::Toggle(motion, "##disable_minimize_display", &disable_display, scale, alpha)) {
    if (actions.SetDisplayMinimizeExcluded(selected->device_name, disable_display)) {
      window.InvalidateOpenWindowsSnapshot();
      window.ForceRender();
    }
  }
  layout.EndRow();
  layout.EndGroup();

  // ── Compact selected-display facts ──
  layout.SectionCaption(window.font_small_, kCaptionTextSize, "SELECTED DISPLAY");
  layout.BeginGroup();

  if (selected == nullptr) {
    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, "No display selected", kSecondaryTextColor);
    layout.EndRow();
  } else {
    const int w = selected->bounds.right - selected->bounds.left;
    const int h = selected->bounds.bottom - selected->bounds.top;

    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, selected->label.c_str(), kPrimaryTextColor);
    layout.RowValue(window.font_small_, kHelperTextSize,
                    selected->is_primary ? "Primary" : "Secondary", kSecondaryTextColor);
    layout.EndRow();

    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Resolution", kPrimaryTextColor);
    layout.RowValue(window.font_small_, kHelperTextSize, std::format("{} × {}", w, h).c_str(),
                    kSecondaryTextColor);
    layout.EndRow();

    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Position", kPrimaryTextColor);
    layout.RowValue(window.font_small_, kHelperTextSize,
                    std::format("{}, {}", selected->bounds.left, selected->bounds.top).c_str(),
                    kSecondaryTextColor);
    layout.EndRow();

    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Open windows", kPrimaryTextColor);
    layout.RowValue(
        window.font_small_, kHelperTextSize,
        std::format("{} ({} disabled)", windows_on_display, excluded_on_display).c_str(),
        kSecondaryTextColor);
    layout.EndRow();

    layout.BeginRow(Metrics::kRowHeightDense);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Minimize", kPrimaryTextColor);
    layout.RowValue(window.font_small_, kHelperTextSize,
                    selected->minimize_excluded ? "Disabled" : "Enabled", kSecondaryTextColor);
    layout.EndRow();
  }

  layout.EndGroup();
}

}  // namespace minimize::ui::pages
