#include "menu/settings/esp/esp.hpp"

namespace menu
{
bool box = true;
bool filled_box = false;
int box_type = 0;
bool skeleton = true;
bool distance = true;
bool fflags = true;
bool health = true;
bool nick = true;
bool weapon = true;
bool ammo = true;
bool arrow = false;
unsigned int arrow_who = 7u;
int arrow_dist = 120;
unsigned int arrow_mask = 31u;
bool radar = false;
float radar_x = -1.f;
float radar_y = -1.f;
int radar_range = 500;
unsigned int radar_who = 7u;
unsigned int radar_mask = 13u;
bool dbg_lens = false;
bool outline_box = true;
bool outline_distance = true;
bool outline_fflags = true;
bool outline_weapon = true;
bool outline_ammo = true;
bool outline_skeleton = true;
bool outline_health = true;
bool outline_arrow = true;
int nick_style = 1;
int nick_font_id = 1;
int info_font_id = 0;
int nick_case = 0;
int info_case = 0;
unsigned int corpse_mask = 255u;
unsigned int team_mask = 32u | 128u;
bool team = true;
bool corpse = false;
bool ai = true;
unsigned int ai_mask = 2u | 8u | 16u;
bool vehicle = false;
unsigned int vehicle_mask = 0;
int dist_vehicle = 3000;
int dist_ai = 1500;
int dist_enemy = 1500;
int dist_team = 1500;
}
