#include "ui/frame/draw-tabs/draw-tabs.hpp"
#include "ui/color/pack-color/pack-color.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/types/canvas/canvas.hpp"
#include "ui/types/tab/tab.hpp"
#include "ui/types/ui-state/ui-state.hpp"
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
    void DrawTabs(Canvas& c, UiState& s)
    {
        static const char* names[] = {
            "esp", "aim", "visuals", "world", "style",
#ifndef RR_INTERNAL
            "debug"
#endif
        };
        const float x = s.window_pos.x + 20;
        const float y = s.window_pos.y + 16;
        const int n = static_cast<int>(sizeof(names) / sizeof(names[0]));
        const float w = InnerSize.x / static_cast<float>(n);
        for (int i = 0; i < n; ++i)
        {
            const float cx = x + w * i + w * 0.5f;
            const float text_w = ImGui::CalcTextSize(names[i]).x;
            const float text_x = cx - text_w * 0.5f;
            const Tab tab = static_cast<Tab>(i);
            ImGui::SetCursorScreenPos(ImVec2(x + w * i, y));
            ImGui::PushID(i);
            ImGui::InvisibleButton("tab", ImVec2(w, 24.0f));
            if (ImGui::IsItemClicked())
                { if (s.tab != tab) s.content_scroll = s.content_height = 0; s.tab = tab; }
            const bool selected = s.tab == tab;
            c.Text(text_x, y + 1.0f, names[i], PackColor(tab_text_color, c.alpha));
            const float line_x = x + w * i + (i == 0 ? -3.0f : 2.0f);
            const float line_y = s.window_pos.y + 35.0f - (selected ? 1.0f : 0.0f);
            const float line_r = x + w * (i + 1) + (i == n - 1 ? 3.0f : -2.0f);
            const ImU32 line_top = selected ? c.Accent() : PackColor(tab_idle_line_color, c.alpha);
            const ImU32 line_bottom = selected ? c.C(160, 16, 37) : PackColor(tab_idle_line_bottom_color, c.alpha);
            const float line_h = selected ? 5.0f : 3.0f;
            if (selected)
            {
                const ImU32 highlight = IM_COL32(255, 92, 112, static_cast<int>(c.alpha * 255.0f));
                c.draw->AddRectFilledMultiColor(ImVec2(line_x, line_y), ImVec2(line_r, line_y + 1.0f), highlight, highlight, line_top, line_top);
                c.draw->AddRectFilledMultiColor(ImVec2(line_x, line_y + 1.0f), ImVec2(line_r, line_y + line_h - 1.0f), line_top, line_top, line_bottom, line_bottom);
                c.draw->AddRectFilled(ImVec2(line_x, line_y + line_h - 1.0f), ImVec2(line_r, line_y + line_h), c.C(95, 8, 22));
            }
            else
                c.draw->AddRectFilledMultiColor(ImVec2(line_x, line_y), ImVec2(line_r, line_y + line_h), line_top, line_top, line_bottom, line_bottom);
            c.draw->AddRectFilledMultiColor(ImVec2(line_x, line_y + line_h), ImVec2(line_r, line_y + line_h + 1.0f), PackColor(tab_line_shadow_color, c.alpha), PackColor(tab_line_shadow_color, c.alpha), PackColor(tab_line_black_color, c.alpha), PackColor(tab_line_black_color, c.alpha));
            ImGui::PopID();
        }
    }
}
