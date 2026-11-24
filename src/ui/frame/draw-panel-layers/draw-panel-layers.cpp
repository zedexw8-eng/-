#include "ui/frame/draw-panel-layers/draw-panel-layers.hpp"
#include "ui/frame/fill/fill.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    void DrawPanelLayers(ImDrawList* draw, ImVec2 min, ImVec2 max, const UiTheme& theme, float alpha)
    {
        Fill(draw, min, max, theme.panel_outer_line_color, alpha);
        Fill(draw, ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), theme.panel_between_color, alpha);
        Fill(draw, ImVec2(min.x + 2, min.y + 2), ImVec2(max.x - 2, max.y - 2), theme.panel_inner_line_color, alpha);
        Fill(draw, ImVec2(min.x + 3, min.y + 3), ImVec2(max.x - 3, max.y - 3), theme.panel_inside_color, alpha);
    }
}
