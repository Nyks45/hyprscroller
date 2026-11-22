#include "Overview.hpp"
#include "Compat.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/history/WindowHistoryTracker.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowBackend.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/render/WindowRenderPresentation.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/keybinds/Bind.hpp>
#include <hyprland/src/keybinds/InputState.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/input/Keys.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BackdropScopePassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
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
constexpr int    OV_SWITCH_DELAY_MS = 150;  // Alt+Tab held this long shows the switcher
constexpr double OV_CLOSE_SIZE      = 26.0; // close button on the selected thumbnail

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

    virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        m_overview->draw(ctx, m_stage);
        return {};
    }
    virtual bool needsLiveBlur(Render::CRenderContext&) override {
        return false;
    }
    virtual bool needsPrecomputeBlur(Render::CRenderContext&) override {
        return false;
    }
    virtual bool undiscardable(Render::CRenderContext&) override {
        return true;
    }
    virtual bool disableSimplification(Render::CRenderContext&) override {
        return true;
    }
    virtual bool requiresFullDamage(Render::CRenderContext&) override {
        return true;
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

static CBox realWindowBox(const PHLWINDOW& w) {
    return CBox{w->positionAnimation()->value(), w->sizeAnimation()->value()};
}

static CBox fitPreviewBox(const CBox& target, const Vector2D& realSize) {
    if (realSize.x < 1.0 || realSize.y < 1.0 || target.w < 1.0 || target.h < 1.0)
        return target;
    const double   S  = std::min(target.w / realSize.x, target.h / realSize.y);
    const Vector2D SZ = realSize * S;
    return CBox{target.middle() - SZ / 2.0, SZ};
}

// Mark release binds bound to any currently held modifier (e.g. SUPER + SUPER_L)
// as suppressed on that pressed-input record so releasing Super after Super+Tab
// never launches the app launcher.
static void suppressHeldReleaseBinds() {
    auto& km = Keybinds::mgr();
    if (!km)
        return;
    for (auto& input : km->inputState().pressed()) {
        if (!input.key.modifier)
            continue;
        for (const auto& bind : km->registry().binds()) {
            if (!bind || !bind->hasFlag(Keybinds::BIND_FLAG_RELEASE) || bind->hasFlag(Keybinds::BIND_FLAG_TRANSPARENT))
                continue;
            if (!bind->containsKey(input.key) && !(bind->chordSize() == 0 && (bind->modifierMask() & *input.key.modifier)))
                continue;
            if (std::ranges::none_of(input.suppressedBinds, [&bind](const auto& weak) { return weak.lock() == bind; }))
                input.suppressedBinds.emplace_back(bind);
        }
    }
    km->shadowBinds();
}

static PHLWINDOW windowOf(const SP<SScrollingWindowData>& data) {
    const auto TARGET = data ? data->target.lock() : nullptr;
    return TARGET ? TARGET->window() : nullptr;
}

static void markInOverview(const PHLWINDOW& w, bool on);

COverview::COverview(CScrollingLayout* layout) : m_layout(layout) {
    ;
}

COverview::~COverview() {
    m_discardGuard.reset();
    m_discardPass.clear();
    m_renderListener.reset();
    m_consumedKeys.clear();
    m_keyListener.reset();
    stopInput();
    m_progress.reset();
    // Timers outliving this (and the plugin, when it's unloaded) would call
    // into freed code.
    for (const auto& t : {m_altTimer, m_showTimer}) {
        if (t)
            g_pEventLoopManager->removeTimer(t);
    }
    m_closeEntries.clear();
    m_visualBoxes.clear();
    m_anims.clear();
    for (const auto& r : m_marked) {
        if (const auto W = r.lock())
            markInOverview(W, false);
    }
    m_marked.clear();
}

PHLMONITOR COverview::monitor() const {
    const auto PARENT = m_layout->m_parent.lock();
    if (!PARENT || !PARENT->space())
        return nullptr;
    const auto WS = PARENT->space()->workspace();
    return WS ? WS->m_monitor.lock() : nullptr;
}

// Show a minimized window again: drop the "minimized" tag, the state hyprflow
// keeps windows in line with (it shows the window with its animation and
// focuses it, deferred). Without hyprflow, unhide and focus it here.
static void restoreMinimized(const PHLWINDOW& w) {
    if (w->m_ruleApplicator->m_tagKeeper.applyTag("-minimized")) {
        w->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        w->presentation().refreshValues();
    }
    g_pEventLoopManager->doLater([WREF = PHLWINDOWREF{w}] {
        const auto W = WREF.lock();
        if (!W || !W->mapped() || !W->isHidden() || W->m_ruleApplicator->m_tagKeeper.isTagged("minimized", true))
            return;
        W->setHidden(false);
        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_OTHER);
    });
}

// Tag the window "hyprscroller-overview" so hyprflow hides its title bar,
// restores rounded top corners, and ignores clicks on its hidden real box.
static void markInOverview(const PHLWINDOW& w, bool on) {
    auto& R = w->m_ruleApplicator;
    if (R->m_tagKeeper.applyTag(on ? "+hyprscroller-overview" : "-hyprscroller-overview"))
        R->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
}

