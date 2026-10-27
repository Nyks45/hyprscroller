#include <unistd.h>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigManager.hpp>

#include "globals.hpp"
#include "Scrolling.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Guard against loading a plugin built against a different Hyprland than
    // the running compositor. Without this, an ABI mismatch surfaces as a
    // cryptic "undefined symbol" crash instead of a clear message.
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Version mismatch - rebuild the plugin against the running Hyprland (HASH: " + HASH + " CLIENT: " + CLIENT_HASH + ")",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Version mismatch");
    }

    g_config.fullscreen_on_one_column = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:fullscreen_on_one_column", "fullscreen single column", 0);
    g_config.column_width             = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:column_width", "default column width", 0.5);
    g_config.window_default_height    = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:window_default_height", "default window height in column mode", 1.0);
    g_config.focus_fit_method         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_fit_method", "0=center 1=fit 2=gap-free center", 0);
    g_config.follow_focus             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_focus", "follow focus: 0=off 1=on 2=always", 1);
    g_config.follow_min_visible       = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:follow_min_visible", "min visible fraction before scrolling (0.0-1.0)", 0.0);
    g_config.follow_debounce_ms       = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_debounce_ms", "follow debounce ms", 0);
    g_config.focus_wrap               = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_wrap", "wrap focus at edges", 1);
    g_config.center_active_column     = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:center_active_column", "always center active column", 0);
    g_config.explicit_column_widths   = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:explicit_column_widths", "explicit column widths for cycling (overrides column_widths when set)", "");
    g_config.column_widths            = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:column_widths", "column widths for cycling (alias)", "0.333,0.5,0.667,1.0");
    g_config.window_heights           = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:window_heights", "window heights for cycling", "0.333,0.5,0.667,1.0");
    g_config.collapsed_width          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:collapsed_width", "collapsed column width in px", 30);
    g_config.focus_history            = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_history", "enable focus history", 1);
    g_config.auto_width_rules         = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:auto_width_rules", "per-class auto widths: class:0.7,class2:0.3", "");
    g_config.click_edge_left          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:click_edge_left", "left edge exclusion for clicks in px", 0);
    g_config.click_edge_right         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:click_edge_right", "right edge exclusion for clicks in px", 0);
    g_config.follow_hover             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_hover", "follow hover: 0=off 1=on with delay", 0);
    g_config.hover_delay_ms           = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:hover_delay_ms", "hover delay in ms before scroll", 500);
    g_config.overview_animate         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:overview_animate", "animate overview transitions (0=instant, 1=animated)", 1);
    g_config.peek_width               = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:peek_width", "px of neighbouring columns kept visible at the sides", 0);
    g_config.stretch_to_fill          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:stretch_to_fill", "stretch columns to fill the screen when they are narrower than it", 0);
    g_config.fullscreen_screen_sized  = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:fullscreen_screen_sized", "fullscreen X11 windows that open at screen size (games) instead of tiling them", 0);
    g_config.new_window_placement     = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:new_window_placement", "where new windows open: column (own column) or stack (focused column)", "column");
    g_config.stack_after_columns      = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:stack_after_columns", "with column placement: once a workspace has this many columns, new windows stack below the focused one (0 = never)", 0);
    g_config.column_scroll            = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:column_scroll", "columns scroll vertically; window heights are independent fractions of the screen", 0);
    g_config.overview_zoom            = makeShared<Config::Values::CFloatValue>("plugin:hyprscrolling:overview_zoom", "maximum overview zoom (it zooms out further if needed to fit everything)", 0.5);
    g_config.overview_titles          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:overview_titles", "show window titles in the overview", 1);
    g_config.overview_accent          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:overview_accent", "overview highlight colour, 0xAARRGGBB", 0xff89b4fa);
    g_config.predict_size             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:predict_size",
                                                                            "tell new windows their column size up front (0: let them pick their own, e.g. when they'll float)", 1);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.fullscreen_on_one_column);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.column_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.window_default_height);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_fit_method);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_focus);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_min_visible);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_debounce_ms);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_wrap);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.center_active_column);
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
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.overview_animate);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.peek_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.stretch_to_fill);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.fullscreen_screen_sized);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.new_window_placement);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.stack_after_columns);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.column_scroll);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.overview_zoom);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.overview_titles);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.overview_accent);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.predict_size);

    bool success = HyprlandAPI::addTiledAlgo(PHANDLE, "hyprscrolling", &typeid(CScrollingLayout), []() -> UP<Layout::ITiledAlgorithm> {
        return makeUnique<CScrollingLayout>();
    });

    if (!success) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Failure in initialization: failed to register tiled algorithm",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Algorithm registration failed");
    }

    HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 3000);

    return {"hyprscrolling", "A scrolling (column-based) layout for Hyprland", "Nyks45, based on hyprscrolling by Vaxry", "2.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Hyprland exiting (logout) with a fullscreen window: unmapping it later
    // takes it out of fullscreen through this layout's state, after the
    // plugin is gone (a crash). Do that now, while it's still loaded.
    if (g_pCompositor->m_isShuttingDown) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w->m_isMapped && Fullscreen::controller()->getFullscreenModes(w).internal != Fullscreen::FSMODE_NONE)
                Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        }
    }
    HyprlandAPI::removeAlgo(PHANDLE, "hyprscrolling");
}
