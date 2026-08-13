#include "pch.hpp"

#include "platform/windows/taskbar_target_provider.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <oleauto.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <wil/resource.h>
#include <winver.h>

#include "core/logger.hpp"
#include "platform/windows/taskbar_locator.hpp"

namespace minimize::platform {
namespace {

std::wstring ToLower(std::wstring str) {
  std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) { return std::towlower(c); });
  return str;
}

std::vector<std::wstring> Tokenize(const std::wstring& str) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (wchar_t c : str) {
    if (std::iswalnum(c)) {
      current += std::towlower(c);
    } else {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::wstring GetProcessDescription(const std::wstring& process_path) {
  DWORD handle = 0;
  DWORD size = GetFileVersionInfoSizeW(process_path.c_str(), &handle);
  if (size == 0) {
    return {};
  }

  std::vector<BYTE> buffer(size);
  if (!GetFileVersionInfoW(process_path.c_str(), 0, size, buffer.data())) {
    return {};
  }

  struct Translation {
    WORD language;
    WORD codepage;
  }* translations = nullptr;
  UINT len = 0;

  if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                      reinterpret_cast<LPVOID*>(&translations), &len) ||
      len < sizeof(Translation)) {
    return {};
  }

  const std::wstring sub_block = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription",
                                             translations[0].language, translations[0].codepage);

  wchar_t* description = nullptr;
  UINT desc_len = 0;
  if (VerQueryValueW(buffer.data(), sub_block.c_str(), reinterpret_cast<LPVOID*>(&description),
                     &desc_len) &&
      description != nullptr) {
    return std::wstring(description, desc_len);
  }

  return {};
}

struct DescendantClassSearch {
  const wchar_t* class_name = nullptr;
  HWND result = nullptr;
};

BOOL CALLBACK FindDescendantClassCallback(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<DescendantClassSearch*>(parameter);
  wchar_t class_name[128]{};
  if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) > 0 &&
      _wcsicmp(class_name, search->class_name) == 0) {
    search->result = window;
    return FALSE;
  }
  return TRUE;
}

HWND FindDescendantByClass(HWND parent, const wchar_t* class_name) {
  if (parent == nullptr || class_name == nullptr) return nullptr;
  DescendantClassSearch search{.class_name = class_name};
  EnumChildWindows(parent, FindDescendantClassCallback, reinterpret_cast<LPARAM>(&search));
  return search.result;
}

