#include "ui/tab/draw-debug/draw-debug.hpp"
#include "rr_controls.hpp"
#include "game/rr_game.h"
#include <cstdio>

namespace ui {
void DrawDebug(Canvas& c, UiState& state) {
    auto& b = *br_get();
    using amphetaminehaxx::check;
    using amphetaminehaxx::color;
    (void)state;
    c.BeginColumn(false);
    c.BeginGroup("game status");
    c.Label(b.telem_game_pid ? "game active" : "waiting for game");
    const char* const collision[]={"visibility off","visibility loading","visibility ready"};
    if (b.telem_los_state >= 0 && b.telem_los_state < 3) c.Label(collision[b.telem_los_state]);
    c.Label(b.telem_status);
    c.EndGroup();
}
}
