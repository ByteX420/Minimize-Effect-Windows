#pragma once

#include <chrono>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <functional>
#include <windows.h>
#include <wrl/client.h>
#include <wil/com.h>

#include "animation/easing.hpp"
#include "animation/minimize_mesh.hpp"
#include "rendering/animation_renderer.hpp"
#include "rendering/d3d_device.hpp"
#include "rendering/desktop_capture.hpp"
#include "rendering/overlay_renderer.hpp"

namespace minimize::rendering {

class OverlayWindow {
public:
  using MinimizeCallback = std::function<bool(HWND)>;
  using RestoreCallback = std::function<bool(HWND)>;

  OverlayWindow() = default;
  ~OverlayWindow();

  OverlayWindow(const OverlayWindow&) = delete;
  OverlayWindow& operator=(const OverlayWindow&) = delete;

  bool Initialize(HINSTANCE instance, D3dDevice* d3d_device, MinimizeCallback minimize_callback,
                  RestoreCallback restore_callback);
  void Shutdown();
  void SetAnimationDuration(float duration_seconds) {
    animation_renderer_.SetDuration(duration_seconds);
  }
  void SetAnimationEasing(minimize::animation::EasingCurve easing,
                          minimize::animation::CubicBezier custom = {}) {
    animation_renderer_.SetEasing(easing, custom);
  }
  void SetReversalAnimation(float duration_seconds, minimize::animation::EasingCurve easing,
                            minimize::animation::CubicBezier custom = {}) {
    animation_renderer_.SetReversalAnimation(duration_seconds, easing, custom);
  }
  void SetAnimationStyle(minimize::animation::AnimationStyle style) {
    animation_renderer_.SetStyle(style);
  }
  void SetMeshSegmentCount(int segment_count) {
    animation_renderer_.SetMeshSegmentCount(segment_count);
  }
  void SetMinimizeStrength(float strength) { animation_renderer_.SetMinimizeStrength(strength); }
  void SetFadeStrength(float strength) { animation_renderer_.SetFadeStrength(strength); }
  void SetTargetIndicatorEnabled(bool enabled) { target_indicator_enabled_ = enabled; }

  [[nodiscard]] HWND window() const { return window_; }
  [[nodiscard]] bool active() const { return animation_renderer_.active(); }
  [[nodiscard]] bool clock_started() const { return animation_renderer_.clock_started(); }
  [[nodiscard]] bool device_lost() const { return device_lost_ || overlay_renderer_.device_lost(); }

  [[nodiscard]] bool StartAnimation(CapturedTexture captured_texture,
                                    const minimize::animation::RectF& source_screen_rect,
                                    const minimize::animation::RectF& target_screen_rect,
                                    minimize::animation::MinimizeEdge edge, float start_progress = 0.0f,
                                    float target_progress = 1.0f,
                                    bool wait_for_first_frame = true,
                                    bool adjust_taskbar_z_order = true);
  void StartAnimationClock();
  void ContinueMinimizeAnimation();
  void ReverseAnimation(bool start_clock = true);
  bool Tick();
  void CancelAnimation();
  void FinishRestoreAnimation();
  [[nodiscard]] bool restoring() const { return animation_renderer_.restoring(); }
  [[nodiscard]] CapturedTexture* mutable_captured_texture() {
    return animation_renderer_.mutable_texture();
  }

private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

  bool RegisterWindowClass(HINSTANCE instance);
  bool CreateOverlayWindow(HINSTANCE instance);
  bool InitializeComposition();
  bool CreateRenderTarget();
  bool ResizeOverlaySurface(const RECT& screen_rect);
  void ApplyVisibleOverlayRegion(HWND taskbar_window);
  [[nodiscard]] bool Render(float progress);
  void ClearFrame();
  void HideOverlay();
  void ShowTargetIndicator(const minimize::animation::RectF& target);
  void HideTargetIndicator();
  void MarkDeviceLost(const wchar_t* operation, HRESULT hr);
  [[nodiscard]] minimize::animation::RectF ToOverlayRect(
      const minimize::animation::RectF& screen_rect) const;

  D3dDevice* d3d_device_ = nullptr;
  HWND window_ = nullptr;
  HWND target_indicator_window_ = nullptr;
  RECT virtual_screen_rect_{};
  RECT overlay_screen_rect_{};
  UINT width_ = 0;
  UINT height_ = 0;

  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view_;
  wil::com_ptr<IDCompositionDevice> composition_device_;
  wil::com_ptr<IDCompositionTarget> composition_target_;
  wil::com_ptr<IDCompositionVisual> composition_visual_;
  AnimationRenderer animation_renderer_;
  OverlayRenderer overlay_renderer_;
  MinimizeCallback minimize_callback_;
  RestoreCallback restore_callback_;
  bool target_indicator_enabled_ = false;
  std::chrono::steady_clock::time_point target_indicator_hide_time_{};
  UINT minimize_attempt_message_ = 0;
  UINT restore_attempt_message_ = 0;
  UINT query_window_state_message_ = 0;
  bool device_lost_ = false;
};

}  // namespace minimize::rendering
