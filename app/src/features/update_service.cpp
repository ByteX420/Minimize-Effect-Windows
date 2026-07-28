#include "pch.hpp"

#include "features/update_service.hpp"

#include <array>
#include <bcrypt.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include "miniz/miniz.h"
#include "nlohmann/json.hpp"
#include "platform/windows/process_info.hpp"


namespace minimize::features {
namespace {

constexpr wchar_t kReleaseApiUrl[] =
    L"https://api.github.com/repos/ByteX420/Minimize-Effect-Windows/releases/latest";
constexpr char kPackageName[] = "MinimizeEffect-windows-x64.zip";
constexpr char kChecksumName[] = "MinimizeEffect-windows-x64.zip.sha256";
constexpr std::uint64_t kMaximumDownloadBytes = 256ULL * 1024ULL * 1024ULL;

template <typename T, auto CloseFunction>
class UniqueResource final {
public:
  UniqueResource() = default;
  explicit UniqueResource(T value) : value_(value) {}
  ~UniqueResource() { Reset(); }
  UniqueResource(const UniqueResource&) = delete;
  UniqueResource& operator=(const UniqueResource&) = delete;
  UniqueResource(UniqueResource&& other) noexcept : value_(std::exchange(other.value_, {})) {}
  UniqueResource& operator=(UniqueResource&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = std::exchange(other.value_, {});
    }
    return *this;
  }
  [[nodiscard]] T Get() const { return value_; }
  [[nodiscard]] explicit operator bool() const { return value_ != T{}; }
  void Reset(T value = {}) {
    if (value_ != T{}) CloseFunction(value_);
    value_ = value;
  }

private:
  T value_{};
};

void CloseAlgorithm(BCRYPT_ALG_HANDLE handle) { (void)BCryptCloseAlgorithmProvider(handle, 0); }
using UniqueAlgorithm = UniqueResource<BCRYPT_ALG_HANDLE, CloseAlgorithm>;
using UniqueHash = UniqueResource<BCRYPT_HASH_HANDLE, BCryptDestroyHash>;

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), length) != length) {
    return {};
  }
  return result;
}

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

std::wstring CurrentExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  path.resize(length);
  return path;
}

std::filesystem::path UpdateRoot() {
  std::wstring local_app_data(32768, L'\0');
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
                                               static_cast<DWORD>(local_app_data.size()));
  if (length == 0 || length >= local_app_data.size()) {
    return std::filesystem::temp_directory_path() / L"MinimizeEffect" / L"updates";
  }
  local_app_data.resize(length);
  return std::filesystem::path(local_app_data) / L"MinimizeEffect" / L"updates";
}

std::filesystem::path UpdateSibling(const std::filesystem::path& target,
                                    std::wstring_view suffix) {
  return std::filesystem::path(target.wstring() + std::wstring(suffix));
}

