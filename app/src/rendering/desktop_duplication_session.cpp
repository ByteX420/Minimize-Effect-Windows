#include "pch.hpp"

#include "rendering/desktop_duplication_session.hpp"

#include <iostream>

#include "rendering/d3d_device.hpp"

namespace minimize::rendering {
namespace {

int RectWidth(const RECT& rect) { return static_cast<int>(rect.right - rect.left); }
int RectHeight(const RECT& rect) { return static_cast<int>(rect.bottom - rect.top); }

bool ContainsRect(const RECT& outer, const RECT& inner) {
  return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
         inner.bottom <= outer.bottom;
}

}  // namespace

DesktopDuplicationSession::DesktopDuplicationSession(D3dDevice* d3d_device)
    : d3d_device_(d3d_device) {}

DesktopDuplicationSession::~DesktopDuplicationSession() { Reset(); }

DesktopDuplicationSession::OutputCapture* DesktopDuplicationSession::AcquireFrameForRect(
    const RECT& screen_rect, UINT first_frame_timeout_ms, bool* frame_updated) {
  if (frame_updated != nullptr) *frame_updated = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (outputs_.empty() && !InitializeOutputs()) return nullptr;
    OutputCapture* output = FindOutputForRect(screen_rect);
    if (output == nullptr) return nullptr;
    const UINT timeout_ms = output->latest_frame == nullptr ? first_frame_timeout_ms : 0;
    const AcquireResult result = TryAcquireLatestFrame(output, timeout_ms);
    if (result == AcquireResult::kAccessLost) {
      Reset();
      continue;
    }
    if (result == AcquireResult::kDeviceLost || result == AcquireResult::kFailed) return nullptr;
    if (result == AcquireResult::kAcquired && frame_updated != nullptr) {
      *frame_updated = true;
    }
    return output;
  }
  return nullptr;
}

DesktopDuplicationSession::AcquireResult DesktopDuplicationSession::TryAcquireLatestFrame(
    OutputCapture* output, UINT timeout_ms) {
  if (output == nullptr || output->duplication == nullptr) return AcquireResult::kFailed;
  // AcquireNextFrame fails with DXGI_ERROR_INVALID_CALL while the previous frame is still
  // held, so the held frame is released right before acquiring the next one. Desktop updates
  // that happen while no frame is owned are accumulated and delivered with the next frame.
  ReleaseHeldFrame(output);
  DXGI_OUTDUPL_FRAME_INFO frame_info{};
  Microsoft::WRL::ComPtr<IDXGIResource> desktop_resource;
  HRESULT result =
      output->duplication->AcquireNextFrame(timeout_ms, &frame_info, &desktop_resource);
  if (result == DXGI_ERROR_WAIT_TIMEOUT) return AcquireResult::kNoNewFrame;
  if (result == DXGI_ERROR_ACCESS_LOST) return AcquireResult::kAccessLost;
  if (D3dDevice::IsDeviceLostError(result)) {
    MarkDeviceLost(L"AcquireNextFrame", result);
    return AcquireResult::kDeviceLost;
  }
  if (FAILED(result)) return AcquireResult::kFailed;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_texture;
  result = desktop_resource.As(&desktop_texture);
  if (FAILED(result)) {
    output->duplication->ReleaseFrame();
    return AcquireResult::kFailed;
  }
  D3D11_TEXTURE2D_DESC desktop_description{};
  desktop_texture->GetDesc(&desktop_description);
  output->latest_frame_format = desktop_description.Format;
  // Keep the acquired desktop frame itself instead of copying the whole monitor into a
  // cached texture; window-region copies read directly from this frame while it is held.
  output->latest_frame = std::move(desktop_texture);
  output->frame_held = true;
  return AcquireResult::kAcquired;
}

void DesktopDuplicationSession::ReleaseHeldFrame(OutputCapture* output) {
  if (output == nullptr || !output->frame_held) return;
  (void)output->duplication->ReleaseFrame();
  output->frame_held = false;
  output->latest_frame.Reset();
}

bool DesktopDuplicationSession::InitializeOutputs() {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  HRESULT result = d3d_device_->dxgi_device()->GetAdapter(&adapter);
  if (FAILED(result)) return false;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    result = adapter->EnumOutputs(index, &output);
    if (result == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(result)) continue;
    DXGI_OUTPUT_DESC description{};
    output->GetDesc(&description);
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) continue;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    result = output1->DuplicateOutput(d3d_device_->device(), &duplication);
    if (FAILED(result)) continue;
    outputs_.push_back(OutputCapture{
        .desktop_coordinates = description.DesktopCoordinates,
        .duplication = duplication,
    });
  }
  return !outputs_.empty();
}

DesktopDuplicationSession::OutputCapture* DesktopDuplicationSession::FindOutputForRect(
    const RECT& screen_rect) {
  for (OutputCapture& output : outputs_) {
    if (ContainsRect(output.desktop_coordinates, screen_rect)) return &output;
  }
  const LONG center_x = screen_rect.left + RectWidth(screen_rect) / 2;
  const LONG center_y = screen_rect.top + RectHeight(screen_rect) / 2;
  for (OutputCapture& output : outputs_) {
    const RECT& rect = output.desktop_coordinates;
    if (center_x >= rect.left && center_x < rect.right && center_y >= rect.top &&
        center_y < rect.bottom) {
      return &output;
    }
  }
  return nullptr;
}

void DesktopDuplicationSession::ClearHistory() {
  for (OutputCapture& output : outputs_) ReleaseHeldFrame(&output);
}

void DesktopDuplicationSession::Reset() {
  for (OutputCapture& output : outputs_) ReleaseHeldFrame(&output);
  outputs_.clear();
}

void DesktopDuplicationSession::MarkDeviceLost(const wchar_t* context, HRESULT result) {
  device_lost_ = true;
  const HRESULT reason = d3d_device_ != nullptr ? d3d_device_->DeviceRemovedReason() : S_OK;
  std::wcerr << L"D3D device lost during " << context << L": hr=0x" << std::hex << result
             << L", reason=0x" << reason << std::dec << L".\n";
}

}  // namespace minimize::rendering