bool ContainsPoint(const RECT& rect, LONG x, LONG y) {
  return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool IsTaskbarButtonCandidate(const RECT& candidate, const RECT& taskbar,
                              const RECT* notification_area) {
  RECT intersection{};
  if (!IntersectRect(&intersection, &candidate, &taskbar)) return false;

  const std::int64_t width = candidate.right - candidate.left;
  const std::int64_t height = candidate.bottom - candidate.top;
  const std::int64_t area = width * height;
  const std::int64_t intersection_area =
      static_cast<std::int64_t>(intersection.right - intersection.left) *
      static_cast<std::int64_t>(intersection.bottom - intersection.top);
  if (area <= 0 || intersection_area * 10 < area * 8) return false;

  // The notification area contains the chevron, status icons, clock and "Show desktop".
  // Those controls can share words with a window title, but are never app task buttons.
  const LONG center_x = candidate.left + static_cast<LONG>(width / 2);
  const LONG center_y = candidate.top + static_cast<LONG>(height / 2);
  return notification_area == nullptr || !ContainsPoint(*notification_area, center_x, center_y);
}

struct WindowMatchContext {
  std::wstring title;
  std::vector<std::wstring> title_tokens;
  std::wstring class_name;
  std::wstring process_no_ext;
  std::vector<std::wstring> process_tokens;
  std::wstring description_lower;
  std::vector<std::wstring> description_tokens;
};

struct TaskbarButtonSearch {
  HWND window = nullptr;
  HWND root_owner = nullptr;
  const WindowMatchContext& window_match;
  const RECT& taskbar_rect;
  const RECT* notification_rect = nullptr;
};

struct TaskbarButtonMatch {
  int score = -1;
  RECT rect{};
};

int CalculateTokenMatches(const std::vector<std::wstring>& button_tokens,
                          const WindowMatchContext& window_match) {
  int token_matches = 0;
  for (const std::wstring& token : button_tokens) {
    if (token.length() < 2) continue;
    if (token == L"the" || token == L"and" || token == L"new" || token == L"tab" ||
        token == L"window") {
      continue;
    }

    if (!window_match.title.empty() &&
        std::find(window_match.title_tokens.begin(), window_match.title_tokens.end(), token) !=
            window_match.title_tokens.end()) {
      token_matches++;
    }
    if (!window_match.process_no_ext.empty() &&
        std::find(window_match.process_tokens.begin(), window_match.process_tokens.end(), token) !=
            window_match.process_tokens.end()) {
      token_matches += 2;
    }
    if (!window_match.description_lower.empty() &&
        std::find(window_match.description_tokens.begin(), window_match.description_tokens.end(),
                  token) != window_match.description_tokens.end()) {
      token_matches += 2;
    }
  }
  return token_matches;
}

int CalculateNameScore(const std::wstring& button_name,
                       const std::vector<std::wstring>& button_tokens,
                       const WindowMatchContext& window_match) {
  const std::wstring normalized_name = ToLower(button_name);
  if (normalized_name.empty()) return 0;

  // Scores are ordered by confidence: exact names, contained names, token overlap, then class.
  if (!window_match.title.empty() && normalized_name == window_match.title) return 100;
  if (!window_match.process_no_ext.empty() && normalized_name == window_match.process_no_ext)
    return 95;
  if (!window_match.description_lower.empty() && normalized_name == window_match.description_lower)
    return 93;
  if (!window_match.title.empty() && window_match.title.find(normalized_name) != std::wstring::npos)
    return 90;
  if (!window_match.title.empty() && normalized_name.find(window_match.title) != std::wstring::npos)
    return 85;
  if (!window_match.description_lower.empty() &&
      window_match.description_lower.find(normalized_name) != std::wstring::npos)
    return 83;
  if (!window_match.description_lower.empty() &&
      normalized_name.find(window_match.description_lower) != std::wstring::npos)
    return 82;
  if (!window_match.process_no_ext.empty() &&
      normalized_name.find(window_match.process_no_ext) != std::wstring::npos)
    return 80;
  if (!window_match.process_no_ext.empty() &&
      window_match.process_no_ext.find(normalized_name) != std::wstring::npos)
    return 75;

  const int token_matches = CalculateTokenMatches(button_tokens, window_match);
  if (token_matches > 0) return 50 + token_matches * 5;

  for (const std::wstring& token : button_tokens) {
    if (token.length() >= 3 && window_match.class_name.find(token) != std::wstring::npos) {
      return 40;
    }
  }
  return 0;
}

int CalculateButtonScore(IUIAutomationElement* element, const std::wstring& button_name,
                         HWND window, HWND root_owner, const WindowMatchContext& window_match) {
  UIA_HWND element_window = nullptr;
  // A native HWND/owner match is definitive and outranks all textual heuristics.
  if (SUCCEEDED(element->get_CurrentNativeWindowHandle(&element_window)) &&
      element_window != nullptr) {
    HWND item_hwnd = reinterpret_cast<HWND>(element_window);
    if (item_hwnd == window || item_hwnd == root_owner ||
        GetAncestor(item_hwnd, GA_ROOTOWNER) == root_owner) {
      return 1000;
    }
  }

  const std::vector<std::wstring> button_tokens = Tokenize(button_name);
  return CalculateNameScore(button_name, button_tokens, window_match);
}

Microsoft::WRL::ComPtr<IUIAutomationCondition> CreateTaskbarQueryCondition(
    IUIAutomation* automation) {
  Microsoft::WRL::ComPtr<IUIAutomationCondition> button_condition;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> list_condition;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> tab_condition;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> app_item_condition;
  Microsoft::WRL::ComPtr<IUIAutomationCondition> combined_condition;

  VARIANT var;
  VariantInit(&var);
  var.vt = VT_I4;

  // App entries can be buttons, list items, or tab items depending on the Windows shell version.
  var.lVal = UIA_ButtonControlTypeId;
  automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var,
                                      button_condition.GetAddressOf());
  var.lVal = UIA_ListItemControlTypeId;
  automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var,
                                      list_condition.GetAddressOf());
  var.lVal = UIA_TabItemControlTypeId;
  automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, tab_condition.GetAddressOf());

  if (button_condition && list_condition) {
    automation->CreateOrCondition(button_condition.Get(), list_condition.Get(),
                                  app_item_condition.GetAddressOf());
  } else {
    app_item_condition = button_condition ? button_condition : list_condition;
  }

  if (app_item_condition && tab_condition) {
    automation->CreateOrCondition(app_item_condition.Get(), tab_condition.Get(),
                                  combined_condition.GetAddressOf());
  } else {
    combined_condition = app_item_condition ? app_item_condition : tab_condition;
  }

  return combined_condition;
}

