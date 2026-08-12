#include "pch.hpp"

#include "ui/components/combo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <unordered_map>

#include "ui/components/component_helpers.hpp"
#include "ui/theme/theme_tokens.hpp"

namespace minimize::ui::components {
using ::minimize::ui::motion::MotionContext;
namespace {
using ::minimize::ui::theme::CenteredTextTop;
using ::minimize::ui::theme::Metrics;
std::unordered_map<ImGuiID, bool> g_combo_was_open;
std::unordered_map<ImGuiID, bool> g_combo_closing;

struct ComboItemRenderContext {
  const MotionContext& motion_context;
  ImDrawList* popup_draw = nullptr;
  std::span<const char* const> items;
  ImFont* item_font = nullptr;
  int item_count = 0;
  float row_height = 0.0f;
  float popup_height = 0.0f;
  float frame_width = 0.0f;
  float open_amount = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
  float content_top = 0.0f;
  float text_pad_left = 0.0f;
  float text_pad_check = 0.0f;
  bool closing = false;
  bool popup_disabled = false;
};

bool RenderComboItem(const ComboItemRenderContext& context, int index, int* current) {
  const MotionContext& motion_context = context.motion_context;
  ImDrawList* popup_draw = context.popup_draw;
  const int item_count = context.item_count;
  const float row_height = context.row_height;
  const float popup_height = context.popup_height;
  const float frame_width = context.frame_width;
  const float open_amount = context.open_amount;
  const bool closing = context.closing;
  const bool popup_disabled = context.popup_disabled;
  const std::span<const char* const> items = context.items;
  const float scale = context.scale;
  const float alpha = context.alpha;
  const float content_top = context.content_top;
  const float text_pad_left = context.text_pad_left;
  const float text_pad_check = context.text_pad_check;
  ImFont* item_font = context.item_font;

  const float row_y = content_top + static_cast<float>(index) * row_height;
  if (row_y + row_height < 0.0f || row_y > popup_height) return false;

  ImGui::PushID(index);
  const ImGuiID item_id = ImGui::GetID("##item_animation");
  ImGui::SetCursorPos(ImVec2(0.0f, row_y));
  const ImVec2 item_min = ImGui::GetCursorScreenPos();

  float row_reveal = open_amount;
  if (!closing) {
    const float row_frac =
        (static_cast<float>(index) + 0.55f) / std::max(1.0f, static_cast<float>(item_count));
    const float row_t = std::clamp((open_amount - row_frac * 0.35f) / 0.65f, 0.0f, 1.0f);
    row_reveal = row_t * row_t * (3.0f - 2.0f * row_t);
  }

  bool item_clicked = false;
  bool item_hovered = false;
  if (!popup_disabled) {
    item_clicked = ImGui::InvisibleButton("##item", ImVec2(frame_width, row_height));
    item_hovered = ImGui::IsItemHovered();
  } else {
    ImGui::Dummy(ImVec2(frame_width, row_height));
  }

  bool changed = false;
  if (item_clicked) {
    *current = index;
    changed = true;
  }

  auto& motion = motion_context.system;
  const auto& tokens = motion_context.tokens;

  const bool is_selected = *current == index;
  const float item_hover = motion.AnimateValue(
      ui::motion::MotionKey("menu.combo.item", std::to_string(item_id), "hover"),
      item_hovered ? 1.0f : 0.0f, tokens.hover_soft, 0.0f);
  const float select_amt = motion.AnimateValue(
      ui::motion::MotionKey("menu.combo.item", std::to_string(item_id), "select"),
      is_selected ? 1.0f : 0.0f, tokens.select_sharp, 0.0f);
  const float indent = motion.AnimateValue(
      ui::motion::MotionKey("menu.combo.item", std::to_string(item_id), "indent"),
      (item_hovered || is_selected) ? 1.0f : 0.0f, tokens.hover_soft, 0.0f);
  const float pad_x = 5.0f * scale;
  const float pad_y = 2.5f * scale;

  if ((item_hover > 0.001f || select_amt > 0.001f) && row_reveal > 0.04f) {
    const float fill = (0.10f * select_amt + 0.05f * item_hover * (1.0f - select_amt)) * row_reveal;
    popup_draw->AddRectFilled(
        ImVec2(item_min.x + pad_x, item_min.y + pad_y),
        ImVec2(item_min.x + frame_width - pad_x, item_min.y + row_height - pad_y),
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, fill * alpha)), 6.0f * scale);
  }

