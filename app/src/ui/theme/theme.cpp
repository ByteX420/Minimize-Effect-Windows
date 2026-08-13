#include "pch.hpp"

#include "ui/theme/theme.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "ui/theme/theme_tokens.hpp"

namespace minimize::ui::theme {
namespace {

ImU32 Alpha(ImU32 color, float alpha) {
  const auto value = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
  return (color & 0x00ffffffu) | (value << 24u);
}

ImU32 Mix(ImU32 from, ImU32 to, float amount) {
  const ImVec4 a = ImGui::ColorConvertU32ToFloat4(from);
  const ImVec4 b = ImGui::ColorConvertU32ToFloat4(to);
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount, a.z + (b.z - a.z) * amount,
             a.w + (b.w - a.w) * amount));
}

// 1:1 from 795f55b2 — center-out arms, AA off (prevents double-blend halo / asymmetry).
void DrawSymmetricX(ImDrawList* draw, const ImVec2& min, float size, ImU32 color, float scale) {
  const ImVec2 center(min.x + size * 0.5f - 0.5f, min.y + size * 0.5f - 0.5f);
  const float arm_length = 4.0f * scale;
  const float thickness = 1.0f;

  const ImDrawListFlags old_flags = draw->Flags;
  draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;

  draw->AddLine(center, ImVec2(center.x - arm_length, center.y - arm_length), color, thickness);
  draw->AddLine(center, ImVec2(center.x + arm_length, center.y + arm_length), color, thickness);
  draw->AddLine(center, ImVec2(center.x + arm_length, center.y - arm_length), color, thickness);
  draw->AddLine(center, ImVec2(center.x - arm_length, center.y + arm_length), color, thickness);

  draw->Flags = old_flags;
}

}  // namespace

void ApplyStyle(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.WindowRounding = Metrics::kWindowRounding * scale;
  style.WindowBorderSize = 0.0f;
  style.ChildRounding = Metrics::kControlRounding * scale;
  style.ChildBorderSize = 0.0f;
  style.PopupRounding = Metrics::kControlRounding * scale;
  style.PopupBorderSize = 1.0f * scale;
  style.FramePadding = ImVec2(12.0f * scale, 8.0f * scale);
  style.FrameRounding = Metrics::kControlRounding * scale;
  style.FrameBorderSize = 1.0f * scale;
  style.ItemSpacing = ImVec2(10.0f * scale, 8.0f * scale);
  style.ItemInnerSpacing = ImVec2(8.0f * scale, 6.0f * scale);
  // Slim edge scrollbar — sits flush on the content pane.
  style.ScrollbarSize = 8.0f * scale;
  style.ScrollbarRounding = 4.0f * scale;
  style.GrabMinSize = 8.0f * scale;
  style.GrabRounding = 3.0f * scale;
  style.TabRounding = Metrics::kControlRounding * scale;
  style.AntiAliasedLines = true;
  style.AntiAliasedFill = true;
  style.Colors[ImGuiCol_WindowBg] = ui::theme::kMainColor;
  style.Colors[ImGuiCol_ChildBg] = ui::theme::kPanelColor;
  style.Colors[ImGuiCol_PopupBg] = ui::theme::kComboBackgroundColor;
  style.Colors[ImGuiCol_Text] = ui::theme::kTextColor;
  style.Colors[ImGuiCol_TextDisabled] = ui::theme::kTextDimColor;
  style.Colors[ImGuiCol_Border] = ui::theme::kBorderColor;
  style.Colors[ImGuiCol_FrameBg] = ui::theme::kComboBackgroundColor;
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
  style.Colors[ImGuiCol_FrameBgActive] = ui::theme::kPanelHeaderColor;
  style.Colors[ImGuiCol_Button] = ui::theme::kPanelHeaderColor;
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.14f, 0.15f, 1.0f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.18f, 0.19f, 1.0f);
  style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
  style.Colors[ImGuiCol_CheckMark] = ui::theme::kAccentColor;
  style.Colors[ImGuiCol_SliderGrab] = ui::theme::kAccentColor;
  style.Colors[ImGuiCol_SliderGrabActive] = ui::theme::kTextColor;
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.24f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.34f);
  // Soft dark veil behind license / other modals (not the default washed-out grey).
  style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

