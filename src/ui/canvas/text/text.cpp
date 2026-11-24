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
    void Canvas::Text(float x0, float y0, const char* text, ImU32 col, bool)
    {
        draw->AddText(ImVec2(std::round(x0), std::round(y0)), col, text);
    }
}
