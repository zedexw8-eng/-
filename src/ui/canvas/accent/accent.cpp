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
    ImU32 Canvas::Accent(int a) const
    {
        return IM_COL32(static_cast<int>(state.accent[0] * 255.0f), static_cast<int>(state.accent[1] * 255.0f), static_cast<int>(state.accent[2] * 255.0f), static_cast<int>(a * alpha));
    }
}
