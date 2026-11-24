#include "ui/frame/fill/fill.hpp"
#include "ui/frame/gui-pack/gui-pack.hpp"
#include "imgui.h"

namespace ui
{
    void Fill(ImDrawList* draw, ImVec2 min, ImVec2 max, const ImVec4& color, float alpha)
    {
        draw->AddRectFilled(min, max, GuiPack(color, alpha));
    }
}
