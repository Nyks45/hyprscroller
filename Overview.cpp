#include "Overview.hpp"
#include "Compat.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <xkbcommon/xkbcommon.h>

using namespace Render::GL;

// Layout of the zoomed-out strip, in logical px.
constexpr double OV_MARGIN          = 48.0; // free space around the strip
constexpr double OV_GAP             = 10.0; // space between thumbnails
constexpr double OV_DRAG_THRESHOLD  = 8.0;  // pointer travel before a press becomes a drag
constexpr double OV_NEW_COLUMN_EDGE = 0.2;  // drop in the outer 20% of a column -> new column beside it
constexpr int    OV_FADE_MS         = 220;  // backdrop/label fade
constexpr int    OV_HIGHLIGHT_MS    = 110;  // highlight slide between windows
constexpr float  OV_DIM             = 0.45F;

// Right-click menu, logical px.
constexpr double MENU_WIDTH    = 250.0;
constexpr double MENU_ITEM_H   = 30.0;
constexpr double MENU_HEADER_H = 30.0;
constexpr double MENU_SEP_H    = 9.0;
constexpr double MENU_PAD      = 6.0;

// Draws the overview's backdrop (before windows) or highlight, labels and
// menu (after windows) for the monitor being rendered.
class COverviewPassElement : public IPassElement {
  public:
    COverviewPassElement(COverview* overview, eRenderStage stage) : m_overview(overview), m_stage(stage) {
        ;
    }
    virtual ~COverviewPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override {
        m_overview->draw(m_stage);
        return {};
    }
    virtual bool needsLiveBlur() override {
        return false;
    }
    virtual bool needsPrecomputeBlur() override {
        return false;
    }
    virtual const char* passName() override {
        return "COverviewPassElement";
    }
    virtual ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    COverview*   m_overview = nullptr;
    eRenderStage m_stage;
};

static CHyprColor accentColor(float alpha) {
    const auto ARGB = (uint32_t)g_config.overview_accent->value();
    return CHyprColor{((ARGB >> 16) & 0xFF) / 255.F, ((ARGB >> 8) & 0xFF) / 255.F, (ARGB & 0xFF) / 255.F, ((ARGB >> 24) & 0xFF) / 255.F * alpha};
}

static double easeOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

static CBox lerpBox(const CBox& a, const CBox& b, double t) {
    return CBox{a.pos() + (b.pos() - a.pos()) * t, a.size() + (b.size() - a.size()) * t};
}

static int msSince(std::chrono::steady_clock::time_point t) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

static PHLWINDOW windowOf(const SP<SScrollingWindowData>& data) {
    const auto TARGET = data ? data->target.lock() : nullptr;
    return TARGET ? TARGET->window() : nullptr;
}

COverview::COverview(CScrollingLayout* layout) : m_layout(layout) {
    ;
}

COverview::~COverview() = default;

PHLMONITOR COverview::monitor() const {
    const auto PARENT = m_layout->m_parent.lock();
    if (!PARENT || !PARENT->space())
        return nullptr;
    const auto WS = PARENT->space()->workspace();
    return WS ? WS->m_monitor.lock() : nullptr;
}

void COverview::damage() const {
    if (const auto MON = monitor())
        g_pHyprRenderer->damageMonitor(MON);
}

float COverview::overlayAlpha() const {
    if (!g_config.overview_animate->value())
        return m_active ? 1.F : 0.F;
    const float T = std::clamp((float)msSince(m_fadeStart) / OV_FADE_MS, 0.F, 1.F);
    return m_active ? T : 1.F - T;
}

void COverview::computeSlots() {
    m_slots.clear();
    m_columnSlots.clear();

    const auto PARENT = m_layout->m_parent.lock();
    if (!PARENT || !PARENT->space())
        return;

    const auto& DATA   = m_layout->m_scrollingData;
    const CBox  WA     = PARENT->space()->workArea();
    const CBox  USABLE = m_layout->usableArea();

    // Same left-to-right order you see on screen: pinned-left, scrollable, pinned-right.
    std::vector<SP<SColumnData>> order;
    for (const auto PIN : {PIN_LEFT, PIN_NONE, PIN_RIGHT}) {
        for (const auto& COL : DATA->columns) {
            if (COL->pinned == PIN && !COL->windowDatas.empty())
                order.push_back(COL);
        }
    }

    struct SColumnLayout {
        SP<SColumnData>     column;
        double              x = 0, w = 0;
        std::vector<double> heights;
    };
    std::vector<SColumnLayout> cols;
    double                     contentW = 0, contentH = 0;
    for (const auto& COL : order) {
        SColumnLayout CL{.column = COL, .x = contentW, .w = DATA->colWidth(COL, USABLE.w), .heights = {}};
        contentH = std::max(contentH, DATA->rowHeights(COL, USABLE.h, CL.heights));
        contentW += CL.w;
        cols.emplace_back(std::move(CL));
    }

    if (contentW <= 0 || contentH <= 0)
        return;

    // Zoom out until everything fits, but never past overview_zoom: a couple of
    // windows shouldn't fill the screen as if nothing happened.
    const double   ZOOM   = std::clamp((double)g_config.overview_zoom->value(), 0.1, 1.0);
    const double   S      = std::min({(WA.w - 2 * OV_MARGIN) / contentW, (WA.h - 2 * OV_MARGIN) / contentH, ZOOM});
    const Vector2D ORIGIN = WA.pos() + (WA.size() - Vector2D{contentW, contentH} * S) / 2.0;

    for (const auto& CL : cols) {
        m_columnSlots.push_back({.column = CL.column, .box = CBox{ORIGIN.x + CL.x * S, ORIGIN.y, CL.w * S, contentH * S}});

        double y = 0;
        for (size_t i = 0; i < CL.column->windowDatas.size(); ++i) {
            CBox box = CBox{ORIGIN.x + CL.x * S, ORIGIN.y + y * S, CL.w * S, CL.heights[i] * S}.expand(-OV_GAP / 2.0);
            m_slots.push_back({.data = CL.column->windowDatas[i], .column = CL.column, .box = box.round()});
            y += CL.heights[i];
        }
    }
}

