#include "pch.hpp"

#include "platform/windows/cbt_hook_manager.hpp"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "app/resource.hpp"
#include "core/logger.hpp"
#include "platform/windows/app_container_permissions.hpp"

namespace minimize::platform::windows {
namespace {

constexpr wchar_t kHookDllName[] = L"MinimizeEffectHook.dll";
constexpr char kCbtProcName[] = "CBTProc";
constexpr char kDecoratedCbtProcName[] = "_CBTProc@12";
constexpr char kCallWndProcName[] = "CallWndProc";
constexpr char kDecoratedCallWndProcName[] = "_CallWndProc@12";

using HookProc = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);

struct EmbeddedResource {
  const unsigned char* data = nullptr;
  std::size_t size = 0;
};

EmbeddedResource LoadEmbeddedResource(int resource_id) {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(instance, MAKEINTRESOURCEW(resource_id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
  if (resource == nullptr) return {};
  HGLOBAL loaded = LoadResource(instance, resource);
  if (loaded == nullptr) return {};
  const DWORD size = SizeofResource(instance, resource);
  const void* data = LockResource(loaded);
  if (data == nullptr || size == 0) return {};
  return {static_cast<const unsigned char*>(data), static_cast<std::size_t>(size)};
}

std::uint64_t ResourceFingerprint(const EmbeddedResource& resource) {
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t index = 0; index < resource.size; ++index) {
    hash ^= resource.data[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool FileMatchesResource(const std::filesystem::path& path, const EmbeddedResource& resource) {
  std::error_code error;
  if (std::filesystem::file_size(path, error) != resource.size || error) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  std::vector<unsigned char> bytes(resource.size);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return input && std::memcmp(bytes.data(), resource.data, resource.size) == 0;
}

std::filesystem::path HookCacheDirectory() {
  DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required != 0) {
    std::wstring local_app_data(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required);
    if (written != 0 && written < required) {
      local_app_data.resize(written);
      return std::filesystem::path(local_app_data) / L"MinimizeEffect" / L"hooks";
    }
  }

  std::wstring temporary(MAX_PATH, L'\0');
  const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
  if (length == 0 || length >= temporary.size()) return {};
  temporary.resize(length);
  return std::filesystem::path(temporary) / L"MinimizeEffect" / L"hooks";
}

std::wstring ExtractEmbeddedHookDll() {
  const EmbeddedResource hook = LoadEmbeddedResource(IDR_MINIMIZE_HOOK);
  if (hook.data == nullptr) return {};

  const std::filesystem::path directory = HookCacheDirectory();
  if (directory.empty()) return {};
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return {};

  const std::filesystem::path destination =
      directory / std::format(L"MinimizeEffectHook-{:016x}.dll", ResourceFingerprint(hook));
  if (FileMatchesResource(destination, hook)) return destination.wstring();

  const std::filesystem::path temporary =
      destination.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return {};
    output.write(reinterpret_cast<const char*>(hook.data), static_cast<std::streamsize>(hook.size));
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(temporary, error);
      return {};
    }
  }

  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary, error);
    return FileMatchesResource(destination, hook) ? destination.wstring() : std::wstring{};
  }
  return destination.wstring();
}

std::wstring ExecutableDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  while (length == path.size()) {
    path.resize(path.size() * 2);
    length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  }
  if (length == 0) return L".\\";
  path.resize(length);
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L".\\" : path.substr(0, slash + 1);
}

}  // namespace

CbtHookManager::~CbtHookManager() { Uninstall(); }

bool CbtHookManager::Install() {
  if (IsInstalled()) return true;
  if (hook_ != nullptr || call_window_hook_ != nullptr || library_ != nullptr) Uninstall();

  std::wstring hook_path = ExtractEmbeddedHookDll();
  if (hook_path.empty()) hook_path = ExecutableDirectory() + kHookDllName;
  const std::wstring hook_directory =
      std::filesystem::path(hook_path).parent_path().wstring() + L"\\";
  (void)minimize::platform::GrantAppContainerPermissions(hook_directory);
  (void)minimize::platform::GrantAppContainerPermissions(hook_path);

  library_ = LoadLibraryW(hook_path.c_str());
  if (library_ == nullptr) {
    core::LogDebug(L"CbtHook", L"LoadLibraryW failed for hook DLL");
    return false;
  }

  FARPROC address = GetProcAddress(library_, kCbtProcName);
  if (address == nullptr) address = GetProcAddress(library_, kDecoratedCbtProcName);
  if (address == nullptr) address = GetProcAddress(library_, MAKEINTRESOURCEA(1));
  auto* procedure = reinterpret_cast<HookProc>(address);
  if (procedure == nullptr) {
    core::LogDebug(L"CbtHook", L"GetProcAddress failed for CBTProc");
    Uninstall();
    return false;
  }

  hook_ = SetWindowsHookExW(WH_CBT, procedure, library_, 0);
  if (hook_ == nullptr) {
    core::LogDebug(L"CbtHook", L"SetWindowsHookExW failed");
    Uninstall();
    return false;
  }

  FARPROC call_window_address = GetProcAddress(library_, kCallWndProcName);
  if (call_window_address == nullptr) {
    call_window_address = GetProcAddress(library_, kDecoratedCallWndProcName);
  }
  auto* call_window_procedure = reinterpret_cast<HookProc>(call_window_address);
  if (call_window_procedure == nullptr) {
    core::LogDebug(L"CbtHook", L"GetProcAddress failed for CallWndProc");
    Uninstall();
    return false;
  }
  call_window_hook_ = SetWindowsHookExW(WH_CALLWNDPROC, call_window_procedure, library_, 0);
  if (call_window_hook_ == nullptr) {
    core::LogDebug(L"CbtHook", L"SetWindowsHookExW failed for CallWndProc");
    Uninstall();
    return false;
  }
  core::LogDebug(L"CbtHook", L"CBT and CallWndProc hooks installed");
  return true;
}

void CbtHookManager::Uninstall() {
  if (call_window_hook_ != nullptr) {
    UnhookWindowsHookEx(call_window_hook_);
    call_window_hook_ = nullptr;
  }
  if (hook_ != nullptr) {
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  if (library_ != nullptr) {
    FreeLibrary(library_);
    library_ = nullptr;
  }
}

}  // namespace minimize::platform::windows
