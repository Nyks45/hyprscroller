#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>

inline HANDLE PHANDLE = nullptr;

struct SConfig {
    SP<Config::Values::CIntValue>    fullscreen_on_one_column;
    SP<Config::Values::CFloatValue>  column_width;
    SP<Config::Values::CFloatValue>  window_default_height;
    SP<Config::Values::CIntValue>    focus_fit_method;
    SP<Config::Values::CIntValue>    follow_focus;
    SP<Config::Values::CFloatValue>  follow_min_visible;
    SP<Config::Values::CIntValue>    follow_debounce_ms;
    SP<Config::Values::CIntValue>    focus_wrap;
    SP<Config::Values::CIntValue>    focus_edge_ms;
    SP<Config::Values::CIntValue>    center_active_column;
    SP<Config::Values::CIntValue>    center_active_window;
    SP<Config::Values::CStringValue> explicit_column_widths;
    SP<Config::Values::CStringValue> column_widths;
    SP<Config::Values::CStringValue> window_heights;
    SP<Config::Values::CIntValue>    collapsed_width;
    SP<Config::Values::CIntValue>    focus_history;
    SP<Config::Values::CStringValue> auto_width_rules;
    SP<Config::Values::CIntValue>    click_edge_left;
    SP<Config::Values::CIntValue>    click_edge_right;
};

inline SConfig g_config;