const COverview::SSlot* COverview::slotAt(const Vector2D& pos) const {
    for (const auto& SLOT : m_slots) {
        if (SLOT.box.containsPoint(pos) && SLOT.data.lock() != m_dragging.lock())
            return &SLOT;
    }
    return nullptr;
}

const COverview::SSlot* COverview::slotFor(const SP<SScrollingWindowData>& data) const {
    for (const auto& SLOT : m_slots) {
        if (SLOT.data.lock() == data)
            return &SLOT;
    }
    return nullptr;
}

void COverview::placeWindow(const PHLWINDOW& window, const CBox& box) {
    // setValueAndWarp, unlike assigning a goal, doesn't fire the size
    // animation's begin callback, which is what sends clients a configure.
    window->positionAnimation()->setValueAndWarp(box.pos());
    window->sizeAnimation()->setValueAndWarp(box.size());
    // The renderer crops a buffer larger than its window box (to avoid
    // squashing during resizes) unless the window is animating in; set that
    // so the whole buffer is drawn scaled into the box.
    window->m_animatingIn = true;
}

void COverview::apply(bool instant) {
    computeSlots();

    // The selected window may have closed or moved away: fall back to the
    // focused window, else the first one, so something is always highlighted.
    if (!m_slots.empty() && !slotFor(m_selected.lock())) {
        const auto FOCUSED = m_layout->focusedWindowData();
        m_selected         = slotFor(FOCUSED) ? FOCUSED : m_slots.front().data.lock();
    }

    const auto DRAGGED = m_dragMoved ? m_dragging.lock() : nullptr;

    // Windows appearing/closing can make Hyprland re-check the pointer and let
    // a client set its cursor; the overview keeps the arrow (or the grab hand).
    if (m_active)
        g_pHyprRenderer->setCursorFromName(DRAGGED ? "grabbing" : "left_ptr", true);

    std::unordered_map<const SScrollingWindowData*, SAnim> anims;
    bool                                                   changed = false;
    for (const auto& SLOT : m_slots) {
        const auto DATA   = SLOT.data.lock();
        const auto WINDOW = windowOf(DATA);
        if (!WINDOW || DATA == DRAGGED)
            continue;

        const auto PREV = m_anims.find(DATA.get());
        changed         = changed || PREV == m_anims.end() || PREV->second.to != SLOT.box;

        // Start from wherever the window is drawn right now, so retargeting
        // mid-animation (opening/closing windows, drops) is smooth. Windows
        // parked off-screen by zen mode, or never laid out yet (opened while
        // the overview is up), would fly in from far away; they appear at
        // their slot instead.
        const CBox CURRENT{WINDOW->positionAnimation()->value(), WINDOW->sizeAnimation()->value()};
        const bool APPEAR = DATA->parked || DATA->layoutBox.empty();
        anims[DATA.get()] = {.from = APPEAR ? SLOT.box : CURRENT, .to = SLOT.box};
        DATA->parked      = false;
    }
    changed = changed || anims.size() != m_anims.size();

    if (!m_progress) {
        Animation::mgr()->createAnimation(0.F, m_progress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { step(); });
    }

    // Recalculations that don't move anything (a notification mapping,
    // focus changes...) must not restart the zoom from the beginning.
    if (!changed && !instant)
        return;

    m_anims = std::move(anims);

    if (instant || !g_config.overview_animate->value()) {
        m_progress->setValueAndWarp(1.F);
        step();
    } else {
        m_progress->setValueAndWarp(0.F);
        *m_progress = 1.F;
    }

    damage();
}

void COverview::step() {
    if (!m_active)
        return;

    const auto  DRAGGED = m_dragMoved ? m_dragging.lock() : nullptr;
    const float P       = std::clamp(m_progress->value(), 0.F, 1.F);
    for (const auto& SLOT : m_slots) {
        const auto DATA = SLOT.data.lock();
        // The dragged window follows the pointer; animating it as well would
        // make the two fight every frame (flicker).
        if (!DATA || DATA == DRAGGED)
            continue;
        const auto IT     = m_anims.find(DATA.get());
        const auto WINDOW = windowOf(DATA);
        if (IT == m_anims.end() || !WINDOW)
            continue;

        placeWindow(WINDOW, lerpBox(IT->second.from, IT->second.to, P));
    }
    // One full-monitor damage covers every moved window.
    damage();
}

void COverview::toggle() {
    if (m_active)
        close(nullptr);
    else
        open();
}

void COverview::select(const SP<SScrollingWindowData>& data) {
    if (!data || m_selected.lock() == data)
        return;

    // Hover/keyboard selection is purely an overlay effect: the highlight
    // slides over quickly; windows themselves don't move.
    m_highlightFrom  = m_highlightLast;
    m_highlightStart = std::chrono::steady_clock::now();
    m_selected       = data;
    damage();
}

