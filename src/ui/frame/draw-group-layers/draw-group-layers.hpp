#pragma once
#include "ui/state/theme/theme.hpp"
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    void DrawGroupLayers(ImDrawList* draw, ImVec2 min, ImVec2 max, float title_x, float title_width, const UiTheme& theme, float alpha);
}
