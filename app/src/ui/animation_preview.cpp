#include "pch.hpp"

#include "ui/animation_preview.hpp"

#include <algorithm>
#include <wil/resource.h>

namespace minimize::ui {
namespace {

constexpr wchar_t kPreviewWindowClass[] = L"MinimizeEffectAnimationPreview";

}  // namespace

AnimationPreview::~AnimationPreview() { Close(); }

void AnimationPreview::Start(HWND owner) {
  Close();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kPreviewWindowClass;
  if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return;
  const RECT& work = info.rcWork;
  const int width = std::min(720, static_cast<int>(work.right - work.left - 80));
  const int height = std::min(420, static_cast<int>(work.bottom - work.top - 80));
  const int x = work.left + (work.right - work.left - width) / 2;
  const int y = work.top + (work.bottom - work.top - height) / 2;
  window_ = CreateWindowExW(WS_EX_APPWINDOW, kPreviewWindowClass, L"Minimize Effect Preview",
                            WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, x, y, width, height, nullptr,
                            nullptr, instance, this);
  if (window_ == nullptr) return;
  active_ = true;
  phase_ = 0;
  phase_started_ms_ = GetTickCount64();
  ShowWindow(window_, SW_SHOWNORMAL);
  SetForegroundWindow(window_);
  UpdateWindow(window_);
}

void AnimationPreview::Update(HWND owner, float minimize_duration, float restore_duration) {
  if (!active_) return;
  if (window_ == nullptr || !IsWindow(window_)) {
    window_ = nullptr;
    active_ = false;
    phase_ = 0;
    return;
  }
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG elapsed = now - phase_started_ms_;
  if (phase_ == 0 && elapsed >= 750) {
    ShowWindow(window_, SW_MINIMIZE);
    phase_ = 1;
    phase_started_ms_ = now;
  } else if (phase_ == 1 && elapsed >= static_cast<ULONGLONG>(minimize_duration * 1000.0f) + 700) {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    phase_ = 2;
    phase_started_ms_ = now;
  } else if (phase_ == 2 && elapsed >= static_cast<ULONGLONG>(restore_duration * 1000.0f) + 850) {
    Close();
    if (owner != nullptr && IsWindowVisible(owner)) SetForegroundWindow(owner);
  }
}

void AnimationPreview::Close() {
  HWND window = window_;
  window_ = nullptr;
  active_ = false;
  phase_ = 0;
  phase_started_ms_ = 0;
  dragging_ = false;
  if (window != nullptr && IsWindow(window)) DestroyWindow(window);
}

LRESULT CALLBACK AnimationPreview::WindowProc(HWND window, UINT message, WPARAM w_param,
                                              LPARAM l_param) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* preview = reinterpret_cast<AnimationPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      wil::unique_hbrush background(CreateSolidBrush(RGB(20, 20, 22)));
      FillRect(dc, &client, background.get());
      RECT accent = client;
      accent.bottom = accent.top + 3;
      wil::unique_hbrush accent_brush(CreateSolidBrush(RGB(232, 232, 236)));
      FillRect(dc, &accent, accent_brush.get());
      const int font_height = -MulDiv(36, GetDeviceCaps(dc, LOGPIXELSY), 72);
      wil::unique_hfont font(CreateFontW(font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Inter"));
      HGDIOBJ old_font = SelectObject(dc, font.get());
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(242, 242, 244));
      DrawTextW(dc, L"Preview", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old_font);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (preview == nullptr) return 0;
      POINT cursor{};
      RECT bounds{};
      GetCursorPos(&cursor);
      GetWindowRect(window, &bounds);
      preview->drag_offset_ = {cursor.x - bounds.left, cursor.y - bounds.top};
      preview->dragging_ = true;
      SetCapture(window);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (preview == nullptr || !preview->dragging_ || (w_param & MK_LBUTTON) == 0) return 0;
      POINT cursor{};
      GetCursorPos(&cursor);
      SetWindowPos(window, nullptr, cursor.x - preview->drag_offset_.x,
                   cursor.y - preview->drag_offset_.y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_LBUTTONUP:
      if (preview != nullptr) preview->dragging_ = false;
      if (GetCapture() == window) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
      if (preview != nullptr) preview->dragging_ = false;
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

}  // namespace minimize::ui
