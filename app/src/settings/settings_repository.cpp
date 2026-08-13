#include "pch.hpp"

#include "settings/settings_repository.hpp"

#include <filesystem>
#include <fstream>

#include "core/logger.hpp"
#include "settings/settings_serializer.hpp"
#include "settings/settings_validator.hpp"

namespace minimize::settings {

std::wstring SettingsRepository::Path() {
  DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return {};
  std::wstring local_app_data(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required);
  if (written == 0 || written >= required) return {};
  local_app_data.resize(written);
  return local_app_data + L"\\MinimizeEffect\\settings.json";
}

AppSettings SettingsRepository::Load() const {
  const std::wstring path = Path();
  if (path.empty()) return {};
  const std::filesystem::path settings_path(path);
  std::error_code ec;
  const auto file_size = std::filesystem::file_size(settings_path, ec);
  if (ec || file_size > 1024 * 1024) {
    if (!ec && file_size > 1024 * 1024) {
      core::LogDebug(L"Settings", L"Ignoring settings file larger than 1 MiB");
    }
    return {};
  }
  std::ifstream input(settings_path, std::ios::binary);
  if (!input) return {};
  std::string json(static_cast<std::size_t>(file_size), '\0');
  if (file_size != 0) {
    input.read(json.data(), static_cast<std::streamsize>(file_size));
    if (input.gcount() != static_cast<std::streamsize>(file_size)) {
      core::LogDebug(L"Settings", L"Ignoring settings file that changed while being read");
      return {};
    }
  }
  auto deserialized = SettingsSerializer::Deserialize(json);
  if (!deserialized.has_value()) {
    core::LogDebug(L"Settings", L"Ignoring malformed settings file");
    return {};
  }
  return SettingsValidator::Normalize(std::move(*deserialized));
}

bool SettingsRepository::Save(const AppSettings& settings, bool create_backup) const {
  const std::wstring path = Path();
  if (path.empty()) return false;
  const std::filesystem::path settings_path(path);
  std::error_code error;
  std::filesystem::create_directories(settings_path.parent_path(), error);
  if (error) return false;

  const std::filesystem::path temporary_path = settings_path.wstring() + L".tmp";
  {
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << SettingsSerializer::Serialize(SettingsValidator::Normalize(settings));
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(temporary_path, error);
      return false;
    }
  }
  if (create_backup && std::filesystem::exists(settings_path, error) && !error) {
    const std::filesystem::path backup_path = settings_path.wstring() + L".bak";
    std::filesystem::copy_file(settings_path, backup_path,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      std::filesystem::remove(temporary_path, error);
      return false;
    }
  }
  if (!MoveFileExW(temporary_path.c_str(), settings_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary_path, error);
    return false;
  }
  return true;
}

}  // namespace minimize::settings