void DrawGradientShadow(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius, float alpha,
                        float scale) {
  (void)radius;
  (void)scale;
  draw->AddRect(ImVec2(min.x - 0.5f, min.y - 0.5f), ImVec2(max.x + 0.5f, max.y + 0.5f),
                IM_COL32(0, 0, 0, static_cast<int>(alpha * 40.0f)), 0.0f, 0, 1.0f);
}

void PathSmoothRoundRect(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding,
                         ImDrawFlags flags, int segments) {
  if (!draw) return;

  if ((flags & ImDrawFlags_RoundCornersMask_) == 0) {
    flags |= ImDrawFlags_RoundCornersAll;
  }

  const float width = std::abs(max.x - min.x);
  const float height = std::abs(max.y - min.y);
  if (width <= 0.0f || height <= 0.0f) return;

  const float max_r = (std::min)(width * 0.5f, height * 0.5f);
  const float clamped_rounding = (std::min)(rounding, max_r);

  if (clamped_rounding < 0.5f || (flags & ImDrawFlags_RoundCornersMask_) == ImDrawFlags_RoundCornersNone) {
    draw->PathLineTo(min);
    draw->PathLineTo(ImVec2(max.x, min.y));
    draw->PathLineTo(max);
    draw->PathLineTo(ImVec2(min.x, max.y));
    return;
  }

  constexpr float kPi = 3.14159265358979323846f;
  const int seg = segments > 0 ? segments : std::clamp(static_cast<int>(clamped_rounding * 1.5f), 4, 16);

  const float tl = (flags & ImDrawFlags_RoundCornersTopLeft) ? clamped_rounding : 0.0f;
  const float tr = (flags & ImDrawFlags_RoundCornersTopRight) ? clamped_rounding : 0.0f;
  const float br = (flags & ImDrawFlags_RoundCornersBottomRight) ? clamped_rounding : 0.0f;
  const float bl = (flags & ImDrawFlags_RoundCornersBottomLeft) ? clamped_rounding : 0.0f;

  // Vertical capsule (e.g. scrollbar thumb, vertical pill): top and bottom form seamless semicircles.
  if ((flags & ImDrawFlags_RoundCornersAll) == ImDrawFlags_RoundCornersAll &&
      tl == clamped_rounding && tr == clamped_rounding && br == clamped_rounding && bl == clamped_rounding &&
      std::abs(width - 2.0f * clamped_rounding) <= 0.5f && height >= width) {
    const float mid_x = (min.x + max.x) * 0.5f;
    const float r = clamped_rounding;
    const int semi_seg = std::max(4, seg);
    draw->PathArcTo(ImVec2(mid_x, min.y + r), r, kPi, kPi * 2.0f, semi_seg);
    draw->PathArcTo(ImVec2(mid_x, max.y - r), r, 0.0f, kPi, semi_seg);
    return;
  }

  // Horizontal capsule (e.g. switch track, horizontal pill): left and right form seamless semicircles.
  if ((flags & ImDrawFlags_RoundCornersAll) == ImDrawFlags_RoundCornersAll &&
      tl == clamped_rounding && tr == clamped_rounding && br == clamped_rounding && bl == clamped_rounding &&
      std::abs(height - 2.0f * clamped_rounding) <= 0.5f && width >= height) {
    const float mid_y = (min.y + max.y) * 0.5f;
    const float r = clamped_rounding;
    const int semi_seg = std::max(4, seg);
    draw->PathArcTo(ImVec2(max.x - r, mid_y), r, kPi * 1.5f, kPi * 2.5f, semi_seg);
    draw->PathArcTo(ImVec2(min.x + r, mid_y), r, kPi * 0.5f, kPi * 1.5f, semi_seg);
    return;
  }

  if (tl > 0.0f) {
    draw->PathArcTo(ImVec2(min.x + tl, min.y + tl), tl, kPi, kPi * 1.5f, seg);
  } else {
    draw->PathLineTo(min);
  }
  if (tr > 0.0f) {
    draw->PathArcTo(ImVec2(max.x - tr, min.y + tr), tr, kPi * 1.5f, kPi * 2.0f, seg);
  } else {
    draw->PathLineTo(ImVec2(max.x, min.y));
  }
  if (br > 0.0f) {
    draw->PathArcTo(ImVec2(max.x - br, max.y - br), br, 0.0f, kPi * 0.5f, seg);
  } else {
    draw->PathLineTo(max);
  }
  if (bl > 0.0f) {
    draw->PathArcTo(ImVec2(min.x + bl, max.y - bl), bl, kPi * 0.5f, kPi, seg);
  } else {
    draw->PathLineTo(ImVec2(min.x, max.y));
  }
}

