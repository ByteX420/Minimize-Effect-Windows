#pragma once

#include "ui/pages/general_page.hpp"

namespace minimize::ui::pages {

class StressTestPage final {
public:
  static void Render(::minimize::ui::SettingsWindow& window, components::PageLayout& layout,
                     const ::minimize::ui::motion::MotionContext& motion, float scale, float alpha);
};

}  // namespace minimize::ui::pages
