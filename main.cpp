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
    g_config.focus_fit_method         = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_fit_method", "focus fit method", 0);
    g_config.follow_focus             = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_focus", "follow focus", 1);
    g_config.follow_debounce_ms       = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:follow_debounce_ms", "follow debounce ms", 0);
    g_config.explicit_column_widths   = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:explicit_column_widths", "explicit column widths", "0.333,0.5,0.667,1.0");
    g_config.collapsed_width          = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:collapsed_width", "collapsed column width in px", 30);
    g_config.focus_history            = makeShared<Config::Values::CIntValue>("plugin:hyprscrolling:focus_history", "enable focus history", 1);
    g_config.auto_width_rules         = makeShared<Config::Values::CStringValue>("plugin:hyprscrolling:auto_width_rules", "auto width rules per class", "");

    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.fullscreen_on_one_column);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.column_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_fit_method);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_focus);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.follow_debounce_ms);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.explicit_column_widths);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.collapsed_width);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.focus_history);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_config.auto_width_rules);

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
