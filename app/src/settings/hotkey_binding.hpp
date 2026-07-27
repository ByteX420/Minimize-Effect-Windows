#pragma once

#include <cstddef>
#include <cstdint>

namespace minimize::settings {

enum class HotkeyAction : std::size_t {
  kToggleEffect,
  kOpenSettings,
  kRepairWindows,
  kMinimizeAllWindows,
  kRestoreAllWindows,
  kCount,
};

struct HotkeyBinding {
  std::uint32_t modifiers = 0;
  std::uint32_t virtual_key = 0;

  bool operator==(const HotkeyBinding&) const = default;
};

}  // namespace minimize::settings