bool RemoveIfPresent(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return true;
  return DeleteFileW(path.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
}

void RestoreInstalledBackups(const std::filesystem::path& target_executable,
                             const std::filesystem::path& target_hook) {
  const std::filesystem::path executable_backup =
      UpdateSibling(target_executable, L".update-backup");
  const std::filesystem::path hook_backup = UpdateSibling(target_hook, L".update-backup");
  const std::filesystem::path executable_incoming =
      UpdateSibling(target_executable, L".update-new");
  const std::filesystem::path hook_incoming = UpdateSibling(target_hook, L".update-new");
  RemoveIfPresent(executable_incoming);
  RemoveIfPresent(hook_incoming);
  if (std::filesystem::exists(executable_backup)) {
    RemoveIfPresent(target_executable);
    MoveFileExW(executable_backup.c_str(), target_executable.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }
  if (std::filesystem::exists(hook_backup)) {
    RemoveIfPresent(target_hook);
    MoveFileExW(hook_backup.c_str(), target_hook.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }
}

bool InstallStagedFiles(const std::filesystem::path& source_executable,
                        const std::filesystem::path& source_hook,
                        const std::filesystem::path& target_executable,
                        const std::filesystem::path& target_hook) {
  const std::filesystem::path executable_backup =
      UpdateSibling(target_executable, L".update-backup");
  const std::filesystem::path hook_backup = UpdateSibling(target_hook, L".update-backup");
  const std::filesystem::path executable_incoming =
      UpdateSibling(target_executable, L".update-new");
  const std::filesystem::path hook_incoming = UpdateSibling(target_hook, L".update-new");
  if (!RemoveIfPresent(executable_backup) || !RemoveIfPresent(hook_backup) ||
      !RemoveIfPresent(executable_incoming) || !RemoveIfPresent(hook_incoming)) {
    return false;
  }
  if (!CopyFileW(source_executable.c_str(), executable_incoming.c_str(), TRUE) ||
      !CopyFileW(source_hook.c_str(), hook_incoming.c_str(), TRUE)) {
    RemoveIfPresent(executable_incoming);
    RemoveIfPresent(hook_incoming);
    return false;
  }

  const bool executable_moved =
      MoveFileExW(target_executable.c_str(), executable_backup.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  const bool hook_moved =
      executable_moved &&
      MoveFileExW(target_hook.c_str(), hook_backup.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  const bool executable_installed =
      hook_moved && MoveFileExW(executable_incoming.c_str(), target_executable.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  const bool hook_installed =
      executable_installed &&
      MoveFileExW(hook_incoming.c_str(), target_hook.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (hook_installed) return true;
  RestoreInstalledBackups(target_executable, target_hook);
  return false;
}

void CleanupInstalledBackups(std::stop_token stop_token) {
  const std::filesystem::path executable = CurrentExecutablePath();
  if (executable.empty()) return;
  const std::filesystem::path hook = executable.parent_path() / L"MinimizeEffectHook.dll";
  const std::array leftovers = {
      UpdateSibling(executable, L".update-backup"), UpdateSibling(hook, L".update-backup"),
      UpdateSibling(executable, L".update-new"), UpdateSibling(hook, L".update-new")};
  for (int attempt = 0; attempt < 120 && !stop_token.stop_requested(); ++attempt) {
    bool clean = true;
    for (const auto& leftover : leftovers) clean = RemoveIfPresent(leftover) && clean;
    if (clean) return;
    Sleep(50);
  }
}

struct ParsedVersion {
  std::array<unsigned long, 4> parts{};
  std::size_t count = 0;
};

std::optional<ParsedVersion> ParseVersion(std::string_view value) {
  while (!value.empty() && (value.front() == 'v' || value.front() == 'V' || value.front() == ' ')) {
    value.remove_prefix(1);
  }
  ParsedVersion result{};
  std::size_t position = 0;
  while (position < value.size() && result.count < result.parts.size()) {
    if (value[position] < '0' || value[position] > '9') return std::nullopt;
    unsigned long part = 0;
    while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
      const unsigned digit = static_cast<unsigned>(value[position] - '0');
      if (part > (std::numeric_limits<unsigned long>::max() - digit) / 10) {
        return std::nullopt;
      }
      part = part * 10 + digit;
      ++position;
    }
    result.parts[result.count++] = part;
    if (position == value.size()) break;
    if (value[position] != '.') return std::nullopt;
    ++position;
  }
  if (position != value.size() || result.count < 3) return std::nullopt;
  return result;
}

bool IsNewerVersion(std::string_view candidate, std::string_view current) {
  const auto candidate_version = ParseVersion(candidate);
  const auto current_version = ParseVersion(current);
  if (!candidate_version || !current_version) return false;
  return candidate_version->parts > current_version->parts;
}

bool IsSameVersion(std::string_view left, std::string_view right) {
  const auto left_version = ParseVersion(left);
  const auto right_version = ParseVersion(right);
  return left_version && right_version && left_version->parts == right_version->parts;
}

std::optional<std::string> JsonStringValue(const nlohmann::json& object, std::string_view key) {
  const auto value = object.find(key);
  if (value == object.end() || !value->is_string()) return std::nullopt;
  return value->get<std::string>();
}

std::optional<std::string> FindAssetValue(const nlohmann::json& release,
                                          std::string_view asset_name,
                                          std::string_view key) {
  const auto assets = release.find("assets");
  if (assets == release.end() || !assets->is_array()) return std::nullopt;
  for (const auto& asset : *assets) {
    if (!asset.is_object() || asset.value("name", std::string{}) != asset_name) continue;
    return JsonStringValue(asset, key);
  }
  return std::nullopt;
}

std::optional<std::string> FindAssetUrl(const nlohmann::json& release,
                                        std::string_view asset_name) {
  return FindAssetValue(release, asset_name, "browser_download_url");
}

struct HttpResponse {
  bool success = false;
  std::uint32_t status_code = 0;
  std::string error;
};

template <typename Sink>
HttpResponse HttpGet(std::wstring_view url, Sink&& sink, std::stop_token stop_token) {
  HttpResponse result{};
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct ApartmentGuard final {
      ~ApartmentGuard() { winrt::uninit_apartment(); }
    } apartment_guard;

    const winrt::Windows::Foundation::Uri uri{std::wstring(url)};
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"MinimizeEffect-Updater/1.0");
    client.DefaultRequestHeaders().Accept().ParseAdd(L"application/vnd.github+json");

    auto response_operation = client.GetAsync(
        uri, winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
    std::stop_callback cancel_response(stop_token, [&response_operation] {
      response_operation.Cancel();
    });
    const auto response = response_operation.get();
    result.status_code = static_cast<std::uint32_t>(response.StatusCode());
    if (!response.IsSuccessStatusCode()) {
      result.error = "GitHub returned HTTP " + std::to_string(result.status_code) + ".";
      return result;
    }

    std::uint64_t total = 0;
    if (const auto length = response.Content().Headers().ContentLength()) {
      total = length.Value();
    }
    const auto stream = response.Content().ReadAsInputStreamAsync().get();
    std::uint64_t received = 0;
    for (;;) {
      if (stop_token.stop_requested()) {
        result.error = "Cancelled.";
        return result;
      }
      winrt::Windows::Storage::Streams::Buffer buffer(64 * 1024);
      auto read_operation =
          stream.ReadAsync(buffer, buffer.Capacity(),
                           winrt::Windows::Storage::Streams::InputStreamOptions::None);
      std::stop_callback cancel_read(stop_token, [&read_operation] { read_operation.Cancel(); });
      const auto bytes = read_operation.get();
      if (bytes.Length() == 0) break;

      std::vector<std::uint8_t> chunk(bytes.Length());
      winrt::Windows::Storage::Streams::DataReader::FromBuffer(bytes).ReadBytes(chunk);
      received += chunk.size();
      if (received > kMaximumDownloadBytes ||
          !sink(reinterpret_cast<const std::byte*>(chunk.data()),
                static_cast<std::uint32_t>(chunk.size()), received, total)) {
        result.error = received > kMaximumDownloadBytes ? "The update package is unexpectedly large."
                                                        : "Could not save the update package.";
        return result;
      }
    }
    result.success = true;
  } catch (const winrt::hresult_canceled&) {
    result.error = "Cancelled.";
  } catch (const winrt::hresult_error&) {
    if (result.status_code == 0) {
      result.error = "GitHub did not respond. Check your connection and try again.";
    }
  }
  return result;
}

std::optional<std::string> GetText(std::wstring_view url, std::stop_token stop_token,
                                   std::string& error) {
  std::string text;
  auto response = HttpGet(
      url,
      [&text](const std::byte* data, std::uint32_t size, std::uint64_t, std::uint64_t) {
        if (text.size() + size > 4 * 1024 * 1024) return false;
        text.append(reinterpret_cast<const char*>(data), size);
        return true;
      },
      stop_token);
  if (!response.success) {
    error = response.error;
    return std::nullopt;
  }
  return text;
}

bool DownloadFile(std::wstring_view url, const std::filesystem::path& destination,
                  std::stop_token stop_token,
                  const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                  const std::function<bool()>& should_cancel, std::string& error) {
  std::ofstream file(destination, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "Could not create the update file.";
    return false;
  }
  auto response = HttpGet(
      url,
      [&](const std::byte* data, std::uint32_t size, std::uint64_t received,
          std::uint64_t total) {
        if (should_cancel()) return false;
        file.write(reinterpret_cast<const char*>(data), size);
        if (!file) return false;
        progress(received, total);
        return true;
      },
      stop_token);
  file.close();
  if (!response.success) {
    error = response.error;
    return false;
  }
  return true;
}

std::optional<std::string> Sha256(const std::filesystem::path& path) {
  UniqueAlgorithm algorithm;
  BCRYPT_ALG_HANDLE algorithm_handle = nullptr;
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&algorithm_handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  algorithm.Reset(algorithm_handle);
  DWORD object_size = 0;
  DWORD bytes = 0;
  if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm.Get(), BCRYPT_OBJECT_LENGTH,
                                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                        &bytes, 0))) {
    return std::nullopt;
  }
  std::vector<UCHAR> object(object_size);
  BCRYPT_HASH_HANDLE hash_handle = nullptr;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.Get(), &hash_handle, object.data(), object_size,
                                       nullptr, 0, 0))) {
    return std::nullopt;
  }
  UniqueHash hash(hash_handle);
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), buffer.size());
    const std::streamsize count = file.gcount();
    if (count > 0 &&
        !BCRYPT_SUCCESS(BCryptHashData(hash.Get(), reinterpret_cast<PUCHAR>(buffer.data()),
                                       static_cast<ULONG>(count), 0))) {
      return std::nullopt;
    }
  }
  std::array<UCHAR, 32> digest{};
  if (!BCRYPT_SUCCESS(
          BCryptFinishHash(hash.Get(), digest.data(), static_cast<ULONG>(digest.size()), 0))) {
    return std::nullopt;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const UCHAR byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

std::optional<std::string> ParseChecksum(std::string_view value) {
  for (std::size_t start = 0; start + 64 <= value.size(); ++start) {
    bool valid = true;
    for (std::size_t index = 0; index < 64; ++index) {
      const char character = value[start + index];
      if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F'))) {
        valid = false;
        break;
      }
    }
    if (!valid) continue;
    std::string checksum(value.substr(start, 64));
    std::transform(checksum.begin(), checksum.end(), checksum.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return checksum;
  }
  return std::nullopt;
}

bool ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& destination,
                std::string& error) {
  FILE* archive_file = nullptr;
  if (_wfopen_s(&archive_file, archive.c_str(), L"rb") != 0 || archive_file == nullptr) {
    error = "The update package could not be opened.";
    return false;
  }
  const std::unique_ptr<FILE, decltype(&fclose)> file_guard(archive_file, fclose);

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_cfile(&zip, archive_file, 0, 0)) {
    error = "The update package is not a valid ZIP archive.";
    return false;
  }
  struct ZipGuard final {
    mz_zip_archive* zip;
    ~ZipGuard() { mz_zip_reader_end(zip); }
  } zip_guard{&zip};

  std::error_code filesystem_error;
  for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_encrypted) {
      error = "The update package contains an unsupported ZIP entry.";
      return false;
    }

    const std::filesystem::path relative = std::filesystem::path(Utf8ToWide(stat.m_filename))
                                               .lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_path() ||
        std::find(relative.begin(), relative.end(), std::filesystem::path(L"..")) !=
            relative.end()) {
      error = "The update package contains an unsafe path.";
      return false;
    }

    const std::filesystem::path output = destination / relative;
    if (stat.m_is_directory) {
      std::filesystem::create_directories(output, filesystem_error);
      if (filesystem_error) {
        error = "Could not create a directory from the update package.";
        return false;
      }
      continue;
    }

    std::filesystem::create_directories(output.parent_path(), filesystem_error);
    if (filesystem_error) {
      error = "Could not create the update package directory.";
      return false;
    }
    std::ofstream extracted(output, std::ios::binary | std::ios::trunc);
    if (!extracted) {
      error = "Could not create an extracted update file.";
      return false;
    }
    const auto write_file = [](void* opaque, mz_uint64 offset, const void* data,
                               size_t size) -> size_t {
      auto& stream = *static_cast<std::ofstream*>(opaque);
      stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
      stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
      return stream ? size : 0;
    };
    if (!mz_zip_reader_extract_to_callback(&zip, index, write_file, &extracted, 0)) {
      error = "The update package could not be extracted.";
      return false;
    }
  }
  return true;
}