void COverview::open() {
    if (m_active)
        return;

    const auto PARENT = m_layout->m_parent.lock();
    if (!PARENT || !PARENT->space())
        return;
    const auto WS = PARENT->space()->workspace();
    if (!WS || m_layout->m_scrollingData->columns.empty() || Fullscreen::controller()->hasFullscreen(WS, true))
        return;

    const auto FOCUSED = m_layout->focusedWindowData();
    m_initialFocus     = FOCUSED ? FOCUSED->target : WP<Layout::ITarget>{};
    m_selected         = FOCUSED ? FOCUSED : m_layout->m_scrollingData->columns.front()->windowDatas.front();
    m_highlightLast    = {};
    m_dragging.reset();
    m_pressed       = false;
    m_dragMoved     = false;
    m_ignoreRelease = false;
    m_menuOpen      = false;
    m_active        = true;
    m_session       = makeShared<bool>(true);
    m_restoring.clear();
    m_overlay       = true;
    m_fadeStart     = std::chrono::steady_clock::now();

    // While open, the overview owns the pointer: show a plain arrow instead of
    // whatever shape the window under it last asked for.
    g_pHyprRenderer->setCursorFromName("left_ptr", true);

    // Own the keyboard: navigation keys act on the overview; nothing reaches
    // clients or keybinds. Modifier keys and releases pass through so no
    // modifier gets stuck.
    m_keyListener = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
        if (!m_active || ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        // Resolve the keysym through the active keyboard's keymap (like
        // Hyprland's own binds), so remaps such as caps:escape work. The bus
        // event doesn't say which keyboard sent it; the seat's keyboard is the
        // one in use. Without a keymap, fall back to evdev keycodes.
        xkb_keysym_t sym = XKB_KEY_NoSymbol;
        if (const auto KB = g_pSeatManager->m_keyboard.lock(); KB && KB->m_xkbState)
            sym = xkb_keysym_to_lower(xkb_state_key_get_one_sym(KB->m_xkbState, ev.keycode + 8));
        if (sym == XKB_KEY_NoSymbol) {
            switch (ev.keycode) {
                case KEY_ESC: sym = XKB_KEY_Escape; break;
                case KEY_TAB: sym = XKB_KEY_Tab; break;
                case KEY_ENTER: sym = XKB_KEY_Return; break;
                case KEY_LEFT: sym = XKB_KEY_Left; break;
                case KEY_RIGHT: sym = XKB_KEY_Right; break;
                case KEY_UP: sym = XKB_KEY_Up; break;
                case KEY_DOWN: sym = XKB_KEY_Down; break;
                default: break;
            }
        }

        switch (sym) {
            case XKB_KEY_Super_L:
            case XKB_KEY_Super_R:
            case XKB_KEY_Shift_L:
            case XKB_KEY_Shift_R:
            case XKB_KEY_Control_L:
            case XKB_KEY_Control_R:
            case XKB_KEY_Alt_L:
            case XKB_KEY_Alt_R:
            case XKB_KEY_Meta_L:
            case XKB_KEY_Meta_R:
            case XKB_KEY_ISO_Level3_Shift: return; // modifiers pass through
            default: break;
        }

        info.cancelled = true;

        if (m_menuOpen) {
            switch (sym) {
                case XKB_KEY_Escape: closeMenu(); break;
                case XKB_KEY_Return:
                case XKB_KEY_KP_Enter:
                case XKB_KEY_space:
                case XKB_KEY_Right:
                case XKB_KEY_l: activateMenuItem(m_menuHover); break;
                case XKB_KEY_Up:
                case XKB_KEY_k: moveMenuHover(-1); break;
                case XKB_KEY_Down:
                case XKB_KEY_j: moveMenuHover(1); break;
                case XKB_KEY_Left:
                case XKB_KEY_h:
                case XKB_KEY_BackSpace:
                    // Back out of a submenu, or close the root menu.
                    if (!m_menuItems.empty() && m_menuItems.front().label.starts_with("‹"))
                        activateMenuItem(0);
                    else
                        closeMenu();
                    break;
                default: break;
            }
            return;
        }

        switch (sym) {
            case XKB_KEY_Escape:
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab: close(nullptr); break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space: confirm(); break;
            case XKB_KEY_Menu: {
                const auto SEL  = m_selected.lock();
                const auto SLOT = SEL ? slotFor(SEL) : nullptr;
                if (SLOT)
                    openMenu(SEL, SLOT->box.middle());
                break;
            }
            case XKB_KEY_Left:
            case XKB_KEY_h: moveSelection(Math::DIRECTION_LEFT); break;
            case XKB_KEY_Right:
            case XKB_KEY_l: moveSelection(Math::DIRECTION_RIGHT); break;
            case XKB_KEY_Up:
            case XKB_KEY_k: moveSelection(Math::DIRECTION_UP); break;
            case XKB_KEY_Down:
            case XKB_KEY_j: moveSelection(Math::DIRECTION_DOWN); break;
            default: break;
        }
    });

    // Pointer motion: hover selection, menu hover and dragging. Cancelling the
    // event keeps focus-follows-mouse and client hover out of it; the cursor
    // still moves.
    m_moveListener = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D pos, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        if (m_menuOpen) {
            const int IDX = menuItemAt(pos);
            if (IDX != m_menuHover) {
                m_menuHover = IDX;
                damage();
            }
            return;
        }

        if (m_pressed && !m_dragMoved && pos.distance(m_pressPos) > OV_DRAG_THRESHOLD && m_dragging.lock()) {
            m_dragMoved = true;
            // Lift the dragged window out of the animation and above its
            // neighbours. Tiled windows have no z-order of their own; the
            // renderer draws the focused one last, so focus it. Keyboard input
            // belongs to the overview meanwhile, so the client sees nothing
            // but the focus change, and the dropped window ends up focused.
            if (const auto DATA = m_dragging.lock()) {
                m_anims.erase(DATA.get());
                if (const auto WINDOW = windowOf(DATA))
                    Desktop::focusState()->rawWindowFocus(WINDOW, Desktop::FOCUS_REASON_OTHER);
            }
            g_pHyprRenderer->setCursorFromName("grabbing", true);
        }

        if (m_dragMoved) {
            const auto DATA   = m_dragging.lock();
            const auto SLOT   = DATA ? slotFor(DATA) : nullptr;
            const auto WINDOW = windowOf(DATA);
            if (SLOT && WINDOW) {
                CBox box{pos - m_grabOffset, SLOT->box.size()};
                placeWindow(WINDOW, box.round());
                damage();
            }
            return;
        }

        if (const auto SLOT = slotAt(pos))
            select(SLOT->data.lock());
    });

    m_buttonListener = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        const auto POS     = g_pInputManager->getMouseCoordsInternal();
        const bool PRESSED = ev.state == WL_POINTER_BUTTON_STATE_PRESSED;

        if (m_menuOpen) {
            if (!PRESSED)
                return;
            if (ev.button == BTN_LEFT && m_menuBox.containsPoint(POS)) {
                activateMenuItem(menuItemAt(POS));
                return;
            }
            // Any click outside closes the menu; a right-click on a window
            // reopens it there.
            closeMenu();
            m_ignoreRelease = true;
            if (ev.button == BTN_RIGHT) {
                if (const auto SLOT = slotAt(POS))
                    openMenu(SLOT->data.lock(), POS);
            }
            return;
        }

        if (ev.button == BTN_RIGHT) {
            if (PRESSED && !m_pressed) {
                if (const auto SLOT = slotAt(POS))
                    openMenu(SLOT->data.lock(), POS);
            }
            return;
        }

        if (ev.button != BTN_LEFT)
            return;

        if (PRESSED) {
            const auto SLOT = slotAt(POS);
            m_pressed       = true;
            m_dragMoved     = false;
            m_ignoreRelease = false;
            m_pressPos      = POS;
            m_dragging      = SLOT ? SLOT->data : WP<SScrollingWindowData>{};
            m_grabOffset    = SLOT ? POS - SLOT->box.pos() : Vector2D{};
            return;
        }

        // Release.
        const bool WAS_PRESSED = m_pressed && !m_ignoreRelease;
        m_pressed              = false;
        m_ignoreRelease        = false;

        if (m_dragMoved) {
            g_pHyprRenderer->setCursorFromName("left_ptr", true);
            drop(POS);
            return;
        }

        const auto PICK = m_dragging.lock();
        m_dragging.reset();
        if (!WAS_PRESSED)
            return;
        // Click on a window picks it; a click on empty space just closes.
        close(PICK);
    });

    // Wheel: step the selection through all windows in reading order.
    m_axisListener = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        if (ev.delta == 0 || m_menuOpen || m_slots.empty())
            return;

        const auto CURRENT = m_selected.lock();
        size_t     idx     = 0;
        for (size_t i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i].data.lock() == CURRENT)
                idx = i;
        }

        if (ev.delta > 0 && idx + 1 < m_slots.size())
            idx++;
        else if (ev.delta < 0 && idx > 0)
            idx--;

        select(m_slots[idx].data.lock());
    });

    if (!m_renderListener) {
        m_renderListener = Event::bus()->m_events.render.stage.listen([this](eRenderStage stage) {
            if (!m_overlay || (stage != RENDER_PRE_WINDOWS && stage != RENDER_POST_WINDOWS))
                return;
            const auto MON = monitor();
            if (!MON || g_pHyprRenderer->m_renderData.pMonitor.lock() != MON)
                return;

            // Switched to another workspace meanwhile (gesture, hyprctl...):
            // close instead of grabbing input for a hidden workspace. Deferred,
            // since closing re-lays out windows and we're mid-render.
            const auto PARENT = m_layout->m_parent.lock();
            const auto WS     = PARENT && PARENT->space() ? PARENT->space()->workspace() : nullptr;
            if (m_active && WS && MON->m_activeWorkspace != WS && MON->m_activeSpecialWorkspace != WS) {
                g_pEventLoopManager->doLater([this, SESSION = WP<bool>{m_session}] {
                    if (const auto S = SESSION.lock(); S && *S)
                        close(nullptr);
                });
                return;
            }

            g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>(this, stage));
        });
    }

    apply(!g_config.overview_animate->value());
}

