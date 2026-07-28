#include "pch.hpp"

#include "features/animation_configuration.hpp"

#include <algorithm>
#include <string_view>

#include "animation/easing.hpp"
#include "rendering/overlay_window.hpp"
#include "settings/settings_service.hpp"

namespace minimize::features {
namespace {

animation::AnimationStyle AnimationStyleFromName(std::string_view style) {
  if (style == "Genie curvy") return animation::AnimationStyle::kCurvy;
  if (style == "Squash") return animation::AnimationStyle::kSquash;
  return animation::AnimationStyle::kClassic;
}

float AnimationStyleDurationScale(std::string_view style) {
  if (style == "Genie curvy") return 0.78f;
  if (style == "Squash") return 0.55f;
  return 1.0f;
}

}  // namespace

AnimationConfiguration::AnimationConfiguration(const settings::SettingsService& settings,
                                               const EffectPolicy& policy)
    : settings_(settings), policy_(policy) {}

float AnimationConfiguration::Apply(rendering::OverlayWindow& overlay, const RECT& source,
                                    bool restoring, const RenderingPressure& pressure) const {
  const settings::AppSettings& settings = settings_.Get();
  const float duration = (restoring ? settings.restore_duration : settings.minimize_duration) *
                         AnimationStyleDurationScale(settings.animation_style);
  overlay.SetAnimationDuration(duration);
  overlay.SetAnimationEasing(
      animation::EasingCurveFromName(restoring ? settings.restore_easing
                                               : settings.minimize_easing),
      restoring ? settings.restore_custom_bezier : settings.minimize_custom_bezier);
  overlay.SetReversalAnimation(settings.cancel_duration,
                               animation::EasingCurveFromName(settings.cancel_easing),
                               settings.cancel_custom_bezier);
  overlay.SetAnimationStyle(AnimationStyleFromName(settings.animation_style));
  overlay.SetMeshSegmentCount(policy_.SelectMeshSegmentCount(source.right - source.left,
                                                             source.bottom - source.top, pressure));
  overlay.SetMinimizeStrength(std::clamp(settings.minimize_strength, 0.0f, 1.0f));
  overlay.SetFadeStrength(settings.fade_strength == "Strong"   ? 0.55f
                          : settings.fade_strength == "Subtle" ? 0.25f
                                                               : 0.0f);
  overlay.SetTargetIndicatorEnabled(settings.show_target_indicator);
  return duration;
}

}  // namespace minimize::features