bool LooksLikeX64PeFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  char magic[2]{};
  file.read(magic, sizeof(magic));
  if (file.gcount() != sizeof(magic) || magic[0] != 'M' || magic[1] != 'Z') return false;
  file.seekg(0x3c, std::ios::beg);
  std::uint32_t pe_offset = 0;
  file.read(reinterpret_cast<char*>(&pe_offset), sizeof(pe_offset));
  if (!file || pe_offset > 16 * 1024 * 1024) return false;
  file.seekg(pe_offset, std::ios::beg);
  std::array<char, 4> signature{};
  std::uint16_t machine = 0;
  file.read(signature.data(), signature.size());
  file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
  return file && signature == std::array<char, 4>{'P', 'E', '\0', '\0'} &&
         machine == IMAGE_FILE_MACHINE_AMD64;
}

std::string CleanReleaseNotes(std::string notes) {
  notes.erase(std::remove(notes.begin(), notes.end(), '\r'), notes.end());
  while (!notes.empty() && (notes.front() == '\n' || notes.front() == ' ')) {
    notes.erase(notes.begin());
  }
  if (notes.size() > 1600) notes.resize(1600);
  return notes;
}

}  // namespace

UpdateService::~UpdateService() { Stop(); }

void UpdateService::Start(HWND notification_window) {
  {
    std::scoped_lock lock(mutex_);
    notification_window_ = notification_window;
    if (snapshot_.current_version.empty()) {
      snapshot_.current_version = platform::ExecutableProductVersion();
      if (snapshot_.current_version.empty()) snapshot_.current_version = "0.0.0";
    }
  }
  if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop_token) { WorkerLoop(stop_token); });
  }
  CheckForUpdates(false);
}

