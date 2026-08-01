#include "pch.hpp"

#include "ui/pages/hotkeys_page.hpp"

#include <array>
#include <format>

#include "ui/components/controls.hpp"
#include "ui/hotkey_presenter.hpp"
#include "ui/settings_window.hpp"

namespace minimize::ui::pages {
namespace {

constexpr float kPageTitleTextSize = 22.0f;
constexpr float kPageSubtitleTextSize = 13.0f;
constexpr float kLabelTextSize = 15.0f;
constexpr float kValueTextSize = 13.0f;
constexpr ImU32 kPrimaryTextColor = ::minimize::ui::theme::kText;
constexpr ImU32 kSecondaryTextColor = ::minimize::ui::theme::kMutedText;

}  // namespace

void HotkeysPage::Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                         const ::minimize::ui::motion::MotionContext& motion, float scale,
                         float alpha) {
  layout.Title(window.font_title_, kPageTitleTextSize, "Hotkeys", window.font_small_,
               kPageSubtitleTextSize, "Click Change, then press a combination");
  constexpr std::array labels = {
      "Toggle effect",
      "Open settings",
      "Repair windows",
      "Minimize windows on mouse display",
      "Restore windows on mouse display",
  };
  const float button_height = ::minimize::ui::theme::Metrics::kButtonHeight * scale;
  layout.BeginGroup();
  for (size_t index = 0; index < labels.size(); ++index) {
    layout.BeginRow(::minimize::ui::theme::Metrics::kRowHeightTall);
    const float change_width = 86.0f * scale;
    const float disable_width = 78.0f * scale;
    const float gap = 8.0f * scale;
    const float total = change_width + gap + disable_width;
    layout.ReserveControl(total);
    layout.RowTitle(window.font_body_, kLabelTextSize, labels[index], kPrimaryTextColor);
    const auto& binding = window.controller_->view_model().hotkeys[index];
    const std::string binding_text =
        window.editing_hotkey_ == static_cast<int>(index) ? "Press keys..." : FormatHotkey(binding);
    const bool available =
        window.controller_->view_model().hotkey_available[index] || binding.virtual_key == 0;
    layout.RowSubtitle(window.font_small_, kValueTextSize, binding_text.c_str(),
                       available ? kSecondaryTextColor : IM_COL32(235, 120, 120, 255));

    const ImVec2 base = layout.ControlCursor(total, button_height);
    layout.SetCursor(base.x, base.y);
    const std::string change_id = std::format("##change_hotkey_{}", index);
    if (ui::components::CompactButton(motion, change_id.c_str(), "Change",
                                      ImVec2(change_width, button_height), window.font_body_, scale,
                                      alpha)) {
      window.editing_hotkey_ = static_cast<int>(index);
      window.hotkey_feedback_ = "Press a combination, or Esc to cancel";
    }
    layout.SetCursor(base.x + change_width + gap, base.y);
    const std::string disable_id = std::format("##disable_hotkey_{}", index);
    if (ui::components::CompactButton(motion, disable_id.c_str(), "Clear",
                                      ImVec2(disable_width, button_height), window.font_body_,
                                      scale, alpha)) {
      const HotkeyUpdateResult result = window.controller_->actions().SetHotkey(
          static_cast<settings::HotkeyAction>(index), settings::HotkeyBinding{});
      window.hotkey_feedback_ = HotkeyUpdateMessage(result);
      window.editing_hotkey_ = -1;
    }
    layout.EndRow();
  }
  layout.EndGroup();

  if (window.hotkey_feedback_.empty()) return;
  const ImVec2 position = layout.ToScreen(layout.content_left(), layout.y());
  ImGui::GetWindowDrawList()->AddText(
      window.font_small_, window.font_small_->FontSize,
      ImVec2(std::floor(position.x + 0.5f), std::floor(position.y + 0.5f)),
      ::minimize::ui::theme::WithAlpha(kSecondaryTextColor, alpha),
      window.hotkey_feedback_.c_str());
  layout.Gap(20.0f);
}

}  // namespace minimize::ui::pages
