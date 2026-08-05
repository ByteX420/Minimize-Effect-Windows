#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace minimize::rendering {

class D3dDevice;

class DesktopDuplicationSession final {
public:
  struct OutputCapture {
    RECT desktop_coordinates{};
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latest_frame;
    DXGI_FORMAT latest_frame_format = DXGI_FORMAT_UNKNOWN;
    bool frame_held = false;
  };

  explicit DesktopDuplicationSession(D3dDevice* d3d_device);
  ~DesktopDuplicationSession();

  [[nodiscard]] OutputCapture* AcquireFrameForRect(const RECT& screen_rect,
                                                   UINT first_frame_timeout_ms);
  void ClearHistory();
  void Reset();
  [[nodiscard]] bool device_lost() const { return device_lost_; }
  void ClearDeviceLost() { device_lost_ = false; }

private:
  enum class AcquireResult {
    kAcquired,
    kNoNewFrame,
    kAccessLost,
    kDeviceLost,
    kFailed,
  };

  [[nodiscard]] bool InitializeOutputs();
  [[nodiscard]] OutputCapture* FindOutputForRect(const RECT& screen_rect);
  AcquireResult TryAcquireLatestFrame(OutputCapture* output, UINT timeout_ms);
  void ReleaseHeldFrame(OutputCapture* output);
  void MarkDeviceLost(const wchar_t* context, HRESULT result);

  D3dDevice* d3d_device_ = nullptr;
  std::vector<OutputCapture> outputs_;
  bool device_lost_ = false;
};

}  // namespace minimize::rendering
