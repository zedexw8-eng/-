#include "ui/frame/draw-brand/draw-brand.hpp"
#include "ui/types/canvas/canvas.hpp"
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
    void DrawBrand(Canvas& c, const UiState& s)
    {
        if (!s.brand_font)
            return;

        const double time = ImGui::GetTime();
        const char* wave = "amphetamine";
        const char* type = "haxx";
        const int suffix_length = static_cast<int>(std::strlen(type));
        const double cycle = std::fmod(time, 5.0);
        int count = static_cast<int>((cycle < 2.5 ? cycle : 5.0 - cycle) * suffix_length / 2.5 + 0.5);
        count = std::clamp(count, 0, suffix_length);
        float brand_width = 0.0f;
        for (int i = 0; wave[i]; ++i)
        {
            const char glyph[] = { wave[i], '\0' };
            brand_width += s.brand_font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, glyph).x;
        }
        for (int i = 0; i < count; ++i)
        {
            const char glyph[] = { type[i], '\0' };
            brand_width += s.brand_font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, glyph).x;
        }
        const ImVec2 origin(std::round(s.window_pos.x + 261.0f - brand_width * 0.5f), std::round(s.window_pos.y - 7.0f));
        float x = origin.x;
        const ImDrawListFlags old_flags = c.draw->Flags;
        c.draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
        ImGui::PushFont(s.brand_font);
        for (int i = 0; wave[i]; ++i)
        {
            const float phase = static_cast<float>(time * 3.2 + i * 0.65);
            const float pulse = (std::sin(phase) + 1.0f) * 0.5f;
            const float lift = std::round(std::sin(phase) * 3.0f);
            const ImU32 col = IM_COL32(static_cast<int>(185 + pulse * 70), static_cast<int>(20 + pulse * 45), static_cast<int>(35 + pulse * 60), static_cast<int>(c.alpha * 255.0f));
            const char glyph[] = { wave[i], '\0' };
            const ImVec2 pos(std::round(x), std::round(origin.y + lift));
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x - 1.0f, pos.y), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x + 1.0f, pos.y), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x, pos.y - 1.0f), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x, pos.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, pos, col, glyph);
            x += ImGui::CalcTextSize(glyph, nullptr, false, 15.0f).x;
        }
        for (int i = 0; i < count; ++i)
        {
            const float phase = static_cast<float>(time * 3.2 + (i + std::strlen(wave)) * 0.65);
            const float pulse = (std::sin(phase) + 1.0f) * 0.5f;
            const float lift = std::round(std::sin(phase) * 3.0f);
            const char glyph[] = { type[i], '\0' };
            const ImU32 col = IM_COL32(static_cast<int>(170 + pulse * 70), static_cast<int>(170 + pulse * 70), static_cast<int>(170 + pulse * 70), static_cast<int>(c.alpha * 255.0f));
            const ImVec2 pos(std::round(x), std::round(origin.y + lift));
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x - 1.0f, pos.y), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x + 1.0f, pos.y), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x, pos.y - 1.0f), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, ImVec2(pos.x, pos.y + 1.0f), IM_COL32(0, 0, 0, static_cast<int>(c.alpha * 255.0f)), glyph);
            c.draw->AddText(s.brand_font, 15.0f, pos, col, glyph);
            x += ImGui::CalcTextSize(glyph, nullptr, false, 15.0f).x;
        }
        ImGui::PopFont();
        c.draw->Flags = old_flags;
    }
}
