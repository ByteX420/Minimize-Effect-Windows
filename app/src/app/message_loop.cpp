#include "pch.hpp"

#include "app/message_loop.hpp"

namespace minimize::app {

int MessageLoop::Run(const MessageLoopCallbacks& callbacks) const {
  MSG message{};
  bool running = true;
  while (running && !callbacks.should_stop()) {
    callbacks.update();
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        running = false;
        break;
      }
      if (message.message == WM_DISPLAYCHANGE) callbacks.display_changed();
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!running || callbacks.should_stop()) break;
    callbacks.render_settings();
    switch (callbacks.tick_runtime()) {
      case MessageLoopWait::kAnimation:
        callbacks.wait_for_animation();
        break;
      case MessageLoopWait::kImmediate:
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 0, QS_ALLINPUT);
        break;
      case MessageLoopWait::kFrame:
        callbacks.wait_for_idle_frame();
        break;
    }
  }
  return static_cast<int>(message.wParam);
}

}  // namespace minimize::app
