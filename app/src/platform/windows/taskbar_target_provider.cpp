#include "pch.hpp"

#include "platform/windows/taskbar_target_provider.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <format>
#include <oleauto.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <winver.h>
#include <wil/resource.h>

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

  const std::wstring sub_block =
      std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription",
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
  EnumChildWindows(parent, FindDescendantClassCallback,
                   reinterpret_cast<LPARAM>(&search));
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
  return notification_area == nullptr ||
         !ContainsPoint(*notification_area, center_x, center_y);
}

bool FindTaskbarIconUIAutomation(HWND window, const RECT& window_rect, RECT* out_rect) {
  if (window == nullptr || !IsWindow(window)) {
    return false;
  }

  // 1. Retrieve window properties
  wchar_t raw_title[512]{};
  GetWindowTextW(window, raw_title, static_cast<int>(std::size(raw_title)));
  std::wstring title = ToLower(raw_title);
  std::vector<std::wstring> title_tokens = Tokenize(raw_title);

  wchar_t raw_class[256]{};
  GetClassNameW(window, raw_class, static_cast<int>(std::size(raw_class)));
  std::wstring class_name = ToLower(raw_class);
  std::vector<std::wstring> class_tokens = Tokenize(raw_class);

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
      if (filename != nullptr) {
        process_name = filename + 1;
      } else {
        process_name = path;
      }
      process_description = GetProcessDescription(path);
    }
  }

  std::wstring process_no_ext = ToLower(process_name);
  size_t dot = process_no_ext.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    process_no_ext = process_no_ext.substr(0, dot);
  }
  std::vector<std::wstring> process_tokens = Tokenize(process_no_ext);

  std::wstring description_lower = ToLower(process_description);
  std::vector<std::wstring> description_tokens = Tokenize(process_description);

  HWND root_owner = GetAncestor(window, GA_ROOTOWNER);
  if (root_owner == nullptr) root_owner = window;

  // 2. Search only the taskbar on the window's monitor. Searching every taskbar can select
  // a same-named pinned item on another display.
  std::vector<HWND> taskbar_windows;
  HWND target_taskbar = FindTaskbarWindowForRect(window_rect);
  if (target_taskbar != nullptr) taskbar_windows.push_back(target_taskbar);

  if (taskbar_windows.empty()) {
    return false;
  }

  // 3. Initialize UI Automation
  IUIAutomation* automation = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation));
  if (FAILED(hr) || automation == nullptr) {
    return false;
  }

  int best_score = -1;
  RECT best_rect{};

  for (HWND tb_hwnd : taskbar_windows) {
    IUIAutomationElement* taskbar_element = nullptr;
    hr = automation->ElementFromHandle(tb_hwnd, &taskbar_element);
    if (FAILED(hr) || taskbar_element == nullptr) {
      continue;
    }

    RECT taskbar_rect{};
    if (!GetWindowRect(tb_hwnd, &taskbar_rect)) {
      taskbar_element->Release();
      continue;
    }
    RECT notification_rect{};
    const HWND notification_area = FindDescendantByClass(tb_hwnd, L"TrayNotifyWnd");
    const bool has_notification_rect =
        notification_area != nullptr && GetWindowRect(notification_area, &notification_rect);

    // 4. App entries are exposed as buttons/list items/tab items. Pane controls are layout
    // containers and previously allowed the right-side tray background to win the match.
    IUIAutomationCondition* cond_button = nullptr;
    IUIAutomationCondition* cond_list = nullptr;
    IUIAutomationCondition* cond_tab = nullptr;
    IUIAutomationCondition* cond_or1 = nullptr;
    IUIAutomationCondition* cond_combined = nullptr;

    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;

    var.lVal = UIA_ButtonControlTypeId;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond_button);

    var.lVal = UIA_ListItemControlTypeId;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond_list);

    var.lVal = UIA_TabItemControlTypeId;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond_tab);

    if (cond_button && cond_list) {
      automation->CreateOrCondition(cond_button, cond_list, &cond_or1);
    } else if (cond_button) {
      cond_or1 = cond_button;
      cond_button->AddRef();
    }

    if (cond_or1 && cond_tab) {
      automation->CreateOrCondition(cond_or1, cond_tab, &cond_combined);
    } else if (cond_or1) {
      cond_combined = cond_or1;
      cond_or1->AddRef();
    }

    if (cond_button) cond_button->Release();
    if (cond_list) cond_list->Release();
    if (cond_tab) cond_tab->Release();
    if (cond_or1) cond_or1->Release();

    if (cond_combined == nullptr) {
      taskbar_element->Release();
      continue;
    }

    IUIAutomationElementArray* elements = nullptr;
    hr = taskbar_element->FindAll(TreeScope_Subtree, cond_combined, &elements);
    cond_combined->Release();

    if (FAILED(hr) || elements == nullptr) {
      taskbar_element->Release();
      continue;
    }

    int length = 0;
    elements->get_Length(&length);

    for (int i = 0; i < length; ++i) {
      IUIAutomationElement* el = nullptr;
      elements->GetElement(i, &el);
      if (el == nullptr) continue;

      BSTR name = nullptr;
      RECT rect{};
      el->get_CurrentName(&name);
      el->get_CurrentBoundingRectangle(&rect);

      std::wstring btn_name = name ? name : L"";
      if (name) SysFreeString(name);

      // Keep candidates inside this taskbar and explicitly outside its notification area.
      if (!IsTaskbarButtonCandidate(
              rect, taskbar_rect, has_notification_rect ? &notification_rect : nullptr)) {
        el->Release();
        continue;
      }

      int score = 0;

      // Check NativeWindowHandle for 100% exact HWND match
      UIA_HWND el_hwnd = nullptr;
      if (SUCCEEDED(el->get_CurrentNativeWindowHandle(&el_hwnd)) && el_hwnd != nullptr) {
        HWND item_hwnd = reinterpret_cast<HWND>(el_hwnd);
        if (item_hwnd == window || item_hwnd == root_owner ||
            GetAncestor(item_hwnd, GA_ROOTOWNER) == root_owner) {
          score = 1000;
        }
      }

      if (score < 1000) {
        std::wstring btn_name_lower = ToLower(btn_name);
        std::vector<std::wstring> btn_tokens = Tokenize(btn_name);

        if (!btn_name_lower.empty()) {
          if (!title.empty() && btn_name_lower == title) {
            score = 100;
          } else if (!process_no_ext.empty() && btn_name_lower == process_no_ext) {
            score = 95;
          } else if (!description_lower.empty() && btn_name_lower == description_lower) {
            score = 93;
          } else if (!title.empty() && title.find(btn_name_lower) != std::wstring::npos) {
            score = 90;
          } else if (!title.empty() && btn_name_lower.find(title) != std::wstring::npos) {
            score = 85;
          } else if (!description_lower.empty() &&
                     description_lower.find(btn_name_lower) != std::wstring::npos) {
            score = 83;
          } else if (!description_lower.empty() &&
                     btn_name_lower.find(description_lower) != std::wstring::npos) {
            score = 82;
          } else if (!process_no_ext.empty() &&
                     btn_name_lower.find(process_no_ext) != std::wstring::npos) {
            score = 80;
          } else if (!process_no_ext.empty() &&
                     process_no_ext.find(btn_name_lower) != std::wstring::npos) {
            score = 75;
          } else {
            // Token overlapping
            int token_matches = 0;
            for (const auto& bt : btn_tokens) {
              if (bt.length() < 2) continue;
              if (bt == L"the" || bt == L"and" || bt == L"new" || bt == L"tab" ||
                  bt == L"window")
                continue;

              if (!title.empty() &&
                  std::find(title_tokens.begin(), title_tokens.end(), bt) != title_tokens.end()) {
                token_matches++;
              }
              if (!process_no_ext.empty() &&
                  std::find(process_tokens.begin(), process_tokens.end(), bt) !=
                      process_tokens.end()) {
                token_matches += 2;
              }
              if (!description_lower.empty() &&
                  std::find(description_tokens.begin(), description_tokens.end(), bt) !=
                      description_tokens.end()) {
                token_matches += 2;
              }
            }
            if (token_matches > 0) {
              score = 50 + token_matches * 5;
            } else {
              // Class matching fallback
              for (const auto& bt : btn_tokens) {
                if (bt.length() < 3) continue;
                if (class_name.find(bt) != std::wstring::npos) {
                  score = 40;
                  break;
                }
              }
            }
          }
        }
      }

      minimize::core::LogTrace(
          L"UIAutomation",
          L"Checking button: '" + btn_name + L"' rect=" + std::to_wstring(rect.left) + L"," +
              std::to_wstring(rect.top) + L"," + std::to_wstring(rect.right) + L"," +
              std::to_wstring(rect.bottom) + L" score=" + std::to_wstring(score));

      if (score > best_score) {
        best_score = score;
        best_rect = rect;
      }

      el->Release();
      if (best_score >= 1000) {
        break;
      }
    }

    elements->Release();
    taskbar_element->Release();
    if (best_score >= 1000) {
      break;
    }
  }

  automation->Release();

  constexpr int kMinimumReliableScore = 50;
  if (best_score >= kMinimumReliableScore) {
    minimize::core::LogTrace(
        L"UIAutomation",
        L"Matched best button for title='" + title + L"' proc='" + process_no_ext +
            L"' with score=" + std::to_wstring(best_score) + L" rect=" +
            std::to_wstring(best_rect.left) + L"," + std::to_wstring(best_rect.top) + L"," +
            std::to_wstring(best_rect.right) + L"," + std::to_wstring(best_rect.bottom));
    *out_rect = best_rect;
    return true;
  }

  minimize::core::LogTrace(L"UIAutomation", L"Failed to match any button for title='" + title +
                                             L"' proc='" + process_no_ext + L"'");
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
  if (target_taskbar != nullptr && monitor != nullptr &&
      GetMonitorInfoW(monitor, &monitor_info)) {
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
  if (auto_hide_restore_deadline_ms_ != 0 &&
      GetTickCount64() >= auto_hide_restore_deadline_ms_) {
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
  const bool has_env = TryGetEnvironmentTarget(&taskbar_rect);

  HMONITOR monitor = MonitorFromRect(&window_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfoW(monitor, &monitor_info)) {
    monitor_info.rcMonitor =
        RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    monitor_info.rcWork = monitor_info.rcMonitor;
  }

  RECT matched_rect{};
  bool has_matched_button = false;
  if (!has_env) {
    has_matched_button = FindTaskbarIconUIAutomation(window, window_rect, &matched_rect);
  }

  if (!has_env && !has_matched_button) {
    HWND taskbar_hwnd = FindTaskbarWindowForRect(window_rect);
    if (taskbar_hwnd != nullptr && GetWindowRect(taskbar_hwnd, &taskbar_rect)) {
      // Successfully retrieved bounds
    } else {
      // Fallback: estimate taskbar based on difference between Monitor and WorkArea
      const RECT& m = monitor_info.rcMonitor;
      const RECT& w = monitor_info.rcWork;
      if (w.bottom < m.bottom) {
        taskbar_rect = RECT{m.left, w.bottom, m.right, m.bottom};
      } else if (w.top > m.top) {
        taskbar_rect = RECT{m.left, m.top, m.right, w.top};
      } else if (w.left > m.left) {
        taskbar_rect = RECT{m.left, m.top, w.left, m.bottom};
      } else if (w.right < m.right) {
        taskbar_rect = RECT{w.right, m.top, m.right, m.bottom};
      } else {
        taskbar_rect = RECT{m.left, m.bottom - 48, m.right, m.bottom};
      }
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
    constexpr float kTargetWidth = 72.0f;
    constexpr float kTargetHeight = 48.0f;
    const minimize::animation::RectF taskbar = ToRectF(taskbar_rect);

    if (tb_width >= tb_height) {
      const float center_x = static_cast<float>(taskbar_rect.left + taskbar_rect.right) * 0.5f;
      target.left = center_x - (kTargetWidth * 0.5f);
      target.right = center_x + (kTargetWidth * 0.5f);
      target.top = taskbar.top;
      target.bottom = taskbar.bottom;
    } else {
      const float center_y = static_cast<float>(taskbar_rect.top + taskbar_rect.bottom) * 0.5f;
      target.top = center_y - (kTargetHeight * 0.5f);
      target.bottom = center_y + (kTargetHeight * 0.5f);
      target.left = taskbar.left;
      target.right = taskbar.right;
    }
  }

  return TaskbarTarget{
      .rect = target,
      .edge = edge,
  };
}

bool TaskbarTargetProvider::TryGetEnvironmentTarget(RECT* target_rect) const {
  wchar_t value[128]{};
  const DWORD length =
      GetEnvironmentVariableW(L"MINIMIZE_TASKBAR_RECT", value, static_cast<DWORD>(std::size(value)));
  if (length == 0 || length >= std::size(value)) {
    return false;
  }

  RECT parsed{};
  if (swscanf_s(value, L"%ld,%ld,%ld,%ld", &parsed.left, &parsed.top, &parsed.right,
                &parsed.bottom) != 4) {
    return false;
  }
  if (parsed.right <= parsed.left || parsed.bottom <= parsed.top) {
    return false;
  }
  *target_rect = parsed;
  return true;
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
