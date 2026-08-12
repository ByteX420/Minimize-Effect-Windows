#include "pch.hpp"

#include "platform/windows/update_installer.hpp"

#include <filesystem>
#include <string_view>
#include <wil/resource.h>

namespace minimize::platform::windows {
namespace {

std::wstring QuoteArgument(std::wstring_view argument) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

bool TryReplaceFile(const std::filesystem::path& destination, const std::filesystem::path& incoming,
                    const std::filesystem::path& backup) {
  if (std::filesystem::exists(destination)) {
    return ReplaceFileW(destination.c_str(), incoming.c_str(), backup.c_str(),
                        REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
  }
  return MoveFileExW(incoming.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool ReplaceOneFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                    const std::filesystem::path& backup) {
  const std::filesystem::path incoming = destination.wstring() + L".update-new";
  std::error_code error;
  std::filesystem::remove(incoming, error);
  if (!CopyFileW(source.c_str(), incoming.c_str(), FALSE)) return false;

  for (int attempt = 0; attempt < 80; ++attempt) {
    if (TryReplaceFile(destination, incoming, backup)) return true;
    Sleep(50);
  }
  std::filesystem::remove(incoming, error);
  return false;
}

void RestoreBackup(const std::filesystem::path& destination, const std::filesystem::path& backup) {
  if (!std::filesystem::exists(backup)) return;
  std::error_code error;
  std::filesystem::remove(destination, error);
  error.clear();
  std::filesystem::rename(backup, destination, error);
}

std::optional<long> ParseLong(const wchar_t* value) {
  if (value == nullptr || *value == L'\0') return std::nullopt;
  wchar_t* end = nullptr;
  const long result = wcstol(value, &end, 10);
  if (end == value || *end != L'\0') return std::nullopt;
  return result;
}

int RunInstaller(int argument_count, wchar_t* arguments[]) {
  if (argument_count != 10 && argument_count != 13) return ERROR_INVALID_PARAMETER;
  const auto parent_id = ParseLong(arguments[2]);
  const auto left = ParseLong(arguments[5]);
  const auto top = ParseLong(arguments[6]);
  const auto right = ParseLong(arguments[7]);
  const auto bottom = ParseLong(arguments[8]);
  if (!parent_id || !left || !top || !right || !bottom || *parent_id <= 0) {
    return ERROR_INVALID_PARAMETER;
  }
  const std::filesystem::path source_directory(arguments[3]);
  const std::filesystem::path target_directory(arguments[4]);
  const std::filesystem::path source_executable = source_directory / L"MinimizeEffect.exe";
  const std::filesystem::path source_hook = source_directory / L"MinimizeEffectHook.dll";
  const std::filesystem::path target_executable = target_directory / L"MinimizeEffect.exe";
  const std::filesystem::path target_hook = target_directory / L"MinimizeEffectHook.dll";
  const std::filesystem::path backup_executable =
      target_directory / L"MinimizeEffect.exe.update-backup";
  const std::filesystem::path backup_hook =
      target_directory / L"MinimizeEffectHook.dll.update-backup";
  if (!std::filesystem::exists(source_executable) || !std::filesystem::exists(source_hook) ||
      !std::filesystem::exists(target_directory)) {
    return ERROR_FILE_NOT_FOUND;
  }

  // Compatibility path for older builds that still launch --apply-update. It is intentionally
  // headless: the parent application owns every visible update frame.
  wil::unique_handle ready_event(OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[9]));
  if (ready_event) SetEvent(ready_event.get());

  wil::unique_handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                        static_cast<DWORD>(*parent_id)));
  if (parent) WaitForSingleObject(parent.get(), 30000);

  std::error_code filesystem_error;
  std::filesystem::remove(backup_executable, filesystem_error);
  filesystem_error.clear();
  std::filesystem::remove(backup_hook, filesystem_error);
  const bool executable_replaced =
      ReplaceOneFile(source_executable, target_executable, backup_executable);
  const bool hook_replaced =
      executable_replaced && ReplaceOneFile(source_hook, target_hook, backup_hook);
  if (!executable_replaced || !hook_replaced) {
    if (executable_replaced) RestoreBackup(target_executable, backup_executable);
    RestoreBackup(target_hook, backup_hook);
    return ERROR_WRITE_FAULT;
  }

  std::wstring command_line = QuoteArgument(target_executable.wstring()) + L" --update-resume " +
                              std::to_wstring(*left) + L" " + std::to_wstring(*top) + L" " +
                              std::to_wstring(*right) + L" " + std::to_wstring(*bottom);
  if (argument_count == 13) {
    command_line += L" " + std::to_wstring(*parent_id) + L" " + QuoteArgument(arguments[9]) + L" " +
                    arguments[10] + L" " + arguments[11] + L" " + arguments[12];
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(target_executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, target_directory.c_str(), &startup, &process)) {
    RestoreBackup(target_executable, backup_executable);
    RestoreBackup(target_hook, backup_hook);
    return static_cast<int>(GetLastError());
  }
  wil::unique_handle process_handle(process.hProcess);
  wil::unique_handle thread_handle(process.hThread);
  std::filesystem::remove(backup_executable, filesystem_error);
  filesystem_error.clear();
  std::filesystem::remove(backup_hook, filesystem_error);
  return 0;
}

}  // namespace

std::optional<int> TryRunUpdateInstaller(int argument_count, wchar_t* arguments[]) {
  if (argument_count < 2 || std::wstring_view(arguments[1]) != L"--apply-update") {
    return std::nullopt;
  }
  return RunInstaller(argument_count, arguments);
}

}  // namespace minimize::platform::windows