void UpdateService::Stop() {
  if (!worker_.joinable()) return;
  worker_.request_stop();
  condition_.notify_all();
  worker_.join();
  std::scoped_lock lock(mutex_);
  notification_window_ = nullptr;
  installer_ready_event_.reset();
  installer_process_.reset();
  installer_started_at_ms_ = 0;
}

void UpdateService::CheckForUpdates(bool user_initiated) {
  {
    std::scoped_lock lock(mutex_);
    if (snapshot_.phase == UpdatePhase::kDownloading ||
        snapshot_.phase == UpdatePhase::kVerifying || snapshot_.phase == UpdatePhase::kStaging ||
        snapshot_.phase == UpdatePhase::kReadyToInstall ||
        snapshot_.phase == UpdatePhase::kInstalling) {
      return;
    }
    pending_action_ = PendingAction::kCheck;
    pending_check_user_initiated_ = user_initiated;
    cancel_requested_ = false;
  }
  condition_.notify_one();
}

void UpdateService::DownloadUpdate() {
  {
    std::scoped_lock lock(mutex_);
    if (snapshot_.phase != UpdatePhase::kAvailable && snapshot_.phase != UpdatePhase::kError) {
      return;
    }
    if (package_url_.empty() || (checksum_url_.empty() && expected_checksum_.empty())) {
      pending_action_ = PendingAction::kCheck;
      pending_check_user_initiated_ = true;
    } else {
      pending_action_ = PendingAction::kDownload;
    }
    cancel_requested_ = false;
  }
  condition_.notify_one();
}