void COverview::stopInput() {
    m_keyListener.reset();
    m_moveListener.reset();
    m_buttonListener.reset();
    m_axisListener.reset();
}

void COverview::confirm() {
    if (m_active)
        close(m_selected.lock());
}

void COverview::close(SP<SScrollingWindowData> pick) {
    if (!m_active)
        return;

    m_active = false;
    if (m_session)
        *m_session = false;
    stopInput();
    if (m_progress)
        m_progress->warp(false);
    m_anims.clear();
    m_pressed   = false;
    m_dragMoved = false;
    m_dragging.reset();
    m_menuOpen = false;
    m_menuItems.clear();
    m_fadeStart = std::chrono::steady_clock::now();
    g_pHyprRenderer->setCursorFromName("left_ptr", true);

    // Zoom back in on the picked window; otherwise on the focused one (a window
    // opened meanwhile takes focus), or whatever was focused when we opened.
    SP<Layout::ITarget> target = pick ? pick->target.lock() : nullptr;
    if (!target) {
        const auto FOCUSED = m_layout->focusedWindowData();
        target             = FOCUSED ? FOCUSED->target.lock() : m_initialFocus.lock();
    }
    if (const auto DATA = target ? m_layout->dataFor(target) : nullptr; DATA) {
        if (const auto COL = DATA->column.lock())
            m_layout->m_scrollingData->centerOrFitCol(COL);
    }

    // Keep drawing buffers scaled (not cropped) while windows grow back;
    // draw() clears it once their animation has ended (see m_restoring).
    m_restoring.clear();
    for (const auto& SLOT : m_slots) {
        if (const auto WINDOW = windowOf(SLOT.data.lock())) {
            WINDOW->m_animatingIn = true;
            m_restoring.emplace_back(WINDOW);
        }
    }

    // Leaving overview mode: this applies the real layout boxes again, animated.
    m_layout->m_scrollingData->recalculate(!g_config.overview_animate->value());

    if (pick && target)
        m_layout->focusTargetUpdate(target);

    damage();
}

void COverview::moveSelection(Math::eDirection dir) {
    const auto CURRENT = m_selected.lock();
    const auto SLOT    = CURRENT ? slotFor(CURRENT) : nullptr;
    if (!SLOT) {
        if (!m_slots.empty())
            select(m_slots.front().data.lock());
        return;
    }

    const auto               COL = SLOT->column.lock();
    SP<SScrollingWindowData> next;

    if (dir == Math::DIRECTION_UP)
        next = COL ? COL->prev(CURRENT) : nullptr;
    else if (dir == Math::DIRECTION_DOWN)
        next = COL ? COL->next(CURRENT) : nullptr;
    else {
        // Neighbouring column on screen, then the window there closest to the
        // current one vertically.
        size_t colIdx = 0;
        for (size_t i = 0; i < m_columnSlots.size(); ++i) {
            if (m_columnSlots[i].column.lock() == COL)
                colIdx = i;
        }
        const bool HAS_NEXT = dir == Math::DIRECTION_LEFT ? colIdx > 0 : colIdx + 1 < m_columnSlots.size();
        if (HAS_NEXT) {
            const auto   TARGET_COL = m_columnSlots[dir == Math::DIRECTION_LEFT ? colIdx - 1 : colIdx + 1].column.lock();
            const double MID_Y      = SLOT->box.middle().y;
            double       best       = INFINITY;
            for (const auto& S : m_slots) {
                if (S.column.lock() != TARGET_COL)
                    continue;
                const double D = std::abs(S.box.middle().y - MID_Y);
                if (D < best) {
                    best = D;
                    next = S.data.lock();
                }
            }
        }
    }

    select(next);
}

