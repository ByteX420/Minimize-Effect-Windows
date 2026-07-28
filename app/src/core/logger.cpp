#include "pch.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <wil/resource.h>

#include "core/environment.hpp"

namespace minimize::core {
namespace {

bool IsSynchronousLoggingEnabled() {
#ifdef _DEBUG
  static const bool enabled = EnvironmentFlagEnabled("MINIMIZE_LOG_SYNC");
  return enabled;
#else
  return false;
#endif
}

class [[nodiscard]] SRWLockGuard final {
public:
  explicit SRWLockGuard(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
  ~SRWLockGuard() noexcept { ReleaseSRWLockExclusive(&lock_); }
  SRWLockGuard(const SRWLockGuard&) = delete;
  SRWLockGuard& operator=(const SRWLockGuard&) = delete;

private:
  SRWLOCK& lock_;
};

class LoggerState final {
public:
  ~LoggerState() noexcept { Close(); }

  LoggerState(const LoggerState&) = delete;
  LoggerState& operator=(const LoggerState&) = delete;

  static LoggerState& Instance() noexcept {
    static LoggerState state;
    return state;
  }

  void Write(std::string_view entry) {
#ifdef _DEBUG
    if (entry.empty()) return;

    SRWLockGuard lock(lock_);
    if (!file_) {
      HANDLE h = CreateFileW(DebugLogPath().c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        file_.reset(h);
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file_.get(), &size) && size.QuadPart == 0) {
          constexpr unsigned char kUtf8Bom[] = {0xef, 0xbb, 0xbf};
          DWORD written = 0;
          WriteFile(file_.get(), kUtf8Bom, static_cast<DWORD>(sizeof(kUtf8Bom)), &written, nullptr);
        }
      }
    }

    if (file_) {
      DWORD written = 0;
      const DWORD requested = static_cast<DWORD>(entry.size());
      if (!WriteFile(file_.get(), entry.data(), requested, &written, nullptr) ||
          written != requested) {
        file_.reset();
      } else if (IsSynchronousLoggingEnabled()) {
        FlushFileBuffers(file_.get());
      }
    }
#else
    (void)entry;
#endif
  }

  void Close() noexcept {
#ifdef _DEBUG
    SRWLockGuard lock(lock_);
    file_.reset();
#endif
  }

private:
  LoggerState() noexcept = default;

  wil::unique_hfile file_;
  SRWLOCK lock_ = SRWLOCK_INIT;
};

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }

  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                          nullptr, nullptr) != size) {
    return {};
  }
  return utf8;
}

const std::wstring& ProcessName() {
  static const std::wstring process_name = [] {
    wchar_t process_path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, process_path, MAX_PATH);
    std::wstring name(process_path, length < MAX_PATH ? length : 0);
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      name.erase(0, slash + 1);
    }
    return name;
  }();
  return process_name;
}

#ifdef _DEBUG
void WriteLogLine(std::wstring_view module_name, std::wstring_view level,
                  std::wstring_view message) noexcept {
  try {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t buffer[1024]{};
    const std::wstring& process_name = ProcessName();
    const std::size_t variable_length =
        process_name.size() + level.size() + module_name.size() + message.size();
    int formatted = -1;
    if (variable_length + 128 < std::size(buffer)) {
      formatted = swprintf_s(
          buffer, std::size(buffer),
          L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%ls:%lu:%lu] [%.*ls] [%.*ls] %.*ls\r\n",
          time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
          time.wMilliseconds, process_name.c_str(), GetCurrentProcessId(), GetCurrentThreadId(),
          static_cast<int>(level.size()), level.data(), static_cast<int>(module_name.size()),
          module_name.data(), static_cast<int>(message.size()), message.data());
    }

    if (formatted > 0) {
      LoggerState::Instance().Write(
          WideToUtf8(std::wstring_view(buffer, static_cast<std::size_t>(formatted))));
      return;
    }

    // Preserve complete diagnostics when a line exceeds the allocation-free fast-path buffer.
    std::wstring line;
    line.reserve(module_name.size() + message.size() + 128);
    line.append(L"[");
    wchar_t time_string[64]{};
    swprintf_s(time_string, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    line.append(time_string);
    line.append(L"] [");
    line.append(process_name);
    line.append(L":");
    line.append(std::to_wstring(GetCurrentProcessId()));
    line.append(L":");
    line.append(std::to_wstring(GetCurrentThreadId()));
    line.append(L"] [");
    line.append(level);
    line.append(L"] [");
    line.append(module_name);
    line.append(L"] ");
    line.append(message);
    line.append(L"\r\n");
    LoggerState::Instance().Write(WideToUtf8(line));
  } catch (...) {
    // Logging must never terminate the host process, including the injected hook callback.
  }
}
#endif

}  // namespace

