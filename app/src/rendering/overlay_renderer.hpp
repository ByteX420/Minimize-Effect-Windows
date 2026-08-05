#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>

#include "animation/minimize_mesh.hpp"
#include "rendering/window_visual_metadata.hpp"

namespace minimize::rendering {

class D3dDevice;

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
  [[nodiscard]] bool CompileShaders();
  [[nodiscard]] bool CreateStaticGrid();
  void MarkDeviceLost(HRESULT result);

  D3dDevice* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> mask_sampler_state_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  UINT index_count_ = 0;
  bool device_lost_ = false;
};

}  // namespace minimize::rendering
