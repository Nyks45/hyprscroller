#include "Overview.hpp"
#include "globals.hpp"

#include <algorithm>
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
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <xkbcommon/xkbcommon.h>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>

using namespace Render::GL;

// Layout of the zoomed-out strip, in logical px.
constexpr double OV_MARGIN         = 48.0; // free space around the strip
constexpr double OV_GAP            = 10.0; // space between thumbnails
constexpr double OV_LIFT           = 6.0;  // hovered/selected thumbnail grows by this on each side
constexpr double OV_DRAG_THRESHOLD = 8.0;  // pointer travel before a press becomes a drag
constexpr double OV_NEW_COLUMN_EDGE = 0.2; // drop in the outer 20% of a column -> new column beside it
constexpr int    OV_FADE_MS        = 220;  // backdrop/label fade
constexpr float  OV_DIM            = 0.45F;

// Draws the overview's backdrop (before windows) or highlight and labels
// (after windows) for the monitor being rendered.
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

COverview::COverview(CScrollingLayout* layout) : m_layout(layout) {
    ;
}

COverview::~COverview() {
    stopInput();
    m_renderListener.reset();
}

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
    const auto  MS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_fadeStart).count();
    const float T  = std::clamp((float)MS / OV_FADE_MS, 0.F, 1.F);
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
            m_slots.push_back({.data = CL.column->windowDatas[i], .column = CL.column, .box = box});
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
    g_pHyprRenderer->damageWindow(window);
    window->positionAnimation()->setValueAndWarp(box.pos());
    window->sizeAnimation()->setValueAndWarp(box.size());
    // The renderer crops a buffer larger than its window box (to avoid
    // squashing during resizes) unless the window is animating in; set that
    // so the whole buffer is drawn scaled into the box. The warp's end
    // callback clears it, so it's set after.
    window->m_animatingIn = true;
    g_pHyprRenderer->damageWindow(window);
}

void COverview::apply(bool instant) {
    computeSlots();

    const auto LIFTED  = m_hovered.lock() ? m_hovered.lock() : m_selected.lock();
    const auto DRAGGED = m_dragMoved ? m_dragging.lock() : nullptr;

    m_anims.clear();
    for (const auto& SLOT : m_slots) {
        const auto DATA   = SLOT.data.lock();
        const auto TARGET = DATA ? DATA->target.lock() : nullptr;
        const auto WINDOW = TARGET ? TARGET->window() : nullptr;
        if (!WINDOW || DATA == DRAGGED)
            continue;

        CBox box = SLOT.box;
        if (DATA == LIFTED)
            box.expand(OV_LIFT);
        box.round();

        // Start from wherever the window is drawn right now, so retargeting
        // mid-animation (hover, selection, opening/closing windows) is smooth.
        m_anims[DATA.get()] = {.from = CBox{WINDOW->positionAnimation()->value(), WINDOW->sizeAnimation()->value()}, .to = box};
    }

    if (!m_progress) {
        Animation::mgr()->createAnimation(0.F, m_progress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { step(); });
    }

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

    const float P = std::clamp(m_progress->value(), 0.F, 1.F);
    for (const auto& SLOT : m_slots) {
        const auto DATA = SLOT.data.lock();
        const auto IT   = DATA ? m_anims.find(DATA.get()) : m_anims.end();
        if (IT == m_anims.end())
            continue;
        const auto TARGET = DATA->target.lock();
        const auto WINDOW = TARGET ? TARGET->window() : nullptr;
        if (!WINDOW)
            continue;

        const auto& A = IT->second;
        placeWindow(WINDOW, CBox{A.from.pos() + (A.to.pos() - A.from.pos()) * P, A.from.size() + (A.to.size() - A.from.size()) * P});
    }
    damage();
}

