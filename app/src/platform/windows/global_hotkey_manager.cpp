#include "pch.hpp"

#include "platform/windows/global_hotkey_manager.hpp"

namespace minimize::platform::windows {
namespace {

constexpr int kHotkeyBaseIdentifier = 4100;
constexpr std::uint32_t kSupportedModifiers = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

}  // namespace

GlobalHotkeyManager::~GlobalHotkeyManager() { UnregisterAll(); }

void GlobalHotkeyManager::SetWindow(HWND window) {
  if (window_ == window) return;
  UnregisterAll();
  window_ = window;
}

void GlobalHotkeyManager::UnregisterAll() {
  if (window_ != nullptr) {
    for (std::size_t index = 0; index < registered_.size(); ++index) {
      if (registered_[index]) {
        UnregisterHotKey(window_, kHotkeyBaseIdentifier + static_cast<int>(index));
      }
    }
  }
  registered_.fill(false);
}

namespace {

bool IsDuplicateBinding(
    const std::array<settings::HotkeyBinding,
                     static_cast<std::size_t>(settings::HotkeyAction::kCount)>& bindings,
    std::size_t index) {
  if (bindings[index].virtual_key == 0) return false;
  for (std::size_t previous = 0; previous < index; ++previous) {
    if (bindings[previous] == bindings[index]) return true;
  }
  return false;
}

}  // namespace

std::array<bool, static_cast<std::size_t>(settings::HotkeyAction::kCount)>
GlobalHotkeyManager::RegisterAll(
    const std::array<settings::HotkeyBinding,
                     static_cast<std::size_t>(settings::HotkeyAction::kCount)>& bindings) {
  UnregisterAll();
  std::array<bool, static_cast<std::size_t>(settings::HotkeyAction::kCount)> available{};
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    const bool duplicate = IsDuplicateBinding(bindings, index);
    available[index] =
        bindings[index].virtual_key == 0 ||
        (!duplicate && Register(static_cast<settings::HotkeyAction>(index), bindings[index]));
  }
  return available;
}

HotkeyRegistrationResult GlobalHotkeyManager::Replace(settings::HotkeyAction action,
                                                      settings::HotkeyBinding previous,
                                                      settings::HotkeyBinding replacement) {
  if (!IsValid(replacement)) return HotkeyRegistrationResult::kInvalid;
  const std::size_t index = static_cast<std::size_t>(action);
  if (index >= registered_.size()) return HotkeyRegistrationResult::kInvalid;
  if (registered_[index] && window_ != nullptr) {
    UnregisterHotKey(window_, Identifier(action));
    registered_[index] = false;
  }
  if (replacement.virtual_key == 0 || Register(action, replacement)) {
    return HotkeyRegistrationResult::kSuccess;
  }
  (void)Restore(action, previous);
  return HotkeyRegistrationResult::kUnavailable;
}

bool GlobalHotkeyManager::Restore(settings::HotkeyAction action, settings::HotkeyBinding binding) {
  if (binding.virtual_key == 0) return true;
  return Register(action, binding);
}

bool GlobalHotkeyManager::IsRegistered(settings::HotkeyAction action) const {
  const std::size_t index = static_cast<std::size_t>(action);
  return index < registered_.size() && registered_[index];
}

bool GlobalHotkeyManager::IsValid(settings::HotkeyBinding binding) {
  if (binding.virtual_key > 254 || (binding.modifiers & ~kSupportedModifiers) != 0) return false;
  return binding.virtual_key != VK_CONTROL && binding.virtual_key != VK_SHIFT &&
         binding.virtual_key != VK_MENU && binding.virtual_key != VK_LWIN &&
         binding.virtual_key != VK_RWIN;
}

int GlobalHotkeyManager::Identifier(settings::HotkeyAction action) {
  return kHotkeyBaseIdentifier + static_cast<int>(action);
}

bool GlobalHotkeyManager::Register(settings::HotkeyAction action, settings::HotkeyBinding binding) {
  const std::size_t index = static_cast<std::size_t>(action);
  if (window_ == nullptr || index >= registered_.size() || !IsValid(binding) ||
      binding.virtual_key == 0) {
    return false;
  }
  const bool registered =
      RegisterHotKey(window_, Identifier(action), binding.modifiers | MOD_NOREPEAT,
                     binding.virtual_key) != FALSE;
  registered_[index] = registered;
  return registered;
}

}  // namespace minimize::platform::windows
