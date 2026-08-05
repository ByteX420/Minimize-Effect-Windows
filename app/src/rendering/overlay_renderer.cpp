#include "pch.hpp"

#include "rendering/overlay_renderer.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

#include "genie_pixel_shader.hpp"
#include "genie_vertex_shader.hpp"
#include "rendering/d3d_device.hpp"

namespace minimize::rendering {
namespace {

constexpr UINT kGridSegments = 64;
constexpr UINT kGridVertexCount = (kGridSegments + 1) * (kGridSegments + 1);
constexpr UINT kGridIndexCount = kGridSegments * kGridSegments * 6;

struct PixelConstants {
  float opacity = 1.0f;
  float padding[3]{};
};

struct VisualConstants {
  float texture_size[2]{};
  float shadow_radius = 0.0f;
  float shadow_opacity = 0.0f;
  std::uint32_t render_shadow = 0;
  float animation_progress = 0.0f;
  std::uint32_t has_per_pixel_alpha = 0;
  std::uint32_t texture_rotation = 0;
};

struct alignas(256) ConstantBlock256 {
  std::uint8_t data[256]{};
};

struct FrameConstants {
  ConstantBlock256 genie;         // Offset 0 B   (FirstConstant = 0)
  ConstantBlock256 pixel;         // Offset 256 B (FirstConstant = 16)
  ConstantBlock256 visual_shadow; // Offset 512 B (FirstConstant = 32)
  ConstantBlock256 visual_main;   // Offset 768 B (FirstConstant = 48)
};
static_assert(sizeof(FrameConstants) == 1024);

}  // namespace

struct OverlayPipelineResources {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;

  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer;

  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> mask_sampler_state;

  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;

  UINT index_count = 0;
  bool static_state_bound = false;

  void BindStaticState() {
    if (static_state_bound || !context) {
      return;
    }

    constexpr UINT stride = sizeof(animation::GridVertex);
    constexpr UINT offset = 0;
    ID3D11Buffer* vertices = vertex_buffer.Get();

    context->IASetInputLayout(input_layout.Get());
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);

    const std::array<ID3D11SamplerState*, 2> samplers = {
        sampler_state.Get(),
        mask_sampler_state.Get(),
    };
    context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());

    constexpr std::array<float, 4> kBlend = {0.0f, 0.0f, 0.0f, 0.0f};
    context->OMSetBlendState(blend_state.Get(), kBlend.data(), 0xffffffff);
    context->RSSetState(rasterizer_state.Get());

    static_state_bound = true;
  }
};