void UpdateService::CancelDownload() {
  std::scoped_lock lock(mutex_);
  cancel_requested_ = true;
}

UpdateSnapshot UpdateService::GetSnapshot() const {
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

void UpdateService::WorkerLoop(std::stop_token stop_token) {
  // Best-effort cleanup from an earlier handover, off the startup/UI thread. The currently
  // running helper may keep its own staged EXE locked briefly; a later launch removes it.
  CleanupInstalledBackups(stop_token);
  std::error_code cleanup_error;
  std::filesystem::remove_all(UpdateRoot(), cleanup_error);
  constexpr auto kBackgroundCheckInterval = std::chrono::hours(6);
  while (!stop_token.stop_requested()) {
    PendingAction action = PendingAction::kNone;
    bool user_initiated = false;
    {
      std::unique_lock lock(mutex_);
      const bool signaled =
          condition_.wait_for(lock, kBackgroundCheckInterval, [this, &stop_token] {
            return stop_token.stop_requested() || pending_action_ != PendingAction::kNone;
          });
      if (stop_token.stop_requested()) return;
      if (signaled) {
        action = std::exchange(pending_action_, PendingAction::kNone);
        user_initiated = pending_check_user_initiated_;
      } else {
        action = PendingAction::kCheck;
        user_initiated = false;
      }
    }
    if (action == PendingAction::kCheck) {
      CheckWorker(stop_token, user_initiated);
    } else if (action == PendingAction::kDownload) {
      DownloadWorker(stop_token);
    }
  }
}

void UpdateService::CheckWorker(std::stop_token stop_token, bool user_initiated) {
  UpdateSnapshot checking = GetSnapshot();
  checking.phase = UpdatePhase::kChecking;
  checking.status = "Checking GitHub for updates...";
  checking.error.clear();
  checking.user_initiated = user_initiated;
  checking.downloaded_bytes = 0;
  checking.total_bytes = 0;
  checking.progress = 0.0f;
  SetSnapshot(std::move(checking));

  std::string error;
  const auto response = GetText(kReleaseApiUrl, stop_token, error);
  if (!response) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = user_initiated ? "Update check failed" : "Could not check for updates";
    failed.error = std::move(error);
    SetSnapshot(std::move(failed));
    return;
  }

  const nlohmann::json release = nlohmann::json::parse(*response, nullptr, false);
  if (release.is_discarded() || !release.is_object()) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not read the release";
    failed.error = "GitHub returned malformed release metadata.";
    SetSnapshot(std::move(failed));
    return;
  }
  const auto tag = JsonStringValue(release, "tag_name");
  const auto release_page = JsonStringValue(release, "html_url");
  const auto release_notes = JsonStringValue(release, "body");
  const auto package_url = FindAssetUrl(release, kPackageName);
  const auto package_digest = FindAssetValue(release, kPackageName, "digest");
  const auto api_checksum =
      package_digest ? ParseChecksum(*package_digest) : std::optional<std::string>{};
  const auto checksum_url = FindAssetUrl(release, kChecksumName);
  if (!tag || !release_page) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "The GitHub release is incomplete";
    failed.error = "The latest release metadata could not be read.";
    SetSnapshot(std::move(failed));
    return;
  }

  std::string latest = *tag;
  if (!latest.empty() && (latest.front() == 'v' || latest.front() == 'V')) latest.erase(0, 1);
  UpdateSnapshot next = GetSnapshot();
  next.latest_version = latest;
  next.release_page_url = *release_page;
  next.release_notes = CleanReleaseNotes(release_notes.value_or(""));
  next.error.clear();
  if (!IsNewerVersion(latest, next.current_version)) {
    next.phase = UpdatePhase::kUpToDate;
    next.status = "You are up to date";
    {
      std::scoped_lock lock(mutex_);
      package_url_.clear();
      checksum_url_.clear();
      expected_checksum_.clear();
    }
    SetSnapshot(std::move(next));
    return;
  }
  if (!package_url || (!checksum_url && !api_checksum)) {
    next.phase = UpdatePhase::kError;
    next.status = "Update v" + latest + " is available on GitHub";
    next.error =
        "This release has no verified update package. Install it manually from the release page.";
    SetSnapshot(std::move(next));
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    package_url_ = *package_url;
    checksum_url_ = checksum_url.value_or("");
    expected_checksum_ = api_checksum.value_or("");
  }
  next.phase = UpdatePhase::kAvailable;
  next.status = "Update v" + latest + " is ready";
  SetSnapshot(std::move(next));
}

