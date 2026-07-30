#pragma once

#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>
#include <windows.h>

#include "runtime/run_state.hpp"

namespace minimize::features {

class AnimationConfiguration;
class EffectPolicy;
class WindowExclusionService;
class WindowRecoveryService;
struct RenderingPressure;
}  // namespace minimize::features
namespace minimize::platform {
class NativeAnimationBlocker;
class TaskbarTargetProvider;
}  // namespace minimize::platform
namespace minimize::rendering {
struct CapturedTexture;
class DesktopCapture;
}
namespace minimize::runtime {
class AnimationRunPool;
class SnapshotCache;
}  // namespace minimize::runtime
namespace minimize::features {

struct MinimizeRequest {
  HWND window = nullptr;
  bool shutting_down = false;
  bool renderer_available = false;
};

struct MinimizeExecutionContext {
  HWND overlay = nullptr;
  bool effect_active = false;
  bool force_animation = false;
  bool renderer_recovering = false;
  bool shutting_down = false;
  rendering::DesktopCapture* capture = nullptr;
  platform::TaskbarTargetProvider* taskbar_targets = nullptr;
  platform::NativeAnimationBlocker* animation_blocker = nullptr;
  AnimationConfiguration* animation_configuration = nullptr;
  const RenderingPressure* rendering_pressure = nullptr;
  std::function<int(HWND)> find_run;
  std::function<int()> find_available_run;
  std::function<void(int, runtime::RunState)> set_state;
  std::function<void(int, HWND, const RECT&)> reset_frame_pacing;
  std::function<void(int)> abort_run;
  std::function<void(HWND)> complete_restore;
  std::function<void(float)> record_capture_duration;
  std::function<bool(HWND, rendering::CapturedTexture*, RECT*)> take_prepared_capture;
};

class MinimizeFeature final {
public:
  class Transaction final {
  public:
    Transaction() = default;
    Transaction(MinimizeFeature& owner, HWND window);
    ~Transaction();
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void HandOff();

  private:
    MinimizeFeature* owner_ = nullptr;
    HWND window_ = nullptr;
  };

  MinimizeFeature(EffectPolicy& policy, WindowRecoveryService& recovery,
                  runtime::AnimationRunPool& runs, runtime::SnapshotCache& snapshots,
                  WindowExclusionService& window_exclusions);

  [[nodiscard]] bool Execute(HWND window, const MinimizeExecutionContext& context);
  [[nodiscard]] std::optional<Transaction> Begin(const MinimizeRequest& request);
  void Complete(HWND window);
  void Cancel(HWND window, bool force_show_if_iconic = true, bool activate = true);
  void CancelAll(bool force_show_if_iconic = true);
  // Drop tracking without touching the window (used on shutdown after ReleaseWithoutShowing).
  void ReleaseAll();
  [[nodiscard]] bool IsAnimating(HWND window) const;
  void UpdatePreMinimizeSnapshot(HWND window, HWND overlay, rendering::DesktopCapture* capture,
                                 bool renderer_recovering);
  // Startup seed: one window at a time (restore → paint frame → capture → minimize → next).
  void BeginSeedSnapshotsForIconicWindows(HWND overlay, rendering::DesktopCapture* capture,
                                          platform::TaskbarTargetProvider* taskbar_targets,
                                          bool renderer_recovering);
  // Returns true while more seed work remains.
  [[nodiscard]] bool TickSeedSnapshotsForIconicWindows();
  void CancelSeedSnapshotsForIconicWindows();
  [[nodiscard]] bool SeedSnapshotsInProgress() const;
  void CompletePendingNativeMinimize(int run_index,
                                     bool start_animation_clock,
                                     const std::function<void(int, runtime::RunState)>& set_state,
                                     const std::function<void(int)>& abort);
  [[nodiscard]] bool CommitPreparedBulkMinimize(
      int run_index, platform::NativeAnimationBlocker* animation_blocker,
      const std::function<void(int, runtime::RunState)>& set_state,
      const std::function<void(int)>& abort);

private:
  friend class Transaction;
  void HandOff(HWND window);

  struct SeedCandidate {
    HWND window = nullptr;
    WINDOWPLACEMENT placement{};
    RECT normal{};
    bool was_maximized = false;
  };

  EffectPolicy& policy_;
  WindowRecoveryService& recovery_;
  runtime::AnimationRunPool& runs_;
  runtime::SnapshotCache& snapshots_;
  WindowExclusionService& window_exclusions_;
  std::unordered_set<HWND> active_;

  enum class SeedPhase {
    kIdle,
    kRestoreCurrent,
    kWaitPaint,
    kCaptureCurrent,
  };
  SeedPhase seed_phase_ = SeedPhase::kIdle;
  std::vector<SeedCandidate> seed_candidates_;
  std::size_t seed_index_ = 0;
  rendering::DesktopCapture* seed_capture_ = nullptr;
  platform::TaskbarTargetProvider* seed_targets_ = nullptr;
};

}  // namespace minimize::features
