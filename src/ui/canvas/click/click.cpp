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
    bool Canvas::Click(float x0, float y0, float w, float h, const char* id, const void* salt)
    {
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        ImGui::PushID(id);
        ImGui::PushID(salt);
        ImGui::PushID(static_cast<int>(x0));
        ImGui::PushID(static_cast<int>(y0));
        const bool hit = ImGui::InvisibleButton("hit", ImVec2(w, h));
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        return hit;
    }
}