void COverview::damage() const {
    if (const auto MON = monitor()) {
        g_pHyprRenderer->damageMonitor(MON);
        MON->scheduleFrame();
    }
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

    // Minimized windows get their real place here (drawn dimmed, see the
    // render hook); picking one restores it.
    g_layoutShowsMinimized = true;
    struct SReset {
        ~SReset() {
            g_layoutShowsMinimized = false;
        }
    } reset;

    // No tiled windows but floating ones (desktop mode): a grid of them, like
    // Windows' Task View, in reading order of where they are.
    if (std::ranges::none_of(DATA->columns, [](const auto& c) { return !c->windowDatas.empty(); }) && !m_floating.empty()) {
        std::vector<SP<SScrollingWindowData>> wins = m_floating;
        std::ranges::sort(wins, [](const auto& a, const auto& b) {
            const auto MA = a->layoutBox.middle(), MB = b->layoutBox.middle();
            return std::abs(MA.y - MB.y) > 120 ? MA.y < MB.y : MA.x < MB.x;
        });
        const double ZOOM  = std::clamp((double)g_config.overview_zoom->value(), 0.1, 1.0);
        const double AVAIL_W = WA.w - 2 * OV_MARGIN, AVAIL_H = WA.h - 2 * OV_MARGIN;
        const size_t N       = wins.size();

        // The column count that gives the biggest thumbnails.
        size_t bestCols = 1;
        double bestScale = 0;
        for (size_t cols = 1; cols <= N; ++cols) {
            const size_t ROWS  = (N + cols - 1) / cols;
            const double CELLW = (AVAIL_W - OV_GAP * 3 * (cols - 1)) / cols, CELLH = (AVAIL_H - OV_GAP * 3 * (ROWS - 1)) / ROWS;
            double       scale = ZOOM;
            for (const auto& w : wins)
                scale = std::min({scale, CELLW / std::max(1.0, w->layoutBox.w), CELLH / std::max(1.0, w->layoutBox.h)});
            if (scale > bestScale) {
                bestScale = scale;
                bestCols  = cols;
            }
        }
        const size_t ROWS  = (N + bestCols - 1) / bestCols;
        const double CELLW = (AVAIL_W - OV_GAP * 3 * (bestCols - 1)) / bestCols, CELLH = (AVAIL_H - OV_GAP * 3 * (ROWS - 1)) / ROWS;
        const double GRID_H = ROWS * CELLH + (ROWS - 1) * OV_GAP * 3;
        double       y      = WA.y + (WA.h - GRID_H) / 2.0;
        for (size_t r = 0; r < ROWS; ++r) {
            const size_t FIRST = r * bestCols, COUNT = std::min(bestCols, N - FIRST);
            const double ROW_W = COUNT * CELLW + (COUNT - 1) * OV_GAP * 3;
            double       x     = WA.x + (WA.w - ROW_W) / 2.0;
            for (size_t i = FIRST; i < FIRST + COUNT; ++i) {
                // Each window keeps its proportions, at its own scale in its cell.
                const auto&  W     = wins[i];
                const double SCALE = std::min({ZOOM, CELLW / std::max(1.0, W->layoutBox.w), CELLH / std::max(1.0, W->layoutBox.h)});
                const Vector2D SZ  = W->layoutBox.size() * SCALE;
                const CBox     BOX{x + (CELLW - SZ.x) / 2.0, y + (CELLH - SZ.y) / 2.0, SZ.x, SZ.y};
                m_slots.push_back({.data = W, .column = m_floatingColumn, .box = BOX.copy().round()});
                x += CELLW + OV_GAP * 3;
            }
            y += CELLH + OV_GAP * 3;
        }
        return;
    }

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
    const auto DRAGGED = m_dragMoved ? m_dragging.lock() : nullptr;
    for (const auto& SLOT : m_slots) {
        const auto DATA = SLOT.data.lock();
        if (!DATA || DATA == DRAGGED)
            continue;
        const auto W   = windowOf(DATA);
        const CBox BOX = W ? visualBoxFor(W) : SLOT.box;
        if (BOX.containsPoint(pos))
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

bool COverview::isManagedWindow(const PHLWINDOW& window) const {
    if (!window)
        return false;
    if (m_active) {
        return std::ranges::any_of(m_slots, [&](const SSlot& slot) { return windowOf(slot.data.lock()) == window; });
    }
    if (m_closing) {
        return std::ranges::any_of(m_closeEntries, [&](const SCloseEntry& entry) { return entry.window.lock() == window; });
    }
    return false;
}

CBox COverview::visualBoxFor(const PHLWINDOW& window) const {
    if (!window)
        return {};
    const auto REAL = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (const auto IT = m_visualBoxes.find(window.get()); IT != m_visualBoxes.end())
        return fitPreviewBox(IT->second, REAL);
    return realWindowBox(window);
}

void COverview::updateVisualBoxes() {
    const auto DRAGGED = m_dragMoved ? m_dragging.lock() : nullptr;

    if (m_active) {
        const float P = m_progress ? std::clamp(m_progress->value(), 0.F, 1.F) : 1.F;
        std::unordered_map<const Desktop::View::CWindow*, CBox> next;
        for (const auto& SLOT : m_slots) {
            const auto DATA   = SLOT.data.lock();
            const auto WINDOW = windowOf(DATA);
            if (!WINDOW)
                continue;
            if (DATA == DRAGGED) {
                if (const auto IT = m_visualBoxes.find(WINDOW.get()); IT != m_visualBoxes.end())
                    next[WINDOW.get()] = IT->second;
                else
                    next[WINDOW.get()] = fitPreviewBox(SLOT.box, WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
                continue;
            }
            if (const auto IT = m_anims.find(WINDOW.get()); IT != m_anims.end())
                next[WINDOW.get()] = lerpBox(IT->second.from, IT->second.to, P);
            else
                next[WINDOW.get()] = fitPreviewBox(SLOT.box, WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
        }
        m_visualBoxes = std::move(next);
        return;
    }

    if (m_closing) {
        const float P = m_progress ? std::clamp(m_progress->value(), 0.F, 1.F) : 1.F;
        std::unordered_map<const Desktop::View::CWindow*, CBox> next;
        for (const auto& E : m_closeEntries) {
            const auto W = E.window.lock();
            if (!W || !W->mapped())
                continue;
            if (E.wasHidden && W->isHidden())
                next[W.get()] = E.from;
            else
                next[W.get()] = lerpBox(E.from, realWindowBox(W), P);
        }
        m_visualBoxes = std::move(next);
    }
}

void COverview::finishClosing() {
    m_closing = false;
    m_closeEntries.clear();
    m_visualBoxes.clear();
    m_slots.clear();
    m_floating.clear();
    for (const auto& r : m_marked) {
        if (const auto W = r.lock())
            markInOverview(W, false);
    }
    m_marked.clear();
    damage();
}

void COverview::queueWindowPreview(Render::CRenderContext& ctx, const PHLWINDOW& window, const PHLMONITOR& mon, const CBox& box, float fadeAlpha) const {
    if (!window || !mon || !window->mapped() || !window->wlSurface() || !window->wlSurface()->resource())
        return;
    if (!ctx.shouldRenderContent(window->m_ruleApplicator->noScreenShare().valueOrDefault()))
        return;
    if (fadeAlpha <= 0.001F)
        return;

    const Vector2D REALSIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (REALSIZE.x < 1.0 || REALSIZE.y < 1.0 || box.w < 1.0 || box.h < 1.0)
        return;

    const CBox     FITTED = fitPreviewBox(box, REALSIZE);
    const float    SCALE  = (float)std::min(FITTED.w / REALSIZE.x, FITTED.h / REALSIZE.y);
    const Vector2D OFFSET_IN_MON{
        std::max(0.0, (mon->m_size.x - REALSIZE.x) / 2.0),
        std::max(0.0, (mon->m_size.y - REALSIZE.y) / 2.0),
    };
    const Vector2D SRC_PX    = OFFSET_IN_MON * mon->m_scale;
    const Vector2D DST_PX    = (FITTED.pos() - mon->m_position) * mon->m_scale;
    const Vector2D TRANSLATE = DST_PX - SRC_PX * SCALE;

    Render::SRenderModifData mod;
    mod.modifs.emplace_back(Render::SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, SCALE);
    mod.modifs.emplace_back(Render::SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE, TRANSLATE);

    CSurfacePassElement::SRenderData renderdata{mon, Time::steadyNow()};
    renderdata.pos           = mon->m_position + OFFSET_IN_MON;
    renderdata.w             = std::max(REALSIZE.x, 5.0);
    renderdata.h             = std::max(REALSIZE.y, 5.0);
    renderdata.dontRound     = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    const float RAW_ROUND    = window->presentation().rounding();
    renderdata.rounding      = (renderdata.dontRound || RAW_ROUND <= 0.F)
             ? 0
             : (int)std::round(std::max(0.0f, RAW_ROUND * mon->m_scale - 1.0f) * SCALE) + 1;
    renderdata.roundingPower = renderdata.dontRound ? 2.0f : window->presentation().roundingPower();
    renderdata.alpha         = window->m_ruleApplicator->opaque().valueOrDefault() ? 1.F : window->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
    renderdata.fadeAlpha     = fadeAlpha;
    renderdata.decorate      = false;
    renderdata.pWindow       = window;

    Render::SWindowRenderPresentation pres;
    pres.alpha                      = renderdata.alpha;
    pres.fadeAlpha                  = renderdata.fadeAlpha;
    pres.alphaVisible               = true;
    renderdata.workspacePresentation = pres;
    renderdata.blur                 = !ctx.m_renderingSnapshot && window->shouldBlur(pres);

    static auto PXWLUSENN = CConfigValue<Config::INTEGER>("xwayland:use_nearest_neighbor");
    if ((window->backend().isX11() && *PXWLUSENN) || window->m_ruleApplicator->nearestNeighbor().valueOrDefault())
        renderdata.useNearestNeighbor = true;

    const auto scope = makeShared<SBackdropScope>();
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CBackdropScopePassElement>(CBackdropScopePassElement::eAction::BEGIN, scope));
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = mod}));

    const auto PREV_WINDOW = ctx.m_data.currentWindow;
    ctx.m_data.currentWindow = window;

    if (window->wlSurface()->small() && !window->wlSurface()->m_fillIgnoreSmall && renderdata.blur) {
        CBox wb = {renderdata.pos.x - mon->m_position.x, renderdata.pos.y - mon->m_position.y, renderdata.w, renderdata.h};
        wb.scale(mon->m_scale).round();
        CRectPassElement::SRectData data;
        data.color                 = CHyprColor(0, 0, 0, 0);
        data.box                   = wb;
        data.round                 = renderdata.dontRound ? 0 : renderdata.rounding - 1;
        data.blur                  = true;
        data.blurA                 = renderdata.fadeAlpha;
        data.xray                  = g_pHyprRenderer->shouldUseNewBlurOptimizations(ctx, nullptr, window);
        data.blurPatternBox        = wb;
        data.blurOwner             = window;
        data.workspacePresentation = pres;
        Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRectPassElement>(data));
        renderdata.blur = false;
    }

    renderdata.surfaceCounter = 0;
    window->wlSurface()->resource()->breadthfirst(
        [&](SP<CWLSurfaceResource> s, const Vector2D& offset, void*) {
            if (!s->m_current.texture)
                return;
            if (s->m_current.size.x < 1 || s->m_current.size.y < 1)
                return;

            renderdata.localPos    = offset;
            renderdata.texture     = s->m_current.texture;
            renderdata.surface     = s;
            renderdata.mainSurface = s == window->wlSurface()->resource();
            Render::IHyprRenderer::addPassElement(ctx, makeUnique<CSurfacePassElement>(renderdata));
            renderdata.surfaceCounter++;
        },
        nullptr);

    ctx.m_data.currentWindow = PREV_WINDOW;
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CBackdropScopePassElement>(CBackdropScopePassElement::eAction::END, scope));
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

    std::unordered_map<const Desktop::View::CWindow*, SAnim> anims;
    bool                                                     changed = false;
    for (const auto& SLOT : m_slots) {
        const auto DATA   = SLOT.data.lock();
        const auto WINDOW = windowOf(DATA);
        if (!WINDOW || DATA == DRAGGED)
            continue;

        const CBox TO   = fitPreviewBox(SLOT.box, WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
        const auto PREV = m_anims.find(WINDOW.get());
        changed         = changed || PREV == m_anims.end() || PREV->second.to != TO;

        // Start from wherever the window is drawn right now, so retargeting
        // mid-animation (opening/closing windows, drops) is smooth.
        const bool APPEAR  = DATA->parked || DATA->layoutBox.empty() || WINDOW->isHidden();
        CBox       current = realWindowBox(WINDOW);
        if (const auto IT = m_visualBoxes.find(WINDOW.get()); IT != m_visualBoxes.end())
            current = IT->second;

        anims[WINDOW.get()] = {.from = APPEAR ? TO : current, .to = TO};
        DATA->parked        = false;
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
    if (!m_active && !m_closing)
        return;

    updateVisualBoxes();
    damage();
}

bool COverview::isFloating(const SP<SScrollingWindowData>& data) const {
    return data && m_floatingColumn && data->column.lock() == m_floatingColumn;
}

// The workspace's floating windows (desktop mode), with where they are now.
void COverview::collectFloating() {
    m_floating.clear();
    if (!m_floatingColumn) {
        m_floatingColumn       = makeShared<SColumnData>(m_layout);
        m_floatingColumn->self = m_floatingColumn;
    }
    const auto PARENT = m_layout->m_parent.lock();
    const auto WS     = PARENT && PARENT->space() ? PARENT->space()->workspace() : nullptr;
    if (!WS)
        return;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w->mapped() || !w->isFloating() || w->m_workspace != WS || (w->backend().isX11() && w->backend().traits().overrideRedirect) || !w->layoutTarget())
            continue;
        auto data       = makeShared<SScrollingWindowData>(w->layoutTarget(), m_floatingColumn);
        data->layoutBox = CBox{w->positionAnimation()->goal(), w->sizeAnimation()->goal()};
        m_floating.push_back(data);
    }
}