namespace {

std::mutex g_pipeline_mutex;
std::weak_ptr<OverlayPipelineResources> g_pipeline;

std::shared_ptr<OverlayPipelineResources> AcquireOverlayPipeline(D3dDevice* device) {
  if (device == nullptr) {
    return {};
  }

  std::lock_guard lock(g_pipeline_mutex);
  if (auto existing = g_pipeline.lock();
      existing && existing->device.Get() == device->device()) {
    return existing;
  }

  auto pipeline = std::make_shared<OverlayPipelineResources>();
  pipeline->device = device->device();

  if (FAILED(device->context()->QueryInterface(IID_PPV_ARGS(&pipeline->context)))) {
    return {};
  }

  if (FAILED(device->device()->CreateVertexShader(
          g_genie_vertex_shader, sizeof(g_genie_vertex_shader), nullptr, &pipeline->vertex_shader))) {
    return {};
  }

  constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 1> kElements = {
      D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                               D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  if (FAILED(device->device()->CreateInputLayout(
          kElements.data(), static_cast<UINT>(kElements.size()),
          g_genie_vertex_shader, sizeof(g_genie_vertex_shader), &pipeline->input_layout))) {
    return {};
  }

  if (FAILED(device->device()->CreatePixelShader(
          g_genie_pixel_shader, sizeof(g_genie_pixel_shader), nullptr, &pipeline->pixel_shader))) {
    return {};
  }

  std::vector<animation::GridVertex> vertices;
  vertices.reserve(kGridVertexCount);
  for (UINT row = 0; row <= kGridSegments; ++row) {
    for (UINT column = 0; column <= kGridSegments; ++column) {
      vertices.push_back(animation::GridVertex{
          .u = static_cast<float>(column) / static_cast<float>(kGridSegments),
          .v = static_cast<float>(row) / static_cast<float>(kGridSegments),
      });
    }
  }

  std::vector<std::uint16_t> indices;
  indices.reserve(kGridIndexCount);
  for (UINT row = 0; row < kGridSegments; ++row) {
    for (UINT column = 0; column < kGridSegments; ++column) {
      const auto lower_left = static_cast<std::uint16_t>(row * (kGridSegments + 1) + column);
      const auto lower_right = static_cast<std::uint16_t>(lower_left + 1);
      const auto upper_left =
          static_cast<std::uint16_t>((row + 1) * (kGridSegments + 1) + column);
      const auto upper_right = static_cast<std::uint16_t>(upper_left + 1);
      indices.insert(indices.end(),
                     {lower_left, upper_left, lower_right, lower_right, upper_left, upper_right});
    }
  }

  D3D11_BUFFER_DESC vertex_desc{};
  vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(animation::GridVertex));
  vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
  vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vertex_data{.pSysMem = vertices.data()};
  if (FAILED(device->device()->CreateBuffer(&vertex_desc, &vertex_data, &pipeline->vertex_buffer))) {
    return {};
  }

  D3D11_BUFFER_DESC index_desc{};
  index_desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
  index_desc.Usage = D3D11_USAGE_IMMUTABLE;
  index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  D3D11_SUBRESOURCE_DATA index_data{.pSysMem = indices.data()};
  if (FAILED(device->device()->CreateBuffer(&index_desc, &index_data, &pipeline->index_buffer))) {
    return {};
  }

  pipeline->index_count = kGridIndexCount;

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->device()->CreateSamplerState(&sampler, &pipeline->sampler_state))) {
    return {};
  }

  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
  if (FAILED(device->device()->CreateSamplerState(&sampler, &pipeline->mask_sampler_state))) {
    return {};
  }

  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->device()->CreateBlendState(&blend, &pipeline->blend_state))) {
    return {};
  }

  D3D11_RASTERIZER_DESC rasterizer{};
  rasterizer.FillMode = D3D11_FILL_SOLID;
  rasterizer.CullMode = D3D11_CULL_NONE;
  rasterizer.DepthClipEnable = TRUE;
  if (FAILED(device->device()->CreateRasterizerState(&rasterizer, &pipeline->rasterizer_state))) {
    return {};
  }

  g_pipeline = pipeline;
  return pipeline;
}

}  // namespace

bool OverlayRenderer::Initialize(D3dDevice* device) {
  Shutdown();
  device_ = device;
  if (device_ == nullptr) {
    return false;
  }

  pipeline_ = AcquireOverlayPipeline(device_);
  if (!pipeline_) {
    Shutdown();
    return false;
  }

  D3D11_BUFFER_DESC constants{};
  constants.ByteWidth = sizeof(FrameConstants);
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return SUCCEEDED(device_->device()->CreateBuffer(&constants, nullptr, &constant_buffer_));
}

void OverlayRenderer::Shutdown() {
  constant_buffer_.Reset();
  pipeline_.reset();
  device_lost_ = false;
  device_ = nullptr;
}

