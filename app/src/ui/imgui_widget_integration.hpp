#pragma once

#include "ui/motion/motion.hpp"
#include "ui/motion/motion_tokens.hpp"
#include "ui/theme/theme_tokens.hpp"

namespace ui {
namespace motion = ::minimize::ui::motion;
}

namespace colors {

inline const ImVec4& main = minimize::ui::theme::kMainColor;
inline const ImVec4& panel = minimize::ui::theme::kPanelColor;
inline const ImVec4& border = minimize::ui::theme::kBorderColor;
inline const ImVec4& panelHeader = minimize::ui::theme::kPanelHeaderColor;
inline const ImVec4& accent = minimize::ui::theme::kAccentColor;
inline const ImVec4& text = minimize::ui::theme::kTextColor;
inline const ImVec4& textDim = minimize::ui::theme::kTextDimColor;
inline const ImVec4& sidebar = minimize::ui::theme::kSidebarColor;
inline const ImVec4& subNamespaceBg = minimize::ui::theme::kSubNamespaceBackgroundColor;
inline const ImVec4& comboBg = minimize::ui::theme::kComboBackgroundColor;

}  // namespace colors

namespace WindowMotion {

class MotionSystemAdapter final {
public:
  void begin_frame(float delta_time) { system_.BeginFrame(delta_time); }
  [[nodiscard]] bool has_active_tracks() const { return system_.HasActiveTracks(); }

  float value(const minimize::ui::motion::MotionKey& key, float target,
              const minimize::ui::motion::MotionSpec& spec, float initial) {
    return system_.AnimateValue(key, target, spec, initial);
  }
  float value(const minimize::ui::motion::MotionKey& key, float target,
              const minimize::ui::motion::MotionSpec& spec) {
    return system_.AnimateValue(key, target, spec);
  }
  ImVec4 color(const minimize::ui::motion::MotionKey& key, const ImVec4& target,
               const minimize::ui::motion::MotionSpec& spec, const ImVec4& initial) {
    return system_.AnimateColor(key, target, spec, initial);
  }
  void set(const minimize::ui::motion::MotionKey& key, float value) { system_.Set(key, value); }

private:
  minimize::ui::motion::MotionSystem system_;
};

struct MotionTokenAdapter final {
  minimize::ui::motion::MotionSpec hoverFast;
  minimize::ui::motion::MotionSpec pressFast;
  minimize::ui::motion::MotionSpec fadeSlow;
  minimize::ui::motion::MotionSpec slideSoft;
  minimize::ui::motion::MotionSpec popupOpen;
  minimize::ui::motion::MotionSpec springSnappy;
};

inline MotionSystemAdapter& System() {
  static MotionSystemAdapter system;
  return system;
}

inline void BeginFrame(float delta_time) { System().begin_frame(delta_time); }

inline bool HasActiveTracks() { return System().has_active_tracks(); }

inline const MotionTokenAdapter& Tokens() {
  static const MotionTokenAdapter tokens = [] {
    const auto source = minimize::ui::motion::MotionTokens::Default();
    return MotionTokenAdapter{
        .hoverFast = source.hover_fast,
        .pressFast = source.press_fast,
        .fadeSlow = source.fade_slow,
        .slideSoft = source.slide_soft,
        .popupOpen = source.popup_open,
        .springSnappy = source.spring_snappy,
    };
  }();
  return tokens;
}

}  // namespace WindowMotion
