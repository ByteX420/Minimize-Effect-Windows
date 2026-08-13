#include "pch.hpp"

#include "ui/components/controls.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <unordered_map>

#include "ui/components/component_helpers.hpp"
#include "ui/theme/theme_tokens.hpp"

namespace minimize::ui::components {
using ::minimize::ui::motion::MotionContext;
namespace {
using ::minimize::ui::theme::CenteredTextTop;
using ::minimize::ui::theme::Metrics;
std::unordered_map<ImGuiID, std::array<char, 64>> g_slider_value_buffers;
bool ReferenceButton(const MotionContext& motion_context, const char* id, const char* label,
                     const ImVec2& size, ImFont* font, float alpha, bool active) {
  if (!font) font = ImGui::GetFont();
  const ImVec2 position = ImGui::GetCursorScreenPos();
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("##button", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool pressed = ImGui::IsItemActive();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  auto& motion = motion_context.system;
  const auto& tokens = motion_context.tokens;
  // Hover + press only — press drives a centered shrink via the motion system.
  const float hover = motion.AnimateValue(detail::MotionKey("menu.button", id, "hover"),
                                          hovered ? 1.0f : 0.0f, tokens.hover_fast, 0.0f);
  const float press = motion.AnimateValue(detail::MotionKey("menu.button", id, "press"),
                                          pressed ? 1.0f : 0.0f, tokens.press_fast, 0.0f);
  const float selected = motion.AnimateValue(detail::MotionKey("menu.button", id, "on_state"),
                                             active ? 1.0f : 0.0f, tokens.select_sharp, 0.0f);

  // Quiet idle surface; selected reads as a solid lifted pill (not a faint tint).
  const ImVec4 base = detail::MixColor(ImVec4(0.10f, 0.10f, 0.11f, 1.0f),
                                       ImVec4(0.22f, 0.22f, 0.24f, 1.0f), selected);
  const ImVec4 hovered_background =
      detail::MixColor(base, ImVec4(0.15f, 0.15f, 0.16f, 1.0f), hover * (1.0f - selected * 0.5f));
  ImVec4 background =
      detail::MixColor(hovered_background, ImVec4(0.18f, 0.18f, 0.19f, 1.0f), press);
  background.w *= alpha;

  // Smooth press shrink (hit target stays full size; only the drawn pill scales).
  const float visual_scale = 1.0f + 0.015f * hover - 0.06f * press;
  const ImVec2 center(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
  const float half_w = size.x * 0.5f * visual_scale;
  const float half_h = size.y * 0.5f * visual_scale;
  const ImVec2 visual_min(center.x - half_w, center.y - half_h);
  const ImVec2 visual_max(center.x + half_w, center.y + half_h);
  const float rounding =
      std::min(Metrics::kControlRounding * (size.y / 34.0f), size.y * 0.35f) * visual_scale;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  theme::DrawSmoothRoundRectFilled(draw, visual_min, visual_max, ImGui::GetColorU32(background),
                                   rounding, ImDrawFlags_RoundCornersAll, 16);
  ImVec4 border =
      detail::MixColor(ImVec4(0.20f, 0.20f, 0.22f, 1.0f), ImVec4(0.48f, 0.48f, 0.52f, 1.0f),
                       selected * 0.75f + hover * 0.25f * (1.0f - selected));
  border.w *= alpha;
  theme::DrawSmoothRoundRectOutline(draw, visual_min, visual_max, ImGui::GetColorU32(border),
                                    rounding, std::max(1.0f, size.y / 30.0f),
                                    ImDrawFlags_RoundCornersAll, 16);

  const float font_size = font->FontSize;
  ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
  float final_size = font_size;
  if (text_size.x > size.x - 10.0f) {
    final_size = font_size * 0.9f;
    text_size = font->CalcTextSizeA(final_size, FLT_MAX, 0.0f, label);
  }
  final_size *= visual_scale;
  text_size = font->CalcTextSizeA(final_size, FLT_MAX, 0.0f, label);
  // Selected / hover → full text. Idle stays clearly dim so the active pill stands out.
  const float text_mix =
      std::clamp(selected * 1.0f + hover * 0.65f + (active ? 0.35f : 0.0f), 0.0f, 1.0f);
  ImVec4 text_color = detail::MixColor(ui::theme::kTextDimColor, ui::theme::kTextColor, text_mix);
  text_color.w *= alpha * (1.0f - 0.08f * press);
  draw->PushClipRect(visual_min, visual_max, true);
  const float text_x = std::floor(center.x - text_size.x * 0.5f + 0.5f);
  const float text_y = CenteredTextTop(font, final_size, visual_min.y, visual_max.y - visual_min.y);
  draw->AddText(font, final_size, ImVec2(text_x, text_y), ImGui::GetColorU32(text_color), label);
  draw->PopClipRect();
  ImGui::PopID();
  return clicked;
}
}  // namespace

void DelayedTooltip(const char* text, float scale) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || ImGui::IsAnyItemActive()) return;
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(360.0f * scale, FLT_MAX));
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(340.0f * scale);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

bool Toggle(const MotionContext& motion, const char* id, bool* value, float scale, float alpha) {
  const float track_width = Metrics::kToggleWidth * scale;
  const float track_height = Metrics::kToggleHeight * scale;
  const float row_height = track_height + 4.0f * scale;
  const bool changed = ImGui::InvisibleButton(id, ImVec2(track_width, row_height));
  const bool hovered = ImGui::IsItemHovered();
  const bool pressed = ImGui::IsItemActive();
  if (changed && value) *value = !*value;
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  const ImVec2 item_min = ImGui::GetItemRectMin();
  const float center_y = item_min.y + row_height * 0.5f;
  const ImVec2 track_min(item_min.x, center_y - track_height * 0.5f);
  const ImVec2 track_max(item_min.x + track_width, center_y + track_height * 0.5f);
  const float track_rounding = track_height * 0.5f;

  auto& reference_motion = motion.system;
  const auto& tokens = motion.tokens;
  const float hover = reference_motion.AnimateValue(detail::MotionKey("menu.checkbox", id, "hover"),
                                                    hovered ? 1.0f : 0.0f, tokens.hover_fast, 0.0f);
  const float press = reference_motion.AnimateValue(detail::MotionKey("menu.checkbox", id, "press"),
                                                    pressed ? 1.0f : 0.0f, tokens.press_fast, 0.0f);
  // Snappy spring so the knob travel has weight; squash rides the same channel.
  const float fill =
      reference_motion.AnimateValue(detail::MotionKey("menu.checkbox", id, "fill"),
                                    value && *value ? 1.0f : 0.0f, tokens.spring_snappy, 0.0f);
  const float t = std::clamp(fill, 0.0f, 1.0f);
  // Peak squash mid-travel (sin), stronger press squish while held.
  const float mid = std::sin(3.14159265f * t);
  const float travel_squash = mid * mid;  // 0 at ends, 1 at midpoint
  const float squash = std::clamp(travel_squash * 1.15f + press * 0.55f, 0.0f, 1.35f);

  // ON track stays mid-zinc so the white knob actually reads (was near-white-on-white).
  ImVec4 track_off(0.16f, 0.16f, 0.17f, alpha);
  ImVec4 track_on(0.48f, 0.48f, 0.52f, alpha);
  ImVec4 track_color = detail::MixColor(track_off, track_on, t);
  track_color = detail::MixColor(
      track_color,
      detail::MixColor(ImVec4(0.22f, 0.22f, 0.24f, alpha), ImVec4(0.56f, 0.56f, 0.60f, alpha), t),
      hover * 0.5f);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  theme::DrawSmoothRoundRectFilled(draw, track_min, track_max, ImGui::GetColorU32(track_color),
                                   track_rounding, ImDrawFlags_RoundCornersAll, 16);
  // Soft rim keeps the pill edged against dark cards.
  {
    ImVec4 rim =
        detail::MixColor(ImVec4(0.28f, 0.28f, 0.30f, alpha), ImVec4(0.62f, 0.62f, 0.66f, alpha), t);
    rim.w *= 0.9f + 0.1f * hover;
    theme::DrawSmoothRoundRectOutline(draw, track_min, track_max, ImGui::GetColorU32(rim),
                                      track_rounding, std::max(1.0f, scale),
                                      ImDrawFlags_RoundCornersAll, 16);
  }

  // White knob + dark contact shadow — high contrast on both OFF and ON tracks.
  const float padding = 3.0f * scale;
  const float knob_d = track_height - padding * 2.0f;
  const float knob_r = knob_d * 0.5f;
  const float travel = track_width - 2.0f * padding - knob_d;
  const float knob_x = track_min.x + padding + knob_r + travel * t;
  // Stretch wide + flatten on the move; clamp so it never leaves the track.
  const float rx = std::min(knob_r + squash * 2.4f * scale, knob_r + travel * 0.22f);
  const float ry = std::max(2.2f * scale, knob_r - squash * 1.35f * scale);
  const float knob_y = center_y + press * 0.4f * scale;
  theme::DrawSmoothRoundRectFilled(
      draw, ImVec2(knob_x - rx, knob_y - ry + 1.0f * scale),
      ImVec2(knob_x + rx, knob_y + ry + 1.0f * scale),
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f * alpha)), ry,
      ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectFilled(draw, ImVec2(knob_x - rx, knob_y - ry),
                                   ImVec2(knob_x + rx, knob_y + ry),
                                   ImGui::GetColorU32(ImVec4(0.97f, 0.97f, 0.98f, alpha)), ry,
                                   ImDrawFlags_RoundCornersAll, 16);
  // Thin graphite ring so the disc never blends into a mid-gray ON track.
  theme::DrawSmoothRoundRectOutline(draw, ImVec2(knob_x - rx, knob_y - ry),
                                    ImVec2(knob_x + rx, knob_y + ry),
                                    ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.18f * alpha)), ry,
                                    std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);
  return changed;
}

