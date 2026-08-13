#pragma once

#include <optional>

#include "settings/settings_repository.hpp"

namespace minimize::settings {

class SettingsService final {
public:
  explicit SettingsService(SettingsRepository repository = {});

  [[nodiscard]] bool Load();
  [[nodiscard]] const AppSettings& Get() const { return settings_; }
  [[nodiscard]] bool Update(AppSettings proposed, bool record_undo = true,
                            bool create_backup = true);
  void Preview(AppSettings proposed);
  [[nodiscard]] bool CanUndo() const { return undo_settings_.has_value(); }
  [[nodiscard]] bool Undo();

private:
  SettingsRepository repository_;
  AppSettings settings_;
  AppSettings persisted_settings_;
  std::optional<AppSettings> undo_settings_;
};

}  // namespace minimize::settings