void UpdateService::DownloadWorker(std::stop_token stop_token) {
  std::string package_url;
  std::string checksum_url;
  std::string expected_checksum_value;
  UpdateSnapshot downloading = GetSnapshot();
  {
    std::scoped_lock lock(mutex_);
    package_url = package_url_;
    checksum_url = checksum_url_;
    expected_checksum_value = expected_checksum_;
  }
  if (package_url.empty() || (checksum_url.empty() && expected_checksum_value.empty())) return;

  downloading.phase = UpdatePhase::kDownloading;
  downloading.status = "Downloading update...";
  downloading.error.clear();
  downloading.user_initiated = true;
  downloading.downloaded_bytes = 0;
  downloading.total_bytes = 0;
  downloading.progress = 0.0f;
  SetSnapshot(std::move(downloading));

  const std::filesystem::path version_root =
      UpdateRoot() / Utf8ToWide(GetSnapshot().latest_version);
  const std::filesystem::path package_partial = version_root / L"package.zip.partial";
  const std::filesystem::path package = version_root / L"package.zip";
  const std::filesystem::path staging = version_root / L"staged";
  std::error_code filesystem_error;
  std::filesystem::remove_all(version_root, filesystem_error);
  filesystem_error.clear();
  std::filesystem::create_directories(version_root, filesystem_error);
  if (filesystem_error) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not prepare the update";
    failed.error = "Check disk space and permissions for Local AppData.";
    SetSnapshot(std::move(failed));
    return;
  }

  std::string error;
  std::optional<std::string> expected_checksum = ParseChecksum(expected_checksum_value);
  if (!expected_checksum && !checksum_url.empty()) {
    const auto checksum_text = GetText(Utf8ToWide(checksum_url), stop_token, error);
    expected_checksum =
        checksum_text ? ParseChecksum(*checksum_text) : std::optional<std::string>{};
  }
  if (!expected_checksum) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not verify the update";
    failed.error = error.empty() ? "The release checksum is invalid." : error;
    SetSnapshot(std::move(failed));
    return;
  }

  const auto progress = [this](std::uint64_t downloaded, std::uint64_t total) {
    MutateSnapshot([downloaded, total](UpdateSnapshot& snapshot) {
      snapshot.downloaded_bytes = downloaded;
      snapshot.total_bytes = total;
      if (total > 0) {
        snapshot.progress =
            std::clamp(static_cast<float>(downloaded) / static_cast<float>(total), 0.0f, 1.0f);
      }
    });
  };
  const auto should_cancel = [this] {
    std::scoped_lock lock(mutex_);
    return cancel_requested_;
  };
  const bool downloaded = DownloadFile(Utf8ToWide(package_url), package_partial, stop_token,
                                       progress, should_cancel, error);
  bool cancelled = false;
  {
    std::scoped_lock lock(mutex_);
    cancelled = cancel_requested_;
  }
  if (!downloaded || cancelled || stop_token.stop_requested()) {
    std::filesystem::remove(package_partial, filesystem_error);
    if (stop_token.stop_requested()) return;
    UpdateSnapshot next = GetSnapshot();
    if (cancelled) {
      next.phase = UpdatePhase::kAvailable;
      next.status = "Update v" + next.latest_version + " is ready";
      next.error.clear();
      next.downloaded_bytes = 0;
      next.total_bytes = 0;
      next.progress = 0.0f;
    } else {
      next.phase = UpdatePhase::kError;
      next.status = "Download failed";
      next.error = error;
    }
    SetSnapshot(std::move(next));
    return;
  }
  std::filesystem::rename(package_partial, package, filesystem_error);
  if (filesystem_error) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not finalize the download";
    failed.error = "The downloaded file could not be moved into place.";
    SetSnapshot(std::move(failed));
    return;
  }

  MutateSnapshot([](UpdateSnapshot& snapshot) {
    snapshot.phase = UpdatePhase::kVerifying;
    snapshot.status = "Verifying SHA-256 checksum...";
    snapshot.progress = 1.0f;
  });
  const auto actual_checksum = Sha256(package);
  if (!actual_checksum || *actual_checksum != *expected_checksum) {
    std::filesystem::remove(package, filesystem_error);
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Update verification failed";
    failed.error = "The package checksum does not match. Nothing was installed.";
    SetSnapshot(std::move(failed));
    return;
  }

  MutateSnapshot([](UpdateSnapshot& snapshot) {
    snapshot.phase = UpdatePhase::kStaging;
    snapshot.status = "Preparing the seamless restart...";
  });
  std::filesystem::create_directories(staging, filesystem_error);
  if (filesystem_error || !ExtractZip(package, staging, error)) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not prepare the update";
    failed.error = error.empty() ? "The staging directory could not be created." : error;
    SetSnapshot(std::move(failed));
    return;
  }
  const std::filesystem::path staged_executable = staging / L"MinimizeEffect.exe";
  const std::filesystem::path staged_hook = staging / L"MinimizeEffectHook.dll";
  if (!LooksLikeX64PeFile(staged_executable) || !LooksLikeX64PeFile(staged_hook)) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "The update package is incomplete";
    failed.error = "Expected MinimizeEffect.exe and MinimizeEffectHook.dll were not found.";
    SetSnapshot(std::move(failed));
    return;
  }
  const std::string staged_version = platform::FileProductVersion(staged_executable.wstring());
  if (!IsSameVersion(staged_version, GetSnapshot().latest_version)) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "The update package has the wrong version";
    failed.error = "The verified executable does not match release v" + failed.latest_version + ".";
    SetSnapshot(std::move(failed));
    return;
  }

  {
    std::scoped_lock lock(mutex_);
    staged_directory_ = staging;
  }
  UpdateSnapshot ready = GetSnapshot();
  ready.phase = UpdatePhase::kReadyToInstall;
  ready.status = "Ready — switching to v" + ready.latest_version;
  ready.progress = 1.0f;
  SetSnapshot(std::move(ready));
}