// The shown windows, most recently used first (Alt+Tab's order).
std::vector<SP<SScrollingWindowData>> COverview::mruOrder() const {
    const auto&                                         HISTORY = Desktop::History::windowTracker()->fullHistory(); // oldest first
    std::vector<std::pair<size_t, SP<SScrollingWindowData>>> ranked;
    for (const auto& SLOT : m_slots) {
        const auto DATA = SLOT.data.lock();
        const auto W    = windowOf(DATA);
        if (!W)
            continue;
        size_t rank = SIZE_MAX;
        for (size_t i = HISTORY.size(); i-- > 0;) {
            if (HISTORY[i].lock() == W) {
                rank = HISTORY.size() - 1 - i;
                break;
            }
        }
        // Minimized windows go last, like Windows: Alt+Tab shouldn't bring
        // back the window you just minimized.
        if (W->isHidden())
            rank = rank == SIZE_MAX ? SIZE_MAX : rank + (SIZE_MAX / 2);
        ranked.emplace_back(rank, DATA);
    }
    std::ranges::stable_sort(ranked, {}, &std::pair<size_t, SP<SScrollingWindowData>>::first);
    std::vector<SP<SScrollingWindowData>> out;
    for (auto& [_, d] : ranked)
        out.push_back(d);
    return out;
}

