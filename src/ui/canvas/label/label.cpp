#include "ui/types/canvas/canvas.hpp"
#include "ui/color/pack-color/pack-color.hpp"
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
    void Canvas::Label(const char* text)
    {
        if (text)
            Text(x + 20, y - 3, text, PackColor(slider_label_color, alpha), true);
        y += 15.0f;
    }
}
