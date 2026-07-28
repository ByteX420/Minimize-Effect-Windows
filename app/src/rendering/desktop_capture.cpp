#include "pch.hpp"

#include "rendering/desktop_capture.hpp"

#include <algorithm>
#include <iostream>
#include <wil/resource.h>

#include "core/logger.hpp"
#include "rendering/capture_geometry.hpp"
#include "rendering/window_capture_mask.hpp"

namespace minimize::rendering {
namespace {

bool IsDeviceLostError(HRESULT hr) { return D3dDevice::IsDeviceLostError(hr); }

TextureRotation ToTextureRotation(DXGI_MODE_ROTATION rotation) {
  switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
      return TextureRotation::kRotate90;
    case DXGI_MODE_ROTATION_ROTATE180:
      return TextureRotation::kRotate180;
    case DXGI_MODE_ROTATION_ROTATE270:
      return TextureRotation::kRotate270;
    default:
      return TextureRotation::kIdentity;
  }
}

struct GpuCopyRegion {
  D3D11_BOX source_box{};
  UINT texture_width = 0;
  UINT texture_height = 0;
  TextureRotation texture_rotation = TextureRotation::kIdentity;
};

bool ResolveGpuCopyRegion(const RECT& clipped_rect, const RECT& desktop_coordinates,
                          const D3D11_TEXTURE2D_DESC& frame_desc, DXGI_MODE_ROTATION rotation,
                          GpuCopyRegion* region) {
  if (region == nullptr) return false;
  const int left = clipped_rect.left - desktop_coordinates.left;
  const int top = clipped_rect.top - desktop_coordinates.top;
  const int right = clipped_rect.right - desktop_coordinates.left;
  const int bottom = clipped_rect.bottom - desktop_coordinates.top;

  int physical_left = left;
  int physical_top = top;
  int physical_right = right;
  int physical_bottom = bottom;
  if (rotation == DXGI_MODE_ROTATION_ROTATE90) {
    physical_left = top;
    physical_top = static_cast<int>(frame_desc.Height) - right;
    physical_right = bottom;
    physical_bottom = static_cast<int>(frame_desc.Height) - left;
  } else if (rotation == DXGI_MODE_ROTATION_ROTATE180) {
    physical_left = static_cast<int>(frame_desc.Width) - right;
    physical_top = static_cast<int>(frame_desc.Height) - bottom;
    physical_right = static_cast<int>(frame_desc.Width) - left;
    physical_bottom = static_cast<int>(frame_desc.Height) - top;
  } else if (rotation == DXGI_MODE_ROTATION_ROTATE270) {
    physical_left = static_cast<int>(frame_desc.Width) - bottom;
    physical_top = left;
    physical_right = static_cast<int>(frame_desc.Width) - top;
    physical_bottom = right;
  }
  if (physical_left < 0 || physical_top < 0 || physical_right <= physical_left ||
      physical_bottom <= physical_top ||
      physical_right > static_cast<int>(frame_desc.Width) ||
      physical_bottom > static_cast<int>(frame_desc.Height)) {
    return false;
  }

  region->source_box = {
      .left = static_cast<UINT>(physical_left),
      .top = static_cast<UINT>(physical_top),
      .front = 0,
      .right = static_cast<UINT>(physical_right),
      .bottom = static_cast<UINT>(physical_bottom),
      .back = 1,
  };
  region->texture_width = static_cast<UINT>(physical_right - physical_left);
  region->texture_height = static_cast<UINT>(physical_bottom - physical_top);
  region->texture_rotation = ToTextureRotation(rotation);
  return true;
}

void NormalizeCapturedAlpha(HWND window, WindowVisualMetadata* metadata,
                            std::vector<std::uint8_t>* pixels) {
  if (metadata == nullptr || pixels == nullptr) return;

  bool has_nonzero_alpha = false;
  if (metadata->has_per_pixel_alpha) {
    for (std::size_t i = 3; i < pixels->size(); i += 4) {
      if ((*pixels)[i] != 0) {
        has_nonzero_alpha = true;
        break;
      }
    }
    if (!has_nonzero_alpha) metadata->has_per_pixel_alpha = false;
  }
  if (!metadata->has_per_pixel_alpha) {
    for (std::size_t i = 3; i < pixels->size(); i += 4) (*pixels)[i] = 0xff;
  }

  if (!metadata->is_layered) return;
  COLORREF color_key = 0;
  BYTE global_alpha = 255;
  DWORD flags = 0;
  if (!GetLayeredWindowAttributes(window, &color_key, &global_alpha, &flags)) return;
  const BYTE key_red = GetRValue(color_key);
  const BYTE key_green = GetGValue(color_key);
  const BYTE key_blue = GetBValue(color_key);
  for (std::size_t i = 0; i + 3 < pixels->size(); i += 4) {
    if ((flags & LWA_COLORKEY) != 0 && (*pixels)[i] == key_blue && (*pixels)[i + 1] == key_green &&
        (*pixels)[i + 2] == key_red) {
      (*pixels)[i + 3] = 0;
    }
    if ((flags & LWA_ALPHA) != 0) {
      (*pixels)[i + 3] = static_cast<std::uint8_t>(
          (static_cast<unsigned int>((*pixels)[i + 3]) * global_alpha + 127U) / 255U);
    }
  }
}

}  // namespace