void COverview::switchWindows(bool reverse) {
    if (!m_active) {
        // Like Windows: nothing shows yet. Let go of Alt quickly and it just
        // flips to the window (quickSwitch); keep holding and the switcher
        // opens (showSwitcher). Further Tabs meanwhile count.
        if (m_switchPending) {
            m_pendingSteps += reverse ? -1 : 1;
            return;
        }
        m_switchPending = true;
        m_pendingSteps  = reverse ? -1 : 1;

        // Hyprland has no modifier-change event: poll, and switch as soon as
        // no keyboard holds Alt any more.
        if (!m_altTimer) {
            m_altTimer = makeShared<CEventLoopTimer>(
                std::nullopt,
                [this, ALIVE = WP<bool>{m_alive}](SP<CEventLoopTimer> self, void*) {
                    if (!ALIVE.lock())
                        return;
                    const bool ALT = std::ranges::any_of(g_pInputManager->m_keyboards, [](const auto& kb) { return kb && (kb->getModifiers() & Input::HL_MODIFIER_ALT); });
                    if (m_switchPending) {
                        if (!ALT) {
                            m_switchPending = false;
                            m_showTimer->updateTimeout(std::nullopt);
                            quickSwitch(m_pendingSteps);
                            return;
                        }
                    } else if (!m_active || !m_switchMode)
                        return;
                    else if (!ALT) {
                        confirm();
                        return;
                    }
                    self->updateTimeout(std::chrono::milliseconds(25));
                },
                nullptr);
            g_pEventLoopManager->addTimer(m_altTimer);
        }
        if (!m_showTimer) {
            m_showTimer = makeShared<CEventLoopTimer>(
                std::nullopt,
                [this, ALIVE = WP<bool>{m_alive}](SP<CEventLoopTimer>, void*) {
                    if (!ALIVE.lock() || !m_switchPending)
                        return;
                    m_switchPending = false;
                    showSwitcher(m_pendingSteps);
                },
                nullptr);
            g_pEventLoopManager->addTimer(m_showTimer);
        }
        m_altTimer->updateTimeout(std::chrono::milliseconds(25));
        m_showTimer->updateTimeout(std::chrono::milliseconds(OV_SWITCH_DELAY_MS));
        return;
    }
    m_switchMode     = true;
    const auto ORDER = mruOrder();
    if (ORDER.empty())
        return;
    const auto   IT  = std::ranges::find(ORDER, m_selected.lock());
    const size_t IDX = IT == ORDER.end() ? 0 : IT - ORDER.begin();
    select(ORDER[(IDX + (reverse ? ORDER.size() - 1 : 1)) % ORDER.size()]);
}

void COverview::showSwitcher(int steps) {
    open();
    if (!m_active)
        return;
    m_switchMode = true;
    m_altTimer->updateTimeout(std::chrono::milliseconds(25));
    // Starts on the window used before this one (or further back after more
    // Tabs), like Alt+Tab.
    const auto ORDER = mruOrder();
    if (ORDER.empty())
        return;
    const int N = (int)ORDER.size();
    select(ORDER[((steps % N) + N) % N]);
}

void COverview::quickSwitch(int steps) {
    const auto PARENT = m_layout->m_parent.lock();
    const auto WS     = PARENT && PARENT->space() ? PARENT->space()->workspace() : nullptr;
    if (!WS)
        return;

    // This workspace's windows (minimized ones too, as in the switcher), most
    // recently used first.
    std::vector<PHLWINDOW> order;
    const auto             add = [&](const PHLWINDOW& w) {
        if (w && w->mapped() && w->m_workspace == WS && std::ranges::find(order, w) == order.end())
            order.push_back(w);
    };
    const auto& HISTORY = Desktop::History::windowTracker()->fullHistory(); // oldest first
    for (size_t i = HISTORY.size(); i-- > 0;)
        add(HISTORY[i].lock());
    for (const auto& w : Desktop::windowState()->windows())
        add(w);
    // Minimized ones last (see mruOrder()).
    std::ranges::stable_partition(order, [](const PHLWINDOW& w) { return !w->isHidden(); });
    if (order.size() < 2)
        return;

    const int  N = (int)order.size();
    const auto W = order[((steps % N) + N) % N];
    if (W->isHidden())
        restoreMinimized(W);
    else if (W->isFloating()) {
        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_OTHER);
        Desktop::windowState()->raise(W);
    } else if (const auto T = W->layoutTarget())
        m_layout->focusTargetUpdate(T);
}

