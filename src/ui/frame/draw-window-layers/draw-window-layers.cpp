#include "ui/frame/draw-window-layers/draw-window-layers.hpp"
#include "ui/frame/fill/fill.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    void DrawWindowLayers(ImDrawList* draw, ImVec2 min, ImVec2 max, const UiTheme& theme, float alpha)
    {
        Fill(draw, min, max, theme.outer_shadow_color, alpha);
        Fill(draw, ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), theme.outer_outer_line_color, alpha);
        Fill(draw, ImVec2(min.x + 2, min.y + 2), ImVec2(max.x - 2, max.y - 2), theme.outer_between_color, alpha);
        Fill(draw, ImVec2(min.x + 5, min.y + 5), ImVec2(max.x - 5, max.y - 5), theme.outer_inner_line_color, alpha);
        Fill(draw, ImVec2(min.x + 6, min.y + 6), ImVec2(max.x - 6, max.y - 6), theme.outer_face_color, alpha);
    }
}