WindowMatchContext BuildWindowMatchContext(HWND window) {
  wchar_t raw_title[512]{};
  GetWindowTextW(window, raw_title, static_cast<int>(std::size(raw_title)));

  wchar_t raw_class[256]{};
  GetClassNameW(window, raw_class, static_cast<int>(std::size(raw_class)));

  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  std::wstring process_name;
  std::wstring process_description;
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (process) {
    wchar_t path[MAX_PATH]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
      wchar_t* filename = wcsrchr(path, L'\\');
      process_name = filename != nullptr ? filename + 1 : path;
      process_description = GetProcessDescription(path);
    }
  }

  std::wstring process_no_ext = ToLower(process_name);
  size_t dot = process_no_ext.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    process_no_ext = process_no_ext.substr(0, dot);
  }

  return WindowMatchContext{
      .title = ToLower(raw_title),
      .title_tokens = Tokenize(raw_title),
      .class_name = ToLower(raw_class),
      .process_no_ext = process_no_ext,
      .process_tokens = Tokenize(process_no_ext),
      .description_lower = ToLower(process_description),
      .description_tokens = Tokenize(process_description),
  };
}

TaskbarButtonMatch FindBestTaskbarButton(IUIAutomationElementArray* elements,
                                         const TaskbarButtonSearch& search) {
  TaskbarButtonMatch best;
  int length = 0;
  elements->get_Length(&length);
  for (int i = 0; i < length; ++i) {
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    elements->GetElement(i, element.GetAddressOf());
    if (!element) continue;

    BSTR name = nullptr;
    RECT rect{};
    element->get_CurrentName(&name);
    element->get_CurrentBoundingRectangle(&rect);
    std::wstring button_name = name != nullptr ? name : L"";
    if (name != nullptr) SysFreeString(name);

    if (!IsTaskbarButtonCandidate(rect, search.taskbar_rect, search.notification_rect)) continue;

    const int score = CalculateButtonScore(element.Get(), button_name, search.window,
                                           search.root_owner, search.window_match);
    minimize::core::LogTrace(
        L"UIAutomation", L"Checking button: '" + button_name + L"' rect=" +
                             std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L"," +
                             std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom) +
                             L" score=" + std::to_wstring(score));

    if (score > best.score) {
      best.score = score;
      best.rect = rect;
    }

    if (best.score >= 1000) break;
  }
  return best;
}

