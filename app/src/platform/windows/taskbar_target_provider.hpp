#pragma once

#include <cstddef>
#include <windows.h>

#include "animation/minimize_mesh.hpp"
#include "animation/geometry.hpp"

namespace minimize::platform {

struct TaskbarTarget {
  minimize::animation::RectF rect;
  minimize::animation::MinimizeEdge edge = minimize::animation::MinimizeEdge::kBottom;
};

class TaskbarTargetProvider {
public:
  ~TaskbarTargetProvider();

  [[nodiscard]] bool RevealAutoHideTaskbarForWindow(const RECT& window_rect);
  void ReleaseAutoHideTaskbar();
  void UpdateAutoHideTaskbarRestore();
  void RestoreAutoHideTaskbar();

  [[nodiscard]] TaskbarTarget GetTargetForWindow(HWND window, const RECT& window_rect) const;

private:
  [[nodiscard]] RECT GetShellTaskbarRect() const;

  std::size_t auto_hide_reveal_count_ = 0;
  ULONGLONG auto_hide_restore_deadline_ms_ = 0;
  UINT_PTR original_taskbar_state_ = 0;
  bool auto_hide_temporarily_disabled_ = false;
};

}  // namespace minimize::platform