void COverview::drop(const Vector2D& pos) {
    const auto DATA = m_dragging.lock();
    m_dragging.reset();
    m_dragMoved = false;

    const auto TARGET = DATA ? DATA->target.lock() : nullptr;
    const auto SRC    = DATA ? DATA->column.lock() : nullptr;
    if (!TARGET || !SRC) {
        apply();
        return;
    }

    auto& LAYOUT = m_layout->m_scrollingData;

    // Where did it land? Over a column: into it at the slot under the pointer,
    // or, near the column's left/right edge, into a new column beside it.
    // Outside all columns: a new column at that end.
    SP<SColumnData> into, besideAfter, besideBefore;
    for (const auto& CS : m_columnSlots) {
        const auto COL = CS.column.lock();
        if (!COL || pos.x < CS.box.x || pos.x >= CS.box.x + CS.box.w)
            continue;

        const double REL = (pos.x - CS.box.x) / CS.box.w;
        if (REL < OV_NEW_COLUMN_EDGE)
            besideBefore = COL;
        else if (REL > 1.0 - OV_NEW_COLUMN_EDGE)
            besideAfter = COL;
        else
            into = COL;
        break;
    }
    if (!into && !besideBefore && !besideAfter && !m_columnSlots.empty()) {
        if (pos.x < m_columnSlots.front().box.x)
            besideBefore = m_columnSlots.front().column.lock();
        else
            besideAfter = m_columnSlots.back().column.lock();
    }

    const bool ALONE = SRC->windowDatas.size() == 1;

    // A lone window dropped next to (or back into) its own column stays put.
    if (ALONE && (into == SRC || besideBefore == SRC || besideAfter == SRC)) {
        apply();
        return;
    }

    const float SRC_WIDTH = SRC->columnWidth;
    SRC->remove(TARGET);
    if (SRC->windowDatas.empty())
        LAYOUT->remove(SRC);

    if (into) {
        // Insert below the last remaining window whose thumbnail midpoint is
        // above the pointer (m_slots still holds the pre-drop thumbnails).
        int after = -1;
        for (const auto& S : m_slots) {
            const auto D = S.data.lock();
            if (S.column.lock() == into && D && D != DATA && pos.y > S.box.middle().y)
                after = std::max(after, (int)into->idx(D->target.lock()));
        }
        into->add(DATA, after);
    } else {
        const auto NEIGHBOUR = besideBefore ? besideBefore : besideAfter;
        const auto IDX       = LAYOUT->idx(NEIGHBOUR);
        const auto COL       = IDX < 0 ? LAYOUT->add() : LAYOUT->add(besideBefore ? (int)IDX - 1 : (int)IDX);
        COL->add(DATA);
        COL->columnWidth = ALONE ? SRC_WIDTH : m_layout->autoWidthForTarget(TARGET);
    }

    m_selected = DATA;
    apply();
}

void COverview::onWindowRemoved() {
    if (!m_active)
        return;

    // When a window unmaps, Hyprland calls forceReportSizesToWindows(): every
    // window on the workspace is sent its size goal, which during the
    // overview is the thumbnail size, so clients would really shrink. Right
    // after that (doLater), send each overview window its real size again, the
    // one it last acknowledged, and put the thumbnail box back. Both
    // configures arrive together, so clients settle on the real size.
    std::vector<PHLWINDOWREF> windows;
    for (const auto& SLOT : m_slots) {
        if (const auto WINDOW = windowOf(SLOT.data.lock()))
            windows.emplace_back(WINDOW);
    }

    g_pEventLoopManager->doLater([windows, SESSION = WP<bool>{m_session}] {
        // Closed in the meantime: windows are zooming back to their real
        // boxes; re-placing thumbnails would freeze them.
        if (const auto S = SESSION.lock(); !S || !*S)
            return;

        for (const auto& REF : windows) {
            const auto WINDOW = REF.lock();
            if (!WINDOW || !WINDOW->m_isMapped)
                continue;

            const CBox     THUMB{WINDOW->positionAnimation()->value(), WINDOW->sizeAnimation()->value()};
            const Vector2D REAL = WINDOW->getReportedSize();
            if (REAL.x < 1 || REAL.y < 1 || REAL == THUMB.size())
                continue;

            WINDOW->sizeAnimation()->setValueAndWarp(REAL);
            WINDOW->sendWindowSize(true);
            placeWindow(WINDOW, THUMB);
            g_pHyprRenderer->damageWindow(WINDOW);
        }
    });
}

// ---------------------------------------------------------------- menu ----

void COverview::setMenuItems(std::vector<SMenuItem>&& items) {
    m_menuItems = std::move(items);
    m_menuItemBoxes.clear();

    // Lay the menu out at its anchor (m_menuBox.pos()), kept on the monitor.
    double h = 2 * MENU_PAD;
    for (const auto& IT : m_menuItems)
        h += IT.separator ? MENU_SEP_H : IT.header ? MENU_HEADER_H : MENU_ITEM_H;

    m_menuBox.w = MENU_WIDTH;
    m_menuBox.h = h;
    if (const auto MON = monitor()) {
        const auto MB = MON->logicalBox();
        m_menuBox.x   = std::clamp(m_menuBox.x, MB.x + 8, std::max(MB.x + 8, MB.x + MB.w - m_menuBox.w - 8));
        m_menuBox.y   = std::clamp(m_menuBox.y, MB.y + 8, std::max(MB.y + 8, MB.y + MB.h - m_menuBox.h - 8));
    }

    double y = m_menuBox.y + MENU_PAD;
    for (const auto& IT : m_menuItems) {
        const double H = IT.separator ? MENU_SEP_H : IT.header ? MENU_HEADER_H : MENU_ITEM_H;
        m_menuItemBoxes.push_back(CBox{m_menuBox.x + MENU_PAD, y, MENU_WIDTH - 2 * MENU_PAD, H});
        y += H;
    }

    m_menuHover = -1;
    damage();
}

