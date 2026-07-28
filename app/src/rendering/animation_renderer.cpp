#include "pch.hpp"

#include "rendering/animation_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace minimize::rendering {

void AnimationRenderer::SetEasing(animation::EasingCurve easing, animation::CubicBezier custom) {
  configured_easing_ = easing;
  configured_custom_bezier_ = custom;
  configured_custom_bezier_.ClampHandles();
}

void AnimationRenderer::SetReversalAnimation(float seconds, animation::EasingCurve easing,
                                             animation::CubicBezier custom) {
  configured_reversal_duration_seconds_ = seconds;
  configured_reversal_easing_ = easing;
  configured_reversal_custom_bezier_ = custom;
  configured_reversal_custom_bezier_.ClampHandles();
}

bool AnimationRenderer::Begin(CapturedTexture texture, const animation::RectF& source,
                              const animation::RectF& target, animation::MinimizeEdge edge,
                              float start_progress, float target_progress) {
  if (texture.shader_resource_view == nullptr) return false;
  active_ = true;
  clock_started_ = false;
  texture_ = std::move(texture);
  source_ = source;
  target_ = target;
  edge_ = edge;
  progress_ = std::clamp(start_progress, 0.0f, 1.0f);
  target_progress_ = std::clamp(target_progress, 0.0f, 1.0f);
  duration_seconds_ = std::max(0.001f, configured_duration_seconds_);
  easing_ = configured_easing_;
  custom_bezier_ = configured_custom_bezier_;
  style_ = configured_style_;
  minimize_strength_ = configured_minimize_strength_;
  fade_strength_ = configured_fade_strength_;
  reversal_segment_active_ = false;
  reversal_start_progress_ = progress_;
  reversal_start_rendered_progress_ = animation::ApplyEasing(easing_, progress_, custom_bezier_);
  return true;
}

void AnimationRenderer::StartClock() {
  if (!active_) return;
  last_tick_time_ = std::chrono::steady_clock::now();
  clock_started_ = true;
}

void AnimationRenderer::ContinueMinimize() {
  BeginReversal(1.0f, true);
}

void AnimationRenderer::Reverse(bool start_clock) {
  BeginReversal(0.0f, start_clock);
}

void AnimationRenderer::BeginReversal(float target_progress, bool start_clock) {
  if (!active_) return;
  reversal_start_rendered_progress_ = eased_progress();
  reversal_start_progress_ = progress_;
  target_progress_ = std::clamp(target_progress, 0.0f, 1.0f);
  duration_seconds_ = std::max(0.001f, configured_reversal_duration_seconds_);
  easing_ = configured_reversal_easing_;
  custom_bezier_ = configured_reversal_custom_bezier_;
  reversal_segment_active_ = true;
  clock_started_ = false;
  if (start_clock) StartClock();
}

AnimationRenderer::AdvanceResult AnimationRenderer::Advance() {
  if (!active_) return {};
  if (!clock_started_) return {.progress = progress_};
  const auto now = std::chrono::steady_clock::now();
  const float elapsed = std::chrono::duration<float>(now - last_tick_time_).count();
  last_tick_time_ = now;
  const float step = elapsed / duration_seconds_;
  if (target_progress_ >= progress_) {
    progress_ = std::min(target_progress_, progress_ + step);
  } else {
    progress_ = std::max(target_progress_, progress_ - step);
  }
  return {
      .should_render = true,
      .reached_target = progress_ == target_progress_,
      .progress = progress_,
  };
}

void AnimationRenderer::CompleteFrame(bool render_succeeded, bool reached_target) {
  if (render_succeeded && !reached_target) return;
  active_ = false;
  if (!render_succeeded || target_progress_ != 0.0f) texture_ = {};
}

void AnimationRenderer::Cancel() {
  active_ = false;
  clock_started_ = false;
  texture_ = {};
}

void AnimationRenderer::FinishRestore() {
  active_ = false;
  clock_started_ = false;
  texture_ = {};
}

float AnimationRenderer::eased_progress() const {
  if (reversal_segment_active_) {
    const float distance = std::abs(target_progress_ - reversal_start_progress_);
    const float travelled = std::abs(progress_ - reversal_start_progress_);
    const float segment_progress =
        distance > 0.000001f ? std::clamp(travelled / distance, 0.0f, 1.0f) : 1.0f;
    const float eased_segment =
        animation::ApplyEasing(easing_, segment_progress, custom_bezier_);
    return std::lerp(reversal_start_rendered_progress_, target_progress_, eased_segment);
  }
  return animation::ApplyEasing(easing_, progress_, custom_bezier_);
}

float AnimationRenderer::opacity(float rendered_progress) const {
  if (!active_) return 1.0f;
  if (style_ == animation::AnimationStyle::kSquash) {
    return 1.0f - std::clamp(rendered_progress, 0.0f, 1.0f);
  }
  return 1.0f - std::clamp(fade_strength_, 0.0f, 0.65f) * std::clamp(progress_, 0.0f, 1.0f);
}

animation::GenieConstants AnimationRenderer::GenieParameters(UINT viewport_width,
                                                              UINT viewport_height) const {
  const float width = static_cast<float>(std::max(viewport_width, 1U));
  const float height = static_cast<float>(std::max(viewport_height, 1U));
  return animation::GenieConstants{
      .source = {.left = source_.left / width,
                 .top = source_.top / height,
                 .right = source_.right / width,
                 .bottom = source_.bottom / height},
      .target = {.left = target_.left / width,
                 .top = target_.top / height,
                 .right = target_.right / width,
                 .bottom = target_.bottom / height},
      .progress = eased_progress(),
      .strength = minimize_strength_,
      .edge = static_cast<std::uint32_t>(edge_),
      .style = static_cast<std::uint32_t>(style_),
  };
}

}  // namespace minimize::rendering
