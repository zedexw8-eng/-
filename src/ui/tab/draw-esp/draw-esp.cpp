#include "ui/tab/draw-esp/draw-esp.hpp"
#include "rr_controls.hpp"
#include "game/rr_game.h"

namespace ui {
void DrawEsp(Canvas& c, UiState& state) {
    auto& b = *br_get();
    using amphetaminehaxx::check;
    using amphetaminehaxx::color;
    (void)state;
    c.BeginColumn(false);
    c.BeginGroup("players");
    check(c,"enabled",b.enabled);
    check(c,"box",b.show_box,BR_COLOR_BOX);
    check(c,"skeleton",b.show_skeleton,BR_COLOR_SKELETON);
    check(c,"name",b.show_name,BR_COLOR_NAME);
    check(c,"weapon",b.show_weapon,BR_COLOR_WEAPON);
    check(c,"distance",b.show_distance,BR_COLOR_DISTANCE);
    check(c,"armor",b.show_armor,BR_COLOR_ARMOR);
    check(c,"team marker",b.show_team);
    check(c,"local player",b.show_self);
    check(c,"show clan",b.show_clan);
    check(c,"show police",b.show_police);
    c.Slider("skeleton width",b.skeleton_width,1,4," px");
    c.Slider("line width",b.line_w,1,4," px");
    int distance=b.max_distance/100;
    c.Slider("max distance",distance,0,BR_MAX_DISTANCE_METRES," m");
    b.max_distance=distance*100;
    c.EndGroup();
    c.BeginGroup("team colors");
    c.Label("criminal"); color(c,"criminal",BR_COLOR_CRIMINAL);
    c.Label("police"); color(c,"police",BR_COLOR_POLICE);
    c.Label("civilian"); color(c,"civilian",BR_COLOR_CIVILIAN);
    c.EndGroup();
    c.BeginColumn(true);
    c.BeginGroup("chams");
#ifdef RR_INTERNAL
    check(c,"overlay chams",b.cham_on,BR_COLOR_OVERLAY_CHAM);
    check(c,"visibility check",b.cham_vis_check);
#else
    check(c,"overlay chams",b.cham_on);
#endif
    check(c,"tint",b.cham_tint);
    c.Dropdown("style",b.cham_style,{"flat","ice","plasma","ember","obsidian","abyss","pulse","invisible","marble"});
    c.Slider("opacity",b.cham_opacity,0,100,"%");
    c.EndGroup();
    c.BeginGroup("radar and arrows");
    check(c,"radar",b.radar_on);
    check(c,"world markers",b.radar_world);
    c.Slider("radar range",b.radar_range,25,500," m");
    check(c,"edge arrows",b.oof_on,BR_COLOR_OOF);
    c.Dropdown("arrow style",b.oof_style,{"arrow","chevron","triangle"});
    c.Slider("arrow size",b.oof_size,3,25," px");
    c.Slider("arrow radius",b.oof_radius,30,500," px");
    c.EndGroup();
}
}