void COverview::toggle() {
    suppressHeldReleaseBinds();
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
    suppressHeldReleaseBinds();
    if (m_active)
        return;

    const auto PARENT = m_layout->m_parent.lock();
    if (!PARENT || !PARENT->space())
        return;
    const auto WS = PARENT->space()->workspace();
    if (!WS)
        return;

    if (m_closing) {
        updateVisualBoxes();
        m_closing = false;
        m_closeEntries.clear();
    }

    collectFloating();
    const bool TILED = std::ranges::any_of(m_layout->m_scrollingData->columns, [](const auto& c) { return !c->windowDatas.empty(); });
    if (!TILED && m_floating.empty())
        return;

    // Remember a fullscreen/maximized window without exiting fullscreen: its
    // preview is scaled visually alongside the windows underneath, and
    // fullscreen is only exited if a different window is picked on close.
    m_fullscreenWindow = Fullscreen::controller()->getFullscreenWindow(WS, true);

    auto FOCUSED = m_layout->focusedWindowData();
    if (!FOCUSED) {
        const auto FW = Desktop::focusState()->window();
        for (const auto& f : m_floating) {
            if (windowOf(f) == FW)
                FOCUSED = f;
        }
    }
    m_initialFocus = FOCUSED ? FOCUSED->target : WP<Layout::ITarget>{};
    if (FOCUSED)
        m_selected = FOCUSED;
    else if (TILED)
        m_selected = std::ranges::find_if(m_layout->m_scrollingData->columns, [](const auto& c) { return !c->windowDatas.empty(); })->get()->windowDatas.front();
    else
        m_selected = m_floating.front();
    m_highlightLast    = {};
    m_dragging.reset();
    m_pressed       = false;
    m_dragMoved     = false;
    m_ignoreRelease = false;
    m_menuOpen      = false;
    m_active        = true;
    m_session       = makeShared<bool>(true);
    m_overlay       = true;
    for (const auto& r : m_marked) {
        if (const auto W = r.lock())
            markInOverview(W, false);
    }
    m_marked.clear();
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w->mapped() && w->m_workspace == WS) {
            markInOverview(w, true);
            m_marked.emplace_back(w);
        }
    }
    m_fadeStart = std::chrono::steady_clock::now();

    // While open, the overview owns the pointer: show a plain arrow instead of
    // whatever shape the window under it last asked for.
    g_pHyprRenderer->setCursorFromName("left_ptr", true);

    // Own the keyboard: navigation keys act on the overview; nothing reaches
    // clients or keybinds. Modifier keys pass through so no modifier gets
    // stuck, while held release binds (e.g. SUPER + SUPER_L) are suppressed
    // and matching releases of keys consumed by overview are swallowed cleanly.
    if (!m_keyListener) {
        m_keyListener = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent ev, Event::SCallbackInfo& info) {
            if (ev.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
                if (m_consumedKeys.erase(ev.keycode) > 0) {
                    info.cancelled = true;
                    suppressHeldReleaseBinds();
                    if (!m_active && m_consumedKeys.empty())
                        m_keyListener.reset();
                    return;
                }
                if (m_active && m_switchMode && (ev.keycode == KEY_LEFTALT || ev.keycode == KEY_RIGHTALT)) {
                    confirm();
                    return;
                }
                if (m_active)
                    suppressHeldReleaseBinds();
                return;
            }

            if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
                return;

            if (!m_active) {
                m_consumedKeys.erase(ev.keycode);
                if (m_consumedKeys.empty())
                    m_keyListener.reset();
                return;
            }

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
            m_consumedKeys.insert(ev.keycode);
            suppressHeldReleaseBinds();

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

            if (m_switchMode && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
                const auto KB    = g_pSeatManager->m_keyboard.lock();
                const bool SHIFT = sym == XKB_KEY_ISO_Left_Tab || (KB && (KB->getModifiers() & Input::HL_MODIFIER_SHIFT));
                switchWindows(SHIFT);
                return;
            }

            switch (sym) {
                case XKB_KEY_Escape:
                case XKB_KEY_Tab:
                case XKB_KEY_ISO_Left_Tab: close(nullptr); break;
                case XKB_KEY_Return:
                case XKB_KEY_KP_Enter:
                case XKB_KEY_space: confirm(); break;
                case XKB_KEY_q:
                case XKB_KEY_Delete: closeSelected(); break;
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
    }

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

        if (m_pressed && !m_dragMoved && pos.distance(m_pressPos) > OV_DRAG_THRESHOLD && m_dragging.lock() && !isFloating(m_dragging.lock())) {
            m_dragMoved = true;
            // Lift the dragged window out of the animation and above its
            // neighbours.
            if (const auto DATA = m_dragging.lock()) {
                if (const auto WINDOW = windowOf(DATA)) {
                    m_anims.erase(WINDOW.get());
                    Desktop::focusState()->rawWindowFocus(WINDOW, Desktop::FOCUS_REASON_OTHER);
                }
            }
            g_pHyprRenderer->setCursorFromName("grabbing", true);
        }

        if (m_dragMoved) {
            const auto DATA   = m_dragging.lock();
            const auto WINDOW = windowOf(DATA);
            if (WINDOW) {
                const CBox CUR              = visualBoxFor(WINDOW);
                m_visualBoxes[WINDOW.get()] = CBox{pos - m_grabOffset, CUR.size()}.round();
                damage();
            }
            return;
        }

        if (const auto SLOT = slotAt(pos))
            select(SLOT->data.lock());

        const auto SEL_WIN = windowOf(m_selected.lock());
        const bool HOVER   = SEL_WIN && closeButtonBox(visualBoxFor(SEL_WIN)).containsPoint(pos);
        if (HOVER != m_closeHover) {
            m_closeHover = HOVER;
            damage();
        }
    });

    m_buttonListener = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;
        suppressHeldReleaseBinds();

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
            if (const auto SEL_WIN = windowOf(m_selected.lock()); SEL_WIN && closeButtonBox(visualBoxFor(SEL_WIN)).containsPoint(POS)) {
                closeSelected();
                m_ignoreRelease = true;
                return;
            }
            const auto SLOT     = slotAt(POS);
            const auto SLOT_WIN = SLOT ? windowOf(SLOT->data.lock()) : nullptr;
            m_pressed           = true;
            m_dragMoved         = false;
            m_ignoreRelease     = false;
            m_pressPos          = POS;
            m_dragging          = SLOT ? SLOT->data : WP<SScrollingWindowData>{};
            m_grabOffset        = SLOT_WIN ? POS - visualBoxFor(SLOT_WIN).pos() : Vector2D{};
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

    // A floating window (desktop mode) closing meanwhile: drop its thumbnail
    // and close the gap. (Tiled ones go through the layout, which re-applies.)
    m_closeListener = Event::bus()->m_events.window.close.listen([this](PHLWINDOW w) {
        if (!m_active || !w)
            return;
        const auto BEFORE = m_floating.size();
        std::erase_if(m_floating, [&](const auto& d) {
            const auto W = windowOf(d);
            return !W || W == w;
        });
        if (m_floating.size() != BEFORE)
            g_pEventLoopManager->doLater([this, SESSION = WP<bool>{m_session}] {
                if (const auto S = SESSION.lock(); S && *S)
                    apply();
            });
    });

    // Wheel: step the selection through all windows in reading order.
    m_axisListener = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent ev, Event::SCallbackInfo& info) {
        if (!m_active)
            return;
        info.cancelled = true;
        suppressHeldReleaseBinds();

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
        m_renderListener = Event::bus()->m_events.render.stage.listen([this](const Event::SRenderStageEvent& ev) {
            const auto stage = ev.stage;
            if (!m_overlay || !ev.context || ev.context->get().m_renderingSnapshot)
                return;
            if (stage != RENDER_PRE_WINDOWS && stage != RENDER_PRE_WINDOW && stage != RENDER_POST_WINDOW && stage != RENDER_POST_WINDOWS)
                return;
            const auto MON = monitor();
            if (!MON || ev.monitor != MON)
                return;
            auto& ctx = ev.context->get();

            if (stage == RENDER_PRE_WINDOW) {
                if (!m_renderingOverview && isManagedWindow(ctx.m_data.currentWindow.lock())) {
                    m_discardPass.clear();
                    m_discardGuard = Render::IHyprRenderer::redirectPass(ctx, &m_discardPass);
                }
                return;
            }

            if (stage == RENDER_POST_WINDOW) {
                if (m_discardGuard) {
                    m_discardGuard.reset();
                    m_discardPass.clear();
                }
                return;
            }

            if (m_discardGuard) {
                m_discardGuard.reset();
                m_discardPass.clear();
            }

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

            if (stage == RENDER_POST_WINDOWS && (m_active || m_closing)) {
                updateVisualBoxes();

                const auto DRAGGED  = m_dragMoved ? m_dragging.lock() : nullptr;
                const auto DRAG_WIN = windowOf(DRAGGED);

                std::vector<PHLWINDOW> drawOrder;
                for (const auto& w : Desktop::windowState()->windows()) {
                    if (w != DRAG_WIN && isManagedWindow(w))
                        drawOrder.push_back(w);
                }
                if (m_active && DRAG_WIN && isManagedWindow(DRAG_WIN))
                    drawOrder.push_back(DRAG_WIN);

                m_renderingOverview = true;
                Hyprutils::Utils::CScopeGuard overviewGuard([this] { m_renderingOverview = false; });

                const float OALPHA = overlayAlpha();
                for (const auto& w : drawOrder) {
                    const float FADE = w->isHidden() ? OALPHA : w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE);
                    queueWindowPreview(ctx, w, MON, visualBoxFor(w), FADE);
                }
            }

            Render::IHyprRenderer::addPassElement(ctx, makeUnique<COverviewPassElement>(this, stage));
        });
    }

    apply(!g_config.overview_animate->value());
}