bool Slider(const MotionContext& motion, const char* id, const char* label, float* value,
            float minimum, float maximum, float width, float scale, float alpha, ImFont* label_font,
            float step, float display_multiplier, int display_precision,
            const char* display_suffix) {
  (void)motion;
  (void)step;
  if (!value || maximum <= minimum) return false;
  if (display_multiplier == 0.0f) display_multiplier = 1.0f;
  display_precision = std::clamp(display_precision, 0, 6);
  if (!display_suffix) display_suffix = "";

  if (!label_font) label_font = ImGui::GetFont();
  const float value_at_frame_start = *value;
  const ImVec2 row_origin = ImGui::GetCursorScreenPos();
  const bool has_label = label != nullptr && label[0] != '\0';
  const float label_height = has_label ? 14.0f * scale : 0.0f;
  if (has_label) {
    ImGui::GetWindowDrawList()->AddText(
        label_font, label_font->FontSize, row_origin,
        ImGui::GetColorU32(ImVec4(ui::theme::kTextDimColor.x, ui::theme::kTextDimColor.y,
                                  ui::theme::kTextDimColor.z, ui::theme::kTextDimColor.w * alpha)),
        label);
  }

  // Slider: simple rail + pearl-gray capsule on the fill tip. Small value chip right.
  const float height = Metrics::kSliderHeight * scale;
  const ImVec2 origin(row_origin.x,
                      row_origin.y + (has_label ? label_height + 4.0f * scale : 0.0f));

  // Fixed chip from min/max glyphs — rail width never jumps with the value.
  // Use full baked font size so glyphs are never soft-scaled or clipped.
  const float value_font_sz = label_font->FontSize;
  const std::string probe_lo =
      std::format("{:.{}f}{}", minimum * display_multiplier, display_precision, display_suffix);
  const std::string probe_hi =
      std::format("{:.{}f}{}", maximum * display_multiplier, display_precision, display_suffix);
  const float probe_w =
      std::max(label_font->CalcTextSizeA(value_font_sz, FLT_MAX, 0.0f, probe_lo.c_str()).x,
               label_font->CalcTextSizeA(value_font_sz, FLT_MAX, 0.0f, probe_hi.c_str()).x);
  const float value_chip_w = std::max(36.0f * scale, probe_w + 8.0f * scale);
  const float value_gap = 6.0f * scale;
  const float track_end = origin.x + width - value_chip_w - value_gap;
  // Pearl sits on the rail; leave half-pearl pad so it never clips the ends.
  const float pearl_half_w = 7.0f * scale;
  const float start = origin.x + pearl_half_w;
  const float end = std::max(start + 16.0f * scale, track_end - pearl_half_w);

  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##slider", ImVec2(width - value_chip_w - value_gap, height));
  const bool hovered = ImGui::IsItemHovered();
  const bool slider_active = ImGui::IsItemActive();
  const bool open_value_editor = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  const ImGuiIO& io = ImGui::GetIO();
  // Value changes only via left-click drag on the rail (no wheel / arrow keys).
  if (slider_active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float ratio = std::clamp((io.MousePos.x - start) / (end - start), 0.0f, 1.0f);
    *value = minimum + (maximum - minimum) * ratio;
  }

  // Value box tall enough for full ascent/descent + caret (no clipping).
  const float value_box_width = value_chip_w;
  const float value_box_height =
      std::max(height, (label_font->Ascent - label_font->Descent) + 2.0f * scale);
  const float value_box_y = origin.y + (height - value_box_height) * 0.5f;
  const ImVec2 value_box_min(origin.x + width - value_box_width, value_box_y);
  const ImVec2 value_box_max(value_box_min.x + value_box_width, value_box_min.y + value_box_height);
  const ImGuiID mode_id = ImGui::GetID("##value_mode");
  int* mode = ImGui::GetStateStorage()->GetIntRef(mode_id, 0);
  if (open_value_editor) *mode = 1;

  auto apply_parsed = [&](const char* text) -> bool {
    char* end_ptr = nullptr;
    const float parsed = std::strtof(text, &end_ptr);
    if (end_ptr == text || !std::isfinite(parsed)) return false;
    *value = std::clamp(parsed / display_multiplier, minimum, maximum);
    return true;
  };

  const ImGuiID input_id = ImGui::GetID("##value_input");
  auto& input_buffer = g_slider_value_buffers[input_id];
  bool editing = *mode > 0;
  if (editing) {
    if (*mode == 1) {
      const auto result =
          std::format_to_n(input_buffer.data(), input_buffer.size() - 1, "{:.{}f}",
                           *value * display_multiplier, display_precision);
      *result.out = '\0';
      ImGui::SetKeyboardFocusHere();
      *mode = 2;
    }
    // Center glyphs: FrameHeight = FontSize + 2*pad_y must equal value_box_height.
    const float input_text_w =
        label_font->CalcTextSizeA(label_font->FontSize, FLT_MAX, 0.0f, input_buffer.data()).x;
    const float pad_x = std::max(0.0f, (value_box_width - input_text_w) * 0.5f);
    const float pad_y = std::max(0.0f, (value_box_height - label_font->FontSize) * 0.5f);
    ImGui::SetCursorScreenPos(value_box_min);
    ImGui::PushFont(label_font);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(ui::theme::kTextColor.x, ui::theme::kTextColor.y,
                                                ui::theme::kTextColor.z, alpha));
    ImGui::SetNextItemWidth(value_box_width);
    const bool submitted = ImGui::InputText(
        "##value_input", input_buffer.data(), input_buffer.size(),
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_NoHorizontalScroll);
    // Live-apply so the rail follows the typed number immediately.
    apply_parsed(input_buffer.data());
    if (submitted) {
      apply_parsed(input_buffer.data());
      *mode = 0;
      editing = false;
    } else if (*mode == 2 && !ImGui::IsItemActive() &&
               (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
      apply_parsed(input_buffer.data());
      *mode = 0;
      editing = false;
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
  }

  *value = std::clamp(*value, minimum, maximum);

  auto& reference_motion = motion.system;
  const auto& tokens = motion.tokens;
  const auto fill_key = detail::MotionKey("menu.slider", id, "fill");
  const float visual = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
  // Snappy while scrubbing. For external jumps (Reset/presets), use a short timed snap so the
  // rail does not lag behind the real value on a soft spring.
  const bool interactive = slider_active || editing || *mode > 0;
  const ui::motion::MotionSpec& fill_spec =
      interactive ? tokens.spring_snappy : tokens.select_sharp;
  float animated = reference_motion.AnimateValue(fill_key, visual, fill_spec, visual);
  if (!interactive && std::abs(animated - visual) > 0.35f) {
    reference_motion.Set(fill_key, visual);
    animated = visual;
  }
  const float track_y = origin.y + height * 0.5f;
  const float pearl_x = start + (end - start) * animated;
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Quiet empty rail, soft fill — pearl is the only bright element.
  const float track_h = 5.0f * scale;
  const float track_rounding = track_h * 0.5f;
  const float rail_left = origin.x;
  const float rail_right = track_end;
  theme::DrawSmoothRoundRectFilled(
      draw, ImVec2(rail_left, track_y - track_h * 0.5f), ImVec2(rail_right, track_y + track_h * 0.5f),
      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f * alpha)), track_rounding,
      ImDrawFlags_RoundCornersAll, 16);
  if (pearl_x > rail_left + 0.5f) {
    theme::DrawSmoothRoundRectFilled(
        draw, ImVec2(rail_left, track_y - track_h * 0.5f), ImVec2(pearl_x, track_y + track_h * 0.5f),
        ImGui::GetColorU32(ImVec4(0.38f, 0.38f, 0.41f, alpha)), track_rounding,
        ImDrawFlags_RoundCornersAll, 16);
  }

  // Pearl bead on the fill tip — lighter than the fill so it actually reads.
  const float pearl_state = reference_motion.AnimateValue(
      detail::MotionKey("menu.slider", id, "knob"), (slider_active || hovered) ? 1.0f : 0.0f,
      tokens.hover_fast, 0.0f);
  const float pearl_h = track_h + 5.0f * scale + 1.0f * scale * pearl_state;
  const float pearl_w = pearl_h * 1.45f;
  const ImVec2 pearl_min(pearl_x - pearl_w * 0.5f, track_y - pearl_h * 0.5f);
  const ImVec2 pearl_max(pearl_x + pearl_w * 0.5f, track_y + pearl_h * 0.5f);
  const float pearl_r = pearl_h * 0.5f;
  theme::DrawSmoothRoundRectFilled(
      draw, ImVec2(pearl_min.x, pearl_min.y + 0.7f * scale),
      ImVec2(pearl_max.x, pearl_max.y + 0.7f * scale),
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.30f * alpha)), pearl_r,
      ImDrawFlags_RoundCornersAll, 16);
  const ImVec4 pearl_body = detail::MixColor(ImVec4(0.78f, 0.78f, 0.81f, alpha),
                                             ImVec4(0.88f, 0.88f, 0.90f, alpha), pearl_state);
  theme::DrawSmoothRoundRectFilled(draw, pearl_min, pearl_max, ImGui::GetColorU32(pearl_body),
                                   pearl_r, ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectFilled(
      draw, ImVec2(pearl_min.x + 1.2f * scale, pearl_min.y + 1.0f * scale),
      ImVec2(pearl_max.x - 1.2f * scale, track_y),
      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f * alpha)), pearl_r * 0.75f,
      ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectOutline(
      draw, pearl_min, pearl_max, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.16f * alpha)),
      pearl_r, std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);

  const std::string display =
      std::format("{:.{}f}{}", *value * display_multiplier, display_precision, display_suffix);
  const ImVec2 display_size =
      label_font->CalcTextSizeA(value_font_sz, FLT_MAX, 0.0f, display.c_str());
  const bool value_hovered = ImGui::IsMouseHoveringRect(value_box_min, value_box_max, false);
  const float value_hover = reference_motion.AnimateValue(
      detail::MotionKey("menu.slider", id, "value-box"), (*mode > 0 || value_hovered) ? 1.0f : 0.0f,
      tokens.hover_fast, 0.0f);
  const float box_rounding = 3.0f * scale;
  if (value_hover > 0.01f || *mode > 0) {
    const float chip_a = std::max(value_hover, *mode > 0 ? 1.0f : 0.0f);
    theme::DrawSmoothRoundRectFilled(
        draw, value_box_min, value_box_max,
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f * chip_a * alpha)), box_rounding,
        ImDrawFlags_RoundCornersAll, 16);
  }

  if (*mode == 0) {
    const ImVec4 value_color = detail::MixColor(ui::theme::kTextDimColor, ui::theme::kTextColor,
                                                0.35f + 0.65f * value_hover);
    // Horizontally + vertically centered — full baked size, no scale-down clip.
    const float text_x =
        std::floor(value_box_min.x + (value_box_width - display_size.x) * 0.5f + 0.5f);
    const float text_y = CenteredTextTop(label_font, value_box_min.y, value_box_height);
    draw->AddText(label_font, value_font_sz, ImVec2(text_x, text_y),
                  ImGui::GetColorU32(ImVec4(value_color.x, value_color.y, value_color.z, alpha)),
                  display.c_str());
    ImGui::SetCursorScreenPos(value_box_min);
    ImGui::InvisibleButton("##value_button", ImVec2(value_box_width, value_box_height));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) *mode = 1;
  }

  ImGui::SetCursorScreenPos(ImVec2(row_origin.x, origin.y + height + 4.0f * scale));
  ImGui::PopID();
  return slider_active || *mode > 0 || std::abs(*value - value_at_frame_start) > 0.0001f;
}