bool UpdateService::LaunchInstaller(const RECT& window_bounds, int selected_page,
                                    float page_scroll, bool maximized) {
  std::filesystem::path staging;
  {
    std::scoped_lock lock(mutex_);
    if (snapshot_.phase != UpdatePhase::kReadyToInstall || staged_directory_.empty()) return false;
    staging = staged_directory_;
  }
  const std::filesystem::path staged_executable = staging / L"MinimizeEffect.exe";
  const std::filesystem::path staged_hook = staging / L"MinimizeEffectHook.dll";
  const std::filesystem::path current = CurrentExecutablePath();
  if (current.empty() || !std::filesystem::exists(staged_executable) ||
      !std::filesystem::exists(staged_hook)) {
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Update files missing";
    failed.error = "The downloaded update package is incomplete or missing.";
    SetSnapshot(std::move(failed));
    return false;
  }
  const std::filesystem::path target_directory = current.parent_path();
  const std::filesystem::path target_hook = target_directory / L"MinimizeEffectHook.dll";
  const std::filesystem::path executable_backup =
      target_directory / L"MinimizeEffect.exe.update-backup";
  const std::filesystem::path hook_backup =
      target_directory / L"MinimizeEffectHook.dll.update-backup";

  std::error_code ec;
  std::filesystem::remove(executable_backup, ec);
  ec.clear();
  std::filesystem::remove(hook_backup, ec);
  ec.clear();

  if (!MoveFileExW(current.c_str(), executable_backup.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return false;
  }
  if (!CopyFileW(staged_executable.c_str(), current.c_str(), FALSE)) {
    MoveFileExW(executable_backup.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);
    return false;
  }
  MoveFileExW(target_hook.c_str(), hook_backup.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!CopyFileW(staged_hook.c_str(), target_hook.c_str(), FALSE)) {
    MoveFileExW(hook_backup.c_str(), target_hook.c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(executable_backup.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);
    return false;
  }

  const DWORD parent_process_id = GetCurrentProcessId();
  const std::wstring ready_event_name = L"Local\\MinimizeEffect.UpdateReady." +
                                        std::to_wstring(parent_process_id) + L"." +
                                        std::to_wstring(GetTickCount64());
  HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, ready_event_name.c_str());
  if (ready_event == nullptr) {
    RestoreInstalledBackups(current, target_hook);
    return false;
  }
  {
    std::scoped_lock lock(mutex_);
    installer_ready_event_.reset(ready_event);
    installer_process_.reset();
    installer_started_at_ms_ = 0;
  }

  const long scroll_milli =
      static_cast<long>(std::clamp(page_scroll, 0.0f, 2147483.0f) * 1000.0f);
  std::wstring command_line = QuoteArgument(current.wstring()) + L" --update-resume " +
                              std::to_wstring(window_bounds.left) + L" " +
                              std::to_wstring(window_bounds.top) + L" " +
                              std::to_wstring(window_bounds.right) + L" " +
                              std::to_wstring(window_bounds.bottom) + L" " +
                              std::to_wstring(parent_process_id) + L" " +
                              QuoteArgument(ready_event_name) + L" " +
                              std::to_wstring(std::clamp(selected_page, 0, 7)) + L" " +
                              std::to_wstring(scroll_milli) + L" " + (maximized ? L"1" : L"0");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(current.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_PROCESS_GROUP, nullptr, target_directory.c_str(), &startup,
                      &process)) {
    RestoreInstalledBackups(current, target_hook);
    {
      std::scoped_lock lock(mutex_);
      installer_ready_event_.reset();
    }
    UpdateSnapshot failed = GetSnapshot();
    failed.phase = UpdatePhase::kError;
    failed.status = "Could not start the update handover";
    failed.error = "Windows error " + std::to_string(GetLastError()) +
                   ". Your current version was restored.";
    SetSnapshot(std::move(failed));
    return false;
  }
  wil::unique_handle process_thread(process.hThread);
  {
    std::scoped_lock lock(mutex_);
    installer_process_.reset(process.hProcess);
    installer_started_at_ms_ = GetTickCount64();
  }
  MutateSnapshot([](UpdateSnapshot& snapshot) {
    snapshot.phase = UpdatePhase::kInstalling;
    snapshot.status = "The new version is taking over this window...";
  });
  return true;
}

