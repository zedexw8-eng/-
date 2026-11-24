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
    void Canvas::Rect(float x0, float y0, float x1, float y1, ImU32 col)
    {
        draw->AddRectFilled(ImVec2(std::round(x0), std::round(y0)), ImVec2(std::round(x1), std::round(y1)), col);
    }
}
