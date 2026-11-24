#include "ui/types/canvas/canvas.hpp"
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
    void Canvas::BeginGroup(const char* title)
    {
        group_x = x - 10.0f * scale_x;
        group_y = y + 5.0f * scale_y;
        group_title = title;
        title_width = ImGui::CalcTextSize(title).x;
        y += 15.0f * scale_y;
        ImGui::PushID(title);
    }
}
