#pragma once

#include <cstdint>
#include <vector>
#include <windows.h>

namespace minimize::rendering {

enum class TextureRotation : std::uint32_t {
  kIdentity = 0,
  kRotate90 = 1,
  kRotate180 = 2,
  kRotate270 = 3,
};

struct Region {
  std::vector<RECT> rectangles;
  bool is_set = false;

  [[nodiscard]] bool empty() const { return rectangles.empty(); }
};

struct WindowVisualMetadata {
  Region window_region;
  float corner_radius = 0.0f;
  float shadow_radius = 0.0f;
  float shadow_opacity = 0.0f;
  bool is_layered = false;
  bool has_per_pixel_alpha = false;
  TextureRotation texture_rotation = TextureRotation::kIdentity;
};

[[nodiscard]] WindowVisualMetadata QueryWindowVisualMetadata(HWND window);

}  // namespace minimize::rendering