void DrawSmoothRoundRectFilled(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 col, float rounding,
                               ImDrawFlags flags, int segments) {
  if (!draw || (col & IM_COL32_A_MASK) == 0) return;
  if (rounding < 0.5f) {
    draw->AddRectFilled(min, max, col);
    return;
  }
  PathSmoothRoundRect(draw, min, max, rounding, flags, segments);
  draw->PathFillConvex(col);
}

void DrawSmoothRoundRectOutline(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 col, float rounding,
                                float stroke, ImDrawFlags flags, int segments) {
  if (!draw || (col & IM_COL32_A_MASK) == 0 || stroke <= 0.0f) return;
  if (rounding < 0.5f) {
    draw->AddRect(min, max, col, 0.0f, 0, stroke);
    return;
  }
  const float half_stroke = stroke * 0.5f;
  const ImVec2 outline_min(min.x + half_stroke, min.y + half_stroke);
  const ImVec2 outline_max(max.x - half_stroke, max.y - half_stroke);
  const float outline_round = std::max(0.0f, rounding - half_stroke);
  PathSmoothRoundRect(draw, outline_min, outline_max, outline_round, flags, segments);
  draw->PathStroke(col, ImDrawFlags_Closed, stroke);
}

void DrawWindowOutline(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float scale,
                       float alpha) {
  if (!draw || alpha <= 0.001f) return;

  const float stroke = std::max(1.0f, scale);
  const float half_stroke = stroke * 0.5f;

  // Center the stroke half_stroke within the window perimeter so the outer boundary aligns
  // smoothly with the window background and maintains uniform thickness everywhere.
  const ImVec2 outline_min(min.x + half_stroke, min.y + half_stroke);
  const ImVec2 outline_max(max.x - half_stroke, max.y - half_stroke);
  const float outline_round = std::max(0.0f, rounding - half_stroke);

  // 1. Apple macOS primary perimeter stroke: subtle translucent zinc/white outline.
  // Using 16 segments per corner guarantees smooth continuous arcs with zero faceting or pixelation.
  const ImU32 perimeter_color =
      IM_COL32(255, 255, 255, static_cast<int>(std::clamp(alpha * 36.0f, 0.0f, 255.0f)));
  PathSmoothRoundRect(draw, outline_min, outline_max, outline_round, ImDrawFlags_RoundCornersAll, 16);
  draw->PathStroke(perimeter_color, ImDrawFlags_Closed, stroke);

  // 2. Apple macOS subtle specular highlight on the top edge (simulates top ambient illumination).
  const float top_inset = rounding;
  if (outline_max.x - outline_min.x > top_inset * 2.0f) {
    const ImU32 specular_color =
        IM_COL32(255, 255, 255, static_cast<int>(std::clamp(alpha * 20.0f, 0.0f, 255.0f)));
    draw->AddLine(ImVec2(outline_min.x + top_inset, outline_min.y),
                  ImVec2(outline_max.x - top_inset, outline_min.y), specular_color, stroke);
  }
}