void COverview::stopInput() {
    if (m_consumedKeys.empty())
        m_keyListener.reset();
    m_moveListener.reset();
    m_buttonListener.reset();
    m_axisListener.reset();
    m_closeListener.reset();
}

void COverview::confirm() {
    if (m_active)
        close(m_selected.lock());
}

CBox COverview::closeButtonBox(const CBox& thumb) const {
    // In the thumbnail's top right corner, like Windows' switcher; not on
    // thumbnails too small to have room for it.
    if (thumb.w < OV_CLOSE_SIZE * 3 || thumb.h < OV_CLOSE_SIZE * 3)
        return {};
    return CBox{thumb.x + thumb.w - OV_CLOSE_SIZE - 8, thumb.y + 8, OV_CLOSE_SIZE, OV_CLOSE_SIZE};
}

// Close the selected window and stay open, the selection moving on to the
// next window (Q / Delete, or its close button).
void COverview::closeSelected() {
    const auto SEL = m_selected.lock();
    const auto W   = windowOf(SEL);
    if (!m_active || m_menuOpen || !W)
        return;
    const auto ORDER = mruOrder();
    if (const auto IT = std::ranges::find(ORDER, SEL); IT != ORDER.end() && ORDER.size() > 1)
        select(ORDER[(IT - ORDER.begin() + 1) % ORDER.size()]);
    m_closeHover = false;
    (void)Config::Actions::closeWindow(W);
}