bool IsTraceLoggingEnabled() {
#ifdef _DEBUG
  static const bool enabled = EnvironmentFlagEnabled("MINIMIZE_TRACE");
  return enabled;
#else
  return false;
#endif
}

const std::wstring& DebugLogPath() {
  static const std::wstring log_path = [] {
    std::wstring path;
    wchar_t environment_path[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"MINIMIZE_DEBUG_LOG", environment_path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      path.assign(environment_path, length);
    } else {
      wchar_t default_path[MAX_PATH]{};
      length = ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\MinimizeEffect\\minimize_debug.log",
                                         default_path, MAX_PATH);
      if (length > 0 && length < MAX_PATH) {
        path.assign(default_path, length - 1);
      } else {
        path = L"minimize_debug.log";
      }
    }

    std::error_code error;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, error);
    }
    return path;
  }();
  return log_path;
}

std::uintmax_t DebugLogFolderSize() {
  std::error_code error;
  const std::filesystem::path folder = std::filesystem::path(DebugLogPath()).parent_path();
  std::uintmax_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (error) {
      break;
    }
    if (entry.is_regular_file(error) && entry.path().filename().wstring().starts_with(L"minimize_"
                                                                                      L"debug")) {
      total += entry.file_size(error);
      error.clear();
    }
  }
  return total;
}

void CleanupDebugLogs(std::size_t maximum_files, std::uintmax_t maximum_total_bytes) {
  std::error_code error;
  const std::filesystem::path active = DebugLogPath();
  const std::filesystem::path folder = active.parent_path();
  if (std::filesystem::exists(active, error) &&
      std::filesystem::file_size(active, error) > 2u * 1024u * 1024u) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t suffix[40]{};
    swprintf_s(suffix, L"minimize_debug_%04u%02u%02u_%02u%02u%02u.log", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::filesystem::rename(active, folder / suffix, error);
    error.clear();
  }

  std::vector<std::filesystem::directory_entry> logs;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (error) {
      break;
    }
    if (entry.is_regular_file(error) && entry.path() != active &&
        entry.path().filename().wstring().starts_with(L"minimize_debug")) {
      logs.push_back(entry);
    }
    error.clear();
  }

  std::sort(logs.begin(), logs.end(), [&error](const auto& left, const auto& right) noexcept {
    const auto left_time = left.last_write_time(error);
    error.clear();
    const auto right_time = right.last_write_time(error);
    error.clear();
    return left_time > right_time;
  });

  std::uintmax_t total = DebugLogFolderSize();
  while (!logs.empty() && (logs.size() + 1 > maximum_files || total > maximum_total_bytes)) {
    const auto& oldest = logs.back();
    const std::uintmax_t size = oldest.file_size(error);
    error.clear();
    std::filesystem::remove(oldest.path(), error);
    error.clear();
    total = (total > size) ? (total - size) : 0;
    logs.pop_back();
  }
}

void ShutdownLogger() { LoggerState::Instance().Close(); }

#ifdef _DEBUG
void LogDebug(std::wstring_view module_name, std::wstring_view message) {
  WriteLogLine(module_name, L"DEBUG", message);
}

void LogTrace(std::wstring_view module_name, std::wstring_view message) {
  if (IsTraceLoggingEnabled()) {
    WriteLogLine(module_name, L"TRACE", message);
  }
}
#endif

}  // namespace minimize::core
