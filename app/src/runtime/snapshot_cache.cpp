#include "pch.hpp"

#include "runtime/snapshot_cache.hpp"

#include <algorithm>
#include <utility>

#include "platform/windows/process_info.hpp"

namespace minimize::runtime {

const CachedSnapshot* SnapshotCache::FindBest(HWND window) const {
  const auto restore = restore_.find(window);
  if (restore != restore_.end()) return &restore->second;
  const auto pre_minimize = pre_minimize_.find(window);
  return pre_minimize == pre_minimize_.end() ? nullptr : &pre_minimize->second;
}

void SnapshotCache::Prune() {
  const auto still_matches_window = [](HWND window, const CachedSnapshot& snapshot) {
    return IsWindow(window) && snapshot.process_id != 0 &&
           platform::WindowProcessId(window) == snapshot.process_id;
  };
  for (Map* snapshots : {&restore_, &pre_minimize_}) {
    for (auto iterator = snapshots->begin(); iterator != snapshots->end();) {
      if (!still_matches_window(iterator->first, iterator->second)) {
        iterator = snapshots->erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  while (pre_minimize_.size() > kMaximumPreMinimizeSnapshots) {
    auto oldest = std::min_element(
        pre_minimize_.begin(), pre_minimize_.end(), [](const auto& left, const auto& right) {
          return left.second.captured_at_ms < right.second.captured_at_ms;
        });
    if (oldest == pre_minimize_.end()) break;
    pre_minimize_.erase(oldest);
  }
  while (restore_.size() > 16) {
    auto oldest = std::min_element(
        restore_.begin(), restore_.end(), [](const auto& left, const auto& right) {
          return left.second.captured_at_ms < right.second.captured_at_ms;
        });
    if (oldest == restore_.end()) break;
    restore_.erase(oldest);
  }
}

void SnapshotCache::Clear() {
  pre_minimize_.clear();
  restore_.clear();
}

SnapshotCache::Contents SnapshotCache::TakeAll() {
  Contents contents{
      .pre_minimize = std::move(pre_minimize_),
      .restore = std::move(restore_),
  };
  pre_minimize_.clear();
  restore_.clear();
  return contents;
}

}  // namespace minimize::runtime
