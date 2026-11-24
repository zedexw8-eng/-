#include "ui/types/canvas/canvas.hpp"
#include "ui/frame/draw-group-layers/draw-group-layers.hpp"
#include "ui/state/theme/theme.hpp"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"

namespace ui
{
    void Canvas::EndGroup()
    {
        const float end_x = x - 10.0f * scale_x;
        const float end_y = y + 5.0f * scale_y;
        const float group_width = GroupWidth * scale_x;
        const float title_x = std::round(group_x + 10 * scale_x);
        const float title_y = std::round(group_y - 6 * scale_y);
        DrawGroupLayers(draw, ImVec2(std::round(group_x), std::round(group_y)), ImVec2(std::round(group_x + group_width), std::round(end_y)), title_x, std::round(title_width), theme, alpha);
        Text(title_x, title_y, group_title.c_str(), IM_COL32(static_cast<int>(text_color.x * 255), static_cast<int>(text_color.y * 255), static_cast<int>(text_color.z * 255), static_cast<int>(text_color.w * 255 * alpha)), true);
        content_bottom = std::max(content_bottom, end_y);
        y += 21.0f * scale_y;
        ImGui::PopID();
    }
}
