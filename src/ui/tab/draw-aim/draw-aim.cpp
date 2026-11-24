#include "ui/tab/draw-aim/draw-aim.hpp"
#include "rr_controls.hpp"
#include "game/rr_game.h"

namespace ui {
void DrawAim(Canvas& c, UiState& state) {
    auto& b = *br_get();
    using amphetaminehaxx::check;
    using amphetaminehaxx::color;
    (void)state;
    c.BeginColumn(false);
    c.BeginGroup("aim assist");
    check(c,"enabled",b.aa_on);
    if (b.aa_mode == 1) b.aa_mode=0;
    int mode=b.aa_mode==2;
    c.Dropdown("mode",mode,{"mouse","camlock"});
    b.aa_mode=mode ? 2 : 0;
    amphetaminehaxx::key(c,"aim key",b.aa_key);
    c.Dropdown("activation",b.aa_bind_mode,{"hold","toggle","always"});
    c.Dropdown("target bone",b.aa_bone,{"head","neck","chest","pelvis","closest","arms","legs","face","hands","feet"});
    c.Slider("distance",b.aa_dist,0,1000," m");
    c.Slider("smoothing",b.aa_smooth,0,20);
    c.Slider("mouse calibration",b.aa_aim_cal,10,2000,"%");
    check(c,"sticky target",b.aa_sticky);
    check(c,"closest first",b.aa_closest_first);
    check(c,"visibility check",b.aa_los_on);
    check(c,"skip party",b.aa_team_check);
    check(c,"target clan",b.aa_target_clan);
    check(c,"target police",b.aa_target_police);
    c.EndGroup();
    c.BeginColumn(true);
    c.BeginGroup("field of view");
    check(c,"draw circle",b.aa_show_fov,BR_COLOR_FOV);
    check(c,"fill",b.aa_fov_fill,BR_COLOR_FOV_FILL);
    check(c,"gradient fill",b.aa_fov_gradient,BR_COLOR_FOV_GRADIENT);
    check(c,"spin",b.aa_fov_spin);
    c.Slider("radius",b.aa_fov_px,30,600," px");
    c.Slider("opacity",b.aa_fov_opacity,0,100,"%");
    c.Slider("spin speed",b.aa_fov_spin_speed,15,360);
    c.Slider("camera fov",b.cam_fov,0,160);
    c.EndGroup();
}
}
