#include "ui/frame/gui-pack/gui-pack.hpp"
#include "imgui.h"

namespace ui
{
    ImU32 GuiPack(const ImVec4& color, float alpha)
    {
        return IM_COL32(static_cast<int>(color.x * 255.0f), static_cast<int>(color.y * 255.0f), static_cast<int>(color.z * 255.0f), static_cast<int>(color.w * 255.0f * alpha));
    }
}