bool FindTaskbarIconUIAutomation(HWND window, const RECT& window_rect, RECT* out_rect) {
  if (window == nullptr || !IsWindow(window) || out_rect == nullptr) return false;

  const WindowMatchContext window_match = BuildWindowMatchContext(window);

  HWND root_owner = GetAncestor(window, GA_ROOTOWNER);
  if (root_owner == nullptr) root_owner = window;

  HWND target_taskbar = FindTaskbarWindowForRect(window_rect);
  if (target_taskbar == nullptr) return false;

  // Search only the taskbar on this window's monitor; another display may contain the same pin.

  Microsoft::WRL::ComPtr<IUIAutomation> automation;
  HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.GetAddressOf()));
  if (FAILED(hr) || !automation) return false;

  Microsoft::WRL::ComPtr<IUIAutomationElement> taskbar_element;
  hr = automation->ElementFromHandle(target_taskbar, taskbar_element.GetAddressOf());
  if (FAILED(hr) || !taskbar_element) return false;

  RECT taskbar_rect{};
  if (!GetWindowRect(target_taskbar, &taskbar_rect)) return false;

  RECT notification_rect{};
  const HWND notification_area = FindDescendantByClass(target_taskbar, L"TrayNotifyWnd");
  const bool has_notification_rect =
      notification_area != nullptr && GetWindowRect(notification_area, &notification_rect);

  Microsoft::WRL::ComPtr<IUIAutomationCondition> condition =
      CreateTaskbarQueryCondition(automation.Get());
  if (!condition) return false;

  Microsoft::WRL::ComPtr<IUIAutomationElementArray> elements;
  hr = taskbar_element->FindAll(TreeScope_Subtree, condition.Get(), elements.GetAddressOf());
  if (FAILED(hr) || !elements) return false;

  const TaskbarButtonSearch search{
      .window = window,
      .root_owner = root_owner,
      .window_match = window_match,
      .taskbar_rect = taskbar_rect,
      .notification_rect = has_notification_rect ? &notification_rect : nullptr,
  };
  const TaskbarButtonMatch best = FindBestTaskbarButton(elements.Get(), search);

  constexpr int kMinimumReliableScore = 50;
  if (best.score >= kMinimumReliableScore) {
    minimize::core::LogTrace(
        L"UIAutomation",
        L"Matched best button for title='" + window_match.title + L"' proc='" +
            window_match.process_no_ext + L"' with score=" + std::to_wstring(best.score) +
            L" rect=" + std::to_wstring(best.rect.left) + L"," + std::to_wstring(best.rect.top) +
            L"," + std::to_wstring(best.rect.right) + L"," + std::to_wstring(best.rect.bottom));
    *out_rect = best.rect;
    return true;
  }

  minimize::core::LogTrace(L"UIAutomation", L"Failed to match any button for title='" +
                                                window_match.title + L"' proc='" +
                                                window_match.process_no_ext + L"'");
  return false;
}

minimize::animation::RectF ToRectF(const RECT& rect) {
  return minimize::animation::RectF{
      .left = static_cast<float>(rect.left),
      .top = static_cast<float>(rect.top),
      .right = static_cast<float>(rect.right),
      .bottom = static_cast<float>(rect.bottom),
  };
}

RECT EstimateTaskbarRect(const MONITORINFO& monitor_info) {
  const RECT& monitor = monitor_info.rcMonitor;
  const RECT& work_area = monitor_info.rcWork;
  if (work_area.bottom < monitor.bottom) {
    return RECT{monitor.left, work_area.bottom, monitor.right, monitor.bottom};
  }
  if (work_area.top > monitor.top) {
    return RECT{monitor.left, monitor.top, monitor.right, work_area.top};
  }
  if (work_area.left > monitor.left) {
    return RECT{monitor.left, monitor.top, work_area.left, monitor.bottom};
  }
  if (work_area.right < monitor.right) {
    return RECT{work_area.right, monitor.top, monitor.right, monitor.bottom};
  }
  return RECT{monitor.left, monitor.bottom - 48, monitor.right, monitor.bottom};
}

float Clamp(float value, float min_value, float max_value) {
  if (min_value > max_value) {
    return (min_value + max_value) * 0.5f;
  }
  return std::clamp(value, min_value, max_value);
}

bool IsTaskbarMostlyVisible(HWND taskbar, const RECT& monitor_rect) {
  RECT taskbar_rect{};
  RECT visible_rect{};
  if (taskbar == nullptr || !GetWindowRect(taskbar, &taskbar_rect) ||
      !IntersectRect(&visible_rect, &taskbar_rect, &monitor_rect)) {
    return false;
  }

  const LONG taskbar_width = taskbar_rect.right - taskbar_rect.left;
  const LONG taskbar_height = taskbar_rect.bottom - taskbar_rect.top;
  const LONG visible_width = visible_rect.right - visible_rect.left;
  const LONG visible_height = visible_rect.bottom - visible_rect.top;
  if (taskbar_width <= 0 || taskbar_height <= 0) return false;
  return taskbar_width >= taskbar_height ? visible_height * 10 >= taskbar_height * 9
                                         : visible_width * 10 >= taskbar_width * 9;
}

}  // namespace