DesktopCapture::DesktopCapture(D3dDevice* d3d_device)
    : d3d_device_(d3d_device), duplication_session_(d3d_device) {}

bool DesktopCapture::CaptureRegion(HWND window, const RECT& screen_rect,
                                   CapturedTexture* captured_texture) {
  if (captured_texture == nullptr || capture_geometry::Width(screen_rect) <= 0 ||
      capture_geometry::Height(screen_rect) <= 0) {
    return false;
  }
  OutputCapture* output = duplication_session_.AcquireFrameForRect(screen_rect, 120);
  if (output == nullptr) {
    std::wcerr << L"No DXGI output contains the target window.\n";
    return false;
  }

  if (output->latest_frame == nullptr) {
    std::wcerr << L"No cached desktop frame is available for the minimize "
                  L"animation yet.\n";
    return false;
  }

  if (!CopyRegionFromFrame(output, screen_rect, captured_texture)) return false;
  const RECT captured_rect =
      capture_geometry::ClampToOutput(screen_rect, output->desktop_coordinates);
  return AttachWindowVisuals(window, captured_rect, QueryWindowVisualMetadata(window),
                             captured_texture);
}

bool DesktopCapture::CaptureWindow(HWND window, const RECT& requested_screen_rect,
                                   CapturedTexture* captured_texture, RECT* captured_screen_rect) {
  if (window == nullptr || !IsWindow(window) || captured_texture == nullptr ||
      captured_screen_rect == nullptr) {
    return false;
  }

  WindowVisualMetadata visual_metadata = QueryWindowVisualMetadata(window);
  RECT window_rect{};
  if (!GetWindowRect(window, &window_rect)) {
    return false;
  }

  RECT capture_rect{};
  if (!IntersectRect(&capture_rect, &window_rect, &requested_screen_rect) ||
      capture_geometry::Width(capture_rect) <= 0 || capture_geometry::Height(capture_rect) <= 0) {
    capture_rect = window_rect;
  }

  const int window_width = capture_geometry::Width(window_rect);
  const int window_height = capture_geometry::Height(window_rect);
  const int capture_width = capture_geometry::Width(capture_rect);
  const int capture_height = capture_geometry::Height(capture_rect);
  if (window_width <= 0 || window_height <= 0 || capture_width <= 0 || capture_height <= 0) {
    return false;
  }

  auto screen_dc = wil::GetDC(nullptr);
  if (!screen_dc) {
    return false;
  }
  wil::unique_hdc memory_dc(CreateCompatibleDC(screen_dc.get()));
  if (!memory_dc) {
    return false;
  }

  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = window_width;
  bitmap_info.bmiHeader.biHeight = -window_height;
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;

  void* bitmap_bits = nullptr;
  wil::unique_hbitmap bitmap(
      CreateDIBSection(screen_dc.get(), &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0));
  screen_dc.reset();
  if (!bitmap || bitmap_bits == nullptr) {
    return false;
  }

  HGDIOBJ old_bitmap = SelectObject(memory_dc.get(), bitmap.get());
  if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
    return false;
  }
  auto restore_bitmap =
      wil::scope_exit([&] { SelectObject(memory_dc.get(), old_bitmap); });
  RECT paint_rect{0, 0, window_width, window_height};
  HBRUSH black_brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  FillRect(memory_dc.get(), &paint_rect, black_brush);

  constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
  BOOL printed = PrintWindow(window, memory_dc.get(), kPrintWindowRenderFullContent);
  if (printed == FALSE) {
    printed = PrintWindow(window, memory_dc.get(), 0);
  }
  GdiFlush();

  std::vector<std::uint8_t> pixels(static_cast<size_t>(capture_width) *
                                   static_cast<size_t>(capture_height) * 4);
  if (printed != FALSE) {
    const auto* source_pixels = static_cast<const std::uint8_t*>(bitmap_bits);
    const int source_x = capture_rect.left - window_rect.left;
    const int source_y = capture_rect.top - window_rect.top;
    const size_t source_stride = static_cast<size_t>(window_width) * 4;
    const size_t dest_stride = static_cast<size_t>(capture_width) * 4;
    for (int row = 0; row < capture_height; ++row) {
      const auto* source_row = source_pixels + static_cast<size_t>(source_y + row) * source_stride +
                               static_cast<size_t>(source_x) * 4;
      auto* dest_row = pixels.data() + static_cast<size_t>(row) * dest_stride;
      std::memcpy(dest_row, source_row, dest_stride);
    }
    NormalizeCapturedAlpha(window, &visual_metadata, &pixels);
  }

  if (printed == FALSE) {
    minimize::core::LogTrace(L"DesktopCapture",
                             L"CaptureWindow PrintWindow failed hwnd=0x" +
                                 std::to_wstring(reinterpret_cast<std::uintptr_t>(window)) +
                                 L" error=" + std::to_wstring(GetLastError()));
    return false;
  }

  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = static_cast<UINT>(capture_width);
  texture_desc.Height = static_cast<UINT>(capture_height);
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = pixels.data();
  initial_data.SysMemPitch = static_cast<UINT>(capture_width * 4);

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = d3d_device_->device()->CreateTexture2D(&texture_desc, &initial_data, &texture);
  if (FAILED(hr)) {
    if (IsDeviceLostError(hr)) {
      MarkDeviceLost(L"CreateTexture2D PrintWindow capture", hr);
      return false;
    }
    minimize::core::LogTrace(L"DesktopCapture",
                             L"CaptureWindow CreateTexture2D failed hr=0x" +
                                 std::to_wstring(static_cast<unsigned long>(hr)));
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
  srv_desc.Format = texture_desc.Format;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Texture2D.MipLevels = 1;

  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_view;
  hr = d3d_device_->device()->CreateShaderResourceView(texture.Get(), &srv_desc,
                                                       &shader_resource_view);
  if (FAILED(hr)) {
    if (IsDeviceLostError(hr)) {
      MarkDeviceLost(L"CreateShaderResourceView PrintWindow capture", hr);
      return false;
    }
    minimize::core::LogTrace(L"DesktopCapture",
                             L"CaptureWindow CreateShaderResourceView failed "
                             L"hr=0x" +
                                 std::to_wstring(static_cast<unsigned long>(hr)));
    return false;
  }

  captured_texture->texture = texture;
  captured_texture->shader_resource_view = shader_resource_view;
  captured_texture->visual_metadata.texture_rotation = TextureRotation::kIdentity;
  captured_texture->size = minimize::animation::SizeF{
      .width = static_cast<float>(capture_width),
      .height = static_cast<float>(capture_height),
  };
  if (!AttachWindowVisuals(window, capture_rect, std::move(visual_metadata), captured_texture,
                           &pixels)) {
    return false;
  }
  *captured_screen_rect = capture_rect;
  minimize::core::LogTrace(
      L"DesktopCapture",
      L"CaptureWindow succeeded hwnd=0x" +
          std::to_wstring(reinterpret_cast<std::uintptr_t>(window)) + L" size=" +
          std::to_wstring(capture_width) + L"x" + std::to_wstring(capture_height) +
          L" capture_rect=(" + std::to_wstring(capture_rect.left) + L"," +
          std::to_wstring(capture_rect.top) + L"," + std::to_wstring(capture_rect.right) + L"," +
          std::to_wstring(capture_rect.bottom) + L") window_rect=(" +
          std::to_wstring(window_rect.left) + L"," + std::to_wstring(window_rect.top) + L"," +
          std::to_wstring(window_rect.right) + L"," + std::to_wstring(window_rect.bottom) + L")");
  return true;
}