void COverview::openMenu(const SP<SScrollingWindowData>& data, const Vector2D& pos) {
    if (!data)
        return;
    select(data);
    m_menuOpen   = true;
    m_menuTarget = data;
    m_menuBox.x  = pos.x;
    m_menuBox.y  = pos.y;
    setMenuItems(rootMenu(data));
    g_pHyprRenderer->setCursorFromName("left_ptr", true);
}

void COverview::closeMenu() {
    m_menuOpen = false;
    m_menuItems.clear();
    m_menuItemBoxes.clear();
    m_menuHover = -1;
    damage();
}

int COverview::menuItemAt(const Vector2D& pos) const {
    for (size_t i = 0; i < m_menuItemBoxes.size(); ++i) {
        const auto& IT = m_menuItems[i];
        if (!IT.separator && !IT.header && m_menuItemBoxes[i].containsPoint(pos))
            return (int)i;
    }
    return -1;
}

void COverview::moveMenuHover(int delta) {
    if (m_menuItems.empty())
        return;
    const int N = (int)m_menuItems.size();
    int       i = m_menuHover < 0 ? (delta > 0 ? -1 : N) : m_menuHover;
    for (int step = 0; step < N; ++step) {
        i = (i + delta + N) % N;
        if (!m_menuItems[i].separator && !m_menuItems[i].header) {
            m_menuHover = i;
            break;
        }
    }
    damage();
}

void COverview::activateMenuItem(int idx) {
    if (idx < 0 || idx >= (int)m_menuItems.size() || m_menuItems[idx].separator || m_menuItems[idx].header || !m_menuItems[idx].action)
        return;

    const bool SUBMENU = m_menuItems[idx].submenu || m_menuItems[idx].label.starts_with("‹");
    // Copy: the action may replace m_menuItems (submenus) or close the overview.
    const auto ACTION = m_menuItems[idx].action;
    if (!SUBMENU)
        closeMenu();
    ACTION();
}

std::vector<COverview::SMenuItem> COverview::rootMenu(const SP<SScrollingWindowData>& data) {
    std::vector<SMenuItem> items;
    const auto             WINDOW = windowOf(data);
    const auto             COL    = data ? data->column.lock() : nullptr;
    if (!WINDOW || !COL)
        return items;

    auto&      LAYOUT  = m_layout->m_scrollingData;
    const WP<SScrollingWindowData> WDATA = data;
    const WP<SColumnData>          WCOL  = COL;
    const bool STACKED = COL->windowDatas.size() > 1;

    const auto pct = [](float f) { return std::format("{}%", (int)std::round(f * 100)); };

    items.push_back({.label = WINDOW->m_title.empty() ? WINDOW->m_class : WINDOW->m_title, .header = true});

    // Window
    items.push_back({.label = "Focus", .action = [this, WDATA] { close(WDATA.lock()); }});
    items.push_back({.label = "Fullscreen", .action = [this, WDATA] {
                         const auto D = WDATA.lock();
                         const auto W = windowOf(D);
                         close(D);
                         if (W)
                             (void)Config::Actions::fullscreenWindow(Fullscreen::FSMODE_FULLSCREEN, false, W);
                     }});
    items.push_back({.label = "Float", .action = [this, WDATA] {
                         const auto D = WDATA.lock();
                         const auto W = windowOf(D);
                         close(D);
                         if (W)
                             (void)Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE, W);
                     }});
    items.push_back({.label = "Move to workspace", .action = [this, WDATA] {
                         std::vector<SMenuItem> sub;
                         sub.push_back({.label = "‹  Back", .action = [this, WDATA] { setMenuItems(rootMenu(WDATA.lock())); }});
                         const auto MON = monitor();
                         const auto CUR = MON ? MON->m_activeWorkspace : nullptr;
                         for (int id = 1; id <= 10; ++id) {
                             if (CUR && CUR->m_id == id)
                                 continue;
                             sub.push_back({.label = std::format("Workspace {}", id), .action = [this, WDATA, id] {
                                                const auto W = windowOf(WDATA.lock());
                                                const auto M = monitor();
                                                if (!W || !M)
                                                    return;
                                                const auto ws = hyprscrolling_compat::getOrCreateWorkspace(id, std::to_string(id), M);
                                                if (ws)
                                                    (void)Config::Actions::moveToWorkspace(ws, true, W);
                                            }});
                         }
                         setMenuItems(std::move(sub));
                     },
                     .submenu = true});
    items.push_back({.label = "Close window", .action = [WDATA] {
                         if (const auto W = windowOf(WDATA.lock()))
                             (void)Config::Actions::closeWindow(W);
                     }});

    items.push_back({.separator = true});

    // Column
    items.push_back({.label = "Column width", .action = [this, WDATA, WCOL, pct] {
                         std::vector<SMenuItem> sub;
                         sub.push_back({.label = "‹  Back", .action = [this, WDATA] { setMenuItems(rootMenu(WDATA.lock())); }});
                         const auto C = WCOL.lock();
                         for (const float W : m_layout->m_config.configuredWidths) {
                             sub.push_back({.label = pct(W), .action = [this, WCOL, W] {
                                                if (const auto C2 = WCOL.lock()) {
                                                    C2->columnWidth = W;
                                                    apply();
                                                }
                                            },
                                            .checked = C && std::abs(C->columnWidth - W) < 0.01F});
                         }
                         setMenuItems(std::move(sub));
                     },
                     .submenu = true});

    if (STACKED || g_config.column_scroll->value()) {
        items.push_back({.label = "Window height", .action = [this, WDATA, WCOL, pct] {
                             std::vector<SMenuItem> sub;
                             sub.push_back({.label = "‹  Back", .action = [this, WDATA] { setMenuItems(rootMenu(WDATA.lock())); }});
                             const auto D = WDATA.lock();
                             for (const float H : m_layout->m_config.configuredHeights) {
                                 sub.push_back({.label = pct(H), .action = [this, WDATA, WCOL, H] {
                                                    const auto D2 = WDATA.lock();
                                                    const auto C2 = WCOL.lock();
                                                    if (!D2 || !C2)
                                                        return;
                                                    // Same rules as rowresize: independent heights with
                                                    // column_scroll, otherwise the others share the rest.
                                                    if (!g_config.column_scroll->value() && C2->windowDatas.size() > 1) {
                                                        const float OTHERS = 1.F - D2->windowSize;
                                                        for (auto& wd : C2->windowDatas) {
                                                            if (wd != D2)
                                                                wd->windowSize = OTHERS > 0 ? wd->windowSize * (1.F - H) / OTHERS : (1.F - H) / (C2->windowDatas.size() - 1);
                                                        }
                                                    }
                                                    D2->windowSize = H;
                                                    apply();
                                                },
                                                .checked = D && std::abs(D->windowSize - H) < 0.01F});
                             }
                             setMenuItems(std::move(sub));
                         },
                         .submenu = true});
    }

    if (STACKED) {
        items.push_back({.label = "Own column", .action = [this, WDATA] {
                             const auto D = WDATA.lock();
                             const auto C = D ? D->column.lock() : nullptr;
                             const auto T = D ? D->target.lock() : nullptr;
                             if (!C || !T || C->windowDatas.size() < 2)
                                 return;
                             auto&      L   = m_layout->m_scrollingData;
                             const auto NEW = L->add((int)L->idx(C));
                             C->remove(T);
                             NEW->add(D);
                             NEW->columnWidth = m_layout->autoWidthForTarget(T);
                             apply();
                         }});
    }

    const auto IDX = LAYOUT->idx(COL);
    if (IDX > 0) {
        items.push_back({.label = "Move column left", .action = [this, WCOL] {
                             auto&      L = m_layout->m_scrollingData;
                             const auto I = L->idx(WCOL.lock());
                             if (I > 0) {
                                 std::swap(L->columns[I], L->columns[I - 1]);
                                 apply();
                             }
                         }});
    }
    if (IDX >= 0 && IDX + 1 < (int64_t)LAYOUT->columns.size()) {
        items.push_back({.label = "Move column right", .action = [this, WCOL] {
                             auto&      L = m_layout->m_scrollingData;
                             const auto I = L->idx(WCOL.lock());
                             if (I >= 0 && I + 1 < (int64_t)L->columns.size()) {
                                 std::swap(L->columns[I], L->columns[I + 1]);
                                 apply();
                             }
                         }});
    }

    const auto setPin = [this, WCOL](ePinState pin) {
        return [this, WCOL, pin] {
            if (const auto C = WCOL.lock()) {
                C->pinned = pin;
                apply();
            }
        };
    };
    if (COL->pinned != PIN_LEFT)
        items.push_back({.label = "Pin column left", .action = setPin(PIN_LEFT)});
    if (COL->pinned != PIN_RIGHT)
        items.push_back({.label = "Pin column right", .action = setPin(PIN_RIGHT)});
    if (COL->pinned != PIN_NONE)
        items.push_back({.label = "Unpin column", .action = setPin(PIN_NONE)});

    items.push_back({.label = COL->collapsed ? "Expand column" : "Collapse column", .action = [this, WCOL] {
                         if (const auto C = WCOL.lock()) {
                             C->setCollapsed(!C->collapsed);
                             apply();
                         }
                     }});

    return items;
}

