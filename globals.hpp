#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>

inline HANDLE PHANDLE = nullptr;

struct SConfig {
    SP<Config::Values::CIntValue>    fullscreen_on_one_column;
    SP<Config::Values::CFloatValue>  column_width;
    SP<Config::Values::CIntValue>    focus_fit_method;
    SP<Config::Values::CIntValue>    follow_focus;
    SP<Config::Values::CIntValue>    follow_debounce_ms;
    SP<Config::Values::CStringValue> explicit_column_widths;
    SP<Config::Values::CIntValue>    collapsed_width;
    SP<Config::Values::CIntValue>    focus_history;
    SP<Config::Values::CStringValue> auto_width_rules;
};

inline SConfig g_config;
