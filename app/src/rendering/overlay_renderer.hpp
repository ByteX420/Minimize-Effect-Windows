#pragma once

#include <memory>
#include <d3d11_4.h>
#include <wrl/client.h>

#include "animation/minimize_mesh.hpp"
#include "rendering/window_visual_metadata.hpp"

namespace minimize::rendering {

class D3dDevice;
struct OverlayPipelineResources;

class OverlayRenderer final {
public:
  [[nodiscard]] bool Initialize(D3dDevice* device);
  void Shutdown();
  [[nodiscard]] bool Render(const animation::GenieConstants& genie_constants,
                            ID3D11ShaderResourceView* texture,
                            ID3D11ShaderResourceView* mask_texture,
                            const WindowVisualMetadata& visual_metadata,
                            ID3D11RenderTargetView* render_target, UINT width, UINT height,
                            float opacity);
  [[nodiscard]] bool device_lost() const { return device_lost_; }

private:
  void MarkDeviceLost(HRESULT result);

  D3dDevice* device_ = nullptr;
  std::shared_ptr<OverlayPipelineResources> pipeline_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
  bool device_lost_ = false;
};

}  // namespace minimize::rendering
