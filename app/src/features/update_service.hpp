#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <windows.h>
#include <wil/resource.h>

namespace minimize::features {

enum class UpdatePhase {
  kIdle,
  kChecking,
  kUpToDate,
  kAvailable,
  kDownloading,
  kVerifying,
  kStaging,
  kReadyToInstall,
  kInstalling,
  kError,
};

struct UpdateSnapshot {
  UpdatePhase phase = UpdatePhase::kIdle;
  std::string current_version;
  std::string latest_version;
  std::string release_notes;
  std::string release_page_url;
  std::uint64_t downloaded_bytes = 0;
  std::uint64_t total_bytes = 0;
  float progress = 0.0f;
  std::string status;
  std::string error;
  bool user_initiated = false;
};

class UpdateService final {
public:
  static constexpr UINT kStateChangedMessage = WM_APP + 102;

  UpdateService() = default;
  ~UpdateService();

  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  void Start(HWND notification_window);
  void Stop();
  void CheckForUpdates(bool user_initiated = false);
  void DownloadUpdate();
  void CancelDownload();
  [[nodiscard]] bool LaunchInstaller(const RECT& window_bounds, int selected_page,
                                     float page_scroll, bool maximized);
  [[nodiscard]] bool InstallerHandoverReady();
  [[nodiscard]] bool InstallerHandoverFailed();
  [[nodiscard]] UpdateSnapshot GetSnapshot() const;

private:
  enum class PendingAction {
    kNone,
    kCheck,
    kDownload,
  };

  void WorkerLoop(std::stop_token stop_token);
  void CheckWorker(std::stop_token stop_token, bool user_initiated);
  void DownloadWorker(std::stop_token stop_token);
  void SetSnapshot(UpdateSnapshot snapshot);
  void MutateSnapshot(const std::function<void(UpdateSnapshot&)>& mutation);
  void NotifyStateChanged() const;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::jthread worker_;
  HWND notification_window_ = nullptr;
  PendingAction pending_action_ = PendingAction::kNone;
  bool pending_check_user_initiated_ = false;
  bool cancel_requested_ = false;
  wil::unique_handle installer_ready_event_;
  wil::unique_handle installer_process_;
  ULONGLONG installer_started_at_ms_ = 0;
  UpdateSnapshot snapshot_;
  std::string package_url_;
  std::string checksum_url_;
  std::string expected_checksum_;
  std::filesystem::path staged_directory_;
};

}  // namespace minimize::features
