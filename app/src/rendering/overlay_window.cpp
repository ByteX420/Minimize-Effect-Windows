#include "pch.hpp"

#include "rendering/overlay_window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <dwmapi.h>
#include <iostream>
#include <sstream>

#include "core/logger.hpp"
#include "platform/windows/display_info.hpp"
#include "platform/windows/taskbar_locator.hpp"
#include "platform/windows/window_properties.hpp"
#include "platform/windows/window_state.hpp"

namespace minimize::rendering {
namespace {

constexpr wchar_t kOverlayWindowClassName[] = L"MinimizeEffectOverlayWindow";
constexpr wchar_t kTargetIndicatorClassName[] = L"MinimizeEffectTargetIndicator";
constexpr LONG kOverlayPadding = 2;

int RectWidth(const RECT& rect) { return static_cast<int>(rect.right - rect.left); }

int RectHeight(const RECT& rect) { return static_cast<int>(rect.bottom - rect.top); }

minimize::animation::RectF RectToRectF(const RECT& rect) {
  return minimize::animation::RectF{
      .left = static_cast<float>(rect.left),
      .top = static_cast<float>(rect.top),
      .right = static_cast<float>(rect.right),
      .bottom = static_cast<float>(rect.bottom),
  };
}

RECT AnimationSurfaceRect(const minimize::animation::RectF& source,
                          const minimize::animation::RectF& target, const RECT& virtual_screen,
                          LONG padding) {
  RECT requested{
      .left = static_cast<LONG>(std::floor(std::min(source.left, target.left))) - padding,
      .top = static_cast<LONG>(std::floor(std::min(source.top, target.top))) - padding,
      .right = static_cast<LONG>(std::ceil(std::max(source.right, target.right))) + padding,
      .bottom = static_cast<LONG>(std::ceil(std::max(source.bottom, target.bottom))) + padding,
  };
  RECT clipped{};
  if (!IntersectRect(&clipped, &requested, &virtual_screen) || RectWidth(clipped) <= 0 ||
      RectHeight(clipped) <= 0) {
    return RECT{};
  }
  return clipped;
}

std::wstring RectFTraceString(const minimize::animation::RectF& rect) {
  std::wstringstream ss;
  ss << L"(" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom << L")";
  return ss.str();
}

void AllowCrossIntegrityMessage(HWND window, UINT message, const wchar_t* message_name) {
  (void)message_name;
  if (window == nullptr || message == 0) {
    return;
  }

  CHANGEFILTERSTRUCT filter_status{};
  filter_status.cbSize = sizeof(filter_status);
  if (!ChangeWindowMessageFilterEx(window, message, MSGFLT_ALLOW, &filter_status)) {
    minimize::core::LogDebug(L"Overlay", std::wstring(L"ChangeWindowMessageFilterEx failed for ") +
                                             message_name + L" message=" +
                                             std::to_wstring(message) + L" error=" +
                                             std::to_wstring(GetLastError()));
    return;
  }

  minimize::core::LogDebug(L"Overlay", std::wstring(L"Allowed cross-integrity window message ") +
                                           message_name + L" message=" + std::to_wstring(message));
}

}  // namespace

OverlayWindow::~OverlayWindow() { Shutdown(); }

bool OverlayWindow::Initialize(HINSTANCE instance, D3dDevice* d3d_device,
                               MinimizeCallback minimize_callback,
                               RestoreCallback restore_callback) {
  d3d_device_ = d3d_device;
  device_lost_ = false;
  minimize_callback_ = std::move(minimize_callback);
  restore_callback_ = std::move(restore_callback);
  minimize_attempt_message_ = RegisterWindowMessageW(L"MinimizeMinimizeAttempt");
  restore_attempt_message_ = RegisterWindowMessageW(L"MinimizeRestoreAttempt");
  virtual_screen_rect_ = platform::GetVirtualScreenRect();
  overlay_screen_rect_ = RECT{virtual_screen_rect_.left, virtual_screen_rect_.top,
                              virtual_screen_rect_.left + 1, virtual_screen_rect_.top + 1};
  width_ = 1;
  height_ = 1;

  if (!RegisterWindowClass(instance) || !CreateOverlayWindow(instance) ||
      !InitializeComposition() || !CreateRenderTarget() ||
      !overlay_renderer_.Initialize(d3d_device_)) {
    Shutdown();
    return false;
  }

  AllowCrossIntegrityMessage(window_, minimize_attempt_message_, L"MinimizeMinimizeAttempt");
  AllowCrossIntegrityMessage(window_, restore_attempt_message_, L"MinimizeRestoreAttempt");

  ClearFrame();
  ShowWindow(window_, SW_SHOWNOACTIVATE);
  return true;
}

void OverlayWindow::Shutdown() {
  animation_renderer_.Cancel();
  overlay_renderer_.Shutdown();
  if (window_ != nullptr) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
  if (target_indicator_window_ != nullptr) {
    DestroyWindow(target_indicator_window_);
    target_indicator_window_ = nullptr;
  }
  render_target_view_.Reset();
  composition_visual_.Reset();
  composition_target_.Reset();
  composition_device_.Reset();
  swap_chain_.Reset();
  d3d_device_ = nullptr;
}

bool OverlayWindow::StartAnimation(CapturedTexture captured_texture,
                                   const minimize::animation::RectF& source_screen_rect,
                                   const minimize::animation::RectF& target_screen_rect,
                                   minimize::animation::MinimizeEdge edge, float start_progress,
                                   float target_progress, bool wait_for_first_frame,
                                   bool adjust_taskbar_z_order) {
  minimize::core::LogTrace(
      L"Overlay", L"StartAnimation requested source=" + RectFTraceString(source_screen_rect) +
                      L" target=" + RectFTraceString(target_screen_rect) + L" start_progress=" +
                      std::to_wstring(start_progress) + L" target_progress=" +
                      std::to_wstring(target_progress) + L" edge=" +
                      std::to_wstring(static_cast<int>(edge)) + L" has_srv=" +
                      std::to_wstring(captured_texture.shader_resource_view != nullptr));

  if (captured_texture.shader_resource_view == nullptr) {
    minimize::core::LogTrace(L"Overlay", L"StartAnimation failed: missing shader resource view");
    std::wcerr << L"Overlay start failed: captured texture has no shader "
                  L"resource view.\n";
    return false;
  }

  const LONG shadow_padding =
      static_cast<LONG>(std::ceil(captured_texture.visual_metadata.shadow_radius * 2.0f));
  const RECT animation_surface =
      AnimationSurfaceRect(source_screen_rect, target_screen_rect, virtual_screen_rect_,
                           std::max(kOverlayPadding, shadow_padding));
  if (RectWidth(animation_surface) <= 0 || RectHeight(animation_surface) <= 0) {
    minimize::core::LogTrace(L"Overlay",
                             L"StartAnimation failed: animation surface is outside virtual screen");
    return false;
  }

  HRGN hidden_region = CreateRectRgn(0, 0, 0, 0);
  (void)minimize::platform::SetOwnedWindowRegion(window_, hidden_region, false);
  if (!ResizeOverlaySurface(animation_surface)) {
    minimize::core::LogTrace(L"Overlay",
                             L"StartAnimation failed: ResizeOverlaySurface returned "
                             L"false");
    HideOverlay();
    return false;
  }

  if (!animation_renderer_.Begin(std::move(captured_texture), ToOverlayRect(source_screen_rect),
                                 ToOverlayRect(target_screen_rect), edge, start_progress,
                                 target_progress)) {
    HideOverlay();
    return false;
  }
  if (target_indicator_enabled_) ShowTargetIndicator(target_screen_rect);

  if (!Render(animation_renderer_.progress())) {
    minimize::core::LogTrace(L"Overlay", L"StartAnimation failed: first Render returned false");
    animation_renderer_.Cancel();
    HideOverlay();
    std::wcerr << L"Overlay start failed: first frame render failed.\n";
    return false;
  }
  RECT target_rect_win{};
  target_rect_win.left = static_cast<LONG>(target_screen_rect.left);
  target_rect_win.top = static_cast<LONG>(target_screen_rect.top);
  target_rect_win.right = static_cast<LONG>(target_screen_rect.right);
  target_rect_win.bottom = static_cast<LONG>(target_screen_rect.bottom);

  HWND taskbar_hwnd = minimize::platform::FindTaskbarWindowForRect(target_rect_win);

  if (adjust_taskbar_z_order && taskbar_hwnd != nullptr) {
    SetWindowPos(taskbar_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }

  ApplyVisibleOverlayRegion(taskbar_hwnd);
  SetWindowPos(window_, HWND_TOPMOST, overlay_screen_rect_.left, overlay_screen_rect_.top,
               static_cast<int>(width_), static_cast<int>(height_),
               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

  if (wait_for_first_frame) {
    DwmFlush();
    const HRESULT hr = composition_device_->WaitForCommitCompletion();
    if (FAILED(hr)) {
      MarkDeviceLost(L"WaitForCommitCompletion", hr);
      minimize::core::LogTrace(L"Overlay",
                               L"StartAnimation failed: WaitForCommitCompletion hr=0x" +
                                   std::to_wstring(static_cast<unsigned long>(hr)));
      animation_renderer_.Cancel();
      HideOverlay();
      std::wcerr << L"Overlay start failed: first frame commit did not "
                    L"complete: 0x"
                 << std::hex << hr << std::dec << L"\n";
      return false;
    }
  }
  minimize::core::LogTrace(L"Overlay", L"StartAnimation first frame visible overlay_source=" +
                                           RectFTraceString(ToOverlayRect(source_screen_rect)) +
                                           L" overlay_target=" +
                                           RectFTraceString(ToOverlayRect(target_screen_rect)));
  std::wcout << L"Overlay first frame visible: source=(" << source_screen_rect.left << L","
             << source_screen_rect.top << L"," << source_screen_rect.right << L","
             << source_screen_rect.bottom << L") target=(" << target_screen_rect.left << L","
             << target_screen_rect.top << L"," << target_screen_rect.right << L","
             << target_screen_rect.bottom << L").\n";
  return true;
}

void OverlayWindow::StartAnimationClock() { animation_renderer_.StartClock(); }

void OverlayWindow::ContinueMinimizeAnimation() { animation_renderer_.ContinueMinimize(); }

void OverlayWindow::ReverseAnimation(bool start_clock) { animation_renderer_.Reverse(start_clock); }

bool OverlayWindow::Tick() {
  if (!animation_renderer_.active()) return false;
  if (target_indicator_window_ != nullptr && IsWindowVisible(target_indicator_window_) &&
      std::chrono::steady_clock::now() >= target_indicator_hide_time_) {
    HideTargetIndicator();
  }
  const AnimationRenderer::AdvanceResult advance = animation_renderer_.Advance();
  if (!advance.should_render) return true;
  const bool rendered = Render(advance.progress);
  animation_renderer_.CompleteFrame(rendered, advance.reached_target);
  if (!rendered || advance.reached_target) {
    if (!rendered || animation_renderer_.target_progress() != 0.0f) HideOverlay();
    return false;
  }
  return true;
}

void OverlayWindow::CancelAnimation() {
  animation_renderer_.Cancel();
  HideOverlay();
  HideTargetIndicator();
}

void OverlayWindow::FinishRestoreAnimation() {
  animation_renderer_.FinishRestore();
  HideOverlay();
  HideTargetIndicator();
}

LRESULT CALLBACK OverlayWindow::WindowProc(HWND window, UINT message, WPARAM w_param,
                                           LPARAM l_param) {
  OverlayWindow* overlay = nullptr;
  if (message == WM_NCCREATE) {
    const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    overlay = static_cast<OverlayWindow*>(create_struct->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    overlay->window_ = window;
  } else {
    overlay = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }

  if (overlay != nullptr) {
    return overlay->HandleMessage(window, message, w_param, l_param);
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT OverlayWindow::HandleMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
  if (message == minimize_attempt_message_ && minimize_attempt_message_ != 0) {
    HWND minimize_window = reinterpret_cast<HWND>(w_param);
    if (minimize_callback_ && minimize_callback_(minimize_window)) {
      return 1;
    }
    if (minimize_window != nullptr && IsWindow(minimize_window)) {
      SetPropW(minimize_window, platform::windows::properties::kAllowMinimize,
               reinterpret_cast<HANDLE>(1));
      ShowWindow(minimize_window, SW_MINIMIZE);
      RemovePropW(minimize_window, platform::windows::properties::kAllowMinimize);
      minimize::core::LogTrace(L"Overlay",
                               L"Minimize callback failed; allowed native minimize fallback");
    }
    return 0;
  }

  if (message == restore_attempt_message_ && restore_attempt_message_ != 0) {
    HWND restore_window = reinterpret_cast<HWND>(w_param);
    if (restore_callback_ && restore_callback_(restore_window)) {
      return 1;
    }
    return 0;
  }

  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_DESTROY:
      window_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(hwnd, message, w_param, l_param);
  }
}

bool OverlayWindow::RegisterWindowClass(HINSTANCE instance) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &OverlayWindow::WindowProc;
  window_class.hInstance = instance;
  // MultiByte builds: IDC_* are LPSTR; cast for *W APIs (integer resource IDs).
  window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  window_class.lpszClassName = kOverlayWindowClassName;

  if (RegisterClassExW(&window_class) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_CLASS_ALREADY_EXISTS) return false;
  }

  WNDCLASSEXW indicator_class{};
  indicator_class.cbSize = sizeof(indicator_class);
  indicator_class.lpfnWndProc = DefWindowProcW;
  indicator_class.hInstance = instance;
  indicator_class.hbrBackground = CreateSolidBrush(RGB(92, 154, 255));
  indicator_class.lpszClassName = kTargetIndicatorClassName;
  if (RegisterClassExW(&indicator_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  return true;
}

bool OverlayWindow::CreateOverlayWindow(HINSTANCE instance) {
  constexpr DWORD kExStyle =
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED;
  window_ =
      CreateWindowExW(kExStyle, kOverlayWindowClassName, L"Minimize Effect Overlay", WS_POPUP,
                      overlay_screen_rect_.left, overlay_screen_rect_.top, static_cast<int>(width_),
                      static_cast<int>(height_), nullptr, nullptr, instance, this);
  target_indicator_window_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
      kTargetIndicatorClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
  if (target_indicator_window_ != nullptr) {
    SetLayeredWindowAttributes(target_indicator_window_, 0, 190, LWA_ALPHA);
  }
  return window_ != nullptr && target_indicator_window_ != nullptr;
}

void OverlayWindow::ShowTargetIndicator(const minimize::animation::RectF& target) {
  if (target_indicator_window_ == nullptr) return;
  const int left = static_cast<int>(std::floor(target.left)) - 3;
  const int top = static_cast<int>(std::floor(target.top)) - 3;
  const int width = std::max(8, static_cast<int>(std::ceil(target.right - target.left)) + 6);
  const int height = std::max(8, static_cast<int>(std::ceil(target.bottom - target.top)) + 6);
  HRGN outer = CreateRectRgn(0, 0, width, height);
  HRGN inner = CreateRectRgn(2, 2, width - 2, height - 2);
  if (outer != nullptr && inner != nullptr) CombineRgn(outer, outer, inner, RGN_DIFF);
  if (inner != nullptr) DeleteObject(inner);
  if (outer != nullptr) SetWindowRgn(target_indicator_window_, outer, TRUE);
  SetWindowPos(target_indicator_window_, HWND_TOPMOST, left, top, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  target_indicator_hide_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
}

void OverlayWindow::HideTargetIndicator() {
  if (target_indicator_window_ != nullptr) ShowWindow(target_indicator_window_, SW_HIDE);
}

bool OverlayWindow::InitializeComposition() {
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
  swap_chain_desc.Width = width_;
  swap_chain_desc.Height = height_;
  swap_chain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_chain_desc.Stereo = FALSE;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.BufferCount = 2;
  swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

  HRESULT hr = d3d_device_->factory()->CreateSwapChainForComposition(
      d3d_device_->device(), &swap_chain_desc, nullptr, &swap_chain_);
  if (FAILED(hr)) {
    std::wcerr << L"CreateSwapChainForComposition failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  hr = DCompositionCreateDevice(d3d_device_->dxgi_device(), IID_PPV_ARGS(&composition_device_));
  if (FAILED(hr)) {
    std::wcerr << L"DCompositionCreateDevice failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  hr = composition_device_->CreateTargetForHwnd(window_, TRUE, &composition_target_);
  if (FAILED(hr)) {
    std::wcerr << L"CreateTargetForHwnd failed: 0x" << std::hex << hr << L"\n";
    return false;
  }

  hr = composition_device_->CreateVisual(&composition_visual_);
  if (FAILED(hr)) {
    return false;
  }
  hr = composition_visual_->SetContent(swap_chain_.Get());
  if (FAILED(hr)) {
    return false;
  }
  hr = composition_target_->SetRoot(composition_visual_.Get());
  if (FAILED(hr)) {
    return false;
  }
  hr = composition_device_->Commit();
  if (FAILED(hr)) {
    MarkDeviceLost(L"Initial DirectComposition commit", hr);
  }
  return SUCCEEDED(hr);
}

bool OverlayWindow::CreateRenderTarget() {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  if (FAILED(hr)) {
    MarkDeviceLost(L"Swap-chain GetBuffer", hr);
    return false;
  }

  hr = d3d_device_->device()->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                                     &render_target_view_);
  if (FAILED(hr)) {
    MarkDeviceLost(L"CreateRenderTargetView", hr);
    std::wcerr << L"CreateRenderTargetView failed: 0x" << std::hex << hr << L"\n";
    return false;
  }
  return true;
}

bool OverlayWindow::ResizeOverlaySurface(const RECT& screen_rect) {
  const UINT new_width = static_cast<UINT>(RectWidth(screen_rect));
  const UINT new_height = static_cast<UINT>(RectHeight(screen_rect));
  if (new_width == 0 || new_height == 0 || swap_chain_ == nullptr || window_ == nullptr) {
    return false;
  }

  if (new_width != width_ || new_height != height_) {
    ID3D11DeviceContext* context = d3d_device_->context();
    context->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_view_.Reset();
    const HRESULT hr = swap_chain_->ResizeBuffers(0, new_width, new_height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      MarkDeviceLost(L"ResizeBuffers", hr);
      std::wcerr << L"ResizeBuffers for animation surface failed: 0x" << std::hex << hr << std::dec
                 << L"\n";
      return false;
    }
    width_ = new_width;
    height_ = new_height;
    if (!CreateRenderTarget()) {
      return false;
    }
  }

  overlay_screen_rect_ = screen_rect;
  return SetWindowPos(window_, HWND_TOPMOST, screen_rect.left, screen_rect.top,
                      static_cast<int>(new_width), static_cast<int>(new_height),
                      SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

void OverlayWindow::ApplyVisibleOverlayRegion(HWND taskbar_window) {
  HRGN visible_region = CreateRectRgn(0, 0, static_cast<int>(width_), static_cast<int>(height_));
  if (visible_region == nullptr) {
    return;
  }

  RECT taskbar_rect{};
  RECT overlap{};
  if (taskbar_window != nullptr && GetWindowRect(taskbar_window, &taskbar_rect) &&
      IntersectRect(&overlap, &overlay_screen_rect_, &taskbar_rect)) {
    OffsetRect(&overlap, -overlay_screen_rect_.left, -overlay_screen_rect_.top);
    HRGN taskbar_region = CreateRectRgn(overlap.left, overlap.top, overlap.right, overlap.bottom);
    if (taskbar_region != nullptr) {
      if (CombineRgn(visible_region, visible_region, taskbar_region, RGN_DIFF) == ERROR) {
        DeleteObject(taskbar_region);
        DeleteObject(visible_region);
        return;
      }
      DeleteObject(taskbar_region);
    }
  }

  (void)minimize::platform::SetOwnedWindowRegion(window_, visible_region, true);
}

bool OverlayWindow::Render(float progress) {
  (void)progress;
  if (device_lost_ || !animation_renderer_.active() || render_target_view_ == nullptr) {
    return false;
  }
  const float eased_progress = animation_renderer_.eased_progress();
  if (!overlay_renderer_.Render(animation_renderer_.GenieParameters(width_, height_),
                                animation_renderer_.texture_view(), animation_renderer_.mask_view(),
                                animation_renderer_.visual_metadata(), render_target_view_.Get(),
                                width_, height_, animation_renderer_.opacity(eased_progress))) {
    return false;
  }
  HRESULT result = swap_chain_->Present(0, 0);
  if (FAILED(result)) {
    MarkDeviceLost(L"Present", result);
    return false;
  }
  result = composition_device_->Commit();
  if (FAILED(result)) {
    MarkDeviceLost(L"DirectComposition commit", result);
    return false;
  }
  return true;
}

void OverlayWindow::ClearFrame() {
  if (render_target_view_ == nullptr || swap_chain_ == nullptr) {
    return;
  }
  constexpr std::array<float, 4> kClearColor = {0.0f, 0.0f, 0.0f, 0.0f};
  ID3D11DeviceContext* context = d3d_device_->context();
  context->OMSetRenderTargets(1, render_target_view_.GetAddressOf(), nullptr);
  context->ClearRenderTargetView(render_target_view_.Get(), kClearColor.data());
  HRESULT hr = swap_chain_->Present(0, 0);
  if (FAILED(hr)) {
    MarkDeviceLost(L"Clear-frame Present", hr);
    return;
  }
  hr = composition_device_->Commit();
  if (FAILED(hr)) {
    MarkDeviceLost(L"Clear-frame DirectComposition commit", hr);
  }
}

void OverlayWindow::MarkDeviceLost(const wchar_t* operation, HRESULT hr) {
  (void)operation;
  if (d3d_device_ == nullptr || !d3d_device_->IsDeviceLost(hr)) {
    return;
  }
  device_lost_ = true;
  minimize::core::LogDebug(
      L"Overlay",
      std::wstring(L"D3D device lost during ") + operation + L" hr=" +
          std::to_wstring(static_cast<unsigned long>(hr)) + L" reason=" +
          std::to_wstring(static_cast<unsigned long>(d3d_device_->DeviceRemovedReason())));
}

void OverlayWindow::HideOverlay() {
  if (window_ != nullptr) {
    ClearFrame();
    HRGN hidden_region = CreateRectRgn(0, 0, 0, 0);
    (void)minimize::platform::SetOwnedWindowRegion(window_, hidden_region, true);
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
}

minimize::animation::RectF OverlayWindow::ToOverlayRect(
    const minimize::animation::RectF& screen_rect) const {
  const minimize::animation::RectF overlay_screen = RectToRectF(overlay_screen_rect_);
  return minimize::animation::RectF{
      .left = screen_rect.left - overlay_screen.left,
      .top = screen_rect.top - overlay_screen.top,
      .right = screen_rect.right - overlay_screen.left,
      .bottom = screen_rect.bottom - overlay_screen.top,
  };
}

}  // namespace minimize::rendering