bool OverlayRenderer::Render(const animation::GenieConstants& genie_constants,
                             ID3D11ShaderResourceView* texture,
                             ID3D11ShaderResourceView* mask_texture,
                             const WindowVisualMetadata& visual_metadata,
                             ID3D11RenderTargetView* render_target, UINT width, UINT height,
                             float opacity) {
  if (device_lost_ || texture == nullptr || mask_texture == nullptr || render_target == nullptr ||
      !pipeline_ || !pipeline_->context) {
    return false;
  }

  ID3D11DeviceContext1* context1 = pipeline_->context.Get();

  const float texture_width = (genie_constants.source.right - genie_constants.source.left) * width;
  const float texture_height =
      (genie_constants.source.bottom - genie_constants.source.top) * height;

  auto build_visual = [&](bool shadow) {
    return VisualConstants{
        .texture_size = {std::max(texture_width, 1.0f), std::max(texture_height, 1.0f)},
        .shadow_radius = std::max(visual_metadata.shadow_radius, 0.0f),
        .shadow_opacity = std::clamp(visual_metadata.shadow_opacity, 0.0f, 1.0f),
        .render_shadow = shadow ? 1U : 0U,
        .animation_progress = std::clamp(genie_constants.progress, 0.0f, 1.0f),
        .has_per_pixel_alpha = visual_metadata.has_per_pixel_alpha ? 1U : 0U,
        .texture_rotation = static_cast<std::uint32_t>(visual_metadata.texture_rotation),
    };
  };

  FrameConstants frame_constants{};
  std::memcpy(frame_constants.genie.data, &genie_constants, sizeof(genie_constants));
  const PixelConstants pixel_constants{.opacity = opacity};
  std::memcpy(frame_constants.pixel.data, &pixel_constants, sizeof(pixel_constants));

  const VisualConstants visual_shadow = build_visual(true);
  std::memcpy(frame_constants.visual_shadow.data, &visual_shadow, sizeof(visual_shadow));

  const VisualConstants visual_main = build_visual(false);
  std::memcpy(frame_constants.visual_main.data, &visual_main, sizeof(visual_main));

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT hr =
      context1->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (FAILED(hr)) {
    MarkDeviceLost(hr);
    return false;
  }
  std::memcpy(mapped.pData, &frame_constants, sizeof(frame_constants));
  context1->Unmap(constant_buffer_.Get(), 0);

  D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                          0.0f, 1.0f};
  constexpr std::array<float, 4> kClear = {0.0f, 0.0f, 0.0f, 0.0f};
  context1->OMSetRenderTargets(1, &render_target, nullptr);
  context1->ClearRenderTargetView(render_target, kClear.data());
  context1->RSSetViewports(1, &viewport);

  pipeline_->BindStaticState();

  // SetConstantBuffers1 requires offsets and lengths aligned to 16 constants (256 bytes).
  constexpr UINT kGenieFirst = 0, kGenieCount = 16;
  constexpr UINT kPixelFirst = 16, kPixelCount = 16;
  constexpr UINT kVisualShadowFirst = 32, kVisualCount = 16;
  constexpr UINT kVisualMainFirst = 48;

  ID3D11Buffer* buf = constant_buffer_.Get();
  context1->VSSetConstantBuffers1(0, 1, &buf, &kGenieFirst, &kGenieCount);
  context1->PSSetConstantBuffers1(1, 1, &buf, &kPixelFirst, &kPixelCount);

  const std::array<ID3D11ShaderResourceView*, 2> resources = {texture, mask_texture};
  context1->PSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());

  if (visual_metadata.shadow_radius > 0.0f && visual_metadata.shadow_opacity > 0.0f) {
    context1->VSSetConstantBuffers1(2, 1, &buf, &kVisualShadowFirst, &kVisualCount);
    context1->PSSetConstantBuffers1(2, 1, &buf, &kVisualShadowFirst, &kVisualCount);
    context1->DrawIndexed(pipeline_->index_count, 0, 0);
  }

  context1->VSSetConstantBuffers1(2, 1, &buf, &kVisualMainFirst, &kVisualCount);
  context1->PSSetConstantBuffers1(2, 1, &buf, &kVisualMainFirst, &kVisualCount);
  context1->DrawIndexed(pipeline_->index_count, 0, 0);

  constexpr std::array<ID3D11ShaderResourceView*, 2> kNullResources = {nullptr, nullptr};
  context1->PSSetShaderResources(0, static_cast<UINT>(kNullResources.size()), kNullResources.data());
  return true;
}

void OverlayRenderer::MarkDeviceLost(HRESULT result) {
  if (device_ != nullptr && device_->IsDeviceLost(result)) device_lost_ = true;
}

}  // namespace minimize::rendering
