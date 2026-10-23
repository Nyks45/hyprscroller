# hyprscrolling

A scrolling (column-based) layout plugin for [Hyprland](https://hyprland.org/).

Based on the `hyprscrolling` plugin from [hyprwm/hyprland-plugins](https://github.com/hyprwm/hyprland-plugins) (BSD-3-Clause), ported to the Hyprland 0.56 API and extended with extra features. The idea goes back to [hyprscroller](https://github.com/dawsers/hyprscroller) by dawsers. The Hyprland API port and parts of the implementation were developed with AI assistance. See [LICENSE](./LICENSE) for which license applies to what.

> **Work in progress.** Core functionality is stable and daily-driven. Built and tested against Hyprland 0.56.2.

---

## Features

- Column-based scrolling layout (like PaperWM / Scroller)
- Column pinning to left/right screen edge
- Column collapse/expand toggle
- Zen mode — show only the focused column (follows focus and new windows)
- Optional vertical scrolling within columns (`column_scroll`), niri-style
- Win+Tab-style overview grid (flat tile view with click-to-focus and keyboard navigation)
- Focus history (back/forward navigation)
- Per-window-class automatic column width rules
- Column-level workspace movement
- Preset column width cycling
- All columns fit-to-screen operations

---

## Installation

The plugin must be built against the exact Hyprland version you run. Rebuild it after every Hyprland update.

### With hyprpm

```bash
hyprpm add https://github.com/Nyks45/hyprscroller
hyprpm enable hyprscrolling
```

### Build from source

**Requirements:** `hyprland` (headers), `libdrm`, `libinput`, `libudev`, `pangocairo`, `pixman-1`, `wayland-server`, `xkbcommon`

```bash
git clone https://github.com/Nyks45/hyprscroller
cd hyprscroller
make
```

This produces `hyprscrolling.so` in the project directory.

### Load the plugin

Lua config (`hyprland.lua`, Hyprland 0.56+):

```lua
hl.plugin.load("/path/to/hyprscroller/hyprscrolling.so")

hl.config({
    general = {
        layout = "hyprscrolling",
    },
})
```

hyprlang config (`hyprland.conf`):

```
plugin = /path/to/hyprscroller/hyprscrolling.so
general {
    layout = hyprscrolling
}
```

To try it in a running session without editing your config: `hyprctl plugin load /path/to/hyprscrolling.so`.

---

## Configuration

All options live under `plugin:hyprscrolling:`, i.e. `plugin = { hyprscrolling = { ... } }` in Lua or `plugin { hyprscrolling { ... } }` in hyprlang.

| Option | Type | Default | Description |
|---|---|---|---|
| `column_width` | float (0–1) | `0.5` | Default column width as a fraction of monitor width |
| `window_default_height` | float (0–1) | `1.0` | Height of a new window in a column. `1.0` splits the column equally; smaller values give the new window that share, never squeezing others below 10%. With `column_scroll`, it's the window's height as a fraction of the screen (`1.0` = full height) |
| `fullscreen_on_one_column` | bool | `false` | If there's only one column, make it fullscreen |
| `focus_fit_method` | int | `0` | Column scroll behaviour on focus: `0` = center, `1` = fit into view, `2` = gap-free center |
| `follow_focus` | int | `1` | `0` = off, `1` = scroll on keyboard/programmatic focus, `2` = scroll on all focus including mouse hover (immediately, no timer) |
| `follow_min_visible` | float (0–1) | `0.0` | Minimum visible fraction of a window before scrolling (0 = always scroll) |
| `follow_debounce_ms` | int | `0` | Debounce time (ms) for follow_focus events |
| `follow_hover` | bool | `false` | Scroll to a window when the cursor hovers over it (uses `hover_delay_ms`) |
| `hover_delay_ms` | int | `500` | Delay in ms before a hover triggers scrolling |
| `focus_wrap` | bool | `true` | Wrap focus at column/window edges |
| `center_active_column` | bool | `false` | Always scroll to center the focused column after every focus change |
| `column_widths` | string | `0.333,0.5,0.667,1.0` | Comma-separated preset widths for `colresize +conf` / `-conf` cycling |
| `explicit_column_widths` | string | *(empty)* | Same as `column_widths`; takes precedence when set |
| `window_heights` | string | `0.333,0.5,0.667,1.0` | Comma-separated preset heights for `rowresize +conf` / `-conf` cycling |
| `collapsed_width` | int (px) | `30` | Width of a collapsed column in pixels |
| `focus_history` | bool | `true` | Enable focus history for `focusback` / `focusfwd` |
| `auto_width_rules` | string | *(empty)* | Per-class automatic column width: `firefox:0.7, kitty:0.3` |
| `click_edge_left` | int (px) | `0` | Extra fixed margin at the left monitor edge where clicks/hover don't scroll. Usually not needed: clicks and hover on the shell (reserved bar areas, and panels/drawers/notifications in the top/overlay layers) are detected and ignored automatically |
| `click_edge_right` | int (px) | `0` | Same for the right monitor edge |
| `overview_animate` | bool | `false` | Animate overview enter/exit transitions. Leave off on iGPUs for an instant snap |
| `peek_width` | int (px) | `0` | Keep this much of the neighbouring columns visible at each side, so off-screen windows stay visible and can be hovered/clicked. Column widths are fractions of the usable width minus the two peek strips |
| `stretch_to_fill` | bool | `false` | When the columns together are narrower than the screen, stretch them to fill it instead of centring them |
| `new_window_placement` | string | `column` | Where new windows open: `column` = in their own new column next to the focused one, `stack` = in the focused column, below the focused window |
| `column_scroll` | bool | `false` | Columns scroll vertically: windows in a column keep their own height instead of sharing the screen, a new stacked window opens below and the column scrolls to it, and focusing a window scrolls its column. `peek_width` also applies vertically. Resizing changes only that window's height |
| `fullscreen_screen_sized` | bool | `false` | Make X11 windows that open asking for the full screen size (borderless games and the like) fullscreen instead of squeezing them into a column. Windows that request fullscreen are always honoured |

A config reload resets `togglefit` and `placement` overrides back to `focus_fit_method` and `new_window_placement`.

### Example

Lua:

```lua
hl.config({
    plugin = {
        hyprscrolling = {
            column_width             = 0.5,
            fullscreen_on_one_column = false,
            focus_fit_method         = 0,
            follow_focus             = 1,
            column_widths            = "0.333, 0.5, 0.667, 1.0",
            collapsed_width          = 30,
            focus_history            = true,
            auto_width_rules         = "firefox:0.7, kitty:0.3, code-oss:0.6",
            peek_width               = 120,
            stretch_to_fill          = true,
            fullscreen_screen_sized  = true,
        },
    },
})
```

hyprlang:

```
plugin {
    hyprscrolling {
        column_width = 0.5
        fullscreen_on_one_column = false
        focus_fit_method = 0
        follow_focus = 1
        column_widths = 0.333, 0.5, 0.667, 1.0
        collapsed_width = 30
        focus_history = true
        auto_width_rules = firefox:0.7, kitty:0.3, code-oss:0.6
        peek_width = 120
        stretch_to_fill = true
        fullscreen_screen_sized = true
    }
}
```

---

## Layout Messages

Send commands with the layout message dispatcher: `hl.dsp.layout("<message>")` in Lua, or `layoutmsg, <message>` in hyprlang.

### Focus

| Message | Description |
|---|---|
| `focus l/r/u/d [nowrap]` | Move focus in direction (`t`/`b` are aliases for `u`/`d`). Wraps at the edges when `focus_wrap` is on, unless `nowrap` is given (useful for scroll-wheel binds) |
| `focusback` | Go back in focus history |
| `focusfwd` | Go forward in focus history |

### Window Movement

| Message | Description |
|---|---|
| `movewindowto l/r/u/d` | Move window to adjacent column/stack, keeping its vertical position. Moving past the first/last column gives the window its own column |
| `promote` | Promote window to its own new column |
| `placement column/stack/toggle` | Switch where new windows open (see `new_window_placement`) on all workspaces until the next config reload. Shows a short notification |

### Column Operations

| Message | Params | Description |
|---|---|---|
| `swapcol l/r` | `l` or `r` | Swap current column with its left/right neighbor. Wraps around |
| `colresize` | `0.5`, `+0.2`, `-0.2`, `+conf`, `-conf`, `all 0.5` | Resize current column width (or all columns). `+conf`/`-conf` cycles through `column_widths` |
| `rowresize` | `0.5`, `+0.1`, `-0.1`, `+conf`, `-conf` | Resize current window height within its column. `+conf`/`-conf` cycles through `window_heights` |
| `movecoltoworkspace` | `1`, `+1`, `-1`, `special`, etc. | Move entire current column to a workspace |
| `togglecollapse` | — | Fold / expand the current column |
| `collapse` | — | Fold the current column |
| `expand` | — | Expand the current column |

### Scrolling

| Message | Params | Description |
|---|---|---|
| `move` | `+col` (or `col`), `-col`, `+200`, `-200` | Scroll layout horizontally by columns or pixels |

### Fit Operations

| Message | Params | Description |
|---|---|---|
| `fit` | `active`, `visible`, `all`, `toend`, `tobeg` | Resize/arrange columns to fit the screen |
| `togglefit` | — | Cycle `focus_fit_method` through center (0) → fit (1) → gap-free center (2) → center … |

### View Modes

| Message | Description |
|---|---|
| `zen` | Show only the focused column (focus mode). Follows focus and new windows; toggle to exit |
| `overview` | Toggle Win+Tab-style flat grid of all windows. Click a window to focus it and restore the layout |
| `overview_confirm` | Confirm the currently focused window in overview and exit (use with `focus l/r/u/d` for keyboard navigation) |
| `pin left` | Pin current column to the left screen edge (stays fixed while scrolling) |
| `pin right` | Pin current column to the right screen edge |
| `unpin` | Remove pin from current column |

---

## Example Keybindings

A full hyprlang example is in [`scrolling.conf`](./scrolling.conf). A starting point in Lua:

```lua
local function lm(msg) return hl.dsp.layout(msg) end

-- Focus movement
hl.bind("SUPER + H", lm("focus l"))
hl.bind("SUPER + L", lm("focus r"))
hl.bind("SUPER + K", lm("focus u"))
hl.bind("SUPER + J", lm("focus d"))

-- Window movement
hl.bind("SUPER + SHIFT + H", lm("movewindowto l"))
hl.bind("SUPER + SHIFT + L", lm("movewindowto r"))
hl.bind("SUPER + SHIFT + K", lm("movewindowto u"))
hl.bind("SUPER + SHIFT + J", lm("movewindowto d"))
hl.bind("SUPER + P", lm("promote"))
hl.bind("SUPER + J", lm("placement toggle")) -- new windows: own column <-> stack

-- Column resize
hl.bind("SUPER + equal", lm("colresize +0.05"))
hl.bind("SUPER + minus", lm("colresize -0.05"))
hl.bind("SUPER + bracketright", lm("colresize +conf"))
hl.bind("SUPER + bracketleft", lm("colresize -conf"))

-- niri-style mouse wheel: Super+wheel moves through the windows of the column,
-- Super+Shift+wheel moves between columns (no wrap-around at the ends)
hl.bind("SUPER + mouse_down", lm("focus d nowrap"))
hl.bind("SUPER + mouse_up", lm("focus u nowrap"))
hl.bind("SUPER + SHIFT + mouse_down", lm("focus r nowrap"))
hl.bind("SUPER + SHIFT + mouse_up", lm("focus l nowrap"))

-- Fit, zen, overview
hl.bind("SUPER + F", lm("fit active"))
hl.bind("SUPER + T", lm("togglefit"))
hl.bind("SUPER + Z", lm("zen"))
hl.bind("SUPER + Tab", lm("overview"))
```

The same in hyprlang:

```
bind = SUPER, H, layoutmsg, focus l
bind = SUPER, L, layoutmsg, focus r
bind = SUPER, K, layoutmsg, focus u
bind = SUPER, J, layoutmsg, focus d

bind = SUPER SHIFT, H, layoutmsg, movewindowto l
bind = SUPER SHIFT, L, layoutmsg, movewindowto r
bind = SUPER SHIFT, K, layoutmsg, movewindowto u
bind = SUPER SHIFT, J, layoutmsg, movewindowto d
bind = SUPER, P, layoutmsg, promote

bind = SUPER, equal, layoutmsg, colresize +0.05
bind = SUPER, minus, layoutmsg, colresize -0.05
bind = SUPER, bracketright, layoutmsg, colresize +conf
bind = SUPER, bracketleft, layoutmsg, colresize -conf

bind = SUPER, F, layoutmsg, fit active
bind = SUPER, T, layoutmsg, togglefit
bind = SUPER, Z, layoutmsg, zen
bind = SUPER, Tab, layoutmsg, overview
```

---

## Build Systems

Three build systems are included — pick whichever fits your setup. All produce `hyprscrolling.so`.

| File | Command |
|---|---|
| `Makefile` | `make` |
| `CMakeLists.txt` | `cmake -B build && cmake --build build` |
| `meson.build` | `meson setup build && ninja -C build` |

---

## License

BSD-3-Clause for code from upstream hyprscrolling, MIT for this fork's changes. See [LICENSE](./LICENSE).
