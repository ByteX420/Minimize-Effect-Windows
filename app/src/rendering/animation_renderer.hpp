#pragma once

#include <chrono>

#include "animation/easing.hpp"
#include "animation/minimize_mesh.hpp"
#include "rendering/desktop_capture.hpp"

namespace minimize::rendering {

class AnimationRenderer final {
public:
  struct AdvanceResult {
    bool should_render = false;
    bool reached_target = false;
    float progress = 0.0f;
  };

  void SetDuration(float seconds) { configured_duration_seconds_ = seconds; }
  void SetEasing(animation::EasingCurve easing, animation::CubicBezier custom);
  void SetReversalAnimation(float seconds, animation::EasingCurve easing,
                            animation::CubicBezier custom);
  void SetStyle(animation::AnimationStyle style) { configured_style_ = style; }
  void SetMeshSegmentCount(int count) { (void)count; }
  void SetMinimizeStrength(float strength) { configured_minimize_strength_ = strength; }
  void SetFadeStrength(float strength) { configured_fade_strength_ = strength; }

  [[nodiscard]] bool Begin(CapturedTexture texture, const animation::RectF& source,
                           const animation::RectF& target, animation::MinimizeEdge edge,
                           float start_progress, float target_progress);
  void StartClock();
  void ContinueMinimize();
  void Reverse(bool start_clock = true);
  [[nodiscard]] AdvanceResult Advance();
  void CompleteFrame(bool render_succeeded, bool reached_target);
  void Cancel();
  void FinishRestore();

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] bool clock_started() const { return clock_started_; }
  [[nodiscard]] bool restoring() const { return active_ && target_progress_ < progress_; }
  [[nodiscard]] float progress() const { return progress_; }
  [[nodiscard]] float target_progress() const { return target_progress_; }
  [[nodiscard]] float eased_progress() const;
  [[nodiscard]] float opacity(float rendered_progress) const;
  [[nodiscard]] animation::GenieConstants GenieParameters(UINT viewport_width,
                                                          UINT viewport_height) const;
  [[nodiscard]] CapturedTexture* mutable_texture() { return &texture_; }
  [[nodiscard]] ID3D11ShaderResourceView* texture_view() const {
    return texture_.shader_resource_view.Get();
  }
  [[nodiscard]] ID3D11ShaderResourceView* mask_view() const {
    return texture_.mask_shader_resource_view.Get();
  }
  [[nodiscard]] const WindowVisualMetadata& visual_metadata() const {
    return texture_.visual_metadata;
  }
  [[nodiscard]] ID3D11ShaderResourceView* const* texture_view_address() const {
    return texture_.shader_resource_view.GetAddressOf();
  }

private:
  void BeginReversal(float target_progress, bool start_clock);

  bool active_ = false;
  bool clock_started_ = false;
  CapturedTexture texture_;
  animation::RectF source_;
  animation::RectF target_;
  animation::MinimizeEdge edge_ = animation::MinimizeEdge::kBottom;
  std::chrono::steady_clock::time_point last_tick_time_{};
  float progress_ = 0.0f;
  float target_progress_ = 1.0f;
  float duration_seconds_ = 0.70f;
  animation::EasingCurve easing_ = animation::EasingCurve::kLinear;
  animation::CubicBezier custom_bezier_ = animation::CubicBezier::EaseInOut();
  animation::AnimationStyle style_ = animation::AnimationStyle::kClassic;
  float minimize_strength_ = 1.0f;
  float fade_strength_ = 0.0f;
  bool reversal_segment_active_ = false;
  float reversal_start_progress_ = 0.0f;
  float reversal_start_rendered_progress_ = 0.0f;

  float configured_duration_seconds_ = 0.70f;
  animation::EasingCurve configured_easing_ = animation::EasingCurve::kLinear;
  animation::CubicBezier configured_custom_bezier_ = animation::CubicBezier::EaseInOut();
  float configured_reversal_duration_seconds_ = 0.35f;
  animation::EasingCurve configured_reversal_easing_ = animation::EasingCurve::kLinear;
  animation::CubicBezier configured_reversal_custom_bezier_ =
      animation::CubicBezier::EaseInOut();
  animation::AnimationStyle configured_style_ = animation::AnimationStyle::kClassic;
  float configured_minimize_strength_ = 1.0f;
  float configured_fade_strength_ = 0.0f;
};

}  // namespace minimize::rendering