// ------------------------------------------------------------- drawing ----

SP<Render::ITexture> COverview::text(const std::string& str, int maxWidthPx) {
    if (str.empty() || maxWidthPx < 8)
        return nullptr;

    const auto KEY = str + "\n" + std::to_string(maxWidthPx);
    if (const auto IT = m_textCache.find(KEY); IT != m_textCache.end())
        return IT->second;

    const auto MON    = monitor();
    const int  SIZE   = (int)std::round(11 * (MON ? MON->m_scale : 1.F));
    auto       tex    = g_pHyprRenderer->renderText(str, CHyprColor{1, 1, 1, 1}, SIZE, false, "", maxWidthPx, 500);
    m_textCache[KEY] = tex;
    return tex;
}

void COverview::drawMenu(const PHLMONITOR& mon, float alpha) {
    const float SCALE = mon->m_scale;
    const auto  local = [&](const CBox& b) { return b.copy().translate(-mon->m_position).scale(SCALE).round(); };

    const CBox BG = local(m_menuBox);
    g_pHyprOpenGL->renderRect(BG.copy().expand(std::round(1 * SCALE)), CHyprColor{1, 1, 1, 0.10F * alpha}, {.round = (int)std::round(11 * SCALE)});
    g_pHyprOpenGL->renderRect(BG, CHyprColor{0.10F, 0.10F, 0.12F, 0.97F * alpha}, {.round = (int)std::round(10 * SCALE)});

    for (size_t i = 0; i < m_menuItems.size(); ++i) {
        const auto& IT  = m_menuItems[i];
        const CBox  ROW = local(m_menuItemBoxes[i]);

        if (IT.separator) {
            g_pHyprOpenGL->renderRect(CBox{ROW.x + 6 * SCALE, ROW.y + ROW.h / 2.0, ROW.w - 12 * SCALE, std::max(1.0, (double)std::round(SCALE))}.round(),
                                      CHyprColor{1, 1, 1, 0.10F * alpha}, {});
            continue;
        }

        if ((int)i == m_menuHover)
            g_pHyprOpenGL->renderRect(ROW, accentColor(0.35F * alpha), {.round = (int)std::round(6 * SCALE)});

        const double TEXT_X = ROW.x + (IT.header ? 8 : 12) * SCALE;
        const auto   TEX    = text(IT.label, (int)(ROW.w - 44 * SCALE));
        if (TEX) {
            const CBox TB{TEXT_X, ROW.y + (ROW.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
            g_pHyprOpenGL->renderTexture(TEX, TB.copy().round(), {.a = (IT.header ? 0.55F : 1.F) * alpha});
        }

        // Trailing marker: chevron for submenus, dot for the current preset.
        const std::string MARK = IT.submenu ? "›" : IT.checked ? "●" : "";
        if (const auto MT = MARK.empty() ? nullptr : text(MARK, (int)(24 * SCALE))) {
            const CBox MB{ROW.x + ROW.w - 12 * SCALE - MT->m_size.x, ROW.y + (ROW.h - MT->m_size.y) / 2.0, MT->m_size.x, MT->m_size.y};
            g_pHyprOpenGL->renderTexture(MT, MB.copy().round(), {.a = 0.7F * alpha});
        }
    }
}

void COverview::draw(eRenderStage stage) {
    const auto MON = monitor();
    if (!MON)
        return;

    const float ALPHA   = overlayAlpha();
    bool        animate = m_active ? ALPHA < 1.F : ALPHA > 0.F;

    if (stage == RENDER_PRE_WINDOWS) {
        // Windows are about to be drawn: make sure every overview window is
        // drawn scaled, not cropped. Any move/resize animation ending on a
        // window (closing or moving a neighbour, focus changes...) clears this
        // flag, so setting it once per placement isn't enough.
        if (m_active) {
            for (const auto& SLOT : m_slots) {
                if (const auto WINDOW = windowOf(SLOT.data.lock()))
                    WINDOW->m_animatingIn = true;
            }
        }

        // Dim the wallpaper behind the zoomed-out windows.
        if (ALPHA > 0.F)
            g_pHyprOpenGL->renderRect(CBox{{}, MON->m_pixelSize}, CHyprColor{0, 0, 0, OV_DIM * ALPHA}, {});
        if (animate)
            damage();
        return;
    }

    if (stage != RENDER_POST_WINDOWS)
        return;

    // Windows that finished zooming back after close: stop drawing them
    // scaled, so later shrink animations crop cleanly again.
    std::erase_if(m_restoring, [](const PHLWINDOWREF& ref) {
        const auto WINDOW = ref.lock();
        if (!WINDOW)
            return true;
        if (WINDOW->positionAnimation()->isBeingAnimated() || WINDOW->sizeAnimation()->isBeingAnimated())
            return false;
        WINDOW->m_animatingIn = false;
        return true;
    });

    if (ALPHA <= 0.F) {
        // Fully faded out: keep the hook only until the restore above is done.
        if (!m_active && m_restoring.empty()) {
            m_overlay = false;
            m_textCache.clear();
            m_renderListener.reset();
        }
        return;
    }

    const float SCALE    = MON->m_scale;
    const auto  SELECTED = m_selected.lock();
    const auto  DRAGGED  = m_dragMoved ? m_dragging.lock() : nullptr;
    const auto  DRAG_WIN = windowOf(DRAGGED);
    const CBox  DRAG_BOX = DRAG_WIN ? CBox{DRAG_WIN->positionAnimation()->value(), DRAG_WIN->sizeAnimation()->value()} : CBox{};

    for (const auto& SLOT : m_slots) {
        const auto DATA   = SLOT.data.lock();
        const auto WINDOW = windowOf(DATA);
        if (!WINDOW)
            continue;

        // Follow the window's drawn box so labels move with the zoom.
        CBox global{WINDOW->positionAnimation()->value(), WINDOW->sizeAnimation()->value()};

        if (DATA == SELECTED && m_active) {
            // Slide the highlight from where it was to this window.
            const CBox TARGET = global.copy().expand(4);
            const int  MS     = msSince(m_highlightStart);
            const CBox HL     = m_highlightFrom.empty() || MS >= OV_HIGHLIGHT_MS ? TARGET : lerpBox(m_highlightFrom, TARGET, easeOutCubic((double)MS / OV_HIGHLIGHT_MS));
            if (MS < OV_HIGHLIGHT_MS)
                animate = true;
            m_highlightLast = HL;

            const int  BORDER = (int)std::round(3 * SCALE);
            const CBox LOCAL  = HL.copy().translate(-MON->m_position).scale(SCALE).round();
            g_pHyprOpenGL->renderBorder(LOCAL.copy().expand(BORDER), Config::CGradientValueData{accentColor(ALPHA)},
                                        {.round = (int)std::round(WINDOW->rounding() * SCALE) + BORDER, .borderSize = BORDER, .a = ALPHA});
        }

        if (!g_config.overview_titles->value())
            continue;

        // The dragged window is drawn above the others; their labels would
        // otherwise show through it.
        if (DRAG_WIN && DATA != DRAGGED && !global.intersection(DRAG_BOX).empty())
            continue;

        // Size the label for the window's slot, not its animated box: the box
        // changes every frame of the zoom and would re-render the text each time.
        const CBox BOX = global.copy().translate(-MON->m_position).scale(SCALE).round();
        const auto TEX = BOX.w > 1 ? text(WINDOW->m_title, (int)std::round((SLOT.box.w - 32) * SCALE)) : nullptr;
        if (!TEX)
            continue;

        const double PAD_X = std::round(10 * SCALE), PAD_Y = std::round(5 * SCALE);
        const CBox   TEXT{BOX.x + (BOX.w - TEX->m_size.x) / 2.0, BOX.y + BOX.h - TEX->m_size.y - PAD_Y - std::round(12 * SCALE), TEX->m_size.x, TEX->m_size.y};
        CBox         pill = TEXT.copy().expand(PAD_Y);
        pill.x -= PAD_X - PAD_Y;
        pill.w += 2 * (PAD_X - PAD_Y);
        pill.round();

        g_pHyprOpenGL->renderRect(pill, CHyprColor{0, 0, 0, 0.65F * ALPHA}, {.round = (int)(pill.h / 2)});
        g_pHyprOpenGL->renderTexture(TEX, TEXT.copy().round(), {.a = ALPHA});
    }

    if (m_menuOpen && m_active)
        drawMenu(MON, ALPHA);

    // Keep repainting while something animates (the fade-out ends with a frame
    // at alpha 0, which drops the overlay above).
    if (animate)
        damage();
}
