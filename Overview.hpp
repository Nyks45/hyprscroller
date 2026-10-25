#pragma once

#include "Scrolling.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/render/Texture.hpp>

// niri-style overview: the whole scrolling strip of a workspace zooms out so
// every column (and every stacked window) is visible in its real arrangement.
//
// The zoom is purely visual: windows' on-screen boxes are moved to scaled
// slots without telling clients (Hyprland sends a configure whenever a
// window's size *animation* begins, so boxes are driven frame by frame from
// one progress animation that uses the windowsMove curve instead). Clients
// keep their size and nothing reflows; their buffers are drawn scaled.
//
// While open, the overview owns pointer and keyboard input: click/Enter to
// pick, arrows/wheel to move the selection, drag to rearrange, right-click
// for a window/column menu, Esc/Tab to close. It draws a dimmed backdrop, a
// highlight that slides to the hovered/selected window, title labels and the
// menu.
class COverview {
  public:
    explicit COverview(CScrollingLayout* layout);
    ~COverview();

    bool active() const {
        return m_active;
    }

    void toggle();
    void open();
    // Alt+Tab: open (or step) with the most recently used windows first; the
    // overview closes on picking the selection once Alt is released.
    void switchWindows(bool reverse);
    // Zoom back in. `pick`, if set, becomes the focused window; otherwise the
    // currently focused window is kept.
    void close(SP<SScrollingWindowData> pick);
    // Close, picking the keyboard selection.
    void confirm();
    // (Re)target every window to its overview slot. recalculate() calls this
    // while the overview is open, so windows opening/closing stay in place.
    void apply(bool instant = false);
    // Called from the render pass element.
    void draw(eRenderStage stage);
    // A window is leaving the layout while open. Hyprland then force-reports
    // every window's current (thumbnail) size to its client; undo that.
    void onWindowRemoved();

  private:
    struct SSlot {
        WP<SScrollingWindowData> data;
        WP<SColumnData>          column;
        CBox                     box; // global logical coords, gaps applied
    };

    struct SColumnSlot {
        WP<SColumnData> column;
        CBox            box; // full-height column area, global logical coords
    };

    struct SMenuItem {
        std::string           label;
        std::function<void()> action;
        bool                  submenu   = false; // shows a chevron; action swaps the item list
        bool                  separator = false;
        bool                  header    = false; // non-interactive title line
        bool                  checked   = false; // current value in a preset list
    };

    CScrollingLayout*                                      m_layout = nullptr;

    bool                                                   m_active  = false;
    bool                                                   m_overlay = false; // drawn while open and while fading out
    std::chrono::steady_clock::time_point                  m_fadeStart;

    std::vector<SSlot>                                     m_slots;
    // Floating windows (desktop mode) stand in as window data of a column that
    // isn't in the layout, while open; their layoutBox is where they were.
    SP<SColumnData>                                        m_floatingColumn;
    std::vector<SP<SScrollingWindowData>>                  m_floating;
    bool                                                   m_switchMode = false;
    // Alt+Tab: watches for Alt being let go (any keyboard), then switches.
    SP<CEventLoopTimer>                                    m_altTimer;
    // Alt+Tab pressed, switcher not shown yet: letting go of Alt quickly just
    // flips windows (no overview); held a little longer, the switcher opens.
    // m_pendingSteps counts Tab presses meanwhile (Shift+Tab back).
    bool                                                   m_switchPending = false;
    int                                                    m_pendingSteps  = 0;
    SP<CEventLoopTimer>                                    m_showTimer;
    void                                                   quickSwitch(int steps);
    void                                                   showSwitcher(int steps);
    // The selected window's close button (global logical), and its hover.
    CBox                                                   closeButtonBox(const CBox& thumb) const;
    bool                                                   m_closeHover = false;
    void                                                   closeSelected();
    bool                                                   isFloating(const SP<SScrollingWindowData>& data) const;
    void                                                   collectFloating();
    std::vector<SP<SScrollingWindowData>>                  mruOrder() const;
    std::vector<SColumnSlot>                               m_columnSlots;