void COverview::close(SP<SScrollingWindowData> pick) {
    suppressHeldReleaseBinds();
    if (!m_active)
        return;

    updateVisualBoxes();

    const auto PICK_WIN = windowOf(pick);
    m_closeEntries.clear();
    for (const auto& SLOT : m_slots) {
        const auto W = windowOf(SLOT.data.lock());
        if (!W || !W->mapped())
            continue;
        // Restoring a hidden window is handed back to restoreMinimized / hyprflow Genie.
        if (W->isHidden() && W == PICK_WIN)
            continue;
        m_closeEntries.push_back({.window = W, .from = visualBoxFor(W), .wasHidden = W->isHidden()});
    }

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
    m_switchMode = false;
    m_fadeStart  = std::chrono::steady_clock::now();
    g_pHyprRenderer->setCursorFromName("left_ptr", true);

    // Picking a minimized window restores it.
    if (PICK_WIN && PICK_WIN->isHidden())
        restoreMinimized(PICK_WIN);

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

    // Only exit fullscreen if a *different* window was picked; closing back
    // to the fullscreen window (or cancelling) keeps it fullscreen untouched.
    if (const auto FSW = m_fullscreenWindow.lock(); FSW && FSW->mapped() && PICK_WIN && PICK_WIN != FSW) {
        Fullscreen::controller()->setFullscreenMode(FSW, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        g_pEventLoopManager->doLater([FSWREF = PHLWINDOWREF{FSW}, ALIVE = WP<bool>{m_alive}] {
            if (!ALIVE.lock())
                return;
            // A floating window that opened fullscreen (e.g. Remmina) has no
            // useful un-fullscreen size of its own: center at 80% of work area.
            if (const auto W = FSWREF.lock(); W && W->mapped() && W->isFloating() && W->layoutTarget()) {
                const auto MON = W->m_monitor.lock();
                const auto SZ  = W->sizeAnimation()->goal();
                if (MON) {
                    const CBox WA = MON->logicalBoxMinusReserved();
                    if (SZ.x < WA.w * 0.4 || SZ.y < WA.h * 0.4) {
                        const Vector2D NS = (WA.size() * 0.8).round();
                        W->layoutTarget()->setPositionGlobal(CBox{WA.pos() + (WA.size() - NS) / 2.0, NS}.round());
                    }
                }
            }
        });
    }
    m_fullscreenWindow.reset();

    // Update layout scroll offset / focus on close (windows themselves keep
    // their real sizes while in overview; floating windows never moved).
    m_layout->m_scrollingData->recalculate(!g_config.overview_animate->value());

    if (pick && target && isFloating(pick)) {
        if (const auto W = target->window()) {
            Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_OTHER);
            Desktop::windowState()->raise(W);
        }
    } else if (pick && target)
        m_layout->focusTargetUpdate(target);

    if (!g_config.overview_animate->value() || m_closeEntries.empty()) {
        finishClosing();
        m_progress.reset();
        m_overlay = false;
        m_textCache.clear();
        m_renderListener.reset();
        damage();
        return;
    }

    m_closing = true;
    if (!m_progress) {
        Animation::mgr()->createAnimation(0.F, m_progress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        m_progress->setUpdateCallback([this](auto) { step(); });
    }
    m_progress->setValueAndWarp(0.F);
    *m_progress = 1.F;
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

    if (!COL || COL == m_floatingColumn || m_columnSlots.empty()) {
        const Vector2D CUR       = SLOT->box.middle();
        double         bestScore = INFINITY;
        for (const auto& S : m_slots) {
            const auto D = S.data.lock();
            if (!D || D == CURRENT)
                continue;
            const Vector2D M  = S.box.middle();
            const double   DX = M.x - CUR.x;
            const double   DY = M.y - CUR.y;
            double         primary = 0.0, ortho = 0.0;
            switch (dir) {
                case Math::DIRECTION_LEFT:  primary = -DX; ortho = std::abs(DY); break;
                case Math::DIRECTION_RIGHT: primary =  DX; ortho = std::abs(DY); break;
                case Math::DIRECTION_UP:    primary = -DY; ortho = std::abs(DX); break;
                case Math::DIRECTION_DOWN:  primary =  DY; ortho = std::abs(DX); break;
                default: continue;
            }
            if (primary <= 1.0)
                continue;
            const double SCORE = primary + 2.5 * ortho;
            if (SCORE < bestScore) {
                bestScore = SCORE;
                next      = D;
            }
        }
        select(next);
        return;
    }

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
    // The menu's items are column operations; floating windows have none.
    if (!data || isFloating(data))
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

    items.push_back({.label = WINDOW->metadata().title().empty() ? WINDOW->metadata().appID() : WINDOW->metadata().title(), .header = true});

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
                             if (hyprscrolling_compat::isNumbered(CUR, id))
                                 continue;
                             sub.push_back({.label = std::format("Workspace {}", id), .action = [this, WDATA, id] {
                                                const auto W = windowOf(WDATA.lock());
                                                const auto M = monitor();
                                                if (!W || !M)
                                                    return;
                                                const auto ws = hyprscrolling_compat::getOrCreateWorkspace(id, M);
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

void COverview::drawMenu(Render::CRenderContext& ctx, const PHLMONITOR& mon, float alpha) {
    const float SCALE = mon->m_scale;
    const auto  local = [&](const CBox& b) { return b.copy().translate(-mon->m_position).scale(SCALE).round(); };

    const CBox BG = local(m_menuBox);
    g_pHyprOpenGL->renderRect(ctx, BG.copy().expand(std::round(1 * SCALE)), CHyprColor{1, 1, 1, 0.10F * alpha}, {.round = (int)std::round(11 * SCALE)});
    g_pHyprOpenGL->renderRect(ctx, BG, CHyprColor{0.10F, 0.10F, 0.12F, 0.97F * alpha}, {.round = (int)std::round(10 * SCALE)});

    for (size_t i = 0; i < m_menuItems.size(); ++i) {
        const auto& IT  = m_menuItems[i];
        const CBox  ROW = local(m_menuItemBoxes[i]);

        if (IT.separator) {
            g_pHyprOpenGL->renderRect(ctx, CBox{ROW.x + 6 * SCALE, ROW.y + ROW.h / 2.0, ROW.w - 12 * SCALE, std::max(1.0, (double)std::round(SCALE))}.round(),
                                      CHyprColor{1, 1, 1, 0.10F * alpha}, {});
            continue;
        }

        if ((int)i == m_menuHover)
            g_pHyprOpenGL->renderRect(ctx, ROW, accentColor(0.35F * alpha), {.round = (int)std::round(6 * SCALE)});

        const double TEXT_X = ROW.x + (IT.header ? 8 : 12) * SCALE;
        const auto   TEX    = text(IT.label, (int)(ROW.w - 44 * SCALE));
        if (TEX) {
            const CBox TB{TEXT_X, ROW.y + (ROW.h - TEX->m_size.y) / 2.0, TEX->m_size.x, TEX->m_size.y};
            g_pHyprOpenGL->renderTexture(ctx, TEX, TB.copy().round(), {.a = (IT.header ? 0.55F : 1.F) * alpha});
        }

        // Trailing marker: chevron for submenus, dot for the current preset.
        const std::string MARK = IT.submenu ? "›" : IT.checked ? "●" : "";
        if (const auto MT = MARK.empty() ? nullptr : text(MARK, (int)(24 * SCALE))) {
            const CBox MB{ROW.x + ROW.w - 12 * SCALE - MT->m_size.x, ROW.y + (ROW.h - MT->m_size.y) / 2.0, MT->m_size.x, MT->m_size.y};
            g_pHyprOpenGL->renderTexture(ctx, MT, MB.copy().round(), {.a = 0.7F * alpha});
        }
    }
}

void COverview::draw(Render::CRenderContext& ctx, eRenderStage stage) {
    const auto MON = monitor();
    if (!MON)
        return;

    const float ALPHA   = overlayAlpha();
    bool        animate = m_active ? ALPHA < 1.F : ALPHA > 0.F;

    if (stage == RENDER_PRE_WINDOWS) {
        // Dim the wallpaper behind the zoomed-out windows.
        if (ALPHA > 0.F)
            g_pHyprOpenGL->renderRect(ctx, CBox{{}, MON->m_pixelSize}, CHyprColor{0, 0, 0, OV_DIM * ALPHA}, {});
        if (animate)
            damage();
        return;
    }

    if (stage != RENDER_POST_WINDOWS)
        return;

    const bool CLOSING_ANIM = m_closing && m_progress && m_progress->isBeingAnimated();

    if (ALPHA <= 0.F) {
        if (m_closing && !CLOSING_ANIM)
            finishClosing();
        if (!m_active && !m_closing) {
            m_progress.reset();
            m_overlay = false;
            m_textCache.clear();
            m_renderListener.reset();
        } else if (m_closing) {
            damage();
        }
        return;
    }

    const float SCALE    = MON->m_scale;
    const auto  SELECTED = m_selected.lock();
    const auto  DRAGGED  = m_dragMoved ? m_dragging.lock() : nullptr;
    const auto  DRAG_WIN = windowOf(DRAGGED);
    const CBox  DRAG_BOX = DRAG_WIN ? visualBoxFor(DRAG_WIN) : CBox{};

    for (const auto& SLOT : m_slots) {
        const auto DATA   = SLOT.data.lock();
        const auto WINDOW = windowOf(DATA);
        if (!WINDOW || (!m_active && !isManagedWindow(WINDOW)))
            continue;

        const CBox     global     = visualBoxFor(WINDOW);
        const Vector2D REAL       = WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const double   WIN_SCALE  = std::min(global.w / std::max(1.0, REAL.x), global.h / std::max(1.0, REAL.y));
        const bool     DONT_ROUND = Fullscreen::controller()->getFullscreenModes(WINDOW).internal == Fullscreen::FSMODE_FULLSCREEN;
        const float    RAW_ROUND  = WINDOW->presentation().rounding();
        const int      WIN_ROUND  = (DONT_ROUND || RAW_ROUND <= 0.F) ? 0 : (int)std::round(RAW_ROUND * WIN_SCALE * SCALE);

        if (WINDOW->isHidden() && m_active) {
            const CBox LOCAL = global.copy().translate(-MON->m_position).scale(SCALE).round();
            g_pHyprOpenGL->renderRect(ctx, LOCAL, CHyprColor{0, 0, 0, 0.55F * ALPHA}, {.round = WIN_ROUND});
        }

        if (DATA == SELECTED && m_active) {
            // Slide the highlight from where it was to this window.
            const CBox TARGET = global.copy().expand(4);
            const int  MS     = msSince(m_highlightStart);
            const CBox HL     = m_highlightFrom.empty() || MS >= OV_HIGHLIGHT_MS ? TARGET : lerpBox(m_highlightFrom, TARGET, easeOutCubic((double)MS / OV_HIGHLIGHT_MS));
            if (MS < OV_HIGHLIGHT_MS)
                animate = true;
            m_highlightLast = HL;

            const int  BORDER   = (int)std::round(3 * SCALE);
            const CBox LOCAL    = HL.copy().translate(-MON->m_position).scale(SCALE).round();
            const int  HL_ROUND = WIN_ROUND > 0 ? WIN_ROUND + BORDER : 0;
            g_pHyprOpenGL->renderBorder(ctx, LOCAL.copy().expand(BORDER), Config::CGradientValueData{accentColor(ALPHA)},
                                        {.round = HL_ROUND, .borderSize = BORDER, .a = ALPHA});

            // Close button, once the highlight has arrived (red on hover).
            if (const CBox BTN = closeButtonBox(global); !BTN.empty() && MS >= OV_HIGHLIGHT_MS && !m_menuOpen && !DRAG_WIN) {
                const CBox B  = BTN.copy().translate(-MON->m_position).scale(SCALE).round();
                const auto BG = m_closeHover ? CHyprColor{0.86F, 0.24F, 0.24F, 0.95F * ALPHA} : CHyprColor{0.08F, 0.08F, 0.10F, 0.72F * ALPHA};
                g_pHyprOpenGL->renderRect(ctx, B, BG, {.round = (int)(B.w / 2)});
                const std::string KEY = "\x01close" + std::to_string((int)std::round(15 * SCALE));
                auto              tex = m_textCache[KEY];
                if (!tex)
                    tex = m_textCache[KEY] = g_pHyprRenderer->renderText("×", CHyprColor{1, 1, 1, 1}, (int)std::round(15 * SCALE), false, "", 0, 400);
                if (tex)
                    g_pHyprOpenGL->renderTexture(ctx, tex, CBox{B.x + (B.w - tex->m_size.x) / 2.0, B.y + (B.h - tex->m_size.y) / 2.0, tex->m_size.x, tex->m_size.y}.round(),
                                                 {.a = ALPHA});
            }
        }

        if (!g_config.overview_titles->value())
            continue;

        // The dragged window is drawn above the others; their labels would
        // otherwise show through it.
        if (DRAG_WIN && DATA != DRAGGED && !global.intersection(DRAG_BOX).empty())
            continue;

        // Size the label for the window's slot, not its animated box: the box
        // changes every frame of the zoom and would re-render the text each time.
        const CBox   BOX   = global.copy().translate(-MON->m_position).scale(SCALE).round();
        const double FIT_W = fitPreviewBox(SLOT.box, REAL).w;
        const auto   TEX   = BOX.w > 1 ? text(WINDOW->metadata().title(), (int)std::round((FIT_W - 32) * SCALE)) : nullptr;
        if (!TEX)
            continue;

        const double PAD_X = std::round(10 * SCALE), PAD_Y = std::round(5 * SCALE);
        const CBox   TEXT{BOX.x + (BOX.w - TEX->m_size.x) / 2.0, BOX.y + BOX.h - TEX->m_size.y - PAD_Y - std::round(12 * SCALE), TEX->m_size.x, TEX->m_size.y};
        CBox         pill = TEXT.copy().expand(PAD_Y);
        pill.x -= PAD_X - PAD_Y;
        pill.w += 2 * (PAD_X - PAD_Y);
        pill.round();

        g_pHyprOpenGL->renderRect(ctx, pill, CHyprColor{0, 0, 0, 0.65F * ALPHA}, {.round = (int)(pill.h / 2)});
        g_pHyprOpenGL->renderTexture(ctx, TEX, TEXT.copy().round(), {.a = ALPHA});
    }

    if (m_menuOpen && m_active)
        drawMenu(ctx, MON, ALPHA);

    if (m_closing && !CLOSING_ANIM)
        finishClosing();

    // Keep repainting while something animates (the fade-out ends with a frame
    // at alpha 0, which drops the overlay above).
    if (animate || m_closing)
        damage();
}
