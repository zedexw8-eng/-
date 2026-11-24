#pragma once
#include "ui/types/ui-theme/ui-theme.hpp"
#include "imgui.h"

namespace ui
{
    extern ImVec2 InnerSize;
    extern float GroupWidth;
    constexpr float ContentLeft = 52.0f;
    constexpr float ContentTop = 57.0f;
    extern UiTheme theme;
    inline auto& background_color = theme.background_color;
    inline auto& group_color = theme.group_color;
    inline auto& border_color = theme.border_color;
    inline auto& text_color = theme.text_color;
    inline auto& control_color = theme.control_color;
    inline auto& track_color = theme.track_color;
    inline auto& checkbox_outline_color = theme.checkbox_outline_color;
    inline auto& checkbox_off_color = theme.checkbox_off_color;
    inline auto& checkbox_on_color = theme.checkbox_on_color;
    inline auto& checkbox_on_shade_color = theme.checkbox_on_shade_color;
    inline auto& checkbox_text_color = theme.checkbox_text_color;
    inline auto& slider_border_color = theme.slider_border_color;
    inline auto& slider_track_bottom_color = theme.slider_track_bottom_color;
    inline auto& slider_fill_shade_color = theme.slider_fill_shade_color;
    inline auto& slider_label_color = theme.slider_label_color;
    inline auto& slider_value_color = theme.slider_value_color;
    inline auto& slider_button_text_color = theme.slider_button_text_color;
    inline auto& control_border_color = theme.control_border_color;
    inline auto& control_text_color = theme.control_text_color;
    inline auto& control_arrow_color = theme.control_arrow_color;
    inline auto& button_text_color = theme.button_text_color;
    inline auto& group_black_line_color = theme.group_black_line_color;
    inline auto& tab_text_color = theme.tab_text_color;
    inline auto& tab_idle_line_color = theme.tab_idle_line_color;
    inline auto& tab_idle_line_bottom_color = theme.tab_idle_line_bottom_color;
    inline auto& tab_line_shadow_color = theme.tab_line_shadow_color;
    inline auto& tab_line_black_color = theme.tab_line_black_color;
    inline auto& outer_shadow_color = theme.outer_shadow_color;
    inline auto& outer_outer_line_color = theme.outer_outer_line_color;
    inline auto& outer_between_color = theme.outer_between_color;
    inline auto& outer_inner_line_color = theme.outer_inner_line_color;
    inline auto& outer_face_color = theme.outer_face_color;
    inline auto& panel_outer_line_color = theme.panel_outer_line_color;
    inline auto& panel_between_color = theme.panel_between_color;
    inline auto& panel_inner_line_color = theme.panel_inner_line_color;
    inline auto& panel_inside_color = theme.panel_inside_color;
}
