#include <unistd.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigManager.hpp>

#include "globals.hpp"
#include "Scrolling.hpp"

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Version mismatch");
    }

    g_config.fullscreen_on_one_column = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:fullscreen_on_one_column", "fullscreen single column", 0);
    g_config.column_width             = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:column_width", "default column width", 0.5);
    g_config.window_default_height    = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:window_default_height", "default window height in column mode", 1.0);
    g_config.focus_fit_method         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_fit_method", "0=center 1=fit", 0);
    g_config.follow_focus             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_focus", "follow focus: 0=off 1=on 2=always", 1);
    g_config.follow_min_visible       = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:follow_min_visible", "min visible fraction before scrolling (0.0-1.0)", 0.0);
    g_config.follow_debounce_ms       = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_debounce_ms", "follow debounce ms", 0);
    g_config.focus_wrap               = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_wrap", "wrap focus at edges", 1);
    g_config.focus_edge_ms            = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_edge_ms", "edge focus timeout ms", 400);
    g_config.center_active_column     = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:center_active_column", "always center active column", 0);
    g_config.center_active_window     = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:center_active_window", "always center active window in column", 0);
    g_config.explicit_column_widths   = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:explicit_column_widths", "explicit column widths for cycling", "0.333,0.5,0.667,1.0");
    g_config.column_widths            = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:column_widths", "column widths for cycling (alias)", "0.333,0.5,0.667,1.0");
    g_config.window_heights           = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:window_heights", "window heights for cycling", "0.333,0.5,0.667,1.0");
    g_config.collapsed_width          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:collapsed_width", "collapsed column width in px", 30);
    g_config.focus_history            = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_history", "enable focus history", 1);
    g_config.auto_width_rules         = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:auto_width_rules", "per-class auto widths: class:0.7,class2:0.3", "");
    g_config.click_edge_left          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:click_edge_left", "left edge exclusion for clicks in px", 90);
    g_config.click_edge_right         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:click_edge_right", "right edge exclusion for clicks in px", 60);
    g_config.follow_hover             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_hover", "follow hover: 0=off 1=on with delay", 0);
    g_config.hover_delay_ms           = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:hover_delay_ms", "hover delay in ms before scroll", 500);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.fullscreen_on_one_column);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.column_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.window_default_height);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_fit_method);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_focus);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_min_visible);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_debounce_ms);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_wrap);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_edge_ms);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.center_active_column);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.center_active_window);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.explicit_column_widths);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.column_widths);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.window_heights);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.collapsed_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_history);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.auto_width_rules);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.click_edge_left);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.click_edge_right);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_hover);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.hover_delay_ms);

    bool success = HyprlandAPI::addTiledAlgo(PHANDLE, "hyprscrolling", &typeid(CScrollingLayout), []() -> UP<Layout::ITiledAlgorithm> {
        return makeUnique<CScrollingLayout>();
    });

    if (!success) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Failure in initialization: failed to register tiled algorithm",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Algorithm registration failed");
    }

    HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 3000);

    return {"hyprscrolling", "A plugin to add a scrolling layout to hyprland", "Vaxry", "2.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    HyprlandAPI::removeAlgo(PHANDLE, "hyprscrolling");
}