TaskbarTargetProvider::~TaskbarTargetProvider() { RestoreAutoHideTaskbar(); }

bool TaskbarTargetProvider::RevealAutoHideTaskbarForWindow(const RECT& window_rect) {
  if (auto_hide_temporarily_disabled_) {
    ++auto_hide_reveal_count_;
    auto_hide_restore_deadline_ms_ = 0;
    return true;
  }

  HWND primary_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (primary_taskbar == nullptr) return false;

  APPBARDATA appbar_data{};
  appbar_data.cbSize = sizeof(appbar_data);
  appbar_data.hWnd = primary_taskbar;
  const UINT_PTR current_state = SHAppBarMessage(ABM_GETSTATE, &appbar_data);
  if ((current_state & ABS_AUTOHIDE) == 0) return false;

  original_taskbar_state_ = current_state;
  appbar_data.lParam = static_cast<LPARAM>(current_state & ~ABS_AUTOHIDE);
  if (SHAppBarMessage(ABM_SETSTATE, &appbar_data) == FALSE) {
    original_taskbar_state_ = 0;
    return false;
  }

  auto_hide_temporarily_disabled_ = true;
  auto_hide_reveal_count_ = 1;

  HWND target_taskbar = FindTaskbarWindowForRect(window_rect);
  HMONITOR monitor = MonitorFromRect(&window_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (target_taskbar != nullptr && monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
    constexpr ULONGLONG kRevealTimeoutMs = 400;
    const ULONGLONG deadline = GetTickCount64() + kRevealTimeoutMs;
    while (!IsTaskbarMostlyVisible(target_taskbar, monitor_info.rcMonitor) &&
           GetTickCount64() < deadline) {
      DwmFlush();
      Sleep(8);
    }
  }

  core::LogTrace(L"Taskbar", L"Temporarily revealed auto-hide taskbar for minimize");
  return true;
}

void TaskbarTargetProvider::ReleaseAutoHideTaskbar() {
  if (auto_hide_reveal_count_ == 0) return;
  --auto_hide_reveal_count_;
  if (auto_hide_reveal_count_ == 0) {
    constexpr ULONGLONG kAutoHideRestoreDelayMs = 500;
    auto_hide_restore_deadline_ms_ = GetTickCount64() + kAutoHideRestoreDelayMs;
    core::LogTrace(L"Taskbar", L"Keeping auto-hide taskbar visible for 500 ms after minimize");
  }
}

void TaskbarTargetProvider::UpdateAutoHideTaskbarRestore() {
  if (auto_hide_restore_deadline_ms_ != 0 && GetTickCount64() >= auto_hide_restore_deadline_ms_) {
    RestoreAutoHideTaskbar();
  }
}

void TaskbarTargetProvider::RestoreAutoHideTaskbar() {
  auto_hide_reveal_count_ = 0;
  auto_hide_restore_deadline_ms_ = 0;
  if (!auto_hide_temporarily_disabled_) return;

  HWND primary_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (primary_taskbar == nullptr) return;

  APPBARDATA appbar_data{};
  appbar_data.cbSize = sizeof(appbar_data);
  appbar_data.hWnd = primary_taskbar;
  appbar_data.lParam = static_cast<LPARAM>(original_taskbar_state_);
  SHAppBarMessage(ABM_SETSTATE, &appbar_data);
  auto_hide_temporarily_disabled_ = false;
  original_taskbar_state_ = 0;
  core::LogTrace(L"Taskbar", L"Restored auto-hide taskbar after minimize");
}

TaskbarTarget TaskbarTargetProvider::GetTargetForWindow(HWND window,
                                                        const RECT& window_rect) const {
  RECT taskbar_rect{};

  HMONITOR monitor = MonitorFromRect(&window_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfoW(monitor, &monitor_info)) {
    monitor_info.rcMonitor =
        RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    monitor_info.rcWork = monitor_info.rcMonitor;
  }

  RECT matched_rect{};
  const bool has_matched_button =
      FindTaskbarIconUIAutomation(window, window_rect, &matched_rect);

  if (!has_matched_button) {
    HWND taskbar_hwnd = FindTaskbarWindowForRect(window_rect);
    if (taskbar_hwnd == nullptr || !GetWindowRect(taskbar_hwnd, &taskbar_rect)) {
      taskbar_rect = EstimateTaskbarRect(monitor_info);
    }
  }

  minimize::animation::MinimizeEdge edge = minimize::animation::MinimizeEdge::kBottom;
  minimize::animation::RectF target{};

  RECT reference_taskbar = has_matched_button ? matched_rect : taskbar_rect;
  if (has_matched_button) {
    HWND taskbar_hwnd = FindTaskbarWindowForRect(window_rect);
    if (taskbar_hwnd == nullptr || !GetWindowRect(taskbar_hwnd, &taskbar_rect)) {
      taskbar_rect = reference_taskbar;
    }
  }

  const float tb_width = static_cast<float>(taskbar_rect.right - taskbar_rect.left);
  const float tb_height = static_cast<float>(taskbar_rect.bottom - taskbar_rect.top);
  const float monitor_height =
      static_cast<float>(monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top);

  if (tb_width >= tb_height) {
    // Horizontal taskbar: either top or bottom
    if (taskbar_rect.top < monitor_info.rcMonitor.top + monitor_height / 2.0f) {
      edge = minimize::animation::MinimizeEdge::kTop;
    } else {
      edge = minimize::animation::MinimizeEdge::kBottom;
    }
  } else {
    // Vertical taskbar: either left or right
    const float monitor_width =
        static_cast<float>(monitor_info.rcMonitor.right - monitor_info.rcMonitor.left);
    if (taskbar_rect.left < monitor_info.rcMonitor.left + monitor_width / 2.0f) {
      edge = minimize::animation::MinimizeEdge::kLeft;
    } else {
      edge = minimize::animation::MinimizeEdge::kRight;
    }
  }

  if (has_matched_button) {
    target = ToRectF(matched_rect);
  } else {
    const HWND dpi_window = FindTaskbarWindowForRect(window_rect);
    const bool taskbar_is_on_target_monitor =
        dpi_window != nullptr && MonitorFromWindow(dpi_window, MONITOR_DEFAULTTONEAREST) == monitor;
    const UINT target_dpi =
        taskbar_is_on_target_monitor ? GetDpiForWindow(dpi_window) : GetDpiForWindow(window);
    const float dpi_scale = static_cast<float>(std::max(target_dpi, 96U)) / USER_DEFAULT_SCREEN_DPI;
    const float target_width = 72.0f * dpi_scale;
    const float target_height = 48.0f * dpi_scale;
    const minimize::animation::RectF taskbar = ToRectF(taskbar_rect);

    if (tb_width >= tb_height) {
      const float center_x = static_cast<float>(taskbar_rect.left + taskbar_rect.right) * 0.5f;
      target.left = center_x - (target_width * 0.5f);
      target.right = center_x + (target_width * 0.5f);
      target.top = taskbar.top;
      target.bottom = taskbar.bottom;
    } else {
      const float center_y = static_cast<float>(taskbar_rect.top + taskbar_rect.bottom) * 0.5f;
      target.top = center_y - (target_height * 0.5f);
      target.bottom = center_y + (target_height * 0.5f);
      target.left = taskbar.left;
      target.right = taskbar.right;
    }
  }

  return TaskbarTarget{
      .rect = target,
      .edge = edge,
  };
}

RECT TaskbarTargetProvider::GetShellTaskbarRect() const {
  APPBARDATA appbar_data{};
  appbar_data.cbSize = sizeof(appbar_data);
  if (SHAppBarMessage(ABM_GETTASKBARPOS, &appbar_data) != FALSE) {
    return appbar_data.rc;
  }

  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  return RECT{
      .left = virtual_left,
      .top = work_area.bottom,
      .right = virtual_left + virtual_width,
      .bottom = virtual_top + virtual_height,
  };
}

}  // namespace minimize::platform
