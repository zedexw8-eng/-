#include "ui/types/canvas/canvas.hpp"
#include "ui/state/theme/theme.hpp"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui
{
    void Canvas::BeginColumn(bool right)
    {
        x = state_pos.x + (right ? 282.0f : ContentLeft);
        y = state_pos.y + ContentTop - state.content_scroll;
    }
}
