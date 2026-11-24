#pragma once
#include "ui/state/theme/theme.hpp"
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    void DrawPanelLayers(ImDrawList* draw, ImVec2 min, ImVec2 max, const UiTheme& theme, float alpha);
}