bool DesktopCapture::AttachWindowVisuals(HWND window, const RECT& capture_rect,
                                         WindowVisualMetadata metadata,
                                         CapturedTexture* captured_texture,
                                         const std::vector<std::uint8_t>* captured_pixels) {
  if (window == nullptr || captured_texture == nullptr || captured_texture->texture == nullptr) {
    return false;
  }
  RECT window_rect{};
  if (!GetWindowRect(window, &window_rect)) return false;
  RECT extended_bounds{};
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended_bounds,
                                   sizeof(extended_bounds)))) {
    extended_bounds = window_rect;
  }

  const int width = static_cast<int>(captured_texture->size.width);
  const int height = static_cast<int>(captured_texture->size.height);
  std::vector<std::uint8_t> mask = window_capture_mask::Build(metadata, width, height, window_rect,
                                                              capture_rect, extended_bounds);
  if (mask.empty()) return false;
  if (metadata.has_per_pixel_alpha && captured_pixels != nullptr &&
      captured_pixels->size() == mask.size() * 4) {
    for (std::size_t i = 0; i < mask.size(); ++i) {
      mask[i] = static_cast<std::uint8_t>(
          (static_cast<unsigned int>(mask[i]) * (*captured_pixels)[i * 4 + 3] + 127U) / 255U);
    }
  } else if (metadata.has_per_pixel_alpha) {
    // Desktop duplication contains the already-composited desktop rather than the source alpha.
    metadata.has_per_pixel_alpha = false;
  }

  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = static_cast<UINT>(width);
  texture_desc.Height = static_cast<UINT>(height);
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = mask.data();
  initial_data.SysMemPitch = static_cast<UINT>(width);

  Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_texture;
  HRESULT hr = d3d_device_->device()->CreateTexture2D(&texture_desc, &initial_data, &mask_texture);
  if (FAILED(hr)) {
    if (IsDeviceLostError(hr)) MarkDeviceLost(L"CreateTexture2D window mask", hr);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_view;
  hr = d3d_device_->device()->CreateShaderResourceView(mask_texture.Get(), nullptr, &mask_view);
  if (FAILED(hr)) {
    if (IsDeviceLostError(hr)) MarkDeviceLost(L"CreateShaderResourceView window mask", hr);
    return false;
  }
  captured_texture->mask_texture = std::move(mask_texture);
  captured_texture->mask_shader_resource_view = std::move(mask_view);
  metadata.texture_rotation = captured_texture->visual_metadata.texture_rotation;
  captured_texture->visual_metadata = std::move(metadata);
  return true;
}

