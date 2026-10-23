#pragma once

#include "Scrolling.hpp"

#include <chrono>
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
// keep their size and nothing reflows; their buffers are drawn scaled. While open, the overview owns pointer and
// keyboard input (click/Enter to pick, arrows/wheel to move the selection,
// drag to rearrange, Esc/Tab to close) and draws a dimmed backdrop, a
// highlight around the hovered/selected window and title labels.
class COverview {
  public:
    explicit COverview(CScrollingLayout* layout);
    ~COverview();

    bool active() const {
        return m_active;
    }

    void toggle();
    void open();
    // Zoom back in. `pick`, if set, becomes the focused window; otherwise the
    // window that was focused when the overview opened is restored.
    void close(SP<SScrollingWindowData> pick);
    // Close, picking the keyboard selection.
    void confirm();
    // (Re)target every window to its overview slot. recalculate() calls this
    // while the overview is open, so windows opening/closing stay in place.
    void apply(bool instant = false);
    // Called from the render pass element.
    void draw(eRenderStage stage);

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

    CScrollingLayout*                                        m_layout = nullptr;

    bool                                                     m_active  = false;
    bool                                                     m_overlay = false; // drawn while open and while fading out
    std::chrono::steady_clock::time_point                    m_fadeStart;

    std::vector<SSlot>                                       m_slots;
    std::vector<SColumnSlot>                                 m_columnSlots;

    WP<SScrollingWindowData>                                 m_selected;
    WP<SScrollingWindowData>                                 m_hovered;
    WP<SScrollingWindowData>                                 m_dragging;
    WP<Layout::ITarget>                                      m_initialFocus;
    bool                                                     m_pressed   = false;
    bool                                                     m_dragMoved = false;
    Vector2D                                                 m_pressPos;
    Vector2D                                                 m_grabOffset;

    std::unordered_map<std::string, SP<Render::ITexture>>    m_titleCache;

    // One progress value (0 -> 1, windowsMove curve) drives every window from
    // its box when the animation (re)started to its slot.
    struct SAnim {
        CBox from, to;
    };
    PHLANIMVAR<float>                                        m_progress;
    std::unordered_map<const SScrollingWindowData*, SAnim>   m_anims;

    CHyprSignalListener                                      m_keyListener;
    CHyprSignalListener                                      m_buttonListener;
    CHyprSignalListener                                      m_moveListener;
    CHyprSignalListener                                      m_axisListener;
    CHyprSignalListener                                      m_renderListener;

    PHLMONITOR                                               monitor() const;
    void                                                     computeSlots();
    const SSlot*                                             slotAt(const Vector2D& pos) const;
    const SSlot*                                             slotFor(const SP<SScrollingWindowData>& data) const;
    float                                                    overlayAlpha() const;
    void                                                     damage() const;
    void                                                     setHovered(const SP<SScrollingWindowData>& data);
    void                                                     moveSelection(Math::eDirection dir);
    void                                                     drop(const Vector2D& pos);
    SP<Render::ITexture>                                     titleTexture(const std::string& title, int maxWidthPx);
    void                                                     stopInput();
    void                                                     step();
    static void                                              placeWindow(const PHLWINDOW& window, const CBox& box);
};