void COverview::toggle() {
    if (m_active)
        close(nullptr);
    else
        open();
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
    m_hovered.reset();
    m_dragging.reset();
    m_pressed   = false;
    m_dragMoved = false;
    m_active    = true;
    m_overlay   = true;
    m_fadeStart = std::chrono::steady_clock::now();

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

        switch (sym) {
            case XKB_KEY_Escape:
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab: close(nullptr); break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space: confirm(); break;
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

    // Pointer motion: hover highlight and dragging. Cancelling the event keeps
    // focus-follows-mouse and client hover out of it; the cursor still moves.
    m_moveListener = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D pos, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        if (m_pressed && !m_dragMoved && pos.distance(m_pressPos) > OV_DRAG_THRESHOLD && m_dragging.lock())
            m_dragMoved = true;

        if (m_dragMoved) {
            const auto DATA   = m_dragging.lock();
            const auto SLOT   = DATA ? slotFor(DATA) : nullptr;
            const auto TARGET = DATA ? DATA->target.lock() : nullptr;
            if (SLOT && TARGET && TARGET->window()) {
                CBox box{pos - m_grabOffset, SLOT->box.size()};
                box.round();
                placeWindow(TARGET->window(), box);
                damage();
            }
            return;
        }

        const auto SLOT = slotAt(pos);
        setHovered(SLOT ? SLOT->data.lock() : nullptr);
    });

    m_buttonListener = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        if (ev.button != BTN_LEFT)
            return;

        const auto POS = g_pInputManager->getMouseCoordsInternal();

        if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto SLOT = slotAt(POS);
            m_pressed       = true;
            m_dragMoved     = false;
            m_pressPos      = POS;
            m_dragging      = SLOT ? SLOT->data : WP<SScrollingWindowData>{};
            m_grabOffset    = SLOT ? POS - SLOT->box.pos() : Vector2D{};
            return;
        }

        // Release.
        const bool WAS_PRESSED = m_pressed;
        m_pressed              = false;

        if (m_dragMoved) {
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

    // Wheel: step through all windows in reading order (column by column).
    m_axisListener = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;

        if (ev.delta == 0)
            return;

        computeSlots();
        if (m_slots.empty())
            return;

        const auto CURRENT = m_selected.lock();
        size_t     idx     = 0;
        for (size_t i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i].data.lock() == CURRENT)
                idx = i;
        }

        const bool FORWARD = ev.delta > 0;
        if (FORWARD && idx + 1 < m_slots.size())
            idx++;
        else if (!FORWARD && idx > 0)
            idx--;

        m_selected = m_slots[idx].data;
        m_hovered.reset();
        apply();
    });

    if (!m_renderListener) {
        m_renderListener = Event::bus()->m_events.render.stage.listen([this](eRenderStage stage) {
            if (!m_overlay || (stage != RENDER_PRE_WINDOWS && stage != RENDER_POST_WINDOWS))
                return;
            const auto MON = monitor();
            if (!MON || g_pHyprRenderer->m_renderData.pMonitor.lock() != MON)
                return;
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
    stopInput();
    if (m_progress)
        m_progress->warp(false);
    m_anims.clear();
    m_pressed   = false;
    m_dragMoved = false;
    m_dragging.reset();
    m_hovered.reset();
    m_fadeStart = std::chrono::steady_clock::now();
    m_titleCache.clear();

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
    // Hyprland clears this when their move/resize animation ends.
    for (const auto& SLOT : m_slots) {
        const auto DATA = SLOT.data.lock();
        const auto TGT  = DATA ? DATA->target.lock() : nullptr;
        if (TGT && TGT->window())
            TGT->window()->m_animatingIn = true;
    }

    // Leaving overview mode: this applies the real layout boxes again, animated.
    m_layout->m_scrollingData->recalculate(!g_config.overview_animate->value());

    if (pick && target)
        m_layout->focusTargetUpdate(target);

    damage();
}

void COverview::setHovered(const SP<SScrollingWindowData>& data) {
    if (m_hovered.lock() == data)
        return;

    m_hovered = data;
    if (data)
        m_selected = data;
    apply();
}

void COverview::moveSelection(Math::eDirection dir) {
    computeSlots();
    const auto CURRENT = m_selected.lock();
    const auto SLOT    = CURRENT ? slotFor(CURRENT) : nullptr;
    if (!SLOT) {
        if (!m_slots.empty())
            m_selected = m_slots.front().data;
        apply();
        return;
    }

    const auto COL = SLOT->column.lock();
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

    if (next) {
        m_selected = next;
        m_hovered.reset();
        apply();
    }
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
    m_hovered  = DATA;
    apply();
}

SP<Render::ITexture> COverview::titleTexture(const std::string& title, int maxWidthPx) {
    if (title.empty() || maxWidthPx < 8)
        return nullptr;

    const auto KEY = title + "\n" + std::to_string(maxWidthPx);
    if (const auto IT = m_titleCache.find(KEY); IT != m_titleCache.end())
        return IT->second;

    const auto MON   = monitor();
    const int  SIZE  = (int)std::round(11 * (MON ? MON->m_scale : 1.F));
    auto       tex   = g_pHyprRenderer->renderText(title, CHyprColor{1, 1, 1, 1}, SIZE, false, "", maxWidthPx, 500);
    m_titleCache[KEY] = tex;
    return tex;
}

void COverview::draw(eRenderStage stage) {
    const auto MON = monitor();
    if (!MON)
        return;

    const float ALPHA = overlayAlpha();

    if (stage == RENDER_PRE_WINDOWS) {
        // Dim the wallpaper behind the zoomed-out windows.
        g_pHyprOpenGL->renderRect(CBox{{}, MON->m_pixelSize}, CHyprColor{0, 0, 0, OV_DIM * ALPHA}, {});
    } else if (stage == RENDER_POST_WINDOWS) {
        const auto  HIGHLIGHT = m_hovered.lock() ? m_hovered.lock() : m_selected.lock();
        const float SCALE     = MON->m_scale;

        for (const auto& SLOT : m_slots) {
            const auto DATA   = SLOT.data.lock();
            const auto TARGET = DATA ? DATA->target.lock() : nullptr;
            const auto WINDOW = TARGET ? TARGET->window() : nullptr;
            if (!WINDOW)
                continue;

            // Follow the window's animated box so labels move with the zoom.
            CBox box{WINDOW->positionAnimation()->value(), WINDOW->sizeAnimation()->value()};
            box.translate(-MON->m_position).scale(SCALE).round();
            if (box.w < 1 || box.h < 1)
                continue;

            const int ROUND = (int)std::round(WINDOW->rounding() * SCALE);

            if (DATA == HIGHLIGHT && m_active) {
                const int BORDER = (int)std::round(3 * SCALE);
                g_pHyprOpenGL->renderBorder(box.copy().expand(BORDER + std::round(2 * SCALE)), Config::CGradientValueData{accentColor(ALPHA)},
                                            {.round = ROUND + BORDER, .borderSize = BORDER, .a = ALPHA});
            }

            if (!g_config.overview_titles->value())
                continue;

            const auto TEX = titleTexture(WINDOW->m_title, (int)(box.w - 32 * SCALE));
            if (!TEX)
                continue;

            const double PAD_X = std::round(10 * SCALE), PAD_Y = std::round(5 * SCALE);
            const CBox   TEXT{box.x + (box.w - TEX->m_size.x) / 2.0, box.y + box.h - TEX->m_size.y - PAD_Y - std::round(12 * SCALE), TEX->m_size.x, TEX->m_size.y};
            CBox         pill = TEXT.copy().expand(PAD_Y);
            pill.x -= PAD_X - PAD_Y;
            pill.w += 2 * (PAD_X - PAD_Y);
            pill.round();

            g_pHyprOpenGL->renderRect(pill, CHyprColor{0, 0, 0, 0.65F * ALPHA}, {.round = (int)(pill.h / 2)});
            g_pHyprOpenGL->renderTexture(TEX, TEXT.copy().round(), {.a = ALPHA});
        }
    }

    // Keep repainting while fading; drop the overlay once fully faded out.
    if (m_active ? ALPHA < 1.F : ALPHA > 0.F)
        damage();
    else if (!m_active && stage == RENDER_POST_WINDOWS) {
        m_overlay = false;
        m_renderListener.reset();
    }
}
