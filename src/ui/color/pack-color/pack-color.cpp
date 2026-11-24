#include "ui/color/pack-color/pack-color.hpp"
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
    ImU32 PackColor(const ImVec4& color, float alpha)
    {
        return IM_COL32(static_cast<int>(color.x * 255), static_cast<int>(color.y * 255), static_cast<int>(color.z * 255), static_cast<int>(color.w * 255 * alpha));
    }
}
