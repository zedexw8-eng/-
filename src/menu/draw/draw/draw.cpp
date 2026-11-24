#include "menu/draw/draw/draw.hpp"
#include "menu/settings/font/font.hpp"
#include "menu/settings/window/window.hpp"
#include "ui/frame/draw/draw.hpp"
#include "ui/types/ui-state/ui-state.hpp"
#include "imgui.h"
#include "ui/amphetaminehaxx/ui.hpp"
#include "ui/menu_bridge.h"

namespace menu
{
static ui::UiState s_ui;

void draw()
{
    s_ui.brand_font = brand_font;
    if (dist_font)
        ImGui::PushFont(dist_font);
    ui::Draw(s_ui, open);
    if (dist_font)
        ImGui::PopFont();
    if (open && br_get()->hud_cursor && ImGui::IsMousePosValid()) {
        const ImVec2 mouse=ImGui::GetIO().MousePos;
        auto* draw=ImGui::GetForegroundDrawList();
        draw->AddLine({mouse.x-5,mouse.y},{mouse.x+6,mouse.y},IM_COL32_WHITE);
        draw->AddLine({mouse.x,mouse.y-5},{mouse.x,mouse.y+6},IM_COL32_WHITE);
    }
}
}
