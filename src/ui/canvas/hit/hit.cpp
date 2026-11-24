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
    bool Canvas::Hit(float x0, float y0, float w, float h) const
    {
        const ImVec2 p = ImGui::GetIO().MousePos;
        return p.x > x0 && p.x < x0 + w && p.y > y0 && p.y < y0 + h;
    }
}