  const float check_alpha =
      (select_amt * 1.0f + item_hover * 0.38f * (1.0f - select_amt)) * row_reveal * alpha;
  if (check_alpha > 0.02f) {
    const ImVec2 cc(item_min.x + 12.0f * scale, item_min.y + row_height * 0.5f);
    const float s = 3.0f * scale * (0.55f + 0.45f * std::max(select_amt, item_hover));
    popup_draw->PathLineTo(ImVec2(cc.x - s, cc.y));
    popup_draw->PathLineTo(ImVec2(cc.x - s * 0.25f, cc.y + s * 0.85f));
    popup_draw->PathLineTo(ImVec2(cc.x + s * 1.15f, cc.y - s * 0.85f));
    popup_draw->PathStroke(
        ImGui::GetColorU32(ImVec4(ui::theme::kTextColor.x, ui::theme::kTextColor.y,
                                  ui::theme::kTextColor.z, check_alpha)),
        0, 1.5f * scale);
  }

  const float text_x = item_min.x + text_pad_left + (text_pad_check - text_pad_left) * indent;
  ImVec4 item_color = detail::MixColor(ui::theme::kTextDimColor, ui::theme::kTextColor,
                                       std::max(select_amt, item_hover * 0.85f));
  item_color.w *= alpha * row_reveal;
  popup_draw->AddText(item_font, item_font->FontSize,
                      ImVec2(text_x, CenteredTextTop(item_font, item_min.y, row_height)),
                      ImGui::GetColorU32(item_color), items[index]);
  ImGui::PopID();
  return changed;
}

}  // namespace

