#include "pch.hpp"

#include "ui/pages/animation_page.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <tuple>
#include <vector>

#include "ui/components/combo.hpp"
#include "ui/components/controls.hpp"
#include "ui/components/easing_editor.hpp"
#include "ui/settings_window.hpp"

namespace minimize::ui::pages {
namespace {

constexpr float kMinimumDuration = 0.10f;
constexpr float kMaximumDuration = 2.00f;
constexpr float kPageTitleTextSize = 22.0f;
constexpr float kPageSubtitleTextSize = 13.0f;
constexpr float kLabelTextSize = 15.0f;
constexpr float kHelperTextSize = 13.0f;
constexpr float kCaptionTextSize = 12.0f;
constexpr ImU32 kPrimaryTextColor = ::minimize::ui::theme::kText;
constexpr ImU32 kSecondaryTextColor = ::minimize::ui::theme::kMutedText;

template <typename Names>
int SelectedIndex(const Names& names, const std::string& value) {
  for (int index = 0; index < static_cast<int>(names.size()); ++index)
    if (value == names[index]) return index;
  return 0;
}

std::string TrimProfileName(std::string value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

int FindProfileIndex(const std::vector<settings::MotionProfile>& profiles, std::string_view name) {
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    if (profiles[index].name == name) return static_cast<int>(index);
  }
  return -1;
}

}  // namespace

void AnimationPage::Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                           const ::minimize::ui::motion::MotionContext& motion, float scale,
                           float alpha, float vertical_offset) {
  using namespace ::minimize::ui::theme;
  using ui::components::Combo;
  using ui::components::CompactButton;
  using ui::components::DelayedTooltip;
  using ui::components::EasingGraphEditor;
  using ui::components::SegmentSelector;
  using ui::components::Slider;
  using ui::components::Toggle;
  auto px = [scale](float value) { return value * scale; };
  auto& model = window.controller_->view_model();
  auto& actions = window.controller_->actions();
  const float button_height = Metrics::kButtonHeight * scale;
  const float combo_height = Metrics::kComboHeight * scale;
  const float slider_height = Metrics::kSliderHeight * scale;
  const float toggle_width = Metrics::kToggleWidth * scale;
  const float toggle_height = (Metrics::kToggleHeight + 4.0f) * scale;
  const float action_width = px(92.0f);
  const float action_gap = px(8.0f);
  const float actions_width = action_width * 2.0f + action_gap;

  layout.Title(window.font_title_, kPageTitleTextSize, "Motion", window.font_small_,
               kPageSubtitleTextSize, "Speed, curve and look of the minimize",
               actions_width + px(8.0f));
  const float title_top = px(16.0f) + vertical_offset;
  layout.SetCursor(layout.group_right() - actions_width, title_top + px(2.0f));
  if (CompactButton(motion, "##preview",
                    window.animation_preview_.active() ? "Running..." : "Preview",
                    ImVec2(action_width, button_height), window.font_body_, scale, alpha,
                    window.animation_preview_.active()) &&
      !window.animation_preview_.active()) {
    window.animation_preview_.Start(window.hwnd_);
  }
  layout.SetCursor(layout.group_right() - action_width, title_top + px(2.0f));
  if (CompactButton(motion, "##reset_motion", "Reset", ImVec2(action_width, button_height),
                    window.font_body_, scale, alpha)) {
    // Single atomic write for every Motion-tab field (chained setters clobber via UpdateState).
    const bool ok = actions.ResetMotionSettings();
    window.minimize_slider_dirty_ = false;
    window.restore_slider_dirty_ = false;
    window.cancel_slider_dirty_ = false;
    window.strength_slider_dirty_ = false;
    window.minimize_bezier_dirty_ = false;
    window.restore_bezier_dirty_ = false;
    window.cancel_bezier_dirty_ = false;
    // Snap slider fills to defaults after UpdateState reloads the model.
    motion.system.Set(ui::motion::MotionKey("menu.slider", "##min_duration", "fill"),
                      std::clamp((model.minimize_duration - kMinimumDuration) /
                                     (kMaximumDuration - kMinimumDuration),
                                 0.0f, 1.0f));
    motion.system.Set(ui::motion::MotionKey("menu.slider", "##restore_duration", "fill"),
                      std::clamp((model.restore_duration - kMinimumDuration) /
                                     (kMaximumDuration - kMinimumDuration),
                                 0.0f, 1.0f));
    motion.system.Set(ui::motion::MotionKey("menu.slider", "##cancel_duration", "fill"),
                      std::clamp((model.cancel_duration - kMinimumDuration) /
                                     (kMaximumDuration - kMinimumDuration),
                                 0.0f, 1.0f));
    motion.system.Set(ui::motion::MotionKey("menu.slider", "##minimize_strength", "fill"),
                      std::clamp((model.minimize_strength - 0.25f) / 0.75f, 0.0f, 1.0f));
    window.RecordSaveResult(ok);
  }

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "PROFILES");
  layout.BeginGroup();
  const float profile_field_width = layout.ControlMaxWidth(340.0f);
  if (!model.motion_profiles.empty()) {
    window.selected_motion_profile_ = std::clamp(
        window.selected_motion_profile_, 0, static_cast<int>(model.motion_profiles.size()) - 1);
    std::vector<const char*> profile_names;
    profile_names.reserve(model.motion_profiles.size());
    for (const settings::MotionProfile& profile : model.motion_profiles) {
      profile_names.push_back(profile.name.c_str());
    }
    layout.BeginRow(Metrics::kRowHeight);
    layout.ReserveControl(profile_field_width);
    layout.RowTitle(window.font_body_, kLabelTextSize, "Saved profile", kPrimaryTextColor);
    const ImVec2 profile_cursor = layout.ControlCursor(profile_field_width, combo_height);
    layout.SetCursor(profile_cursor.x, profile_cursor.y);
    if (Combo(motion, "##motion_profile", "", &window.selected_motion_profile_, profile_names,
              ImVec2(profile_field_width, combo_height), window.font_small_, window.font_body_,
              scale, alpha)) {
      const std::string& selected =
          model.motion_profiles[static_cast<std::size_t>(window.selected_motion_profile_)].name;
      std::snprintf(window.motion_profile_name_.data(), window.motion_profile_name_.size(), "%s",
                    selected.c_str());
    }
    layout.EndRow();
  }

  layout.BeginRow(Metrics::kRowHeightTall);
  layout.ReserveControl(profile_field_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Profile name", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize,
                     "Save the complete timing, curve, style, and quality setup",
                     kSecondaryTextColor);
  ImVec2 profile_cursor = layout.ControlCursor(profile_field_width, combo_height);
  layout.SetCursor(profile_cursor.x, profile_cursor.y);
  ImGui::SetNextItemWidth(profile_field_width);
  ImGui::InputText("##motion_profile_name", window.motion_profile_name_.data(),
                   window.motion_profile_name_.size());
  layout.EndRow();

  constexpr float kProfileButtonGap = 8.0f;
  const float profile_button_width = px(92.0f);
  const float profile_controls_width = profile_button_width * 3.0f + px(kProfileButtonGap * 2.0f);
  layout.BeginRow(Metrics::kRowHeightTall);
  layout.ReserveControl(profile_controls_width);
  layout.RowTitle(window.font_body_, kLabelTextSize,
                  model.motion_profiles.empty() ? "No saved profiles" : "Profile actions",
                  kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize,
                     "Saving an existing name updates that profile", kSecondaryTextColor);
  const ImVec2 profile_actions = layout.ControlCursor(profile_controls_width, button_height);
  layout.SetCursor(profile_actions.x, profile_actions.y);
  if (CompactButton(motion, "##save_motion_profile", "Save",
                    ImVec2(profile_button_width, button_height), window.font_body_, scale, alpha)) {
    const bool saved = actions.SaveMotionProfile(window.motion_profile_name_.data());
    if (saved) {
      const int index = FindProfileIndex(model.motion_profiles,
                                         TrimProfileName(window.motion_profile_name_.data()));
      if (index >= 0) window.selected_motion_profile_ = index;
    }
    window.RecordSaveResult(saved);
  }

  std::string selected_profile;
  if (!model.motion_profiles.empty()) {
    const int selected = std::clamp(window.selected_motion_profile_, 0,
                                    static_cast<int>(model.motion_profiles.size()) - 1);
    selected_profile = model.motion_profiles[static_cast<std::size_t>(selected)].name;
  }
  layout.SetCursor(profile_actions.x + profile_button_width + px(kProfileButtonGap),
                   profile_actions.y);
  if (CompactButton(motion, "##apply_motion_profile", "Apply",
                    ImVec2(profile_button_width, button_height), window.font_body_, scale, alpha,
                    !selected_profile.empty()) &&
      !selected_profile.empty()) {
    window.RecordSaveResult(actions.ApplyMotionProfile(selected_profile));
  }
  layout.SetCursor(profile_actions.x + (profile_button_width + px(kProfileButtonGap)) * 2.0f,
                   profile_actions.y);
  if (CompactButton(motion, "##delete_motion_profile", "Delete",
                    ImVec2(profile_button_width, button_height), window.font_body_, scale, alpha) &&
      !selected_profile.empty()) {
    const bool saved = actions.DeleteMotionProfile(selected_profile);
    if (saved) {
      window.selected_motion_profile_ = std::max(0, window.selected_motion_profile_ - 1);
    }
    window.RecordSaveResult(saved);
  }
  layout.EndRow();
  layout.EndGroup();

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "TIMING");
  layout.BeginGroup();
  layout.BeginStackRow(18.0f, Metrics::kButtonHeight);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Preset", kPrimaryTextColor);
  constexpr std::array presets = {
      std::tuple{"Snappy", 0.30f, 0.25f},
      std::tuple{"Balanced", 0.70f, 0.70f},
      std::tuple{"Smooth", 1.00f, 0.90f},
      std::tuple{"Film", 1.35f, 1.15f},
  };
  const float preset_gap = px(8.0f);
  const float preset_width =
      (layout.content_width() - preset_gap * static_cast<float>(presets.size() - 1)) /
      static_cast<float>(presets.size());
  float preset_x = layout.content_left();
  for (size_t index = 0; index < presets.size(); ++index) {
    const auto& [label, minimize, restore] = presets[index];
    layout.SetCursor(preset_x, layout.StackControlY());
    const std::string id = std::format("##preset_{}", index);
    const bool active = std::abs(model.minimize_duration - minimize) < 0.0001f &&
                        std::abs(model.restore_duration - restore) < 0.0001f;
    if (CompactButton(motion, id.c_str(), label, ImVec2(preset_width, button_height),
                      active ? window.font_medium_ : window.font_body_, scale, alpha, active)) {
      model.minimize_duration = minimize;
      model.restore_duration = restore;
      window.minimize_slider_dirty_ = false;
      window.restore_slider_dirty_ = false;
      window.RecordSaveResult(actions.SetAnimationDurations(
          model.minimize_duration, model.restore_duration, model.cancel_duration, true));
    }
    preset_x += preset_width + preset_gap;
  }
  layout.EndRow();

  const auto duration_slider = [&](const char* id, const char* title, float* duration,
                                   bool* was_active, bool* dirty) {
    layout.BeginRow(Metrics::kRowHeight);
    const float width = layout.ControlMaxWidth(340.0f);
    layout.ReserveControl(width);
    layout.RowTitle(window.font_body_, kLabelTextSize, title, kPrimaryTextColor);
    const ImVec2 cursor = layout.ControlCursor(width, slider_height);
    layout.SetCursor(cursor.x, cursor.y);
    float proposed = *duration;
    const bool active = Slider(motion, id, "", &proposed, kMinimumDuration, kMaximumDuration, width,
                               scale, alpha, window.font_small_, 0.01f);
    if (active && std::abs(proposed - *duration) > 0.0001f) {
      float delta = proposed - *duration;
      if (model.link_speeds) {
        const float minimum_delta = (std::max)({kMinimumDuration - model.minimize_duration,
                                                kMinimumDuration - model.restore_duration,
                                                kMinimumDuration - model.cancel_duration});
        const float maximum_delta = (std::min)({kMaximumDuration - model.minimize_duration,
                                                kMaximumDuration - model.restore_duration,
                                                kMaximumDuration - model.cancel_duration});
        delta = std::clamp(delta, minimum_delta, maximum_delta);
        model.minimize_duration += delta;
        model.restore_duration += delta;
        model.cancel_duration += delta;
      } else {
        *duration += delta;
      }
      *dirty = true;
      actions.SetAnimationDurations(model.minimize_duration, model.restore_duration,
                                    model.cancel_duration, false);
    }
    if (*was_active && !active && *dirty) {
      const bool saved = actions.SetAnimationDurations(
          model.minimize_duration, model.restore_duration, model.cancel_duration, true);
      window.RecordSaveResult(saved);
      if (saved) *dirty = false;
    }
    *was_active = active;
    layout.EndRow();
  };
  duration_slider("##min_duration", "Minimize", &model.minimize_duration,
                  &window.minimize_slider_active_, &window.minimize_slider_dirty_);
  duration_slider("##restore_duration", "Restore", &model.restore_duration,
                  &window.restore_slider_active_, &window.restore_slider_dirty_);

  layout.BeginRow(Metrics::kRowHeightTall);
  const float cancel_width = layout.ControlMaxWidth(340.0f);
  layout.ReserveControl(cancel_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Cancel / reverse", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize,
                     "Used when direction changes mid-animation", kSecondaryTextColor);
  ImVec2 cursor = layout.ControlCursor(cancel_width, slider_height);
  layout.SetCursor(cursor.x, cursor.y);
  float cancel_duration = model.cancel_duration;
  const bool cancel_active =
      Slider(motion, "##cancel_duration", "", &cancel_duration, kMinimumDuration, kMaximumDuration,
             cancel_width, scale, alpha, window.font_small_, 0.01f);
  if (cancel_active && std::abs(cancel_duration - model.cancel_duration) > 0.0001f) {
    float delta = cancel_duration - model.cancel_duration;
    if (model.link_speeds) {
      const float minimum_delta = (std::max)({kMinimumDuration - model.minimize_duration,
                                              kMinimumDuration - model.restore_duration,
                                              kMinimumDuration - model.cancel_duration});
      const float maximum_delta = (std::min)({kMaximumDuration - model.minimize_duration,
                                              kMaximumDuration - model.restore_duration,
                                              kMaximumDuration - model.cancel_duration});
      delta = std::clamp(delta, minimum_delta, maximum_delta);
      model.minimize_duration += delta;
      model.restore_duration += delta;
      model.cancel_duration += delta;
    } else {
      model.cancel_duration += delta;
    }
    window.cancel_slider_dirty_ = true;
    actions.SetAnimationDurations(model.minimize_duration, model.restore_duration,
                                  model.cancel_duration, false);
  }
  if (window.cancel_slider_active_ && !cancel_active && window.cancel_slider_dirty_) {
    const bool saved = actions.SetAnimationDurations(
        model.minimize_duration, model.restore_duration, model.cancel_duration, true);
    window.RecordSaveResult(saved);
    if (saved) window.cancel_slider_dirty_ = false;
  }
  window.cancel_slider_active_ = cancel_active;
  layout.EndRow();

  layout.BeginRow(Metrics::kRowHeightTall);
  layout.ReserveControl(toggle_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Link durations", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize, "Move all three sliders together",
                     kSecondaryTextColor);
  bool link_speeds = model.link_speeds;
  cursor = layout.ControlCursor(toggle_width, toggle_height);
  layout.SetCursor(cursor.x, cursor.y);
  if (Toggle(motion, "##link_speeds", &link_speeds, scale, alpha)) {
    const bool previous = model.link_speeds;
    model.link_speeds = link_speeds;
    const bool saved = actions.SetLinkSpeeds(model.link_speeds);
    if (!saved) model.link_speeds = previous;
    window.RecordSaveResult(saved);
  }
  layout.EndRow();
  layout.EndGroup();

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "STYLE");
  layout.BeginGroup();
  constexpr std::array easing_names = {
      "Linear", "Ease In", "Ease Out", "Ease In Out", "Cubic", "Back", "Elastic", "Custom",
  };
  constexpr std::array style_names = {
      "Genie classic",
      "Genie curvy",
      "Squash",
  };
  const float combo_width = layout.ControlMaxWidth(340.0f);
  const float graph_side = combo_width * 0.8f;
  const float graph_height = graph_side + px(28.0f);
  const auto combo_row = [&](const char* id, const char* title, int* index,
                             std::span<const char* const> values, const auto& changed) {
    layout.BeginRow(Metrics::kRowHeight);
    layout.ReserveControl(combo_width);
    layout.RowTitle(window.font_body_, kLabelTextSize, title, kPrimaryTextColor);
    const ImVec2 combo_cursor = layout.ControlCursor(combo_width, combo_height);
    layout.SetCursor(combo_cursor.x, combo_cursor.y);
    if (Combo(motion, id, "", index, values, ImVec2(combo_width, combo_height), window.font_small_,
              window.font_body_, scale, alpha))
      changed();
    layout.EndRow();
  };
  int style_index = SelectedIndex(style_names, model.animation_style);
  combo_row("##animation_style", "Animation", &style_index, style_names, [&] {
    const std::string previous = model.animation_style;
    model.animation_style = style_names[style_index];
    const bool saved = actions.SetAnimationStyle(model.animation_style);
    if (!saved) model.animation_style = previous;
    window.RecordSaveResult(saved);
  });

  const auto easing_block = [&](const char* combo_id, const char* graph_id, const char* title,
                                std::string* easing, animation::CubicBezier* bezier, bool* dirty,
                                bool minimize) {
    int index = SelectedIndex(easing_names, *easing);
    combo_row(combo_id, title, &index, easing_names, [&] {
      const std::string previous = *easing;
      *easing = easing_names[index];
      const bool saved = actions.SetEasing(model.minimize_easing, model.restore_easing);
      if (!saved) *easing = previous;
      window.RecordSaveResult(saved);
    });
    if (*easing != "Custom") return;
    layout.BeginStackRow(0.0f, graph_height / scale + 8.0f);
    layout.SetCursor(layout.content_right() - graph_side,
                     layout.StackControlY() + (layout.StackControlHeight() - graph_height) * 0.5f);
    bool changed = false;
    const bool active =
        EasingGraphEditor(motion, graph_id, bezier, ImVec2(graph_side, graph_height), scale, alpha,
                          &changed, window.font_small_);
    DelayedTooltip(
        "Drag handles or type x1, y1, x2, y2. Hold Shift while dragging for finer steps.", scale);
    if (changed) {
      *dirty = true;
      actions.SetCustomEasingBezier(minimize, *bezier, false);
      window.ForceRender();
    }
    bool& was_active = minimize ? window.minimize_bezier_active_ : window.restore_bezier_active_;
    if (was_active && !active && *dirty) {
      const bool saved = actions.SetCustomEasingBezier(minimize, *bezier, true);
      window.RecordSaveResult(saved);
      if (saved) *dirty = false;
    }
    was_active = active;
    layout.EndRow();
  };
  easing_block("##minimize_easing", "##minimize_bezier_graph", "Minimize easing",
               &model.minimize_easing, &model.minimize_custom_bezier,
               &window.minimize_bezier_dirty_, true);
  easing_block("##restore_easing", "##restore_bezier_graph", "Restore easing",
               &model.restore_easing, &model.restore_custom_bezier, &window.restore_bezier_dirty_,
               false);

  int cancel_easing_index = SelectedIndex(easing_names, model.cancel_easing);
  combo_row("##cancel_easing", "Cancel / reverse easing", &cancel_easing_index, easing_names, [&] {
    const std::string previous = model.cancel_easing;
    model.cancel_easing = easing_names[cancel_easing_index];
    const bool saved = actions.SetCancelEasing(model.cancel_easing);
    if (!saved) model.cancel_easing = previous;
    window.RecordSaveResult(saved);
  });
  if (model.cancel_easing == "Custom") {
    layout.BeginStackRow(0.0f, graph_height / scale + 8.0f);
    layout.SetCursor(layout.content_right() - graph_side,
                     layout.StackControlY() + (layout.StackControlHeight() - graph_height) * 0.5f);
    bool changed = false;
    const bool active = EasingGraphEditor(
        motion, "##cancel_bezier_graph", &model.cancel_custom_bezier,
        ImVec2(graph_side, graph_height), scale, alpha, &changed, window.font_small_);
    DelayedTooltip(
        "Cancel curve used after reversing direction. Drag handles or type x1, y1, x2, y2.", scale);
    if (changed) {
      window.cancel_bezier_dirty_ = true;
      actions.SetCancelCustomBezier(model.cancel_custom_bezier, false);
      window.ForceRender();
    }
    if (window.cancel_bezier_active_ && !active && window.cancel_bezier_dirty_) {
      const bool saved = actions.SetCancelCustomBezier(model.cancel_custom_bezier, true);
      window.RecordSaveResult(saved);
      if (saved) window.cancel_bezier_dirty_ = false;
    }
    window.cancel_bezier_active_ = active;
    layout.EndRow();
  }
  layout.EndGroup();

  layout.SectionCaption(window.font_small_, kCaptionTextSize, "LOOK");
  layout.BeginGroup();
  layout.BeginStackRow(20.0f, Metrics::kSegmentHeight);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Quality", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize, "Mesh resolution and power usage",
                     kSecondaryTextColor);
  constexpr std::array quality_labels = {
      "Automatic",
      "Best quality",
      "Power saving",
  };
  int quality = model.quality_mode == "best_quality"   ? 1
                : model.quality_mode == "power_saving" ? 2
                                                       : 0;
  layout.SetCursor(layout.content_left(), layout.StackControlY());
  if (SegmentSelector(motion, "##quality_mode", quality_labels, &quality, layout.content_width(),
                      window.font_body_, scale, alpha)) {
    const std::string previous = model.quality_mode;
    model.quality_mode = quality == 1   ? "best_quality"
                         : quality == 2 ? "power_saving"
                                        : "automatic";
    const bool saved = actions.SetQualityMode(model.quality_mode);
    if (!saved) model.quality_mode = previous;
    window.RecordSaveResult(saved);
  }
  layout.EndRow();

  layout.BeginRow(Metrics::kRowHeight);
  const float strength_width = layout.ControlMaxWidth(340.0f);
  layout.ReserveControl(strength_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Strength", kPrimaryTextColor);
  cursor = layout.ControlCursor(strength_width, slider_height);
  layout.SetCursor(cursor.x, cursor.y);
  float strength = model.minimize_strength;
  const bool strength_active =
      Slider(motion, "##minimize_strength", "", &strength, 0.25f, 1.0f, strength_width, scale,
             alpha, window.font_small_, 0.01f, 100.0f, 0, "%");
  DelayedTooltip("How strongly the window bends toward the taskbar target.", scale);
  if (strength_active && std::abs(strength - model.minimize_strength) > 0.0001f) {
    model.minimize_strength = strength;
    window.strength_slider_dirty_ = true;
    actions.SetMinimizeStrength(model.minimize_strength, false);
  }
  if (window.strength_slider_active_ && !strength_active && window.strength_slider_dirty_) {
    const bool saved = actions.SetMinimizeStrength(model.minimize_strength, true);
    window.RecordSaveResult(saved);
    if (saved) window.strength_slider_dirty_ = false;
  }
  window.strength_slider_active_ = strength_active;
  layout.EndRow();

  layout.BeginRow(Metrics::kRowHeight);
  constexpr std::array fade_names = {"No fade", "Subtle", "Strong"};
  int fade_index = SelectedIndex(fade_names, model.fade_strength);
  layout.ReserveControl(combo_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Fade", kPrimaryTextColor);
  cursor = layout.ControlCursor(combo_width, combo_height);
  layout.SetCursor(cursor.x, cursor.y);
  if (Combo(motion, "##fade_strength", "", &fade_index, fade_names,
            ImVec2(combo_width, combo_height), window.font_small_, window.font_body_, scale,
            alpha)) {
    const std::string previous = model.fade_strength;
    model.fade_strength = fade_names[fade_index];
    const bool saved = actions.SetFadeStrength(model.fade_strength);
    if (!saved) model.fade_strength = previous;
    window.RecordSaveResult(saved);
  }
  layout.EndRow();

  layout.BeginRow(Metrics::kRowHeightTall);
  layout.ReserveControl(toggle_width);
  layout.RowTitle(window.font_body_, kLabelTextSize, "Target indicator", kPrimaryTextColor);
  layout.RowSubtitle(window.font_small_, kHelperTextSize, "Flash the taskbar slot during minimize",
                     kSecondaryTextColor);
  bool indicator = model.show_target_indicator;
  cursor = layout.ControlCursor(toggle_width, toggle_height);
  layout.SetCursor(cursor.x, cursor.y);
  if (Toggle(motion, "##target_indicator", &indicator, scale, alpha)) {
    const bool saved = actions.SetTargetIndicator(indicator);
    if (saved) model.show_target_indicator = indicator;
    window.RecordSaveResult(saved);
  }
  layout.EndRow();
  layout.EndGroup();
}

}  // namespace minimize::ui::pages
