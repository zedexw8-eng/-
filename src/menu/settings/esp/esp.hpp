#pragma once

namespace menu
{
extern bool box;
extern bool filled_box;
extern int box_type;
extern bool skeleton;
extern bool distance;
extern bool fflags;
extern bool health;
extern bool nick;
extern bool weapon;
extern bool ammo;
extern bool arrow;
extern unsigned int arrow_who;
extern int arrow_dist;
extern unsigned int arrow_mask;
extern bool radar;
extern float radar_x;
extern float radar_y;
extern int radar_range;
extern unsigned int radar_who;
extern unsigned int radar_mask;
extern bool dbg_lens;
extern bool outline_box;
extern bool outline_distance;
extern bool outline_fflags;
extern bool outline_weapon;
extern bool outline_ammo;
extern bool outline_skeleton;
extern bool outline_health;
extern bool outline_arrow;
extern int nick_style;
extern int nick_font_id;
extern int info_font_id;
extern int nick_case;
extern int info_case;
extern unsigned int corpse_mask;
extern unsigned int team_mask;
extern bool team;
extern bool corpse;
extern bool ai;
extern unsigned int ai_mask;
extern bool vehicle;
extern unsigned int vehicle_mask;
extern int dist_vehicle;
extern int dist_ai;
extern int dist_enemy;
extern int dist_team;
}
