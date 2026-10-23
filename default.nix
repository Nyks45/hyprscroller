{
  lib,
  hyprland,
  hyprlandPlugins,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprscrolling";
  version = "2.1";
  src = ./.;

  inherit (hyprland) nativeBuildInputs;

  meta = with lib; {
    homepage = "https://github.com/Nyks45/hyprscroller";
    description = "Hyprland scrolling layout plugin";
    # Upstream hyprscrolling code is BSD-3-Clause, fork changes are MIT (see LICENSE).
    license = with licenses; [ bsd3 mit ];
    platforms = platforms.linux;
  };
}
