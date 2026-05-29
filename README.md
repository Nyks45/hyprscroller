# hyprscrolling

A scrolling (column-based) layout plugin for [Hyprland](https://hyprland.org/).

Inspired by [hyprscroller](https://github.com/dawsers/hyprscroller) (MIT) — rebuilt from scratch because upstream wasn't being updated, with extra features added on top. The Hyprland API and initial implementation were developed with AI assistance.

> **Work in progress.** Core functionality is stable and daily-driven.

---

## Features

- Column-based scrolling layout (like PaperWM / Scroller)
- Column pinning to left/right screen edge
- Column collapse/expand toggle
- Zen mode — show only the focused column
- Win+Tab-style overview grid (flat tile view with click-to-focus and keyboard navigation)
- Focus history (back/forward navigation)
- Per-window-class automatic column width rules
- Column-level workspace movement
- Preset column width cycling
- All columns fit-to-screen operations

---

## Installation

### Build from source

**Requirements:** `hyprland`, `libdrm`, `libinput`, `libudev`, `pangocairo`, `pixman-1`, `wayland-server`, `xkbcommon`

```bash
git clone https://github.com/kuroiko0429/hyprscrolling
cd hyprscrolling
make
```

This produces `hyprscrolling.so` in the project directory.

### Load the plugin

Add to your `hyprland.conf`:

```
plugin = /path/to/hyprscrolling.so
general {
    layout = hyprscrolling
}
```

---

## Configuration

All options go inside `plugin { hyprscrolling { ... } }` in your Hyprland config.

| Option | Type | Default | Description |
|---|---|---|---|
| `column_width` | float (0–1) | `0.5` | Default column width as a fraction of monitor width |
| `window_default_height` | float (0–1) | `1.0` | Default height for new windows in a column (1.0 = equal split) |
| `fullscreen_on_one_column` | bool | `false` | If there's only one column, make it fullscreen |
| `focus_fit_method` | int | `0` | Column scroll behaviour on focus: `0` = center, `1` = fit into view, `2` = gap-free center |
| `follow_focus` | int | `1` | `0` = off, `1` = scroll on keyboard/programmatic focus, `2` = scroll on all focus including mouse hover (immediately, no timer) |
| `follow_min_visible` | float (0–1) | `0.0` | Minimum visible fraction of a window before scrolling (0 = always scroll) |
| `follow_debounce_ms` | int | `0` | Debounce time (ms) for follow_focus events |
| `follow_hover` | bool | `false` | Scroll to a window when the cursor hovers over it (uses `hover_delay_ms`) |
| `hover_delay_ms` | int | `500` | Delay in ms before a hover triggers scrolling |
| `focus_wrap` | bool | `true` | Wrap focus at column/window edges |
| `center_active_column` | bool | `false` | Always scroll to center the focused column after every focus change |
| `explicit_column_widths` | string | `0.333,0.5,0.667,1.0` | Comma-separated preset widths for `colresize +conf` / `-conf` cycling |
| `column_widths` | string | `0.333,0.5,0.667,1.0` | Alias for `explicit_column_widths` |
| `window_heights` | string | `0.333,0.5,0.667,1.0` | Comma-separated preset heights for `rowresize +conf` / `-conf` cycling |
| `collapsed_width` | int (px) | `30` | Width of a collapsed column in pixels |
| `focus_history` | bool | `true` | Enable focus history for `focusback` / `focusfwd` |
| `auto_width_rules` | string | `` | Per-class automatic column width: `firefox:0.7, kitty:0.3` |
| `click_edge_left` | int (px) | `90` | Cursor x distance from left monitor edge that suppresses click-to-scroll |
| `click_edge_right` | int (px) | `60` | Cursor x distance from right monitor edge that suppresses click-to-scroll |
| `overview_animate` | bool | `true` | Animate overview enter/exit transitions. Disable on iGPUs for instant snap |

### Example

```
plugin {
    hyprscrolling {
        column_width = 0.5
        fullscreen_on_one_column = false
        focus_fit_method = 0
        follow_focus = true
        follow_debounce_ms = 0
        explicit_column_widths = 0.333, 0.5, 0.667, 1.0
        collapsed_width = 30
        focus_history = true
        auto_width_rules = firefox:0.7, kitty:0.3, code-oss:0.6
    }
}
```

---

## Layout Messages

Use `layoutmsg` dispatcher to send commands:

```
bind = SUPER, key, layoutmsg, <message>
```

### Focus

| Message | Description |
|---|---|
| `focus l/r/u/d` | Move focus in direction, wraps instead of jumping to adjacent monitor |
| `focusback` | Go back in focus history |
| `focusfwd` | Go forward in focus history |

### Window Movement

| Message | Description |
|---|---|
| `movewindowto l/r/u/d` | Move window to adjacent column/stack. Moving right at the last column promotes the window to a new column |
| `promote` | Promote window to its own new column |

### Column Operations

| Message | Params | Description |
|---|---|---|
| `swapcol l/r` | `l` or `r` | Swap current column with its left/right neighbor. Wraps around |
| `colresize` | `0.5`, `+0.2`, `-0.2`, `+conf`, `-conf`, `all 0.5` | Resize current column width (or all columns). `+conf`/`-conf` cycles through `explicit_column_widths` |
| `rowresize` | `0.5`, `+0.1`, `-0.1`, `+conf`, `-conf` | Resize current window height within its column. `+conf`/`-conf` cycles through `window_heights` |
| `movecoltoworkspace` | `1`, `+1`, `-1`, `special`, etc. | Move entire current column to a workspace |
| `togglecollapse` | — | Fold / expand the current column |

### Scrolling

| Message | Params | Description |
|---|---|---|
| `move` | `+col`, `-col`, `+200`, `-200` | Scroll layout horizontally by columns or pixels |

### Fit Operations

| Message | Params | Description |
|---|---|---|
| `fit` | `active`, `visible`, `all`, `toend`, `tobeg` | Resize/arrange columns to fit the screen |
| `togglefit` | — | Cycle `focus_fit_method` through center (0) → fit (1) → gap-free center (2) → center … |

### View Modes

| Message | Description |
|---|---|
| `zen` | Show only the focused column (focus mode). Toggle to exit |
| `overview` | Toggle Win+Tab-style flat grid of all windows. Click a window to focus it and restore the layout |
| `overview_confirm` | Confirm the currently focused window in overview and exit (use with `focus l/r/u/d` for keyboard navigation) |
| `pin left` | Pin current column to the left screen edge (stays fixed while scrolling) |
| `pin right` | Pin current column to the right screen edge |
| `unpin` | Remove pin from current column |

---

## Example Keybindings

Copy from [`scrolling.conf`](./scrolling.conf) or use this as a starting point:

```
# Focus movement
bind = SUPER, H, layoutmsg, focus l
bind = SUPER, L, layoutmsg, focus r
bind = SUPER, K, layoutmsg, focus u
bind = SUPER, J, layoutmsg, focus d

# Window movement
bind = SUPER SHIFT, H, layoutmsg, movewindowto l
bind = SUPER SHIFT, L, layoutmsg, movewindowto r
bind = SUPER SHIFT, K, layoutmsg, movewindowto u
bind = SUPER SHIFT, J, layoutmsg, movewindowto d
bind = SUPER, P, layoutmsg, promote

# Column swap
bind = SUPER ALT, H, layoutmsg, swapcol l
bind = SUPER ALT, L, layoutmsg, swapcol r

# Column resize
bind = SUPER, equal, layoutmsg, colresize +0.05
bind = SUPER, minus, layoutmsg, colresize -0.05
bind = SUPER, bracketright, layoutmsg, colresize +conf
bind = SUPER, bracketleft, layoutmsg, colresize -conf
bind = SUPER SHIFT, equal, layoutmsg, colresize all 0.5

# Scrolling
bind = SUPER, period, layoutmsg, move +col
bind = SUPER, comma, layoutmsg, move -col
bind = SUPER SHIFT, period, layoutmsg, move +200
bind = SUPER SHIFT, comma, layoutmsg, move -200

# Fit
bind = SUPER, F, layoutmsg, fit active
bind = SUPER SHIFT, F, layoutmsg, fit visible
bind = SUPER CTRL, F, layoutmsg, fit all
bind = SUPER, T, layoutmsg, togglefit

# Pin
bind = SUPER CTRL, bracketleft, layoutmsg, pin left
bind = SUPER CTRL, bracketright, layoutmsg, pin right
bind = SUPER CTRL, backslash, layoutmsg, unpin

# Collapse
bind = SUPER, C, layoutmsg, togglecollapse

# Zen mode
bind = SUPER, Z, layoutmsg, zen

# Focus history
bind = SUPER ALT, bracketleft, layoutmsg, focusback
bind = SUPER ALT, bracketright, layoutmsg, focusfwd

# Move column to workspace
bind = SUPER CTRL SHIFT, 1, layoutmsg, movecoltoworkspace 1
bind = SUPER CTRL SHIFT, 2, layoutmsg, movecoltoworkspace 2
bind = SUPER CTRL SHIFT, 3, layoutmsg, movecoltoworkspace 3
bind = SUPER CTRL SHIFT, right, layoutmsg, movecoltoworkspace +1
bind = SUPER CTRL SHIFT, left, layoutmsg, movecoltoworkspace -1
bind = SUPER CTRL SHIFT, S, layoutmsg, movecoltoworkspace special
```

---

## Build Systems

Three build systems are included — pick whichever fits your setup:

| File | Command |
|---|---|
| `Makefile` | `make` |
| `CMakeLists.txt` | `cmake -B build && cmake --build build` |
| `meson.build` | `meson setup build && ninja -C build` |

---

## License

MIT
