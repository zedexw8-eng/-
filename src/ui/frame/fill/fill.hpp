#pragma once
#include "imgui.h"

namespace ui
{
    void Fill(ImDrawList* draw, ImVec2 min, ImVec2 max, const ImVec4& color, float alpha);
}
