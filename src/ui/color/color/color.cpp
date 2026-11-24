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
    ImU32 Color(int r, int g, int b, int a)
    {
        return IM_COL32(r, g, b, a);
    }
}
