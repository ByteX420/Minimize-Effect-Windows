#include "pch.hpp"

#include "runtime/animation_run_pool.hpp"

namespace minimize::runtime {

AnimationRun& AnimationRunPool::Add() { return runs_.emplace_back(); }

void AnimationRunPool::RemoveLast() {
  if (!runs_.empty()) runs_.pop_back();
}

void AnimationRunPool::Clear() { runs_.clear(); }

std::vector<HWND> AnimationRunPool::DetachAnimatingWindows() {
  std::vector<HWND> windows;
  windows.reserve(runs_.size());
  for (AnimationRun& run : runs_) {
    windows.push_back(run.animating_window);
    run.animating_window = nullptr;
    run.pending_native_minimize_window = nullptr;
    run.animating_restore = false;
    run.bulk_animation = false;
  }
  return windows;
}

void AnimationRunPool::ShutdownOverlays() {
  for (AnimationRun& run : runs_) run.overlay.Shutdown();
}

}  // namespace minimize::runtime