bool Combo(const MotionContext& motion_context, const char* id, const char* label, int* current,
           std::span<const char* const> items, const ImVec2& size, ImFont* label_font,
           ImFont* item_font, float scale, float alpha) {
  if (!current || items.empty()) return false;
  if (!label_font) label_font = ImGui::GetFont();
  if (!item_font) item_font = ImGui::GetFont();

  ImGui::PushID(id);
  ImGui::BeginGroup();
  auto& motion = motion_context.system;
  const auto& tokens = motion_context.tokens;
  const float rounding = detail::ControlRounding(scale);

  if (label && label[0] != '\0') {
    ImGui::PushFont(label_font);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImVec4(ui::theme::kTextDimColor.x, ui::theme::kTextDimColor.y,
                                 ui::theme::kTextDimColor.z, ui::theme::kTextDimColor.w * alpha));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }

  const ImVec2 frame_min = ImGui::GetCursorScreenPos();
  const float frame_width = std::max(1.0f, size.x);
  const float frame_height = std::max(1.0f, size.y);
  const bool pressed = ImGui::InvisibleButton("##combo_button", ImVec2(frame_width, frame_height));
  const bool hovered = ImGui::IsItemHovered();
  const bool item_active = ImGui::IsItemActive();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 frame_max = ImGui::GetItemRectMax();
  const bool anchor_visible = ImGui::IsRectVisible(frame_min, frame_max);

  const ImGuiID popup_id = ImGui::GetID("##combo_dropdown");
  const auto popup_alpha_key = detail::MotionKey("menu.combo", id, "popup-alpha");
  const auto popup_slide_key = detail::MotionKey("menu.combo", id, "popup-slide");
  const auto arrow_open_key = detail::MotionKey("menu.combo", id, "arrow-open");
  const auto arrow_color_key = detail::MotionKey("menu.combo", id, "arrow-color");

  bool& was_open = g_combo_was_open[popup_id];
  bool& closing = g_combo_closing[popup_id];
  const ui::motion::MotionSpec& open_spec = tokens.popup_open;
  const ui::motion::MotionSpec& close_spec = tokens.popup_close;
  const ui::motion::MotionSpec& expand_spec = closing ? close_spec : open_spec;

  // The dropdown is deliberately non-modal so the rest of the settings UI remains interactive.
  if (pressed) {
    if (was_open && !closing) {
      closing = true;
    } else {
      motion.Set(popup_alpha_key, 0.0f);
      motion.Set(popup_slide_key, 0.0f);
      was_open = true;
      closing = false;
    }
  }
  if (was_open && !anchor_visible) closing = true;

  const bool session_active = was_open;
  const bool open_visual = session_active && !closing;
  const float open_amount =
      session_active ? std::clamp(motion.AnimateValue(popup_slide_key, closing ? 0.0f : 1.0f,
                                                      expand_spec, 0.0f),
                                  0.0f, 1.0f)
                     : 0.0f;
  (void)motion.AnimateValue(popup_alpha_key, open_visual ? 1.0f : 0.0f, expand_spec, 0.0f);

  const float hover_target = session_active ? 0.0f : (hovered ? 1.0f : 0.0f);
  const float frame_hover = motion.AnimateValue(detail::MotionKey("menu.combo", id, "frame-hover"),
                                                hover_target, tokens.hover_soft, 0.0f);
  const float frame_press =
      motion.AnimateValue(detail::MotionKey("menu.combo", id, "frame-press"),
                          item_active && !session_active ? 1.0f : 0.0f, tokens.press_fast, 0.0f);

  ImVec4 frame_bg = detail::MixColor(ImVec4(0.11f, 0.11f, 0.12f, 1.0f),
                                     ImVec4(0.15f, 0.15f, 0.16f, 1.0f), frame_hover);
  frame_bg = detail::MixColor(frame_bg, ImVec4(0.13f, 0.13f, 0.14f, 1.0f), frame_press);
  ImVec4 frame_border = detail::MixColor(ImVec4(0.22f, 0.22f, 0.24f, 1.0f),
                                         ImVec4(0.36f, 0.36f, 0.39f, 1.0f), frame_hover);
  if (open_amount > 0.001f) {
    frame_bg = detail::MixColor(frame_bg, ImVec4(0.12f, 0.12f, 0.13f, 1.0f), open_amount);
    frame_border = detail::MixColor(frame_border, ImVec4(0.28f, 0.28f, 0.30f, 1.0f), open_amount);
  }
  frame_bg.w *= alpha;
  frame_border.w *= alpha;

  const float row_height = 30.0f * scale;
  const int item_count = static_cast<int>(items.size());
  const int visible_count = std::min(8, item_count);
  const float popup_pad_y = 4.0f * scale;
  const float popup_full_h = static_cast<float>(visible_count) * row_height + popup_pad_y * 2.0f;
  const float popup_height = popup_full_h * open_amount;

  bool opens_upward = false;
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (frame_max.y + popup_full_h > viewport->Pos.y + viewport->Size.y) {
    opens_upward = true;
  }
  // A slight overlap prevents the parent background from showing through at the animated join.
  const float join_overlap = 1.5f * scale;
  ImVec2 popup_position(frame_min.x, frame_max.y - join_overlap);
  if (opens_upward) {
    popup_position.y = frame_min.y - popup_height + join_overlap;
  }

  // Outside-click tests use the final list bounds, independent of the animated list height.
  ImVec2 list_hit_min(frame_min.x, frame_max.y - join_overlap);
  ImVec2 list_hit_max(frame_min.x + frame_width, frame_max.y - join_overlap + popup_full_h);
  if (opens_upward) {
    list_hit_min = ImVec2(frame_min.x, frame_min.y - popup_full_h + join_overlap);
    list_hit_max = ImVec2(frame_min.x + frame_width, frame_min.y + join_overlap);
  }
  // Closing does not consume the click, so the first click on another control still reaches it.
  if (was_open && !closing && !pressed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over_field = mouse.x >= frame_min.x && mouse.x <= frame_max.x &&
                            mouse.y >= frame_min.y && mouse.y <= frame_max.y;
    const bool over_list = mouse.x >= list_hit_min.x && mouse.x <= list_hit_max.x &&
                           mouse.y >= list_hit_min.y && mouse.y <= list_hit_max.y;
    if (!over_field && !over_list) closing = true;
  }

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 frame_bg_u32 = ImGui::GetColorU32(frame_bg);
  const ImU32 frame_border_u32 = ImGui::GetColorU32(frame_border);
  const float th = std::max(1.0f, scale);
  const bool connected = open_amount > 0.001f && popup_height > 0.5f;

  // While open, the field and list form one shell with a single outside border.
  if (!connected) {
    draw->AddRectFilled(frame_min, frame_max, frame_bg_u32, rounding, ImDrawFlags_RoundCornersAll);
    draw->AddRect(frame_min, frame_max, frame_border_u32, rounding, ImDrawFlags_RoundCornersAll,
                  th);
  } else {
    const ImDrawFlags field_corners =
        opens_upward ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_RoundCornersTop;
    draw->AddRectFilled(frame_min, frame_max, frame_bg_u32, rounding, field_corners);
  }

  bool changed = false;
  const bool show_popup = session_active && popup_height > 0.5f;
  if (show_popup) {
    // A regular ImGui window avoids the modal popup stack and keeps titlebar controls responsive.
    const std::string win_name = std::format("##combo_dd_{}", popup_id);
    ImGui::SetNextWindowPos(popup_position);
    ImGui::SetNextWindowSize(ImVec2(frame_width, popup_height));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);

    const ImGuiWindowFlags dd_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(win_name.c_str(), nullptr, dd_flags)) {
      ImDrawList* popup_draw = ImGui::GetWindowDrawList();
      const ImVec2 popup_min = ImGui::GetWindowPos();
      const ImVec2 popup_max(popup_min.x + frame_width, popup_min.y + popup_height);

      ImVec4 body = frame_bg;
      body.w = alpha;
      ImVec4 body_border = frame_border;
      body_border.w = alpha;
      const ImU32 body_u32 = ImGui::GetColorU32(body);
      const ImU32 border_u32 = ImGui::GetColorU32(body_border);

      const ImVec2 shell_min = opens_upward ? popup_min : ImVec2(frame_min.x, frame_min.y);
      const ImVec2 shell_max = opens_upward ? ImVec2(frame_max.x, frame_max.y) : popup_max;
      const float shell_h = shell_max.y - shell_min.y;
      const float shell_r = std::min(rounding, std::max(1.0f, shell_h * 0.5f - 0.5f));

      const ImDrawFlags list_corners =
          opens_upward ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersBottom;
      const float list_r =
          std::min(shell_r, std::max(1.0f, (popup_max.y - popup_min.y) * 0.5f - 0.5f));
      popup_draw->AddRectFilled(popup_min, popup_max, body_u32, list_r, list_corners);
      // Bridge the field/list seam, then draw one outline around the combined shell.
      const float seam = 2.0f * scale;
      if (opens_upward) {
        popup_draw->AddRectFilled(ImVec2(popup_min.x, popup_max.y - seam),
                                  ImVec2(popup_max.x, popup_max.y + seam), body_u32);
      } else {
        popup_draw->AddRectFilled(ImVec2(popup_min.x, popup_min.y - seam),
                                  ImVec2(popup_max.x, popup_min.y + seam), body_u32);
      }
      ImGui::GetForegroundDrawList()->AddRect(shell_min, shell_max, border_u32, shell_r,
                                              ImDrawFlags_RoundCornersAll, th);

      popup_draw->PushClipRect(popup_min, popup_max, true);

      const bool popup_disabled = closing || open_amount < 0.92f;
      if (popup_disabled) ImGui::BeginDisabled();
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

      const float content_block_h = static_cast<float>(item_count) * row_height;
      const float content_top =
          opens_upward ? (popup_height - popup_pad_y - content_block_h) : popup_pad_y;
      const float text_pad_left = 11.0f * scale;
      const float text_pad_check = 24.0f * scale;
      ComboItemRenderContext item_context{
          .motion_context = motion_context,
          .popup_draw = popup_draw,
          .items = items,
          .item_font = item_font,
          .item_count = item_count,
          .row_height = row_height,
          .popup_height = popup_height,
          .frame_width = frame_width,
          .open_amount = open_amount,
          .scale = scale,
          .alpha = alpha,
          .content_top = content_top,
          .text_pad_left = text_pad_left,
          .text_pad_check = text_pad_check,
          .closing = closing,
          .popup_disabled = popup_disabled,
      };
      for (int index = 0; index < item_count; ++index) {
        item_context.closing = closing;
        if (RenderComboItem(item_context, index, current)) {
          changed = true;
          closing = true;
        }
      }
      ImGui::PopStyleVar(2);
      if (popup_disabled) ImGui::EndDisabled();
      popup_draw->PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);

    if (closing && open_amount <= 0.02f) {
      motion.Forget(popup_alpha_key);
      motion.Forget(popup_slide_key);
      motion.Forget(arrow_open_key);
      motion.Forget(arrow_color_key);
      was_open = false;
      closing = false;
    }
  } else if (session_active && closing && open_amount <= 0.02f) {
    motion.Forget(popup_alpha_key);
    motion.Forget(popup_slide_key);
    motion.Forget(arrow_open_key);
    motion.Forget(arrow_color_key);
    was_open = false;
    closing = false;
  }

  const float chevron_zone = 26.0f * scale;
  const char* preview =
      *current >= 0 && *current < static_cast<int>(items.size()) ? items[*current] : "";
  if (preview && preview[0]) {
    draw->PushClipRect(frame_min, ImVec2(frame_max.x - chevron_zone, frame_max.y), true);
    draw->AddText(item_font, item_font->FontSize,
                  ImVec2(std::floor(frame_min.x + 11.0f * scale + 0.5f),
                         CenteredTextTop(item_font, frame_min.y, frame_height)),
                  ImGui::GetColorU32(ImVec4(ui::theme::kTextColor.x, ui::theme::kTextColor.y,
                                            ui::theme::kTextColor.z, alpha)),
                  preview);
    draw->PopClipRect();
  }

  const float chevron_spin =
      motion.AnimateValue(arrow_open_key, open_visual ? 1.0f : 0.0f, expand_spec, 0.0f);
  const float arrow_size = 5.0f * scale;
  const ImVec2 arrow_center(frame_max.x - 13.0f * scale, (frame_min.y + frame_max.y) * 0.5f);
  const ImVec4 arrow_target =
      open_visual || hovered ? ui::theme::kTextColor : ImVec4(0.52f, 0.52f, 0.55f, 1.0f);
  ImVec4 arrow_color = motion.AnimateColor(arrow_color_key, arrow_target, tokens.hover_soft,
                                           ui::theme::kTextDimColor);
  arrow_color.w *= alpha;

  const float angle = chevron_spin * 3.14159265f;
  const float cos_a = std::cos(angle);
  const float sin_a = std::sin(angle);
  auto rot = [&](float lx, float ly) -> ImVec2 {
    return ImVec2(arrow_center.x + lx * cos_a - ly * sin_a,
                  arrow_center.y + lx * sin_a + ly * cos_a);
  };
  draw->PathLineTo(rot(-arrow_size * 0.55f, -arrow_size * 0.2f));
  draw->PathLineTo(rot(0.0f, arrow_size * 0.3f));
  draw->PathLineTo(rot(arrow_size * 0.55f, -arrow_size * 0.2f));
  draw->PathStroke(ImGui::GetColorU32(arrow_color), 0, 1.5f * scale);

  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

}  // namespace minimize::ui::components