bool DesktopCapture::RefreshCapturedTexture(const RECT& screen_rect,
                                            CapturedTexture* captured_texture) {
  if (captured_texture == nullptr || captured_texture->texture == nullptr ||
      capture_geometry::Width(screen_rect) <= 0 || capture_geometry::Height(screen_rect) <= 0) {
    return false;
  }
  OutputCapture* output = duplication_session_.AcquireFrameForRect(screen_rect, 0);
  if (output == nullptr) {
    return false;
  }

  if (output->latest_frame == nullptr) {
    return false;
  }

  return CopyRegionIntoTexture(output, screen_rect, captured_texture);
}

bool DesktopCapture::CopyRegionFromFrame(OutputCapture* output, const RECT& screen_rect,
                                         CapturedTexture* captured_texture) {
  if (output == nullptr || output->latest_frame == nullptr || captured_texture == nullptr) {
    return false;
  }

  const RECT clipped_rect =
      capture_geometry::ClampToOutput(screen_rect, output->desktop_coordinates);
  const int width = capture_geometry::Width(clipped_rect);
  const int height = capture_geometry::Height(clipped_rect);
  if (width <= 0 || height <= 0) {
    return false;
  }

  ID3D11Texture2D* source_frame = output->latest_frame.Get();

  DXGI_OUTDUPL_DESC dupl_desc{};
  output->duplication->GetDesc(&dupl_desc);
  const DXGI_MODE_ROTATION rotation = dupl_desc.Rotation;

  if (rotation != DXGI_MODE_ROTATION_IDENTITY &&
      rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    D3D11_TEXTURE2D_DESC frame_desc{};
    source_frame->GetDesc(&frame_desc);
    GpuCopyRegion copy_region;
    if (!ResolveGpuCopyRegion(clipped_rect, output->desktop_coordinates, frame_desc, rotation,
                              &copy_region)) {
      return false;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = copy_region.texture_width;
    texture_desc.Height = copy_region.texture_height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = output->latest_frame_format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> copied_texture;
    HRESULT hr = d3d_device_->device()->CreateTexture2D(&texture_desc, nullptr, &copied_texture);
    if (FAILED(hr)) {
      if (IsDeviceLostError(hr)) MarkDeviceLost(L"CreateTexture2D rotated capture", hr);
      return false;
    }
    d3d_device_->context()->CopySubresourceRegion(copied_texture.Get(), 0, 0, 0, 0, source_frame, 0,
                                                  &copy_region.source_box);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_view;
    hr = d3d_device_->device()->CreateShaderResourceView(copied_texture.Get(), nullptr,
                                                         &shader_resource_view);
    if (FAILED(hr)) {
      if (IsDeviceLostError(hr)) MarkDeviceLost(L"CreateShaderResourceView rotated capture", hr);
      return false;
    }
    captured_texture->texture = std::move(copied_texture);
    captured_texture->shader_resource_view = std::move(shader_resource_view);
    captured_texture->size = {
        .width = static_cast<float>(width),
        .height = static_cast<float>(height),
    };
    captured_texture->visual_metadata.texture_rotation = copy_region.texture_rotation;
    return true;
  }

  if (rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = static_cast<UINT>(width);
    texture_desc.Height = static_cast<UINT>(height);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = output->latest_frame_format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> copied_texture;
    HRESULT hr = d3d_device_->device()->CreateTexture2D(&texture_desc, nullptr, &copied_texture);
    if (FAILED(hr)) {
      if (IsDeviceLostError(hr)) {
        MarkDeviceLost(L"CreateTexture2D captured window", hr);
      }
      return false;
    }

    D3D11_BOX source_box{};
    source_box.left = static_cast<UINT>(clipped_rect.left - output->desktop_coordinates.left);
    source_box.top = static_cast<UINT>(clipped_rect.top - output->desktop_coordinates.top);
    source_box.front = 0;
    source_box.right = static_cast<UINT>(clipped_rect.right - output->desktop_coordinates.left);
    source_box.bottom = static_cast<UINT>(clipped_rect.bottom - output->desktop_coordinates.top);
    source_box.back = 1;

    d3d_device_->context()->CopySubresourceRegion(copied_texture.Get(), 0, 0, 0, 0, source_frame, 0,
                                                  &source_box);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = texture_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shader_resource_view;
    hr = d3d_device_->device()->CreateShaderResourceView(copied_texture.Get(), &srv_desc,
                                                         &shader_resource_view);
    if (FAILED(hr)) {
      if (IsDeviceLostError(hr)) {
        MarkDeviceLost(L"CreateShaderResourceView captured window", hr);
      }
      return false;
    }

    captured_texture->texture = copied_texture;
    captured_texture->shader_resource_view = shader_resource_view;
    captured_texture->visual_metadata.texture_rotation = TextureRotation::kIdentity;
    captured_texture->size = minimize::animation::SizeF{
        .width = static_cast<float>(width),
        .height = static_cast<float>(height),
    };
    return true;
  }
  return false;
}

bool DesktopCapture::CopyRegionIntoTexture(OutputCapture* output, const RECT& screen_rect,
                                           CapturedTexture* captured_texture) {
  if (output == nullptr || output->latest_frame == nullptr || captured_texture == nullptr ||
      captured_texture->texture == nullptr) {
    return false;
  }

  const RECT clipped_rect =
      capture_geometry::ClampToOutput(screen_rect, output->desktop_coordinates);
  const int width = capture_geometry::Width(clipped_rect);
  const int height = capture_geometry::Height(clipped_rect);
  if (width <= 0 || height <= 0) {
    return false;
  }

  D3D11_TEXTURE2D_DESC texture_desc{};
  captured_texture->texture->GetDesc(&texture_desc);

  ID3D11Texture2D* source_frame = output->latest_frame.Get();

  DXGI_OUTDUPL_DESC dupl_desc{};
  output->duplication->GetDesc(&dupl_desc);
  const DXGI_MODE_ROTATION rotation = dupl_desc.Rotation;

  if (rotation != DXGI_MODE_ROTATION_IDENTITY &&
      rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    D3D11_TEXTURE2D_DESC frame_desc{};
    source_frame->GetDesc(&frame_desc);
    GpuCopyRegion copy_region;
    if (!ResolveGpuCopyRegion(clipped_rect, output->desktop_coordinates, frame_desc, rotation,
                              &copy_region) ||
        texture_desc.Width != copy_region.texture_width ||
        texture_desc.Height != copy_region.texture_height) {
      return false;
    }
    d3d_device_->context()->CopySubresourceRegion(captured_texture->texture.Get(), 0, 0, 0, 0,
                                                  source_frame, 0, &copy_region.source_box);
    captured_texture->visual_metadata.texture_rotation = copy_region.texture_rotation;
    return true;
  }

  if (rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
    if (texture_desc.Width != static_cast<UINT>(width) ||
        texture_desc.Height != static_cast<UINT>(height)) {
      return false;
    }
    D3D11_BOX source_box{};
    source_box.left = static_cast<UINT>(clipped_rect.left - output->desktop_coordinates.left);
    source_box.top = static_cast<UINT>(clipped_rect.top - output->desktop_coordinates.top);
    source_box.front = 0;
    source_box.right = static_cast<UINT>(clipped_rect.right - output->desktop_coordinates.left);
    source_box.bottom = static_cast<UINT>(clipped_rect.bottom - output->desktop_coordinates.top);
    source_box.back = 1;

    d3d_device_->context()->CopySubresourceRegion(captured_texture->texture.Get(), 0, 0, 0, 0,
                                                  source_frame, 0, &source_box);
    captured_texture->visual_metadata.texture_rotation = TextureRotation::kIdentity;
    return true;
  }
  return false;
}

void DesktopCapture::MarkDeviceLost(const wchar_t* context, HRESULT hr) {
  device_lost_.store(true, std::memory_order_release);
  HRESULT reason = S_OK;
  if (d3d_device_ != nullptr && d3d_device_->device() != nullptr) {
    reason = d3d_device_->device()->GetDeviceRemovedReason();
  }
  std::wcerr << L"D3D device lost during " << context << L": hr=0x" << std::hex << hr
             << L", reason=0x" << reason << std::dec << L". Reinitializing renderer.\n";
}

}  // namespace minimize::rendering
