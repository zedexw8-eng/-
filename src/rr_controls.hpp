#pragma once
#include "ui/menu_bridge.h"
#include "ui/types/canvas/canvas.hpp"

namespace amphetaminehaxx {
void check(ui::Canvas& canvas, const char* label, int& setting, int color = -1);
void color(ui::Canvas& canvas, const char* label, int slot);
void key(ui::Canvas& canvas, const char* label, int& setting, bool menu_key = false);
void profiles(ui::Canvas& canvas);
void theme_controls(ui::Canvas& canvas, ui::UiState& state);
}
