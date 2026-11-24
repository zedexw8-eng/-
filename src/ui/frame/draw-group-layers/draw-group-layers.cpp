#include "ui/frame/draw-group-layers/draw-group-layers.hpp"
#include "ui/frame/group-pack/group-pack.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    void DrawGroupLayers(ImDrawList* draw, ImVec2 min, ImVec2 max, float title_x, float title_width, const UiTheme& theme, float alpha)
    {
        const float right = max.x;
        const ImU32 face = GroupPack(theme.group_color, alpha * theme.layer_alpha.group_face);
        const ImU32 edge = GroupPack(theme.border_color, alpha * theme.layer_alpha.group_outline);
        const ImU32 black = GroupPack(theme.group_black_line_color, alpha * theme.layer_alpha.group_black_edge);
        draw->AddRectFilled(ImVec2(min.x + 1, min.y + 1), ImVec2(right, max.y), face);
        draw->AddRectFilled(ImVec2(min.x, min.y), ImVec2(title_x - 2.0f, min.y + 1), edge);
        draw->AddRectFilled(ImVec2(title_x + title_width + 2.0f, min.y), ImVec2(right + 1, min.y + 1), edge);
        draw->AddRectFilled(ImVec2(min.x, min.y), ImVec2(min.x + 1, max.y + 1), edge);
        draw->AddRectFilled(ImVec2(right, min.y), ImVec2(right + 1, max.y + 1), edge);
        draw->AddRectFilled(ImVec2(right + 1, min.y), ImVec2(right + 2, max.y + 1), black);
        draw->AddRectFilled(ImVec2(min.x, max.y), ImVec2(right + 1, max.y + 1), edge);
        draw->AddRectFilled(ImVec2(min.x, max.y + 1), ImVec2(right + 2, max.y + 2), black);
    }
}
