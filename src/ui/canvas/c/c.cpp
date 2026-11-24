#include "ui/types/canvas/canvas.hpp"
#include "ui/color/color/color.hpp"
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
    ImU32 Canvas::C(int r, int g, int b, int a) const
    {
        return Color(r, g, b, static_cast<int>(a * alpha));
    }
}
