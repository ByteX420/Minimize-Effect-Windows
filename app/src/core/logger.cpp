#include "pch.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#define SPDLOG_WCHAR_FILENAMES
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "core/environment.hpp"

namespace minimize::core {
namespace {

constexpr std::size_t kRotationBytes = 2u * 1024u * 1024u;
constexpr std::size_t kRotationFiles = 5;
constexpr std::size_t kAsyncQueueSize = 8192;

bool IsSynchronousLoggingEnabled() {
#ifdef _DEBUG
  static const bool enabled = EnvironmentFlagEnabled("MINIMIZE_LOG_SYNC");
  return enabled;
#else
  return false;
#endif
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                          nullptr, nullptr) != size) {
    return {};
  }
  return utf8;
}

const std::wstring& ProcessName() {
  static const std::wstring process_name = [] {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length > 0 && length < path.size() ? length : 0);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.erase(0, slash + 1);
    return path;
  }();
  return process_name;
}

bool IsMainApplication() {
  return _wcsicmp(ProcessName().c_str(), L"MinimizeEffect.exe") == 0;
}

class LoggerState final {
public:
  static LoggerState& Instance() noexcept {
    static LoggerState state;
    return state;
  }

  void Write(spdlog::level::level_enum level, std::wstring_view module,
             std::wstring_view message) noexcept {
#ifdef _DEBUG
    try {
      std::scoped_lock lock(mutex_);
      EnsureInitialized();
      if (!logger_) return;
      logger_->log(level, "[{}] {}", WideToUtf8(module), WideToUtf8(message));
      if (IsSynchronousLoggingEnabled()) logger_->flush();
    } catch (...) {
      // Diagnostics must never terminate the app or an injected hook callback.
    }
#else
    (void)level;
    (void)module;
    (void)message;
#endif
  }

  void Close() noexcept {
#ifdef _DEBUG
    try {
      std::scoped_lock lock(mutex_);
      if (logger_) logger_->flush();
      logger_.reset();
      spdlog::shutdown();
    } catch (...) {
    }
#endif
  }

private:
  void EnsureInitialized() {
    if (logger_) return;
    const auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        spdlog::filename_t(DebugLogPath()), kRotationBytes, kRotationFiles);
    const std::string logger_name = WideToUtf8(ProcessName());
    if (IsMainApplication()) {
      spdlog::init_thread_pool(kAsyncQueueSize, 1);
      logger_ = std::make_shared<spdlog::async_logger>(
          logger_name, sink, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    } else {
      logger_ = std::make_shared<spdlog::logger>(logger_name, sink);
    }
    logger_->set_level(spdlog::level::trace);
    logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n:%P:%t] [%l] %v");
  }

  std::mutex mutex_;
  std::shared_ptr<spdlog::logger> logger_;
};

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
      path = length > 0 && length < MAX_PATH ? std::wstring(default_path, length - 1)
                                             : std::wstring(L"minimize_debug.log");
    }
    std::error_code error;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    return path;
  }();
  return log_path;
}

std::uintmax_t DebugLogFolderSize() {
  std::error_code error;
  const auto folder = std::filesystem::path(DebugLogPath()).parent_path();
  std::uintmax_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (error) break;
    if (entry.is_regular_file(error) &&
        entry.path().filename().wstring().starts_with(L"minimize_debug")) {
      total += entry.file_size(error);
    }
    error.clear();
  }
  return total;
}

void CleanupDebugLogs(std::size_t maximum_files, std::uintmax_t maximum_total_bytes) {
  std::error_code error;
  const auto active = std::filesystem::path(DebugLogPath());
  const auto folder = active.parent_path();
  std::vector<std::filesystem::directory_entry> logs;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (error) break;
    if (entry.is_regular_file(error) &&
        entry.path().filename().wstring().starts_with(L"minimize_debug")) {
      logs.push_back(entry);
    }
    error.clear();
  }
  std::sort(logs.begin(), logs.end(), [&error](const auto& left, const auto& right) {
    const auto left_time = left.last_write_time(error);
    error.clear();
    const auto right_time = right.last_write_time(error);
    error.clear();
    return left_time > right_time;
  });
  std::uintmax_t total = DebugLogFolderSize();
  while (!logs.empty() && (logs.size() > maximum_files || total > maximum_total_bytes)) {
    const auto& oldest = logs.back();
    const auto size = oldest.file_size(error);
    error.clear();
    if (oldest.path() != active) std::filesystem::remove(oldest.path(), error);
    error.clear();
    total = total > size ? total - size : 0;
    logs.pop_back();
  }
}

void ShutdownLogger() { LoggerState::Instance().Close(); }

#ifdef _DEBUG
void LogDebug(std::wstring_view module_name, std::wstring_view message) {
  LoggerState::Instance().Write(spdlog::level::debug, module_name, message);
}

void LogTrace(std::wstring_view module_name, std::wstring_view message) {
  if (IsTraceLoggingEnabled()) {
    LoggerState::Instance().Write(spdlog::level::trace, module_name, message);
  }
}
#endif

}  // namespace minimize::core