bool UpdateService::InstallerHandoverReady() {
  std::scoped_lock lock(mutex_);
  return installer_ready_event_ &&
         WaitForSingleObject(installer_ready_event_.get(), 0) == WAIT_OBJECT_0;
}

bool UpdateService::InstallerHandoverFailed() {
  HANDLE process = nullptr;
  HANDLE ready_event = nullptr;
  ULONGLONG started_at = 0;
  {
    std::scoped_lock lock(mutex_);
    process = installer_process_.get();
    ready_event = installer_ready_event_.get();
    started_at = installer_started_at_ms_;
  }
  if (process == nullptr || ready_event == nullptr ||
      WaitForSingleObject(ready_event, 0) == WAIT_OBJECT_0) {
    return false;
  }
  const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  const bool timed_out = started_at != 0 && GetTickCount64() - started_at >= 30000;
  if (!exited && !timed_out) return false;
  if (timed_out) {
    TerminateProcess(process, ERROR_TIMEOUT);
    WaitForSingleObject(process, 3000);
  }

  const std::filesystem::path current = CurrentExecutablePath();
  if (!current.empty()) {
    RestoreInstalledBackups(current, current.parent_path() / L"MinimizeEffectHook.dll");
  }
  {
    std::scoped_lock lock(mutex_);
    installer_process_.reset();
    installer_ready_event_.reset();
    installer_started_at_ms_ = 0;
  }
  UpdateSnapshot failed = GetSnapshot();
  failed.phase = UpdatePhase::kError;
  failed.status = "The new version could not take over";
  failed.error = timed_out ? "The handover timed out. Your current version was restored."
                           : "The new process closed early. Your current version was restored.";
  SetSnapshot(std::move(failed));
  return true;
}

void UpdateService::SetSnapshot(UpdateSnapshot snapshot) {
  {
    std::scoped_lock lock(mutex_);
    snapshot_ = std::move(snapshot);
  }
  NotifyStateChanged();
}

void UpdateService::MutateSnapshot(const std::function<void(UpdateSnapshot&)>& mutation) {
  {
    std::scoped_lock lock(mutex_);
    mutation(snapshot_);
  }
  NotifyStateChanged();
}

void UpdateService::NotifyStateChanged() const {
  HWND window = nullptr;
  {
    std::scoped_lock lock(mutex_);
    window = notification_window_;
  }
  if (window != nullptr) PostMessageW(window, kStateChangedMessage, 0, 0);
}

}  // namespace minimize::features
