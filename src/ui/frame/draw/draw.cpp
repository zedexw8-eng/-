#include "ui/frame/draw/draw.hpp"
#include "ui/color/pack-color/pack-color.hpp"
#include "ui/frame/draw-brand/draw-brand.hpp"
#include "ui/frame/draw-panel-layers/draw-panel-layers.hpp"
#include "ui/frame/draw-tabs/draw-tabs.hpp"
#include "ui/frame/draw-window-layers/draw-window-layers.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/tab/draw-aim/draw-aim.hpp"
#include "ui/tab/draw-esp/draw-esp.hpp"
#include "ui/tab/draw-exploits/draw-exploits.hpp"
#include "ui/tab/draw-misc/draw-misc.hpp"
#include "ui/tab/draw-style/draw-style.hpp"
#ifndef RR_INTERNAL
#include "ui/tab/draw-debug/draw-debug.hpp"
#endif
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
#include "ui/amphetaminehaxx/ui.hpp"

namespace ui
{
    void Draw(UiState& state, bool open)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::GetStyle().Colors[ImGuiCol_Text] = text_color;
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = background_color;
        static float alpha = 0.0f;
        const float fade = static_cast<float>(state.fade_speed > 0 ? state.fade_speed : 255);
        state.menu_open = open;
        alpha = std::clamp(alpha + (open ? fade : -fade), 0.0f, 255.0f);
        if (alpha <= 0.0f) return;
        if (amphetaminehaxx::drag_moves_window) state.window_pos = amphetaminehaxx::menu_pos;
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("ui_canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, PackColor(control_color, alpha / 255.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, PackColor(control_color, alpha / 255.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, PackColor(control_color, alpha / 255.0f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::GetWindowDrawList()->Flags &= ~(ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        Canvas c{ draw, state, alpha / 255.0f, 0, 0, 0, 0, 0.0f, state.window_pos, 1.0f, 1.0f, {} };
        const float height = std::min(560.f, io.DisplaySize.y-8.f);
        InnerSize.y = height-60.f;
        if (!amphetaminehaxx::drag_moves_window) {
            state.window_pos.x=std::clamp(state.window_pos.x,0.f,std::max(0.f,io.DisplaySize.x-522.f));
            state.window_pos.y=std::clamp(state.window_pos.y,8.f,std::max(8.f,io.DisplaySize.y-height));
        }
        c.state_pos = state.window_pos;
        float x = state.window_pos.x;
        float y = state.window_pos.y;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::InvisibleButton("move_window", ImVec2(522.0f, 8));
        if (!amphetaminehaxx::drag_moves_window && ImGui::IsItemActivated())
        {
            state.dragging = true;
            state.drag_offset = ImVec2(io.MousePos.x - x, io.MousePos.y - y);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            state.dragging = false;
        if (state.dragging)
        {
            state.window_pos.x = std::clamp(io.MousePos.x - state.drag_offset.x, 0.0f, std::max(0.0f, io.DisplaySize.x - 522.0f));
            state.window_pos.y = std::clamp(io.MousePos.y - state.drag_offset.y, 0.0f, std::max(0.0f, io.DisplaySize.y - height));
            x = state.window_pos.x;
            y = state.window_pos.y;
            c.state_pos = state.window_pos;
        }
        DrawWindowLayers(draw, ImVec2(x, y), ImVec2(x + 522.0f, y + height), theme, alpha / 255.0f);
        DrawBrand(c, state);
        const float cx = x + 20, cy = y + 44;
        DrawPanelLayers(draw, ImVec2(cx, cy), ImVec2(cx + InnerSize.x, cy + InnerSize.y), theme, alpha / 255.0f);
        DrawTabs(c, state);
        const ImVec2 content_min(cx + 3, cy + 8);
        const ImVec2 content_max(cx + InnerSize.x - 3, cy + InnerSize.y - 3);
        const ImVec2 mouse = io.MousePos;
        if (mouse.x >= content_min.x && mouse.x <= content_max.x && mouse.y >= content_min.y && mouse.y <= content_max.y)
            state.content_scroll -= io.MouseWheel * 24.f;
        const float max_scroll = std::max(0.f, state.content_height - (content_max.y-y) + 8.f);
        state.content_scroll = std::clamp(state.content_scroll,0.f,max_scroll);
        ImGui::PushClipRect(ImVec2(cx + 3, cy + 8), ImVec2(cx + InnerSize.x - 3, cy + InnerSize.y - 3), true);
        if (state.tab == Tab::Esp)
            DrawEsp(c, state);
        else if (state.tab == Tab::Aim)
            DrawAim(c, state);
        else if (state.tab == Tab::Exploits)
            DrawExploits(c, state);
        else if (state.tab == Tab::Misc)
            DrawMisc(c, state);
        else if (state.tab == Tab::Style)
            DrawStyle(c, state);
#ifndef RR_INTERNAL
        else if (state.tab == Tab::Debug)
            DrawDebug(c, state);
#endif
        ImGui::PopClipRect();
        state.content_height = c.content_bottom > 0 ? c.content_bottom-y+state.content_scroll : 0.f;
        if (max_scroll > 0.f) {
            const float track_height = content_max.y-content_min.y;
            const float thumb = std::max(20.f, track_height*track_height/(track_height+max_scroll));
            const float top = content_min.y + (track_height-thumb)*state.content_scroll/max_scroll;
            draw->AddRectFilled({content_max.x-3,top},{content_max.x-1,top+thumb},c.Accent(175));
        }
        ImGui::PopStyleVar(7);
        ImGui::PopStyleColor(9);
        ImGui::End();
    }
}
