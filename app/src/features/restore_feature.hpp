#pragma once

#include <functional>
#include <optional>
#include <unordered_set>
#include <windows.h>

#include "runtime/run_state.hpp"

namespace minimize::features {

class EffectPolicy;
class AnimationConfiguration;
class MinimizeFeature;
class WindowExclusionService;
class WindowRecoveryService;
struct RenderingPressure;
}  // namespace minimize::features
namespace minimize::platform {
class NativeAnimationBlocker;
}
namespace minimize::runtime {
class AnimationRunPool;
struct CachedSnapshot;
class SnapshotCache;
}  // namespace minimize::runtime
namespace minimize::features {

struct RestoreRequest {
  HWND window = nullptr;
  bool shutting_down = false;
  bool renderer_available = false;
};

struct RestoreExecutionContext {
  HWND overlay = nullptr;
  bool effect_active = false;
  bool defer_first_frame_wait = false;
  bool renderer_recovering = false;
  bool shutting_down = false;
  platform::NativeAnimationBlocker* animation_blocker = nullptr;
  AnimationConfiguration* animation_configuration = nullptr;
  const RenderingPressure* rendering_pressure = nullptr;
  std::function<int(HWND)> find_run;
  std::function<int()> find_available_run;
  std::function<void(int, runtime::RunState)> set_state;
  std::function<void(int, HWND, const RECT&)> reset_frame_pacing;
  std::function<void(int)> finish_run;
  std::function<void(int)> abort_run;
};

class RestoreFeature final {
public:
  class Transaction final {
  public:
    Transaction() = default;
    Transaction(RestoreFeature& owner, HWND window);
    ~Transaction();
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void HandOff();

  private:
    RestoreFeature* owner_ = nullptr;
    HWND window_ = nullptr;
  };

  RestoreFeature(EffectPolicy& policy, WindowRecoveryService& recovery,
                 runtime::SnapshotCache& snapshots, runtime::AnimationRunPool& runs,
                 MinimizeFeature& minimize, WindowExclusionService& window_exclusions);

  [[nodiscard]] bool Execute(HWND window, const RestoreExecutionContext& context);
  [[nodiscard]] std::optional<Transaction> Begin(const RestoreRequest& request);
  void Complete(HWND window);
  void Cancel(HWND window, bool force_show_if_iconic = false, bool activate = true);
  void CancelAll(bool force_show_if_iconic = true);
  void ReleaseAll();
  [[nodiscard]] bool PreservePlacementAndMarkOffscreen(HWND window,
                                                       runtime::CachedSnapshot* snapshot) const;
  [[nodiscard]] bool IsWindowRestored(HWND window) const;

private:
  friend class Transaction;
  void HandOff(HWND window);

  EffectPolicy& policy_;
  WindowRecoveryService& recovery_;
  runtime::SnapshotCache& snapshots_;
  runtime::AnimationRunPool& runs_;
  MinimizeFeature& minimize_;
  WindowExclusionService& window_exclusions_;
  std::unordered_set<HWND> active_;
};

}  // namespace minimize::features
