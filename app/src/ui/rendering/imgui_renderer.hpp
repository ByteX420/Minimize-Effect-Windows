#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>

struct ImFont;

namespace minimize::ui::rendering {

class ImguiRenderer final {
public:
  ~ImguiRenderer();

  bool Initialize(HWND window);
  void Shutdown();
  [[nodiscard]] bool BeginFrame();
  [[nodiscard]] bool EndFrame();
  void Resize(UINT width, UINT height);
  [[nodiscard]] bool HandleWin32Message(HWND window, UINT message, WPARAM w_param,
                                        LPARAM l_param) const;
  void UpdateDpi(UINT dpi);

  [[nodiscard]] bool ready() const;
  [[nodiscard]] UINT dpi() const { return dpi_; }
  [[nodiscard]] float scale() const { return scale_; }
  [[nodiscard]] ImFont* small_font() const { return small_font_; }
  [[nodiscard]] ImFont* body_font() const { return body_font_; }
  [[nodiscard]] ImFont* medium_font() const { return medium_font_; }
  [[nodiscard]] ImFont* title_font() const { return title_font_; }
  [[nodiscard]] HANDLE frame_latency_waitable_object() const {
    return frame_latency_waitable_object_;
  }

private:
  bool CreateDeviceResources();
  bool CreateRenderTarget();
  void ReleaseDeviceResources();
  void HandleDeviceLost();
  bool TryRecoverDeviceResources();
  void RebuildFonts();
  void ApplyStyle() const;
  [[nodiscard]] static bool IsDeviceLostError(HRESULT result);

  HWND window_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view_;
  HANDLE frame_latency_waitable_object_ = nullptr;
  UINT swap_chain_flags_ = 0;
  bool context_ready_ = false;
  bool win32_ready_ = false;
  bool dx11_ready_ = false;
  bool recovery_pending_ = false;
  ULONGLONG next_recovery_ms_ = 0;
  DWORD recovery_delay_ms_ = 0;
  bool recovery_test_pending_ = false;
  UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
  float scale_ = 1.0f;
  ImFont* small_font_ = nullptr;
  ImFont* body_font_ = nullptr;
  ImFont* medium_font_ = nullptr;
  ImFont* title_font_ = nullptr;
};

}  // namespace minimize::ui::rendering