bool CompactButton(const MotionContext& motion, const char* id, const char* label,
                   const ImVec2& size, ImFont* font, float scale, float alpha, bool active) {
  (void)motion;
  (void)scale;
  return ReferenceButton(motion, id, label, size, font, alpha, active);
}

bool SegmentSelector(const MotionContext& motion, const char* id,
                     std::span<const char* const> labels, int* selected, float width, ImFont* font,
                     float scale, float alpha) {
  if (!selected || labels.empty()) return false;
  if (!font) font = ImGui::GetFont();
  *selected = std::clamp(*selected, 0, static_cast<int>(labels.size()) - 1);

  const ImVec2 min = ImGui::GetCursorScreenPos();
  const float height = Metrics::kSegmentHeight * scale;
  const float segment_width = width / static_cast<float>(labels.size());
  const float rounding = detail::ControlRounding(scale);
  bool changed = false;

  ImDrawList* draw = ImGui::GetWindowDrawList();

  const ImVec2 max(min.x + width, min.y + height);
  ImVec4 track_bg = ui::theme::kComboBackgroundColor;
  track_bg.w *= alpha;
  ImVec4 track_border = ui::theme::kBorderColor;
  track_border.w *= alpha;

  // Same elevated track as combo fields.
  track_bg = ImVec4(0.10f, 0.10f, 0.11f, alpha);
  track_border = ImVec4(0.22f, 0.22f, 0.24f, alpha);
  theme::DrawSmoothRoundRectFilled(draw, min, max, ImGui::GetColorU32(track_bg), rounding,
                                   ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectOutline(draw, min, max, ImGui::GetColorU32(track_border), rounding,
                                    std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);

  const float inset = 3.0f * scale;
  const float target_x = static_cast<float>(*selected) * segment_width;
  const float current_x = motion.system.AnimateValue(
      ui::motion::MotionKey("segment-selector", id ? id : "close_behavior", "slide-x"), target_x,
      motion.tokens.spring_soft, target_x);

  const ImVec2 pill_min(min.x + current_x + inset, min.y + inset);
  const ImVec2 pill_max(min.x + current_x + segment_width - inset, min.y + height - inset);
  const float pill_rounding = std::max(2.0f * scale, rounding - 2.0f * scale);

  ImVec4 pill_bg(0.18f, 0.18f, 0.20f, alpha);
  theme::DrawSmoothRoundRectFilled(draw, pill_min, pill_max, ImGui::GetColorU32(pill_bg),
                                   pill_rounding, ImDrawFlags_RoundCornersAll, 16);
  theme::DrawSmoothRoundRectOutline(
      draw, pill_min, pill_max, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.08f * alpha)),
      pill_rounding, std::max(1.0f, scale), ImDrawFlags_RoundCornersAll, 16);

  for (int index = 0; index < static_cast<int>(labels.size()); ++index) {
    const ImVec2 segment_min(min.x + segment_width * static_cast<float>(index), min.y);
    const ImVec2 segment_max(segment_min.x + segment_width, min.y + height);

    ImGui::SetCursorScreenPos(segment_min);
    const std::string segment_id = std::format("{}##segment_{}", id ? id : "segment", index);
    if (ImGui::InvisibleButton(segment_id.c_str(), ImVec2(segment_width, height))) {
      if (*selected != index) {
        *selected = index;
        changed = true;
      }
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float hover_val =
        motion.system.AnimateValue(ui::motion::MotionKey("segment-selector", segment_id, "hover"),
                                   hovered ? 1.0f : 0.0f, motion.tokens.hover_fast, 0.0f);
    if (hover_val > 0.0f && *selected != index) {
      ImVec4 hover_glow(1.0f, 1.0f, 1.0f, 0.03f * hover_val * alpha);
      theme::DrawSmoothRoundRectFilled(
          draw, ImVec2(segment_min.x + inset, segment_min.y + inset),
          ImVec2(segment_max.x - inset, segment_max.y - inset), ImGui::GetColorU32(hover_glow),
          pill_rounding, ImDrawFlags_RoundCornersAll, 16);
    }

    const bool is_active = (*selected == index);
    const ImVec4 text_target =
        is_active || hovered ? ui::theme::kTextColor : ui::theme::kTextDimColor;
    ImVec4 text_color = motion.system.AnimateColor(
        ui::motion::MotionKey("segment-selector", segment_id, "text-color"), text_target,
        is_active ? motion.tokens.select_sharp : motion.tokens.hover_fast,
        ui::theme::kTextDimColor);
    text_color.w *= alpha;

    const ImVec2 text_size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, labels[index]);
    const float text_x = std::floor(segment_min.x + (segment_width - text_size.x) * 0.5f + 0.5f);
    const float text_y = CenteredTextTop(font, segment_min.y, height);
    draw->AddText(font, font->FontSize, ImVec2(text_x, text_y), ImGui::GetColorU32(text_color),
                  labels[index]);
  }

  ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + height));
  return changed;
}

}  // namespace minimize::ui::components
