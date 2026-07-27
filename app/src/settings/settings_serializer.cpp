#include "pch.hpp"

#include "settings/settings_serializer.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "settings/exclusion_rules.hpp"

namespace minimize::settings {
namespace {

constexpr float kMinimumDuration = 0.10f;
constexpr float kMaximumDuration = 2.00f;
constexpr size_t kMaximumJsonNestingDepth = 64;
constexpr std::uint32_t kSupportedHotkeyModifiers = 0x000fu;

struct HotkeyField {
  size_t action_index;
  bool is_modifier;
};

std::optional<HotkeyField> FindHotkeyField(std::string_view key) {
  constexpr std::array prefixes = {
      std::string_view{"toggleEffectHotkey"},      std::string_view{"openSettingsHotkey"},
      std::string_view{"repairWindowsHotkey"},     std::string_view{"minimizeAllWindowsHotkey"},
      std::string_view{"restoreAllWindowsHotkey"},
  };
  for (size_t index = 0; index < prefixes.size(); ++index) {
    if (key.starts_with(prefixes[index]) && key.substr(prefixes[index].size()) == "Modifiers") {
      return HotkeyField{.action_index = index, .is_modifier = true};
    }
    if (key.starts_with(prefixes[index]) && key.substr(prefixes[index].size()) == "Key") {
      return HotkeyField{.action_index = index, .is_modifier = false};
    }
  }
  return std::nullopt;
}

bool IsValidEasingName(std::string_view value) {
  constexpr std::array names = {
      std::string_view{"Linear"},      std::string_view{"Ease In"}, std::string_view{"Ease Out"},
      std::string_view{"Ease In Out"}, std::string_view{"Cubic"},   std::string_view{"Back"},
      std::string_view{"Elastic"},     std::string_view{"Custom"},
  };
  return std::find(names.begin(), names.end(), value) != names.end();
}

bool IsValidAnimationStyle(std::string_view value) {
  return value == "Genie classic" || value == "Genie curvy" || value == "Squash" ||
         value == "Classic Minimize";
}

void AppendUtf8(std::string* output, unsigned int code_point) {
  if (code_point <= 0x7f) {
    output->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

class SettingsJsonParser {
public:
  explicit SettingsJsonParser(std::string_view json) : json_(json) {}

  bool Parse(AppSettings* settings) {
    SkipWhitespace();
    if (!Consume('{')) return false;
    SkipWhitespace();
    if (Consume('}')) return AtEnd();

    while (true) {
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (!Consume(':')) return false;
      SkipWhitespace();

      const size_t value_start = position_;
      bool value_valid = false;
      if (key == "enabled") {
        bool value = false;
        value_valid = ParseBoolean(&value);
        if (value_valid) settings->enabled = value;
      } else if (key == "minimizeDuration" || key == "restoreDuration") {
        double value = 0.0;
        value_valid = ParseNumber(&value);
        if (value_valid && std::isfinite(value) && value >= kMinimumDuration &&
            value <= kMaximumDuration) {
          if (key == "minimizeDuration") {
            settings->minimize_duration = static_cast<float>(value);
          } else {
            settings->restore_duration = static_cast<float>(value);
          }
        }
      } else if (key == "minimizeStrength") {
        double value = 0.0;
        value_valid = ParseNumber(&value);
        if (value_valid && std::isfinite(value) && value >= 0.25 && value <= 1.0) {
          settings->minimize_strength = static_cast<float>(value);
        }
      } else if (key == "fadeStrength") {
        std::string value;
        value_valid = ParseString(&value);
        if (value_valid && (value == "No fade" || value == "Subtle" || value == "Strong")) {
          settings->fade_strength = std::move(value);
        }
      } else if (key == "linkSpeeds" || key == "adaptiveDuration" ||
                 key == "disableAnimationsFullscreen" || key == "reduceEffectsOnBattery" ||
                 key == "disableEffectsBatterySaver" || key == "showTargetIndicator" ||
                 key == "smartSkipUnderLoad" || key == "followWindowsAnimationPreference" ||
                 key == "startMinimized" || key == "runAtStartup") {
        bool value = false;
        value_valid = ParseBoolean(&value);
        if (value_valid) {
          if (key == "linkSpeeds") settings->link_speeds = value;
          if (key == "disableAnimationsFullscreen") {
            settings->disable_animations_fullscreen = value;
          }
          if (key == "disableEffectsBatterySaver") {
            settings->disable_effects_battery_saver = value;
          }
          if (key == "showTargetIndicator") settings->show_target_indicator = value;
          if (key == "smartSkipUnderLoad") settings->smart_skip_under_load = value;
          if (key == "startMinimized") settings->start_minimized = value;
          if (key == "runAtStartup") settings->run_at_startup = value;
        }
      } else if (key == "closeBehavior") {
        std::string value;
        value_valid = ParseString(&value);
        if (value_valid && (value == "exit" || value == "tray")) {
          settings->close_behavior = std::move(value);
        }
      } else if (key == "minimizeEasing" || key == "restoreEasing") {
        std::string value;
        value_valid = ParseString(&value);
        if (value_valid && IsValidEasingName(value)) {
          if (key == "minimizeEasing")
            settings->minimize_easing = std::move(value);
          else
            settings->restore_easing = std::move(value);
        }
      } else if (key == "minimizeCustomBezier" || key == "restoreCustomBezier") {
        animation::CubicBezier bezier;
        value_valid = ParseCubicBezier(&bezier);
        if (value_valid) {
          if (key == "minimizeCustomBezier")
            settings->minimize_custom_bezier = bezier;
          else
            settings->restore_custom_bezier = bezier;
        }
      } else if (key == "animationStyle") {
        std::string value;
        value_valid = ParseString(&value);
        if (value_valid && IsValidAnimationStyle(value)) {
          settings->animation_style = std::move(value);
        }
      } else if (key == "qualityMode") {
        std::string value;
        value_valid = ParseString(&value);
        if (value_valid &&
            (value == "automatic" || value == "best_quality" || value == "power_saving")) {
          settings->quality_mode = std::move(value);
        }
      } else if (key == "excludedApplications") {
        std::vector<std::string> values;
        value_valid = ParseStringArray(&values);
        if (value_valid) settings->excluded_applications = std::move(values);
      } else if (key == "excludedDisplays") {
        std::vector<std::string> values;
        value_valid = ParseStringArray(&values);
        if (value_valid) settings->excluded_displays = std::move(values);
      } else if (const std::optional<HotkeyField> field = FindHotkeyField(key); field.has_value()) {
        double value = 0.0;
        value_valid = ParseNumber(&value);
        if (value_valid && std::isfinite(value) && value == std::floor(value)) {
          if (field->is_modifier && value >= 0.0 &&
              value <= static_cast<double>(kSupportedHotkeyModifiers)) {
            settings->hotkeys[field->action_index].modifiers = static_cast<std::uint32_t>(value);
          } else if (!field->is_modifier && value >= 0.0 && value <= 254.0) {
            settings->hotkeys[field->action_index].virtual_key = static_cast<std::uint32_t>(value);
          }
        }
      } else {
        value_valid = SkipValue();
      }

      if (!value_valid) {
        position_ = value_start;
        if (!SkipValue()) return false;
      }

      SkipWhitespace();
      if (Consume('}')) return AtEnd();
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
  }

private:
  bool AtEnd() {
    SkipWhitespace();
    return position_ == json_.size();
  }

  void SkipWhitespace() {
    while (position_ < json_.size() && (json_[position_] == ' ' || json_[position_] == '\t' ||
                                        json_[position_] == '\r' || json_[position_] == '\n')) {
      ++position_;
    }
  }

  bool Consume(char expected) {
    if (position_ >= json_.size() || json_[position_] != expected) return false;
    ++position_;
    return true;
  }

  bool ParseString(std::string* output) {
    if (!Consume('"')) return false;
    output->clear();
    while (position_ < json_.size()) {
      const unsigned char ch = static_cast<unsigned char>(json_[position_++]);
      if (ch == '"') return true;
      if (ch < 0x20) return false;
      if (ch != '\\') {
        output->push_back(static_cast<char>(ch));
        continue;
      }
      if (position_ >= json_.size()) return false;
      const char escaped = json_[position_++];
      switch (escaped) {
        case '"':
          output->push_back('"');
          break;
        case '\\':
          output->push_back('\\');
          break;
        case '/':
          output->push_back('/');
          break;
        case 'b':
          output->push_back('\b');
          break;
        case 'f':
          output->push_back('\f');
          break;
        case 'n':
          output->push_back('\n');
          break;
        case 'r':
          output->push_back('\r');
          break;
        case 't':
          output->push_back('\t');
          break;
        case 'u': {
          if (position_ + 4 > json_.size()) return false;
          unsigned int code_point = 0;
          for (int i = 0; i < 4; ++i) {
            const char hex = json_[position_++];
            code_point <<= 4;
            if (hex >= '0' && hex <= '9')
              code_point += hex - '0';
            else if (hex >= 'a' && hex <= 'f')
              code_point += hex - 'a' + 10;
            else if (hex >= 'A' && hex <= 'F')
              code_point += hex - 'A' + 10;
            else
              return false;
          }
          AppendUtf8(output, code_point);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool ParseBoolean(bool* output) {
    if (json_.substr(position_, 4) == "true") {
      position_ += 4;
      *output = true;
      return true;
    }
    if (json_.substr(position_, 5) == "false") {
      position_ += 5;
      *output = false;
      return true;
    }
    return false;
  }

  bool ParseNumber(double* output) {
    const size_t start = position_;
    if (position_ < json_.size() && json_[position_] == '-') ++position_;
    if (position_ >= json_.size()) return false;
    if (json_[position_] == '0') {
      ++position_;
    } else {
      if (json_[position_] < '1' || json_[position_] > '9') return false;
      while (position_ < json_.size() && json_[position_] >= '0' && json_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < json_.size() && json_[position_] == '.') {
      ++position_;
      const size_t fraction_start = position_;
      while (position_ < json_.size() && json_[position_] >= '0' && json_[position_] <= '9') {
        ++position_;
      }
      if (fraction_start == position_) return false;
    }
    if (position_ < json_.size() && (json_[position_] == 'e' || json_[position_] == 'E')) {
      ++position_;
      if (position_ < json_.size() && (json_[position_] == '+' || json_[position_] == '-')) {
        ++position_;
      }
      const size_t exponent_start = position_;
      while (position_ < json_.size() && json_[position_] >= '0' && json_[position_] <= '9') {
        ++position_;
      }
      if (exponent_start == position_) return false;
    }
    const std::string token(json_.substr(start, position_ - start));
    char* end = nullptr;
    errno = 0;
    *output = std::strtod(token.c_str(), &end);
    return errno != ERANGE && end == token.c_str() + token.size();
  }

  bool ParseStringArray(std::vector<std::string>* output) {
    if (!Consume('[')) return false;
    SkipWhitespace();
    if (Consume(']')) return true;
    while (true) {
      std::string value;
      if (!ParseString(&value)) return false;
      output->push_back(std::move(value));
      SkipWhitespace();
      if (Consume(']')) return true;
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
  }

  bool ParseFloatArray(std::vector<float>* output, size_t expected = 0) {
    if (!Consume('[')) return false;
    SkipWhitespace();
    output->clear();
    if (Consume(']')) return expected == 0;
    while (true) {
      double value = 0.0;
      if (!ParseNumber(&value) || !std::isfinite(value)) return false;
      output->push_back(static_cast<float>(value));
      SkipWhitespace();
      if (Consume(']')) {
        return expected == 0 || output->size() == expected;
      }
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
  }

  bool ParseCubicBezier(animation::CubicBezier* output) {
    std::vector<float> values;
    if (!ParseFloatArray(&values, 4) || values.size() != 4) return false;
    output->x1 = values[0];
    output->y1 = values[1];
    output->x2 = values[2];
    output->y2 = values[3];
    output->ClampHandles();
    return true;
  }

  bool SkipValue(size_t depth = 0) {
    SkipWhitespace();
    if (position_ >= json_.size()) return false;
    if (depth > kMaximumJsonNestingDepth) return false;
    if (json_[position_] == '"') {
      std::string ignored;
      return ParseString(&ignored);
    }
    if (json_[position_] == '{') {
      ++position_;
      SkipWhitespace();
      if (Consume('}')) return true;
      while (true) {
        std::string ignored;
        if (!ParseString(&ignored)) return false;
        SkipWhitespace();
        if (!Consume(':') || !SkipValue(depth + 1)) return false;
        SkipWhitespace();
        if (Consume('}')) return true;
        if (!Consume(',')) return false;
        SkipWhitespace();
      }
    }
    if (json_[position_] == '[') {
      ++position_;
      SkipWhitespace();
      if (Consume(']')) return true;
      while (true) {
        if (!SkipValue(depth + 1)) return false;
        SkipWhitespace();
        if (Consume(']')) return true;
        if (!Consume(',')) return false;
        SkipWhitespace();
      }
    }
    bool ignored_bool = false;
    if (ParseBoolean(&ignored_bool)) return true;
    if (json_.substr(position_, 4) == "null") {
      position_ += 4;
      return true;
    }
    double ignored_number = 0.0;
    return ParseNumber(&ignored_number);
  }

  std::string_view json_;
  size_t position_ = 0;
};

std::string EscapeJsonString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(kHex[ch >> 4]);
          escaped.push_back(kHex[ch & 0xf]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

}  // namespace

std::optional<AppSettings> SettingsSerializer::Deserialize(std::string_view json) {
  AppSettings loaded;
  SettingsJsonParser parser(json);
  if (!parser.Parse(&loaded)) return std::nullopt;
  NormalizeExcludedApplications(&loaded.excluded_applications);
  for (HotkeyBinding& binding : loaded.hotkeys) {
    binding.modifiers &= kSupportedHotkeyModifiers;
    if (binding.virtual_key == 0) binding.modifiers = 0;
  }
  if (loaded.animation_style == "Classic Minimize") {
    loaded.animation_style = "Genie classic";
  }
  // Preserve saved easing names (including Custom) and clamp custom handles.
  if (!IsValidEasingName(loaded.minimize_easing)) loaded.minimize_easing = "Ease In Out";
  if (!IsValidEasingName(loaded.restore_easing)) loaded.restore_easing = "Ease In Out";
  loaded.minimize_custom_bezier.ClampHandles();
  loaded.restore_custom_bezier.ClampHandles();
  return loaded;
}

std::string SettingsSerializer::Serialize(const AppSettings& settings) {
  std::ostringstream output;
  std::vector<std::string> excluded_applications = settings.excluded_applications;
  NormalizeExcludedApplications(&excluded_applications);
  output << std::boolalpha << std::fixed << std::setprecision(2);
  output << "{\n"
         << "  \"enabled\": " << settings.enabled << ",\n"
         << "  \"minimizeDuration\": " << settings.minimize_duration << ",\n"
         << "  \"restoreDuration\": " << settings.restore_duration << ",\n"
         << "  \"linkSpeeds\": " << settings.link_speeds << ",\n"
         << "  \"disableAnimationsFullscreen\": " << settings.disable_animations_fullscreen << ",\n"
         << "  \"disableEffectsBatterySaver\": " << settings.disable_effects_battery_saver << ",\n"
         << "  \"minimizeEasing\": \"" << EscapeJsonString(settings.minimize_easing) << "\",\n"
         << "  \"restoreEasing\": \"" << EscapeJsonString(settings.restore_easing) << "\",\n"
         << "  \"minimizeCustomBezier\": [" << settings.minimize_custom_bezier.x1 << ", "
         << settings.minimize_custom_bezier.y1 << ", " << settings.minimize_custom_bezier.x2 << ", "
         << settings.minimize_custom_bezier.y2 << "],\n"
         << "  \"restoreCustomBezier\": [" << settings.restore_custom_bezier.x1 << ", "
         << settings.restore_custom_bezier.y1 << ", " << settings.restore_custom_bezier.x2 << ", "
         << settings.restore_custom_bezier.y2 << "],\n"
         << "  \"animationStyle\": \"" << EscapeJsonString(settings.animation_style) << "\",\n"
         << "  \"qualityMode\": \"" << EscapeJsonString(settings.quality_mode) << "\",\n"
         << "  \"minimizeStrength\": " << settings.minimize_strength << ",\n"
         << "  \"fadeStrength\": \"" << EscapeJsonString(settings.fade_strength) << "\",\n"
         << "  \"showTargetIndicator\": " << settings.show_target_indicator << ",\n"
         << "  \"smartSkipUnderLoad\": " << settings.smart_skip_under_load << ",\n"
         << "  \"closeBehavior\": \"" << EscapeJsonString(settings.close_behavior) << "\",\n"
         << "  \"startMinimized\": " << settings.start_minimized << ",\n"
         << "  \"runAtStartup\": " << settings.run_at_startup << ",\n";
  constexpr std::array hotkey_names = {
      std::string_view{"toggleEffectHotkey"},      std::string_view{"openSettingsHotkey"},
      std::string_view{"repairWindowsHotkey"},     std::string_view{"minimizeAllWindowsHotkey"},
      std::string_view{"restoreAllWindowsHotkey"},
  };
  for (size_t index = 0; index < hotkey_names.size(); ++index) {
    const HotkeyBinding& binding = settings.hotkeys[index];
    output << "  \"" << hotkey_names[index] << "Modifiers\": " << binding.modifiers << ",\n"
           << "  \"" << hotkey_names[index] << "Key\": " << binding.virtual_key << ",\n";
  }
  output << "  \"excludedApplications\": [";
  for (size_t i = 0; i < excluded_applications.size(); ++i) {
    if (i != 0) output << ", ";
    output << '"' << EscapeJsonString(excluded_applications[i]) << '"';
  }
  output << "],\n";
  output << "  \"excludedDisplays\": [";
  for (size_t i = 0; i < settings.excluded_displays.size(); ++i) {
    if (i != 0) output << ", ";
    output << '"' << EscapeJsonString(settings.excluded_displays[i]) << '"';
  }
  output << "]\n}\n";
  return output.str();
}

}  // namespace minimize::settings