void DrawCard(ImDrawList* draw, ImVec2 min, ImVec2 max, float scale, float alpha) {
  const float rounding = Metrics::kCardRounding * scale;
  const float stroke = std::max(1.0f, scale);
  ImVec4 panel = ui::theme::kPanelColor;
  panel.w *= alpha;
  ImVec4 border = ui::theme::kBorderColor;
  border.w *= 0.85f * alpha;
  DrawSmoothRoundRectFilled(draw, min, max, ImGui::GetColorU32(panel), rounding,
                            ImDrawFlags_RoundCornersAll, 16);
  DrawSmoothRoundRectOutline(draw, min, max, ImGui::GetColorU32(border), rounding, stroke,
                             ImDrawFlags_RoundCornersAll, 16);
}

void DrawSeparator(ImDrawList* draw, ImVec2 min, ImVec2 max, float alpha) {
  draw->AddLine(min, max, Alpha(kSeparator, alpha * 0.7f), 1.0f);
}

TrafficLightAction DrawTrafficLights(const motion::MotionContext& motion, ImVec2 window_origin,
                                     float scale, float alpha) {
  constexpr const char* ids[] = {"##traffic_close", "##traffic_minimize", "##traffic_zoom"};
  // Compact macOS-style dots, left-aligned with the nav column.
  const float inset = Metrics::kSidebarMargin * scale;
  const float half_size = 6.0f * scale;
  const float spacing = 20.0f * scale;
  const ImVec2 base(window_origin.x + inset + half_size + 2.0f * scale,
                    window_origin.y + 18.0f * scale);
  // Hit pad must leave a gap between targets (spacing - 2*(half+pad) > 0) so two lights
  // cannot be under the cursor at once — was 5px and overlapped by ~2px.
  const float hit_pad = 3.0f * scale;
  const float hit_half = half_size + hit_pad;

  ImVec2 centers[3]{};
  bool clicked[3]{};
  bool pressed[3]{};
  bool item_hovered[3]{};
  for (int index = 0; index < 3; ++index) {
    centers[index] = ImVec2(base.x + spacing * static_cast<float>(index), base.y);
    const ImVec2 hit_min(centers[index].x - hit_half, centers[index].y - hit_half);
    ImGui::SetCursorScreenPos(hit_min);
    ImGui::InvisibleButton(ids[index], ImVec2(hit_half * 2.0f, hit_half * 2.0f));
    // Allow hover even if a (legacy) popup is up — combo itself is non-modal now.
    item_hovered[index] = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    pressed[index] = ImGui::IsItemActive();
    clicked[index] = ImGui::IsItemClicked();
  }

  // Exactly one light may own hover (later submitted items sit on top if anything overlaps).
  int exclusive = -1;
  for (int index = 0; index < 3; ++index) {
    if (item_hovered[index]) exclusive = index;
  }
  if (exclusive >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  for (int index = 0; index < 3; ++index) {
    const bool is_hot = exclusive == index;
    // Snap non-hot targets to 0 so exit/enter never leave two glyphs mid-fade.
    const float hover =
        motion.system.AnimateValue(ui::motion::MotionKey("window_controls", ids[index], "hover"),
                                   is_hot ? 1.0f : 0.0f, motion.tokens.hover_fast);
    const float press = motion.system.AnimateValue(
        ui::motion::MotionKey("window_controls", ids[index], "press"),
        is_hot && pressed[index] ? 1.0f : 0.0f, motion.tokens.press_fast);
    const float grow = 1.0f + 0.08f * hover - 0.06f * press;
    const float r = half_size * grow;
    const ImVec2 center = centers[index];
    // Quiet idle dots; only tint on hover (close → red, others → slight lift).
    const ImU32 idle = IM_COL32(58, 58, 62, 255);
    const ImU32 hover_col =
        index == 0 ? IM_COL32(200, 72, 72, 255)
                   : (index == 1 ? IM_COL32(200, 160, 70, 255) : IM_COL32(90, 170, 100, 255));
    // Only the exclusive hot light tints/grows — ignore residual motion on the others.
    const float visual_hover = is_hot ? hover : 0.0f;
    draw->AddCircleFilled(center, is_hot ? r : half_size,
                          Alpha(Mix(idle, hover_col, visual_hover), alpha), 24);
    if (is_hot && hover > 0.15f) {
      // Glyph only on the single hot light — never on a fading neighbour.
      const ImU32 icon = Alpha(IM_COL32(30, 30, 32, 255), alpha * hover);
      if (index == 0) {
        const float icon_size = 10.0f * scale;
        DrawSymmetricX(draw, ImVec2(center.x - icon_size * 0.5f, center.y - icon_size * 0.5f),
                       icon_size, icon, scale * 0.7f);
      } else if (index == 1) {
        const ImDrawListFlags old_flags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
        draw->AddLine(ImVec2(center.x - 3.0f * scale, center.y),
                      ImVec2(center.x + 3.0f * scale, center.y), icon, 1.0f);
        draw->Flags = old_flags;
      } else {
        draw->AddRect(ImVec2(center.x - 2.4f * scale, center.y - 2.4f * scale),
                      ImVec2(center.x + 2.4f * scale, center.y + 2.4f * scale), icon, 1.0f * scale,
                      0, 1.0f);
      }
    }
  }
  if (clicked[0]) return TrafficLightAction::kClose;
  if (clicked[1]) return TrafficLightAction::kMinimize;
  if (clicked[2]) return TrafficLightAction::kZoom;
  return TrafficLightAction::kNone;
}

bool SidebarItem(const motion::MotionContext& motion, const char* id, const char* label,
                 bool selected, ImVec2 position, ImVec2 size, ImFont* regular, ImFont* emphasis,
                 float scale, float alpha) {
  if (!regular) regular = ImGui::GetFont();
  if (!emphasis) emphasis = regular;
  ImGui::SetCursorScreenPos(position);
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hovered = ImGui::IsItemHovered();
  const bool focused = ImGui::IsItemFocused();
  const bool active = ImGui::IsItemActive();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Same interaction channels as segment rows / buttons: hover + press.
  // Drive fill from the animated hover value (not is_hot) so enter/leave both ease —
  // matching SegmentSelector. Selection fill is the shared sliding pill in SettingsShell.
  const bool is_hot = hovered || focused;
  // Slightly slower than global hover_soft so the pill ease is easy to read.
  const ui::motion::MotionSpec hover_spec =
      ui::motion::MotionSpec::Timed(0.28f, ui::motion::MotionEasing::kSmootherStep);
  const float hover = motion.system.AnimateValue(
      ui::motion::MotionKey("sidebar-main", id, "hover"), is_hot ? 1.0f : 0.0f, hover_spec, 0.0f);
  const float press =
      motion.system.AnimateValue(ui::motion::MotionKey("sidebar-main", id, "press"),
                                 is_hot && active ? 1.0f : 0.0f, motion.tokens.press_fast, 0.0f);
  const float rounding = 8.0f * scale;
  if (!selected && hover > 0.001f) {
    const float fill_alpha = 0.05f * hover * (1.0f - 0.35f * press);
    DrawSmoothRoundRectFilled(draw, position, ImVec2(position.x + size.x, position.y + size.y),
                              ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, fill_alpha * alpha)),
                              rounding, ImDrawFlags_RoundCornersAll, 16);
  }

  // Weight rule: selected nav uses SemiBold (emphasis), idle uses Regular.
  ImFont* font = selected ? emphasis : regular;
  const float font_size = font->FontSize;
  const ImVec4 text_target =
      selected || is_hot ? ui::theme::kTextColor : ui::theme::kTextDimColor;
  ImVec4 text_color = motion.system.AnimateColor(
      ui::motion::MotionKey("sidebar-main", id, "text"), text_target,
      selected ? motion.tokens.select_sharp : motion.tokens.hover_fast, ui::theme::kTextDimColor);
  // Press subtly tightens text (same idea as menu buttons).
  text_color.w *= alpha * (1.0f - 0.08f * press);
  const float text_nudge = 0.5f * press * scale;
  draw->AddText(font, font_size,
                ImVec2(std::floor(position.x + 14.0f * scale + text_nudge + 0.5f),
                       CenteredTextTop(font, position.y, size.y)),
                ImGui::GetColorU32(text_color), label);
  return clicked;
}

}  // namespace minimize::ui::theme
