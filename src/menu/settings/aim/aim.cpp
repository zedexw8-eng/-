#include "menu/settings/aim/aim.hpp"

namespace menu
{
bool aim = false;
int aim_key = 0;
int aim_type = 0;
int aim_bone = 0;
bool aim_fov_on = false;
int aim_fov = 90;
float aim_fov_col[4] = { 1.f, 1.f, 1.f, 1.f };
bool aim_fov_fill = false;
float aim_fov_fill_col[4] = { 1.f, 1.f, 1.f, 0.2f };
int aim_dist = 10;
int aim_lead = 100;
int aim_mode = 0;
bool aim_trace = false;
float aim_trace_col[4] = { 1.f, 1.f, 1.f, 1.f };
bool aim_hc = false;
int aim_hc_val = 100;
bool aim_team = false;
bool aim_show = false;
bool aim_part = false;
float aim_part_col[4] = { 1.f, 0.f, 0.f, 1.f };
}
