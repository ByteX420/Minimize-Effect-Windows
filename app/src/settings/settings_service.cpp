#include "pch.hpp"

#include "settings/settings_service.hpp"

#include <utility>

#include "settings/settings_validator.hpp"

namespace minimize::settings {

SettingsService::SettingsService(SettingsRepository repository)
    : repository_(std::move(repository)) {}

bool SettingsService::Load() {
  settings_ = SettingsValidator::Normalize(repository_.Load());
  persisted_settings_ = settings_;
  undo_settings_.reset();
  return true;
}

bool SettingsService::Update(AppSettings proposed, bool record_undo, bool create_backup) {
  proposed = SettingsValidator::Normalize(std::move(proposed));
  if (proposed == persisted_settings_) {
    settings_ = std::move(proposed);
    return true;
  }
  if (!repository_.Save(proposed, create_backup)) return false;
  if (record_undo) undo_settings_ = persisted_settings_;
  persisted_settings_ = proposed;
  settings_ = std::move(proposed);
  return true;
}

void SettingsService::Preview(AppSettings proposed) {
  settings_ = SettingsValidator::Normalize(std::move(proposed));
}

bool SettingsService::Undo() {
  if (!undo_settings_.has_value()) return false;
  AppSettings replacement = *undo_settings_;
  if (!repository_.Save(replacement, false)) return false;
  undo_settings_ = persisted_settings_;
  persisted_settings_ = replacement;
  settings_ = std::move(replacement);
  return true;
}

}  // namespace minimize::settings