    WP<SScrollingWindowData>                               m_selected; // keyboard/hover selection (highlighted)
    WP<SScrollingWindowData>                               m_dragging; // pressed window; dragged once m_dragMoved
    WP<Layout::ITarget>                                    m_initialFocus;
    bool                                                   m_pressed       = false;
    bool                                                   m_dragMoved     = false;
    bool                                                   m_ignoreRelease = false;
    Vector2D                                               m_pressPos;
    Vector2D                                               m_grabOffset;

    // Highlight slides from where it was drawn last to the selected window.
    CBox                                                   m_highlightFrom;
    CBox                                                   m_highlightLast;
    std::chrono::steady_clock::time_point                  m_highlightStart;

    // Right-click menu.
    bool                                                   m_menuOpen = false;
    WP<SScrollingWindowData>                               m_menuTarget;
    std::vector<SMenuItem>                                 m_menuItems;
    std::vector<CBox>                                      m_menuItemBoxes; // global logical, parallel to m_menuItems
    CBox                                                   m_menuBox;
    int                                                    m_menuHover = -1;

    std::unordered_map<std::string, SP<Render::ITexture>>  m_textCache;

    // True while this open/close cycle is open; deferred callbacks hold a weak
    // ref so they never act on a closed overview or a destroyed layout.
    SP<bool>                                               m_session;
    // Windows zooming back after close. They keep m_animatingIn (drawn scaled)
    // until their move/resize animation ends, then it's cleared: Hyprland
    // won't, and leaving it set would squash every later shrink animation.
    std::vector<PHLWINDOWREF>                              m_restoring;
    // Windows marked as in the overview (see markInOverview): from open until
    // they're back at full size after close.
    std::vector<PHLWINDOWREF>                              m_marked;
    // Lives as long as this object; deferred callbacks that aren't tied to an
    // open session (opening after leaving fullscreen) check it.
    SP<bool>                                               m_alive = makeShared<bool>(true);

    // Window that was fullscreen (or maximized) when the overview opened, and
    // its modes; it is restored if it's picked again (or nothing else is).
    PHLWINDOWREF                                           m_fullscreenWindow;
    int8_t                                                 m_fullscreenInternal = 0;
    int8_t                                                 m_fullscreenClient   = 0;

    // One progress value (0 -> 1, windowsMove curve) drives every window from
    // its box when the animation (re)started to its slot.
    struct SAnim {
        CBox from, to;
    };
    PHLANIMVAR<float>                                      m_progress;
    std::unordered_map<const SScrollingWindowData*, SAnim> m_anims;

    CHyprSignalListener                                    m_keyListener;
    CHyprSignalListener                                    m_buttonListener;
    CHyprSignalListener                                    m_moveListener;
    CHyprSignalListener                                    m_axisListener;
    CHyprSignalListener                                    m_closeListener; // floating windows closing while open
    CHyprSignalListener                                    m_renderListener;

    PHLMONITOR                                             monitor() const;
    void                                                   computeSlots();
    const SSlot*                                           slotAt(const Vector2D& pos) const;
    const SSlot*                                           slotFor(const SP<SScrollingWindowData>& data) const;
    float                                                  overlayAlpha() const;
    void                                                   damage() const;
    void                                                   select(const SP<SScrollingWindowData>& data);
    void                                                   moveSelection(Math::eDirection dir);
    void                                                   drop(const Vector2D& pos);
    SP<Render::ITexture>                                   text(const std::string& str, int maxWidthPx);
    void                                                   stopInput();
    void                                                   step();
    static void                                            placeWindow(const PHLWINDOW& window, const CBox& box);

    void                                                   openMenu(const SP<SScrollingWindowData>& data, const Vector2D& pos);
    void                                                   setMenuItems(std::vector<SMenuItem>&& items);
    std::vector<SMenuItem>                                 rootMenu(const SP<SScrollingWindowData>& data);
    void                                                   closeMenu();
    int                                                    menuItemAt(const Vector2D& pos) const;
    void                                                   activateMenuItem(int idx);
    void                                                   moveMenuHover(int delta);
    void                                                   drawMenu(const PHLMONITOR& mon, float alpha);
};
