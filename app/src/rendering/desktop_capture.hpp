#pragma once

#include <atomic>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include "animation/geometry.hpp"
#include "rendering/d3d_device.hpp"
#include "rendering/desktop_duplication_session.hpp"
#include "rendering/window_visual_metadata.hpp"

namespace minimize::rendering {

struct CapturedTexture {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_view;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_texture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_shader_resource_view;
  minimize::animation::SizeF size;
  WindowVisualMetadata visual_metadata;
  std::uint64_t frame_generation = 0;
};

class DesktopCapture {
public:
  explicit DesktopCapture(D3dDevice* d3d_device);

  DesktopCapture(const DesktopCapture&) = delete;
  DesktopCapture& operator=(const DesktopCapture&) = delete;

  [[nodiscard]] bool CaptureRegion(HWND window, const RECT& screen_rect,
                                   CapturedTexture* captured_texture);
  [[nodiscard]] bool CaptureWindow(HWND window, const RECT& requested_screen_rect,
                                   CapturedTexture* captured_texture, RECT* captured_screen_rect);
  [[nodiscard]] bool RefreshCapturedTexture(const RECT& screen_rect,
                                            CapturedTexture* captured_texture);
  void ClearHistory() { duplication_session_.ClearHistory(); }
  [[nodiscard]] bool device_lost() const {
    return device_lost_.load(std::memory_order_acquire) || duplication_session_.device_lost();
  }
  void ClearDeviceLost() {
    device_lost_.store(false, std::memory_order_release);
    duplication_session_.ClearDeviceLost();
  }

private:
  using OutputCapture = DesktopDuplicationSession::OutputCapture;
  [[nodiscard]] bool CopyRegionFromFrame(OutputCapture* output, const RECT& screen_rect,
                                         CapturedTexture* captured_texture);
  [[nodiscard]] bool CopyRegionIntoTexture(OutputCapture* output, const RECT& screen_rect,
                                           CapturedTexture* captured_texture);
  [[nodiscard]] bool AttachWindowVisuals(
      HWND window, const RECT& capture_rect, WindowVisualMetadata metadata,
      CapturedTexture* captured_texture,
      const std::vector<std::uint8_t>* captured_pixels = nullptr);
  void MarkDeviceLost(const wchar_t* context, HRESULT hr);

  D3dDevice* d3d_device_ = nullptr;
  DesktopDuplicationSession duplication_session_;
  std::atomic_bool device_lost_{false};
};

}  // namespace minimize::rendering
