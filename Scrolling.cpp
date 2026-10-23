#include "Scrolling.hpp"
#include "Overview.hpp"
#include "globals.hpp"

#include <algorithm>
#include <linux/input-event-codes.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/time/Timer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/macros.hpp>

// ---- Hyprland 0.56 compatibility includes (view refactor) ----
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/protocols/wlr-layer-shell-unstable-v1.hpp>

#include <hyprutils/string/VarList.hpp>
#include <hyprutils/string/ConstVarList.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

// ---- Hyprland 0.56 compatibility shims ----
// The 0.56 "view" refactor removed several helpers from CCompositor and moved
// global state into State:: trackers. These shims restore the old behaviour.
namespace hyprscrolling_compat {
    inline PHLMONITOR monitorFromCursor() {
        const auto COORDS = g_pInputManager->getMouseCoordsInternal();
        for (auto& m : State::monitorState()->monitors()) {
            if (!m)
                continue;
            // Use logicalBox() for consistency with the rest of the plugin
            // (accounts for scaling/transforms, not just raw position/size).
            const auto BOX = m->logicalBox();
            if (COORDS.x >= BOX.x && COORDS.y >= BOX.y && COORDS.x < BOX.x + BOX.w && COORDS.y < BOX.y + BOX.h)
                return m;
        }
        return nullptr;
    }
    inline PHLWORKSPACE workspaceByID(const WORKSPACEID& id) {
        for (auto& w : State::workspaceState()->workspacesCopy()) {
            if (w && w->m_id == id)
                return w;
        }
        return nullptr;
    }
}
using namespace Hyprutils::String;
using namespace Hyprutils::Utils;

constexpr float MIN_COLUMN_WIDTH = 0.05F;
constexpr float MAX_COLUMN_WIDTH = 1.F;
constexpr float MIN_ROW_HEIGHT   = 0.1F;
constexpr float MAX_ROW_HEIGHT   = 1.F;

// Coordinate used to park hidden windows (e.g. non-zen columns in zen mode)
// well outside any realistic multi-monitor logical coordinate space.
constexpr double OFFSCREEN_COORD = -100000.0;

// Runtime override of new_window_placement set by 'placement' layout messages:
// -1 = use config, 0 = new column, 1 = stack. Shared by all workspaces.
static int g_placementOverride = -1;

// An X11 window that asked to be at least as large as its monitor
// (borderless-fullscreen games and the like). Override-redirect windows are
// menus/tooltips, never layout targets worth fullscreening.
static bool opensAtScreenSize(const PHLWINDOW& w) {
    if (!w->m_isX11 || w->isX11OverrideRedirect())
        return false;

    const auto XSURF = w->m_xwaylandSurface.lock();
    const auto PMON  = w->m_monitor.lock();
    if (!XSURF || !PMON)
        return false;

    const auto MON    = PMON->logicalBox().size();
    const auto covers = [&MON](double width, double height) { return width >= MON.x - 1 && height >= MON.y - 1; };

    // By the time the layout sees the window XWayland has usually configured
    // it already, so m_geometry alone misses most cases. WM_NORMAL_HINTS keeps
    // the size the client asked for, and a minimum size can rule out tiling.
    if (covers(XSURF->m_geometry.w, XSURF->m_geometry.h))
        return true;

    if (const auto* HINTS = XSURF->m_sizeHints.get()) {
        if ((HINTS->flags & (XCB_ICCCM_SIZE_HINT_US_SIZE | XCB_ICCCM_SIZE_HINT_P_SIZE)) && covers(HINTS->width, HINTS->height))
            return true;
        if ((HINTS->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) && covers(HINTS->min_width, HINTS->min_height))
            return true;
    }

    return false;
}

// Whether a click/hover at `pos` should be ignored for scrolling: the pointer
// is on the shell, i.e. in the area reserved by bars/panels (exclusive zones),
// or over a layer-shell surface in the top/overlay layers that takes input
// there (panels, drawers, notifications), or inside the optional fixed
// click_edge_left/right margins.
static bool pointerBlocked(const PHLMONITOR& mon, const Vector2D& pos) {
    const auto BOX = mon->logicalBox();
    if (pos.x < BOX.x + (double)g_config.click_edge_left->value() || pos.x > BOX.x + BOX.w - (double)g_config.click_edge_right->value())
        return true;

    // Windows scrolled off-screen extend under the bar; a click on an empty
    // part of the bar passes through to them but is still meant for the shell.
    if (!mon->logicalBoxMinusReserved().containsPoint(pos))
        return true;

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP}) {
        Vector2D surfaceCoords;
        PHLLS    found;
        if (Desktop::viewState()->hitTest().layerSurfaceAt(pos, &mon->m_layerSurfaceLayers[LAYER], &surfaceCoords, &found))
            return true;
    }

    return false;
}

// Push a window's layoutBox to its target. Windows coming back from being
// parked off-screen are warped, so they don't fly in from OFFSCREEN_COORD.
static void applyLayoutBox(const SP<SScrollingWindowData>& WDATA, bool forceInstant) {
    auto target = WDATA->target.lock();
    if (!target)
        return;

    CBox box = WDATA->layoutBox;
    box.round();
    target->setPositionGlobal(box);
    if (forceInstant || WDATA->parked) {
        target->warpPositionSize();
        target->damageEntire();
    }
    WDATA->parked = false;
}


//
void SColumnData::add(SP<Layout::ITarget> t) {
    add(t, (int)windowDatas.size() - 1);
}

void SColumnData::add(SP<Layout::ITarget> t, int after) {
    const size_t n    = windowDatas.size();
    const float  pref = std::clamp((float)g_config.window_default_height->value(), MIN_ROW_HEIGHT, 1.0f);

    // column_scroll: heights are independent fractions of the screen height and
    // the column scrolls vertically, so a new window doesn't shrink the others.
    if (g_config.column_scroll->value()) {
        windowDatas.insert(windowDatas.begin() + after + 1, makeShared<SScrollingWindowData>(t, self.lock(), pref));
        return;
    }

    float newSize = 1.0f / (float)(n + 1);
    if (n > 0 && pref < 1.0f) {
        // Give the new window `pref`, but never squeeze the others below
        // MIN_ROW_HEIGHT: repeated multiplication by (1 - pref) used to shrink
        // the oldest windows towards zero height.
        newSize = std::max(newSize, std::min(pref, 1.0f - (float)n * MIN_ROW_HEIGHT));
    }

    const float scale        = 1.0f - newSize;
    const bool  PROPORTIONAL = std::ranges::all_of(windowDatas, [scale](const auto& wd) { return wd->windowSize * scale >= MIN_ROW_HEIGHT; });

    for (auto& wd : windowDatas)
        wd->windowSize = PROPORTIONAL ? wd->windowSize * scale : scale / (float)n;

    windowDatas.insert(windowDatas.begin() + after + 1, makeShared<SScrollingWindowData>(t, self.lock(), newSize));
}

void SColumnData::add(SP<SScrollingWindowData> w) {
    add(w, (int)windowDatas.size() - 1);
}

void SColumnData::add(SP<SScrollingWindowData> w, int after) {
    // column_scroll: a moved window keeps its own height (see add(t, after)).
    if (g_config.column_scroll->value()) {
        windowDatas.insert(windowDatas.begin() + after + 1, w);
        w->column = self;
        return;
    }

    for (auto& wd : windowDatas) {
        wd->windowSize *= (float)windowDatas.size() / (float)(windowDatas.size() + 1);
    }

    windowDatas.insert(windowDatas.begin() + after + 1, w);
    w->column     = self;
    w->windowSize = 1.F / (float)(windowDatas.size());
}

size_t SColumnData::idx(SP<Layout::ITarget> t) {
    for (size_t i = 0; i < windowDatas.size(); ++i) {
        if (windowDatas[i]->target.lock() == t)
            return i;
    }
    return SIZE_MAX;
}

size_t SColumnData::idxForHeight(float y) {
    for (size_t i = 0; i < windowDatas.size(); ++i) {
        if (windowDatas[i]->layoutBox.y < y)
            continue;
        return i > 0 ? i - 1 : 0;
    }
    return windowDatas.size() > 0 ? windowDatas.size() - 1 : 0;
}

void SColumnData::remove(SP<Layout::ITarget> t) {
    const auto SIZE_BEFORE = windowDatas.size();
    std::erase_if(windowDatas, [&t](const auto& e) { return e->target.lock() == t; });

    if (SIZE_BEFORE == windowDatas.size() && SIZE_BEFORE > 0)
        return;

    // column_scroll: remaining windows keep their heights; the column's
    // vertical offset is re-clamped on the next recalculate().
    if (g_config.column_scroll->value())
        return;

    float newMaxSize = 0.F;
    for (auto& wd : windowDatas) {
        newMaxSize += wd->windowSize;
    }

    for (auto& wd : windowDatas) {
        wd->windowSize *= 1.F / newMaxSize;
    }
}

void SColumnData::up(SP<SScrollingWindowData> w) {
    for (size_t i = 1; i < windowDatas.size(); ++i) {
        if (windowDatas[i] != w)
            continue;

        std::swap(windowDatas[i], windowDatas[i - 1]);
        break;
    }
}

void SColumnData::down(SP<SScrollingWindowData> w) {
    if (windowDatas.empty())
        return;

    for (size_t i = 0; i + 1 < windowDatas.size(); ++i) {
        if (windowDatas[i] != w)
            continue;

        std::swap(windowDatas[i], windowDatas[i + 1]);
        break;
    }
}

SP<SScrollingWindowData> SColumnData::next(SP<SScrollingWindowData> w) {
    if (windowDatas.empty())
        return nullptr;

    for (size_t i = 0; i + 1 < windowDatas.size(); ++i) {
        if (windowDatas[i] != w)
            continue;

        return windowDatas[i + 1];
    }

    return nullptr;
}

SP<SScrollingWindowData> SColumnData::prev(SP<SScrollingWindowData> w) {
    for (size_t i = 1; i < windowDatas.size(); ++i) {
        if (windowDatas[i] != w)
            continue;

        return windowDatas[i - 1];
    }

    return nullptr;
}

bool SColumnData::has(SP<Layout::ITarget> t) {
    return std::ranges::find_if(windowDatas, [t](const auto& e) { return e->target.lock() == t; }) != windowDatas.end();
}

SP<SColumnData> SScrollingLayoutData::add() {
    auto col       = columns.emplace_back(makeShared<SColumnData>(layout));
    col->self      = col;
    col->columnWidth = layout->defaultColumnWidth();
    return col;
}

SP<SColumnData> SScrollingLayoutData::add(int after) {
    auto col       = makeShared<SColumnData>(layout);
    col->self      = col;
    col->columnWidth = layout->defaultColumnWidth();
    columns.insert(columns.begin() + after + 1, col);
    return col;
}

int64_t SScrollingLayoutData::idx(SP<SColumnData> c) {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == c)
            return i;
    }

    return -1;
}

void SScrollingLayoutData::remove(SP<SColumnData> c) {
    std::erase(columns, c);
}

SP<SColumnData> SScrollingLayoutData::next(SP<SColumnData> c) {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] != c)
            continue;

        if (i == columns.size() - 1)
            return nullptr;

        return columns[i + 1];
    }

    return nullptr;
}

SP<SColumnData> SScrollingLayoutData::prev(SP<SColumnData> c) {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] != c)
            continue;

        if (i == 0)
            return nullptr;

        return columns[i - 1];
    }

    return nullptr;
}

double SScrollingLayoutData::peekWidth(double usableW) {
    return std::clamp((double)g_config.peek_width->value(), 0.0, usableW / 4.0);
}

// Scrollable column widths are fractions of the usable width minus a peek
// strip on each side, so that e.g. two 0.5 columns leave the edges of their
// neighbours visible.
static double flexWidth(const SP<SColumnData>& c, double usableW, double peek) {
    return (usableW - 2.0 * peek) * c->columnWidth;
}

double SScrollingLayoutData::fillScale(double usableW) {
    if (!g_config.stretch_to_fill->value())
        return 1.0;

    const double PEEK  = peekWidth(usableW);
    double       flex  = 0;
    double       fixed = 0;
    double       pinned = 0;
    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            pinned += COL->collapsed ? g_config.collapsed_width->value() : usableW * COL->columnWidth;
        else if (COL->collapsed)
            fixed += g_config.collapsed_width->value();
        else
            flex += flexWidth(COL, usableW, PEEK);
    }

    // Only stretch when the columns don't fill the viewport on their own.
    const double SCROLL_W = usableW - pinned;
    if (flex <= 0 || flex + fixed >= SCROLL_W)
        return 1.0;
    return (SCROLL_W - fixed) / flex;
}

double SScrollingLayoutData::colWidth(const SP<SColumnData>& c, double usableW) {
    if (c->collapsed)
        return g_config.collapsed_width->value();
    if (c->pinned != PIN_NONE)
        return usableW * c->columnWidth;
    if (g_config.fullscreen_on_one_column->value() && columns.size() == 1)
        return usableW;
    return flexWidth(c, usableW, peekWidth(usableW)) * fillScale(usableW);
}

double SScrollingLayoutData::rowHeights(const SP<SColumnData>& c, double usableH, std::vector<double>& out) {
    out.clear();

    if (!g_config.column_scroll->value()) {
        // Classic: window sizes are shares of the column and sum to 1.
        for (const auto& WD : c->windowDatas)
            out.push_back(WD->windowSize * usableH);
        return usableH;
    }

    // column_scroll: each window is a fraction of the screen height (minus a
    // peek strip above and below once there is more than one window), so the
    // column can be taller than the screen and scrolls vertically.
    const double PEEK  = c->windowDatas.size() > 1 ? std::clamp((double)g_config.peek_width->value(), 0.0, usableH / 4.0) : 0.0;
    double       total = 0;
    for (const auto& WD : c->windowDatas) {
        out.push_back(WD->windowSize * (usableH - 2.0 * PEEK));
        total += out.back();
    }

    // A column shorter than the screen is stretched to fill it.
    if (total > 0 && total < usableH) {
        for (auto& h : out)
            h *= usableH / total;
        total = usableH;
    }

    return total;
}

void SScrollingLayoutData::placeColumn(const SP<SColumnData>& c, double x, double width, const Vector2D& origin, double usableH, bool forceInstant) {
    std::vector<double> heights;
    const double        TOTAL = rowHeights(c, usableH, heights);

    c->topOffset = g_config.column_scroll->value() ? std::clamp(c->topOffset, 0.0, std::max(0.0, TOTAL - usableH)) : 0.0;

    double currentTop = -c->topOffset;
    for (size_t i = 0; i < c->windowDatas.size(); ++i) {
        const auto& WDATA = c->windowDatas[i];
        WDATA->layoutBox  = CBox{x, currentTop, width, heights[i]}.translate(origin);
        currentTop += heights[i];
        applyLayoutBox(WDATA, forceInstant);
    }
}

// Vertical counterpart of fitCol() for column_scroll: scroll the column so `w`
// is fully visible, plus a peek strip of the windows above/below it.
void SScrollingLayoutData::fitRow(const SP<SColumnData>& c, const SP<SScrollingWindowData>& w) {
    if (!g_config.column_scroll->value() || !c || !w)
        return;

    const double        USABLE_H = layout->usableArea().h;
    std::vector<double> heights;
    const double        TOTAL = rowHeights(c, USABLE_H, heights);

    const auto IT = std::ranges::find(c->windowDatas, w);
    if (IT == c->windowDatas.end())
        return;
    const size_t I = IT - c->windowDatas.begin();

    double top = 0;
    for (size_t j = 0; j < I; ++j)
        top += heights[j];
    const double H = heights[I];

    const double PEEK   = c->windowDatas.size() > 1 ? std::clamp((double)g_config.peek_width->value(), 0.0, USABLE_H / 4.0) : 0.0;
    const double PEEK_T = I > 0 ? PEEK : 0.0;
    const double PEEK_B = I + 1 < c->windowDatas.size() ? PEEK : 0.0;
    const double LO     = top + H + PEEK_B - USABLE_H;
    const double HI     = top - PEEK_T;

    double offset;
    if (LO <= HI)
        offset = std::clamp(c->topOffset, LO, HI);
    else if (H > USABLE_H)
        offset = top;
    else
        offset = (LO + HI) / 2.0;

    c->topOffset = std::clamp(offset, 0.0, std::max(0.0, TOTAL - USABLE_H));
}

double SScrollingLayoutData::scrollWidth() {
    return layout->usableArea().w - pinnedWidthLeft() - pinnedWidthRight();
}

std::optional<double> SScrollingLayoutData::scrollLeft(const SP<SColumnData>& c) {
    if (!c || c->pinned != PIN_NONE)
        return std::nullopt;

    const double USABLE_W    = layout->usableArea().w;
    double       currentLeft = 0;

    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            continue;
        if (COL == c)
            return currentLeft;
        currentLeft += colWidth(COL, USABLE_W);
    }

    return std::nullopt;
}

// Pinned columns live outside the scrollable viewport, so centring/fitting
// works in scroll space: column positions skip pinned columns, and the
// viewport is the usable width minus pinned widths (matches recalculate()).
void SScrollingLayoutData::centerCol(SP<SColumnData> c) {
    const auto LEFT = scrollLeft(c);
    if (!LEFT)
        return;

    const double ITEM_WIDTH = colWidth(c, layout->usableArea().w);
    const double target     = *LEFT - (scrollWidth() - ITEM_WIDTH) / 2.0;
    if (std::abs(target - leftOffset) < 0.5)
        return;
    leftOffset = target;
}

void SScrollingLayoutData::fitCol(SP<SColumnData> c) {
    const auto LEFT = scrollLeft(c);
    if (!LEFT)
        return;

    const double USABLE_W   = layout->usableArea().w;
    const double SCROLL_W   = scrollWidth();
    const double ITEM_WIDTH = colWidth(c, USABLE_W);
    const double MAX_W      = maxWidth();
    const double PEEK       = peekWidth(USABLE_W);

    // Keep the focused column in view, plus a peek strip of whatever lies
    // beyond it on each side, so neighbours stay visible and reachable.
    const double PEEK_L = *LEFT > 0.5 ? PEEK : 0.0;
    const double PEEK_R = *LEFT + ITEM_WIDTH < MAX_W - 0.5 ? PEEK : 0.0;
    const double LO     = *LEFT + ITEM_WIDTH + PEEK_R - SCROLL_W;
    const double HI     = *LEFT - PEEK_L;

    double clamped;
    if (LO <= HI)
        clamped = std::clamp((double)leftOffset, LO, HI);
    else if (ITEM_WIDTH > SCROLL_W)
        clamped = *LEFT; // wider than the viewport: keep its left edge visible
    else
        clamped = (LO + HI) / 2.0; // column + peeks don't fit: split the difference

    // Never leave empty space at either side when the columns are wide enough
    // to fill the viewport (e.g. after switching from center mode). Narrower
    // content is centred by recalculate() regardless of leftOffset.
    if (MAX_W >= SCROLL_W)
        clamped = std::clamp(clamped, 0.0, MAX_W - SCROLL_W);
    if (std::abs(clamped - leftOffset) < 0.5)
        return;
    leftOffset = clamped;
}

void SScrollingLayoutData::fitCol2(SP<SColumnData> c) {
    const auto LEFT = scrollLeft(c);
    if (!LEFT)
        return;

    const double SCROLL_W    = scrollWidth();
    const double ITEM_WIDTH  = colWidth(c, layout->usableArea().w);
    const double maxExtent   = maxWidth();
    const double idealOffset = *LEFT - (SCROLL_W - ITEM_WIDTH) / 2.0;

    double target;
    if (maxExtent < SCROLL_W)
        target = (maxExtent - SCROLL_W) / 2.0;
    else
        target = std::clamp(idealOffset, 0.0, maxExtent - SCROLL_W);

    if (std::abs(target - leftOffset) < 0.5)
        return;
    leftOffset = target;
}

void SScrollingLayoutData::centerOrFitCol(SP<SColumnData> c) {
    if (!c)
        return;

    const int method = layout->effectiveFitMethod();
    if (method == 1)
        fitCol(c);
    else if (method == 2)
        fitCol2(c);
    else
        centerCol(c);

    // column_scroll: also bring the column's focused (or last focused) window
    // into view vertically.
    const auto FOCUSED = layout->focusedWindowData();
    fitRow(c, FOCUSED && FOCUSED->column.lock() == c ? FOCUSED : c->lastFocusedWindow.lock());
}

SP<SColumnData> SScrollingLayoutData::atCenter() {
    double       currentLeft = 0;
    const double USABLE_W    = layout->usableArea().w;
    const double threshold   = leftOffset + scrollWidth() / 2.0 - 2;

    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            continue;

        currentLeft += colWidth(COL, USABLE_W);

        if (currentLeft >= threshold)
            return COL;
    }

    return nullptr;
}

void SScrollingLayoutData::recalculate(bool forceInstant) {

    auto parent = layout->m_parent.lock();
    if (!parent || !parent->space()) {
        return;
    }

    // Like Hyprland's built-in layouts: while a window covers the workspace
    // (fullscreen/maximized), let the fullscreen handler own geometry instead
    // of pushing column boxes onto the targets.
    if (const auto WS = parent->space()->workspace(); WS && Fullscreen::controller()->hasFullscreen(WS, true)) {
        layout->m_defaultFullscreenHandler->syncTargetSizeAndPosition();
        return;
    }

    const CBox   USABLE    = layout->usableArea();
    const auto   WORKAREA  = parent->space()->workArea();

    // Zen mode: only show the zen column (or focused column)
    if (layout->m_zenMode) {
        SP<SColumnData> zenCol = layout->m_zenColumn;
        if (!zenCol || zenCol->windowDatas.empty()) {
            layout->m_zenMode = false;
            layout->m_zenColumn = nullptr;
        } else {
            // Render only the zen column at full width
            placeColumn(zenCol, 0, USABLE.w, WORKAREA.pos(), USABLE.h, forceInstant);

            // Park all other columns off-screen at their normal size, so
            // clients don't reflow to 1x1 while hidden. Warp them there
            // rather than animating them across the screen.
            for (const auto& COL : columns) {
                if (COL == zenCol)
                    continue;
                const double ITEM_WIDTH = colWidth(COL, USABLE.w);
                for (const auto& WDATA : COL->windowDatas) {
                    WDATA->layoutBox = CBox{OFFSCREEN_COORD, OFFSCREEN_COORD, ITEM_WIDTH, WDATA->windowSize * USABLE.h};
                    auto target = WDATA->target.lock();
                    if (target) {
                        CBox box = WDATA->layoutBox;
                        box.round();
                        target->setPositionGlobal(box);
                        target->warpPositionSize();
                    }
                    WDATA->parked = true;
                }
            }
            return;
        }
    }

    // Overview: windows sit at their zoomed-out slots (see Overview.cpp);
    // re-target them so windows opening/closing meanwhile stay in place.
    if (layout->m_overview && layout->m_overview->active()) {
        layout->m_overview->apply(forceInstant);
        return;
    }

    // Single pass: pinned widths + scrollable total. colWidth() applies
    // fullscreen_on_one_column, peek_width and stretch_to_fill.
    double PINNED_L = 0, PINNED_R = 0, scrollableMaxWidth = 0;
    for (const auto& COL : columns) {
        const double W = colWidth(COL, USABLE.w);
        if (COL->pinned == PIN_LEFT)       PINNED_L += W;
        else if (COL->pinned == PIN_RIGHT) PINNED_R += W;
        else                               scrollableMaxWidth += W;
    }
    const double SCROLL_W = USABLE.w - PINNED_L - PINNED_R;

    const double cameraLeft = scrollableMaxWidth < SCROLL_W ? std::round((scrollableMaxWidth - SCROLL_W) / 2.0) : leftOffset;

    // Render pinned-left columns first
    double pinnedLeftX = 0;
    for (const auto& COL : columns) {
        if (COL->pinned != PIN_LEFT)
            continue;

        const double ITEM_WIDTH = colWidth(COL, USABLE.w);
        placeColumn(COL, pinnedLeftX, ITEM_WIDTH, WORKAREA.pos(), USABLE.h, forceInstant);
        pinnedLeftX += ITEM_WIDTH;
    }

    // Render pinned-right columns
    double pinnedRightX = USABLE.w;
    for (int64_t i = (int64_t)columns.size() - 1; i >= 0; --i) {
        const auto& COL = columns[i];
        if (COL->pinned != PIN_RIGHT)
            continue;

        const double ITEM_WIDTH = colWidth(COL, USABLE.w);
        pinnedRightX -= ITEM_WIDTH;
        placeColumn(COL, pinnedRightX, ITEM_WIDTH, WORKAREA.pos(), USABLE.h, forceInstant);
    }

    // Render scrollable (unpinned) columns
    double currentLeft = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto&  COL = columns[i];
        if (COL->pinned != PIN_NONE)
            continue;

        const double ITEM_WIDTH = colWidth(COL, USABLE.w);
        placeColumn(COL, currentLeft, ITEM_WIDTH, WORKAREA.pos() + Vector2D{PINNED_L - cameraLeft, 0.0}, USABLE.h, forceInstant);
        currentLeft += ITEM_WIDTH;
    }
}

double SScrollingLayoutData::maxWidth() {
    double       currentLeft = 0;
    const double USABLE_W    = layout->usableArea().w;

    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            continue;
        currentLeft += colWidth(COL, USABLE_W);
    }

    return currentLeft;
}

double SScrollingLayoutData::pinnedWidthLeft() {
    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    double       w            = 0;
    for (const auto& COL : columns) {
        if (COL->pinned == PIN_LEFT)
            w += COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
    }
    return w;
}

double SScrollingLayoutData::pinnedWidthRight() {
    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    double       w            = 0;
    for (const auto& COL : columns) {
        if (COL->pinned == PIN_RIGHT)
            w += COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
    }
    return w;
}

bool SScrollingLayoutData::visible(SP<SColumnData> c) {
    if (!c)
        return false;

    if (c->pinned != PIN_NONE)
        return true;

    const auto LEFT = scrollLeft(c);
    if (!LEFT)
        return false;

    return *LEFT < leftOffset + scrollWidth() && leftOffset < *LEFT + colWidth(c, layout->usableArea().w);
}

// ======================
// CScrollingLayout
// ======================

CScrollingLayout::CScrollingLayout() {
    m_scrollingData = makeShared<SScrollingLayoutData>(this);
    m_scrollingData->self = m_scrollingData;
    m_overview = makeUnique<COverview>(this);
}

CScrollingLayout::~CScrollingLayout() {
    m_configCallback.reset();
    m_focusCallback.reset();
    m_buttonCallback.reset();
    m_tickCallback.reset();
}

float CScrollingLayout::defaultColumnWidth() {
    return g_config.column_width->value();
}

CBox CScrollingLayout::usableArea() {
    auto parent = m_parent.lock();
    if (!parent || !parent->space())
        return {};

    const auto WORKAREA = parent->space()->workArea();
    auto       ws       = parent->space()->workspace();
    if (!ws)
        return {};

    auto PMONITOR = ws->m_monitor.lock();
    if (!PMONITOR)
        return {};

    // Work area is in global coords; make it relative to monitor
    CBox result = WORKAREA;
    result.translate(-PMONITOR->m_position);
    return result;
}

void CScrollingLayout::newTarget(SP<Layout::ITarget> target) {
    if (!target)
        return;

    if (!m_configCallback) {
        parseConfig();
        m_configCallback = Event::bus()->m_events.config.reloaded.listen([this]() { parseConfig(); });
    }

    if (!m_focusCallback) {
        m_focusCallback = Event::bus()->m_events.window.active.listen([this](PHLWINDOW pWindow, Desktop::eFocusReason reason) {
            if (!pWindow)
                return;

            auto parent = m_parent.lock();
            if (!parent || !parent->space())
                return;

            auto ws = parent->space()->workspace();
            if (!ws || pWindow->m_workspace != ws)
                return;

            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (!t || t->window() != pWindow)
                    continue;

                auto WDATA = dataFor(t);
                if (!WDATA)
                    break;

                // Applies to every focus reason, including clicks and
                // external dispatchers, and regardless of follow_focus.
                syncZenColumn(WDATA->column.lock());

                if (!g_config.follow_focus->value())
                    return;

                if (reason == Desktop::FOCUS_REASON_FFM) {
                    // Always check edge exclusion for hover/always-follow
                    if (const auto PMONITOR = hyprscrolling_compat::monitorFromCursor();
                        PMONITOR && pointerBlocked(PMONITOR, g_pInputManager->getMouseCoordsInternal()))
                        return;

                    if (g_config.follow_focus->value() == 2) {
                        // follow_focus=2: immediate scroll on hover, fall through to scroll logic
                    } else {
                        if (!g_config.follow_hover->value())
                            return;
                        m_hoverTimer.reset();
                        m_hoverTarget = t;
                        return;
                    }
                }
                if (reason == Desktop::FOCUS_REASON_CLICK)
                    return;

                float minVisible = g_config.follow_min_visible->value();
                if (minVisible > 0.F) {
                    auto PMONITOR = ws->m_monitor.lock();
                    if (PMONITOR) {
                        const auto MON_BOX = PMONITOR->logicalBox();
                        const auto WIN_BOX = WDATA->layoutBox;
                        double visibleW = std::max(0.0,
                            std::min(MON_BOX.x + MON_BOX.w, WIN_BOX.x + WIN_BOX.w) -
                            std::max(MON_BOX.x, WIN_BOX.x)
                        );
                        double visibleH = std::max(0.0,
                            std::min(MON_BOX.y + MON_BOX.h, WIN_BOX.y + WIN_BOX.h) -
                            std::max(MON_BOX.y, WIN_BOX.y)
                        );
                        if (visibleW >= MON_BOX.w * minVisible && visibleH >= MON_BOX.h * minVisible)
                            return;
                    }
                }

                if (m_debounceTimer.getMillis() < g_config.follow_debounce_ms->value())
                    return;

                m_scrollingData->centerOrFitCol(WDATA->column.lock());
                m_scrollingData->recalculate();
                m_debounceTimer.reset();
                break;
            }
        });
    }

    if (!m_buttonCallback) {
        m_buttonCallback = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo&) {
            if (ev.button != BTN_LEFT || ev.state != WL_POINTER_BUTTON_STATE_PRESSED)
                return;

            // The overview owns clicks while open; don't also run click-to-center.
            if (m_overview->active())
                return;

            auto parent = m_parent.lock();
            if (!parent || !parent->space())
                return;

            auto ws = parent->space()->workspace();
            if (!ws)
                return;

            Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
            auto PMONITOR = hyprscrolling_compat::monitorFromCursor();
            if (!PMONITOR)
                return;

            // Only react if this layout's workspace is the one currently shown
            // on the monitor under the cursor. Without this, a hidden
            // same-monitor workspace's instance could process the click. An
            // open special workspace is shown on top of the regular one.
            const auto SHOWN_WS = PMONITOR->m_activeSpecialWorkspace ? PMONITOR->m_activeSpecialWorkspace : PMONITOR->m_activeWorkspace;
            if (SHOWN_WS != ws)
                return;

            // Clicks on the shell (bar, panels) or in the edge margins aren't
            // meant for the window underneath.
            if (pointerBlocked(PMONITOR, cursor))
                return;

            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (!t || !t->window())
                    continue;

                auto PWIN = t->window();
                CBox box = {PWIN->positionAnimation()->value().x, PWIN->positionAnimation()->value().y, PWIN->sizeAnimation()->value().x, PWIN->sizeAnimation()->value().y};
                if (cursor.x >= box.x && cursor.x <= box.x + box.w &&
                    cursor.y >= box.y && cursor.y <= box.y + box.h) {

                    auto WDATA = dataFor(t);
                    if (!WDATA)
                        continue;

                    auto COL = WDATA->column.lock();
                    if (!COL)
                        continue;

                    m_scrollingData->centerOrFitCol(COL);
                    m_scrollingData->recalculate();
                    break;
                }
            }
        });
    }

    updateHoverTick();

    // Try to find the focused target for determining placement
    SP<Layout::ITarget> droppingOn = nullptr;
    auto                parent     = m_parent.lock();

    if (parent && parent->space()) {
        // Find the most recently focused target in this space
        for (auto& wt : parent->space()->targets()) {
            auto t = wt.lock();
            if (!t || t == target)
                continue;

            auto w = t->window();
            if (w && w == Desktop::focusState()->window()) {
                droppingOn = t;
                break;
            }
        }
    }

    SP<SScrollingWindowData> droppingData   = droppingOn ? dataFor(droppingOn) : nullptr;
    SP<SColumnData>          droppingColumn = droppingData ? droppingData->column.lock() : nullptr;

    SP<SColumnData> col;
    if (droppingColumn && !droppingColumn->collapsed && stackNewWindows()) {
        // new_window_placement = stack: open below the focused window, in its column.
        col = droppingColumn;
        col->add(target, (int)col->idx(droppingOn));
        col->lastFocusedWindow = dataFor(target);
    } else {
        const auto idx = droppingColumn ? m_scrollingData->idx(droppingColumn) : -1;
        col            = idx == -1 ? m_scrollingData->add() : m_scrollingData->add(idx);
        col->add(target);
        col->columnWidth = autoWidthForTarget(target);
    }
    m_scrollingData->centerOrFitCol(col);

    // A new window in zen mode would otherwise be parked off-screen.
    if (m_zenMode)
        m_zenColumn = col;

    m_scrollingData->recalculate();

    // Games and borderless apps often open an X11 window at screen size
    // without requesting fullscreen. Don't squeeze those into a column: make
    // them fullscreen. Hyprland settles a window's initial fullscreen state
    // during map, after this call, so apply it once mapping has finished.
    if (const auto W = target->window(); g_config.fullscreen_screen_sized->value() && W && W->m_firstMap && opensAtScreenSize(W)) {
        g_pEventLoopManager->doLater([WREF = PHLWINDOWREF{W}] {
            const auto WIN = WREF.lock();
            if (!WIN || !WIN->m_isMapped || Fullscreen::controller()->isFullscreen(WIN))
                return;
            Fullscreen::controller()->setFullscreenMode(WIN, Fullscreen::FSMODE_FULLSCREEN, Fullscreen::FSMODE_FULLSCREEN);
        });
    }
}

void CScrollingLayout::movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint) {
    if (!target)
        return;

    // Without a drop position, or before any columns exist, behave like a
    // fresh insert next to the focused window (previous behaviour).
    if (!focalPoint || m_scrollingData->columns.empty()) {
        newTarget(target);
        return;
    }

    // Make sure parsed config (column widths etc.) is available even if this
    // is the very first target to enter the layout via a drop.
    if (!m_configCallback) {
        parseConfig();
        m_configCallback = Event::bus()->m_events.config.reloaded.listen([this]() { parseConfig(); });
    }

    const Vector2D DROP = *focalPoint;

    // Find the scrollable column whose horizontal span contains the drop point.
    // layoutBox and the drop point are both in global logical coordinates.
    SP<SColumnData> dropCol;
    for (const auto& COL : m_scrollingData->columns) {
        if (COL->windowDatas.empty())
            continue;

        double left  = COL->windowDatas.front()->layoutBox.x;
        double right = left;
        for (const auto& WD : COL->windowDatas) {
            left  = std::min(left, WD->layoutBox.x);
            right = std::max(right, WD->layoutBox.x + WD->layoutBox.w);
        }

        if (DROP.x >= left && DROP.x < right) {
            dropCol = COL;
            break;
        }
    }

    // If the drop point isn't over any column, fall back to the default
    // placement. This is also the safe path if focalPoint ever arrives in a
    // different coordinate space than layoutBox: no regression vs. before.
    if (!dropCol) {
        newTarget(target);
        return;
    }

    // Insert into the resolved column at the vertical slot under the drop point.
    // add(t, after) inserts at index (after + 1), so that's where the new
    // window ends up.
    const size_t afterIdx = dropCol->idxForHeight(DROP.y);
    dropCol->add(target, (int)afterIdx);
    if (afterIdx + 1 < dropCol->windowDatas.size())
        dropCol->lastFocusedWindow = dropCol->windowDatas[afterIdx + 1];

    if (m_zenMode)
        m_zenColumn = dropCol;

    m_scrollingData->centerOrFitCol(dropCol);
    m_scrollingData->recalculate();
}

void CScrollingLayout::removeTarget(SP<Layout::ITarget> target) {
    auto DATA = dataFor(target);

    if (!DATA)
        return;

    auto COL = DATA->column.lock();
    if (!COL)
        return;

    // Determine next focus before removal so we can pre-scroll to it.
    // This ensures the focus callback's centerOrFitCol is a no-op and
    // doesn't restart the layout animation mid-flight.
    auto nextFocus = getNextCandidate(target);

    // Measure before removal: colWidth depends on the column count.
    const bool   WAS_LAST = !m_scrollingData->next(COL);
    const double COL_W    = COL->pinned == PIN_NONE ? m_scrollingData->colWidth(COL, usableArea().w) : 0.0;

    COL->remove(target);

    // Remove empty columns, and move the view back if it was the last one.
    // A column that still holds windows keeps its width, so the view stays.
    if (COL->windowDatas.empty()) {
        if (WAS_LAST)
            m_scrollingData->leftOffset -= COL_W;
        m_scrollingData->remove(COL);
    }

    // clamp before recalculate so windows don't get positioned with stale offset
    const auto   USABLE    = usableArea();
    const double SCROLL_W  = USABLE.w - m_scrollingData->pinnedWidthLeft() - m_scrollingData->pinnedWidthRight();
    m_scrollingData->leftOffset = std::clamp((double)m_scrollingData->leftOffset, 0.0,
        std::max(m_scrollingData->maxWidth() - SCROLL_W, 0.0));

    // Pre-scroll to the next focus so the focus callback computes the same
    // leftOffset and skips its recalculate — one animation instead of two.
    if (nextFocus) {
        auto NDATA = dataFor(nextFocus);
        if (NDATA) {
            if (auto ncol = NDATA->column.lock())
                m_scrollingData->centerOrFitCol(ncol);
        }
    }

    m_scrollingData->recalculate();
}

void CScrollingLayout::recalculate(Layout::eRecalculateReason) {
    m_scrollingData->recalculate();
}

void CScrollingLayout::resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner) {
    if (!target)
        return;

    const auto DATA = dataFor(target);

    if (!DATA || !DATA->column.lock())
        return;

    const auto USABLE = usableArea();
    // usableArea() is empty when the workspace has no monitor; dividing by it
    // would turn column widths into NaN.
    if (USABLE.w <= 0 || USABLE.h <= 0)
        return;

    const auto DELTA_AS_PERC = delta / USABLE.size();

    const auto CURR_COLUMN = DATA->column.lock();
    const auto NEXT_COLUMN = m_scrollingData->next(CURR_COLUMN);
    const auto PREV_COLUMN = m_scrollingData->prev(CURR_COLUMN);

    switch (corner) {
        case Layout::CORNER_BOTTOMLEFT:
        case Layout::CORNER_TOPLEFT: {
            if (!PREV_COLUMN)
                break;

            PREV_COLUMN->columnWidth = std::clamp(PREV_COLUMN->columnWidth + (float)DELTA_AS_PERC.x, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            CURR_COLUMN->columnWidth = std::clamp(CURR_COLUMN->columnWidth - (float)DELTA_AS_PERC.x, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            break;
        }
        case Layout::CORNER_BOTTOMRIGHT:
        case Layout::CORNER_TOPRIGHT: {
            if (!NEXT_COLUMN)
                break;

            NEXT_COLUMN->columnWidth = std::clamp(NEXT_COLUMN->columnWidth - (float)DELTA_AS_PERC.x, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            CURR_COLUMN->columnWidth = std::clamp(CURR_COLUMN->columnWidth + (float)DELTA_AS_PERC.x, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            break;
        }

        default: break;
    }

    Vector2D modDelta = delta;

    // column_scroll: only this window's height changes (dragging a top edge
    // up or a bottom edge down makes it taller); the column scrolls instead.
    if (g_config.column_scroll->value()) {
        const bool TOP_EDGE = corner == Layout::CORNER_TOPLEFT || corner == Layout::CORNER_TOPRIGHT;
        DATA->windowSize    = std::clamp(DATA->windowSize + (float)((TOP_EDGE ? -delta.y : delta.y) / USABLE.h), MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
        m_scrollingData->fitRow(CURR_COLUMN, DATA);
    } else if (DATA->column.lock()->windowDatas.size() > 1) {
        const auto CURR_WD = DATA;
        const auto NEXT_WD = DATA->column.lock()->next(DATA);
        const auto PREV_WD = DATA->column.lock()->prev(DATA);

        auto effectiveCorner = corner;
        if (effectiveCorner == Layout::CORNER_NONE) {
            if (!PREV_WD)
                effectiveCorner = Layout::CORNER_BOTTOMRIGHT;
            else {
                effectiveCorner = Layout::CORNER_TOPRIGHT;
                modDelta.y *= -1.0f;
            }
        }

        switch (effectiveCorner) {
            case Layout::CORNER_BOTTOMLEFT:
            case Layout::CORNER_BOTTOMRIGHT: {
                if (!NEXT_WD)
                    break;

                if (NEXT_WD->windowSize <= MIN_ROW_HEIGHT && delta.y >= 0)
                    break;

                float adjust = std::clamp((float)(delta.y / USABLE.h), (-CURR_WD->windowSize + MIN_ROW_HEIGHT), (NEXT_WD->windowSize - MIN_ROW_HEIGHT));

                NEXT_WD->windowSize = std::clamp(NEXT_WD->windowSize - adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                CURR_WD->windowSize = std::clamp(CURR_WD->windowSize + adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                break;
            }
            case Layout::CORNER_TOPLEFT:
            case Layout::CORNER_TOPRIGHT: {
                if (!PREV_WD)
                    break;

                if ((PREV_WD->windowSize <= MIN_ROW_HEIGHT && modDelta.y <= 0) || (CURR_WD->windowSize <= MIN_ROW_HEIGHT && delta.y >= 0))
                    break;

                float adjust = std::clamp((float)(modDelta.y / USABLE.h), -(PREV_WD->windowSize - MIN_ROW_HEIGHT), (CURR_WD->windowSize - MIN_ROW_HEIGHT));

                PREV_WD->windowSize = std::clamp(PREV_WD->windowSize + adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                CURR_WD->windowSize = std::clamp(CURR_WD->windowSize - adjust, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                break;
            }

            default: break;
        }
    }

    m_scrollingData->recalculate(true);
}

SP<Layout::ITarget> CScrollingLayout::getNextCandidate(SP<Layout::ITarget> old) {
    auto DATA = dataFor(old);
    if (!DATA)
        return nullptr;

    auto COL = DATA->column.lock();
    if (!COL)
        return nullptr;

    // Try next/prev in same column
    auto NEXT = COL->next(DATA);
    if (NEXT)
        return NEXT->target.lock();

    auto PREV = COL->prev(DATA);
    if (PREV)
        return PREV->target.lock();

    // Try adjacent columns
    auto NEXTCOL = m_scrollingData->next(COL);
    if (NEXTCOL && !NEXTCOL->windowDatas.empty())
        return NEXTCOL->windowDatas.front()->target.lock();

    auto PREVCOL = m_scrollingData->prev(COL);
    if (PREVCOL && !PREVCOL->windowDatas.empty())
        return PREVCOL->windowDatas.back()->target.lock();

    return nullptr;
}

void CScrollingLayout::syncZenColumn(SP<SColumnData> col) {
    if (!m_zenMode || !col || col == m_zenColumn)
        return;

    m_zenColumn = col;
    m_scrollingData->recalculate();
}

void CScrollingLayout::focusTargetUpdate(SP<Layout::ITarget> target) {
    if (!target) {
        Desktop::focusState()->fullWindowFocus(PHLWINDOW{nullptr}, Desktop::FOCUS_REASON_OTHER);
        return;
    }

    auto w = target->window();
    if (!w) {
        Desktop::focusState()->fullWindowFocus(PHLWINDOW{nullptr}, Desktop::FOCUS_REASON_OTHER);
        return;
    }

    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_OTHER);

    const auto WDATA = dataFor(target);
    if (WDATA) {
        if (auto col = WDATA->column.lock()) {
            col->lastFocusedWindow = WDATA;
            syncZenColumn(col);
            if (g_config.column_scroll->value()) {
                m_scrollingData->fitRow(col, WDATA);
                m_scrollingData->recalculate();
            }
            if (g_config.center_active_column->value()) {
                m_scrollingData->centerOrFitCol(col);
                m_scrollingData->recalculate();
            }
        }
    }

    pushFocusHistory(target);
}

SP<SScrollingWindowData> CScrollingLayout::findBestNeighbor(SP<SScrollingWindowData> pCurrent, SP<SColumnData> pTargetCol) {
    if (!pCurrent || !pTargetCol || pTargetCol->windowDatas.empty())
        return nullptr;

    const double                          currentTop    = pCurrent->layoutBox.y;
    const double                          currentBottom = pCurrent->layoutBox.y + pCurrent->layoutBox.h;
    std::vector<SP<SScrollingWindowData>> overlappingWindows;
    for (const auto& candidate : pTargetCol->windowDatas) {
        const double candidateTop    = candidate->layoutBox.y;
        const double candidateBottom = candidate->layoutBox.y + candidate->layoutBox.h;
        const bool   overlaps        = (candidateTop < currentBottom) && (candidateBottom > currentTop);

        if (overlaps)
            overlappingWindows.emplace_back(candidate);
    }
    if (!overlappingWindows.empty()) {
        auto lastFocused = pTargetCol->lastFocusedWindow.lock();

        if (lastFocused) {
            auto it = std::ranges::find(overlappingWindows, lastFocused);
            if (it != overlappingWindows.end())
                return lastFocused;
        }

        auto topmost = std::ranges::min_element(overlappingWindows, std::less<>{}, [](const SP<SScrollingWindowData>& w) { return w->layoutBox.y; });
        return *topmost;
    }
    if (!pTargetCol->windowDatas.empty())
        return pTargetCol->windowDatas.front();
    return nullptr;
}

Config::ErrorResult CScrollingLayout::layoutMsg(const std::string_view& sv) {
    const std::string message{sv};

    auto centerOrFit = [this](const SP<SScrollingLayoutData> WS, const SP<SColumnData> COL) -> void {
        WS->centerOrFitCol(COL);
    };

    const auto ARGS = CVarList(message, 0, ' ');
    if (ARGS[0] == "move") {
        if (ARGS[1] == "+col" || ARGS[1] == "col") {
            auto WDATA = focusedWindowData();
            if (!WDATA)
                return {};

            const auto COL = m_scrollingData->next(WDATA->column.lock());
            if (!COL) {
                m_scrollingData->leftOffset = m_scrollingData->maxWidth();
                m_scrollingData->recalculate();
                focusTargetUpdate(nullptr);
                return {};
            }

            centerOrFit(m_scrollingData, COL);
            m_scrollingData->recalculate();

            auto target = COL->windowDatas.front()->target.lock();
            focusTargetUpdate(target);
            if (target && target->window())
                Pointer::mgr()->warpTo(target->window()->middle());

            return {};
        } else if (ARGS[1] == "-col") {
            auto WDATA = focusedWindowData();

            if (!WDATA) {
                if (m_scrollingData->columns.size() > 0) {
                    m_scrollingData->centerOrFitCol(m_scrollingData->columns.back());
                    m_scrollingData->recalculate();
                    auto target = m_scrollingData->columns.back()->windowDatas.back()->target.lock();
                    focusTargetUpdate(target);
                    if (target && target->window())
                        Pointer::mgr()->warpTo(target->window()->middle());
                }

                return {};
            }

            const auto COL = m_scrollingData->prev(WDATA->column.lock());
            if (!COL)
                return {};

            centerOrFit(m_scrollingData, COL);
            m_scrollingData->recalculate();

            auto target = COL->windowDatas.back()->target.lock();
            focusTargetUpdate(target);
            if (target && target->window())
                Pointer::mgr()->warpTo(target->window()->middle());

            return {};
        }

        const auto PLUSMINUS = getPlusMinusKeywordResult(ARGS[1], 0);

        if (!PLUSMINUS.has_value())
            return {};

        m_scrollingData->leftOffset -= *PLUSMINUS;
        m_scrollingData->recalculate();

        const auto ATCENTER = m_scrollingData->atCenter();

        focusTargetUpdate(ATCENTER ? (*ATCENTER->windowDatas.begin())->target.lock() : nullptr);
    } else if (ARGS[0] == "colresize") {
        auto WDATA = focusedWindowData();

        if (!WDATA)
            return {};

        if (ARGS[1] == "all") {
            float abs = 0;
            try {
                abs = std::stof(ARGS[2]);
            } catch (...) { return {}; }

            abs = std::clamp(abs, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            for (const auto& c : m_scrollingData->columns) {
                c->columnWidth = abs;
            }

            m_scrollingData->recalculate();
            return {};
        }

        auto COL = WDATA->column.lock();
        CScopeGuard x([this, COL] {
            COL->columnWidth = std::clamp(COL->columnWidth, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
            m_scrollingData->centerOrFitCol(COL);
            m_scrollingData->recalculate();
        });

        if (ARGS[1][0] == '+' || ARGS[1][0] == '-') {
            if (ARGS[1] == "+conf") {
                for (size_t i = 0; i < m_config.configuredWidths.size(); ++i) {
                    if (m_config.configuredWidths[i] > COL->columnWidth) {
                        COL->columnWidth = m_config.configuredWidths[i];
                        break;
                    }

                    if (i == m_config.configuredWidths.size() - 1)
                        COL->columnWidth = m_config.configuredWidths[0];
                }

                return {};
            } else if (ARGS[1] == "-conf") {
                for (size_t i = m_config.configuredWidths.size() - 1;; --i) {
                    if (m_config.configuredWidths[i] < COL->columnWidth) {
                        COL->columnWidth = m_config.configuredWidths[i];
                        break;
                    }

                    if (i == 0) {
                        COL->columnWidth = m_config.configuredWidths.back();
                        break;
                    }
                }

                return {};
            }

            const auto PLUSMINUS = getPlusMinusKeywordResult(ARGS[1], 0);

            if (!PLUSMINUS.has_value())
                return {};

            COL->columnWidth += *PLUSMINUS;
        } else {
            float abs = 0;
            try {
                abs = std::stof(ARGS[1]);
            } catch (...) { return {}; }

            COL->columnWidth = abs;
        }
    } else if (ARGS[0] == "rowresize") {
        if (ARGS[1].empty())
            return {};

        auto WDATA = focusedWindowData();

        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        if (COL->windowDatas.size() <= 1)
            return {};

        // Resize the window to targetSize, scaling all other windows in the column proportionally.
        auto applyRowSize = [&](float targetSize) {
            targetSize = std::clamp(targetSize, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
            // column_scroll: heights are independent, the column just gets taller/shorter.
            if (!g_config.column_scroll->value() && COL->windowDatas.size() > 1) {
                const float currentOthers = 1.0f - WDATA->windowSize;
                const float newOthers     = 1.0f - targetSize;
                if (currentOthers > 0.0f) {
                    const float scale = newOthers / currentOthers;
                    for (auto& wd : COL->windowDatas) {
                        if (wd != WDATA)
                            wd->windowSize = std::clamp(wd->windowSize * scale, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                    }
                } else {
                    const float equalShare = newOthers / (float)(COL->windowDatas.size() - 1);
                    for (auto& wd : COL->windowDatas) {
                        if (wd != WDATA)
                            wd->windowSize = std::clamp(equalShare, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                    }
                }
            }
            WDATA->windowSize = targetSize;
            m_scrollingData->fitRow(COL, WDATA);
            m_scrollingData->recalculate(true);
        };

        if (ARGS[1] == "+conf") {
            for (size_t i = 0; i < m_config.configuredHeights.size(); ++i) {
                if (m_config.configuredHeights[i] > WDATA->windowSize) {
                    applyRowSize(m_config.configuredHeights[i]);
                    return {};
                }
                if (i == m_config.configuredHeights.size() - 1)
                    applyRowSize(m_config.configuredHeights[0]);
            }
        } else if (ARGS[1] == "-conf") {
            for (size_t i = m_config.configuredHeights.size() - 1;; --i) {
                if (m_config.configuredHeights[i] < WDATA->windowSize) {
                    applyRowSize(m_config.configuredHeights[i]);
                    break;
                }
                if (i == 0) {
                    applyRowSize(m_config.configuredHeights.back());
                    break;
                }
            }
        } else if (ARGS[1][0] == '+' || ARGS[1][0] == '-') {
            const auto PLUSMINUS = getPlusMinusKeywordResult(ARGS[1], 0);
            if (!PLUSMINUS.has_value())
                return {};
            applyRowSize(WDATA->windowSize + (float)*PLUSMINUS);
        } else {
            float abs = 0;
            try {
                abs = std::stof(ARGS[1]);
            } catch (...) { return {}; }
            applyRowSize(abs);
        }
    } else if (ARGS[0] == "movewindowto") {
        if (ARGS[1].empty())
            return {};

        auto WDATA = focusedWindowData();
        if (WDATA) {
            if (auto t = WDATA->target.lock())
                moveTargetTo(t, Math::fromChar(ARGS[1][0]), false);
        }
    } else if (ARGS[0] == "fit") {

        auto WDATA = focusedWindowData();

        // Helper: scroll offset that puts columns[count] at the left edge, minus
        // a peek strip when there are columns before it. Call after widths change.
        const auto scrollableOffsetBefore = [&](size_t count) -> double {
            const double USABLE_W = usableArea().w;
            double       off      = 0;
            for (size_t j = 0; j < count; ++j) {
                const auto& col = m_scrollingData->columns[j];
                if (col->pinned != PIN_NONE)
                    continue;
                off += m_scrollingData->colWidth(col, USABLE_W);
            }
            return off > 0 ? off - m_scrollingData->peekWidth(USABLE_W) : off;
        };

        if (ARGS[1] == "active") {
            if (!WDATA || m_scrollingData->columns.size() == 0)
                return {};

            WDATA->column.lock()->columnWidth = 1.F;

            size_t activeIdx = 0;
            auto   target    = WDATA->target.lock();
            for (size_t i = 0; i < m_scrollingData->columns.size(); ++i) {
                if (m_scrollingData->columns[i]->has(target)) {
                    activeIdx = i;
                    break;
                }
            }
            m_scrollingData->leftOffset = scrollableOffsetBefore(activeIdx);
            m_scrollingData->recalculate();
        } else if (ARGS[1] == "all") {
            if (m_scrollingData->columns.size() == 0)
                return {};

            size_t LEN = 0;
            for (const auto& c : m_scrollingData->columns) {
                if (c->pinned == PIN_NONE)
                    LEN++;
            }
            if (LEN == 0)
                return {};

            for (const auto& c : m_scrollingData->columns) {
                if (c->pinned == PIN_NONE)
                    c->columnWidth = 1.F / (float)LEN;
            }

            m_scrollingData->recalculate();
        } else if (ARGS[1] == "toend") {
            if (m_scrollingData->columns.size() == 0)
                return {};

            auto target = WDATA ? WDATA->target.lock() : nullptr;
            if (!target)
                return {};

            bool   begun   = false;
            size_t foundAt = 0;
            for (size_t i = 0; i < m_scrollingData->columns.size(); ++i) {
                if (!begun && !m_scrollingData->columns[i]->has(target))
                    continue;

                if (!begun) {
                    begun   = true;
                    foundAt = i;
                }

                if (m_scrollingData->columns[i]->pinned == PIN_NONE) {
                    size_t remaining = 0;
                    for (size_t j = i; j < m_scrollingData->columns.size(); ++j) {
                        if (m_scrollingData->columns[j]->pinned == PIN_NONE)
                            remaining++;
                    }
                    m_scrollingData->columns[i]->columnWidth = remaining > 0 ? 1.F / (float)remaining : 1.F;
                }
            }

            if (!begun)
                return {};

            m_scrollingData->leftOffset = scrollableOffsetBefore(foundAt);
            m_scrollingData->recalculate();
        } else if (ARGS[1] == "tobeg") {
            if (m_scrollingData->columns.size() == 0)
                return {};

            auto target = WDATA ? WDATA->target.lock() : nullptr;
            if (!target)
                return {};

            bool   begun   = false;
            size_t foundAt = 0;
            for (int64_t i = (int64_t)m_scrollingData->columns.size() - 1; i >= 0; --i) {
                if (!begun && !m_scrollingData->columns[i]->has(target))
                    continue;

                if (!begun) {
                    begun   = true;
                    foundAt = i;
                }

                if (m_scrollingData->columns[i]->pinned == PIN_NONE) {
                    size_t scrollableInRange = 0;
                    for (size_t j = 0; j <= (size_t)foundAt; ++j) {
                        if (m_scrollingData->columns[j]->pinned == PIN_NONE)
                            scrollableInRange++;
                    }
                    m_scrollingData->columns[i]->columnWidth = scrollableInRange > 0 ? 1.F / (float)scrollableInRange : 1.F;
                }
            }

            if (!begun)
                return {};

            m_scrollingData->leftOffset = 0;
            m_scrollingData->recalculate();
        } else if (ARGS[1] == "visible") {
            if (m_scrollingData->columns.size() == 0)
                return {};

            bool                         begun   = false;
            size_t                       foundAt = 0;
            std::vector<SP<SColumnData>> visible;
            for (size_t i = 0; i < m_scrollingData->columns.size(); ++i) {
                if (!begun && !m_scrollingData->visible(m_scrollingData->columns[i]))
                    continue;

                if (!begun) {
                    begun   = true;
                    foundAt = i;
                }

                if (!m_scrollingData->visible(m_scrollingData->columns[i]))
                    break;

                visible.emplace_back(m_scrollingData->columns[i]);
            }

            if (!begun)
                return {};

            m_scrollingData->leftOffset = scrollableOffsetBefore(foundAt);

            for (const auto& v : visible) {
                v->columnWidth = 1.F / (float)visible.size();
            }

            m_scrollingData->recalculate();
        }
    } else if (ARGS[0] == "focus") {
        auto WDATA = focusedWindowData();

        if (!WDATA || ARGS[1].empty())
            return {};

        // "focus <dir> nowrap" stops at the edges regardless of focus_wrap
        // (wrapping is jarring on a scroll wheel).
        const bool WRAP = g_config.focus_wrap->value() && ARGS[2] != "nowrap";

        switch (ARGS[1][0]) {
            case 'u':
            case 't': {
                auto PREV = WDATA->column.lock()->prev(WDATA);
                if (!PREV) {
                    if (!WRAP)
                        break;
                    PREV = WDATA->column.lock()->windowDatas.back();
                }

                focusTargetUpdate(PREV->target.lock());
                auto w = PREV->target.lock() ? PREV->target.lock()->window() : nullptr;
                if (w)
                    Pointer::mgr()->warpTo(w->middle());
                break;
            }

            case 'b':
            case 'd': {
                auto NEXT = WDATA->column.lock()->next(WDATA);
                if (!NEXT) {
                    if (!WRAP)
                        break;
                    NEXT = WDATA->column.lock()->windowDatas.front();
                }

                focusTargetUpdate(NEXT->target.lock());
                auto w = NEXT->target.lock() ? NEXT->target.lock()->window() : nullptr;
                if (w)
                    Pointer::mgr()->warpTo(w->middle());
                break;
            }

            case 'l': {
                auto COL  = WDATA->column.lock();
                auto PREV = m_scrollingData->prev(COL);
                if (!PREV) {
                    if (!WRAP) {
                        centerOrFit(m_scrollingData, COL);
                        m_scrollingData->recalculate();
                        auto w = WDATA->target.lock() ? WDATA->target.lock()->window() : nullptr;
                        if (w)
                            Pointer::mgr()->warpTo(w->middle());
                        break;
                    }
                    PREV = m_scrollingData->columns.back();
                }

                auto pTargetWindowData = findBestNeighbor(WDATA, PREV);
                if (pTargetWindowData) {
                    focusTargetUpdate(pTargetWindowData->target.lock());
                    centerOrFit(m_scrollingData, PREV);
                    m_scrollingData->recalculate();
                    auto w = pTargetWindowData->target.lock() ? pTargetWindowData->target.lock()->window() : nullptr;
                    if (w)
                        Pointer::mgr()->warpTo(w->middle());
                }
                break;
            }

            case 'r': {
                auto COL  = WDATA->column.lock();
                auto NEXT = m_scrollingData->next(COL);
                if (!NEXT) {
                    if (!WRAP) {
                        centerOrFit(m_scrollingData, COL);
                        m_scrollingData->recalculate();
                        auto w = WDATA->target.lock() ? WDATA->target.lock()->window() : nullptr;
                        if (w)
                            Pointer::mgr()->warpTo(w->middle());
                        break;
                    }
                    NEXT = m_scrollingData->columns.front();
                }

                auto pTargetWindowData = findBestNeighbor(WDATA, NEXT);
                if (pTargetWindowData) {
                    focusTargetUpdate(pTargetWindowData->target.lock());
                    centerOrFit(m_scrollingData, NEXT);
                    m_scrollingData->recalculate();
                    auto w = pTargetWindowData->target.lock() ? pTargetWindowData->target.lock()->window() : nullptr;
                    if (w)
                        Pointer::mgr()->warpTo(w->middle());
                }
                break;
            }

            default: return {};
        }
    } else if (ARGS[0] == "promote") {
        auto WDATA = focusedWindowData();

        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        auto idx = m_scrollingData->idx(COL);
        auto col = idx == -1 ? m_scrollingData->add() : m_scrollingData->add(idx);

        COL->remove(WDATA->target.lock());

        // Remove empty column
        if (COL->windowDatas.empty())
            m_scrollingData->remove(COL);

        col->add(WDATA);
        col->columnWidth = autoWidthForTarget(WDATA->target.lock());

        m_scrollingData->recalculate();
    } else if (ARGS[0] == "swapcol") {
        if (ARGS.size() < 2)
            return {};

        auto WDATA = focusedWindowData();

        if (!WDATA)
            return {};

        const auto CURRENT_COL = WDATA->column.lock();
        if (!CURRENT_COL || m_scrollingData->columns.size() < 2)
            return {};

        const int64_t current_idx = m_scrollingData->idx(CURRENT_COL);
        const size_t  col_count   = m_scrollingData->columns.size();

        if (current_idx == -1)
            return {};

        const std::string& direction  = ARGS[1];
        int64_t            target_idx = -1;

        if (direction == "l")
            target_idx = (current_idx == 0) ? (col_count - 1) : (current_idx - 1);
        else if (direction == "r")
            target_idx = (current_idx == (int64_t)col_count - 1) ? 0 : (current_idx + 1);
        else
            return {};

        std::swap(m_scrollingData->columns[current_idx], m_scrollingData->columns[target_idx]);
        m_scrollingData->centerOrFitCol(CURRENT_COL);
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "placement") {
        // placement column|stack|toggle: where new windows open, for all workspaces.
        if (ARGS[1] == "column")
            g_placementOverride = 0;
        else if (ARGS[1] == "stack")
            g_placementOverride = 1;
        else if (ARGS[1] == "toggle")
            g_placementOverride = stackNewWindows() ? 0 : 1;
        else
            return {};

        HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprscrolling] New windows: "} + (stackNewWindows() ? "stack in column" : "new column"),
                                     CHyprColor{0.4, 0.6, 1.0, 1.0}, 1500);
    } else if (ARGS[0] == "togglefit") {
        m_fitMethodOverride = (effectiveFitMethod() + 1) % 3;

        if (m_scrollingData->columns.empty())
            return {};

        auto focusedData = focusedWindowData();

        const auto columnToUse = (focusedData && focusedData->column.lock()) ? focusedData->column.lock() : m_scrollingData->atCenter();
        if (!columnToUse)
            return {};

        m_scrollingData->centerOrFitCol(columnToUse);
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "pin") {
        // pin left / pin right — pin the focused column to a screen edge
        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        if (ARGS.size() < 2 || ARGS[1].empty())
            return {};

        if (ARGS[1] == "left")
            COL->pinned = PIN_LEFT;
        else if (ARGS[1] == "right")
            COL->pinned = PIN_RIGHT;
        else
            return {};

        m_scrollingData->recalculate();
    } else if (ARGS[0] == "unpin") {
        // unpin — unpin the focused column
        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        COL->pinned = PIN_NONE;
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "movecoltoworkspace") {
        // movecoltoworkspace <workspace> — move all windows in current column to another workspace
        if (ARGS.size() < 2 || ARGS[1].empty())
            return {};

        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        moveColToWorkspace(COL, ARGS[1]);
    } else if (ARGS[0] == "collapse") {
        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL || COL->collapsed)
            return {};

        COL->savedColumnWidth = COL->columnWidth;
        COL->collapsed = true;
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "expand") {
        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL || !COL->collapsed)
            return {};

        COL->collapsed = false;
        if (COL->savedColumnWidth > 0)
            COL->columnWidth = COL->savedColumnWidth;
        COL->savedColumnWidth = 0;
        m_scrollingData->centerOrFitCol(COL);
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "togglecollapse") {
        auto WDATA = focusedWindowData();
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        if (COL->collapsed) {
            COL->collapsed = false;
            if (COL->savedColumnWidth > 0)
                COL->columnWidth = COL->savedColumnWidth;
            COL->savedColumnWidth = 0;
            m_scrollingData->centerOrFitCol(COL);
        } else {
            COL->savedColumnWidth = COL->columnWidth;
            COL->collapsed = true;
        }
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "zen") {
        if (m_zenMode) {
            // Exit zen mode
            m_zenMode = false;
            m_zenColumn = nullptr;
            m_scrollingData->recalculate();
        } else {
            // Enter zen mode with focused column
            auto WDATA = focusedWindowData();
            if (!WDATA)
                return {};

            auto COL = WDATA->column.lock();
            if (!COL)
                return {};

            m_zenMode = true;
            m_zenColumn = COL;
            m_scrollingData->recalculate();
        }
    } else if (ARGS[0] == "focusback") {
        if (!g_config.focus_history->value() || m_focusHistory.empty())
            return {};

        if (m_focusHistoryIdx < 0)
            m_focusHistoryIdx = (int64_t)m_focusHistory.size() - 1;

        m_focusHistoryNavigating = true;
        while (m_focusHistoryIdx > 0) {
            m_focusHistoryIdx--;
            auto target = m_focusHistory[m_focusHistoryIdx].lock();
            if (!target)
                continue;
            auto WDATA = dataFor(target);
            if (!WDATA)
                continue;

            m_scrollingData->centerOrFitCol(WDATA->column.lock());
            m_scrollingData->recalculate();
            Desktop::focusState()->fullWindowFocus(target->window(), Desktop::FOCUS_REASON_OTHER);
            if (target->window())
                Pointer::mgr()->warpTo(target->window()->middle());
            break;
        }
        m_focusHistoryNavigating = false;
    } else if (ARGS[0] == "focusfwd") {
        if (!g_config.focus_history->value() || m_focusHistory.empty() || m_focusHistoryIdx < 0)
            return {};

        m_focusHistoryNavigating = true;
        while (m_focusHistoryIdx < (int64_t)m_focusHistory.size() - 1) {
            m_focusHistoryIdx++;
            auto target = m_focusHistory[m_focusHistoryIdx].lock();
            if (!target)
                continue;
            auto WDATA = dataFor(target);
            if (!WDATA)
                continue;

            m_scrollingData->centerOrFitCol(WDATA->column.lock());
            m_scrollingData->recalculate();
            Desktop::focusState()->fullWindowFocus(target->window(), Desktop::FOCUS_REASON_OTHER);
            if (target->window())
                Pointer::mgr()->warpTo(target->window()->middle());
            break;
        }
        m_focusHistoryNavigating = false;
    } else if (ARGS[0] == "overview_confirm") {
        m_overview->confirm();
    } else if (ARGS[0] == "overview") {
        m_overview->toggle();
    }
    return {};
}

std::optional<Vector2D> CScrollingLayout::predictSizeForNewTarget() {
    return std::nullopt;
}

void CScrollingLayout::swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) {
    auto DATA1 = dataFor(a);
    auto DATA2 = dataFor(b);

    if (!DATA1 || !DATA2)
        return;

    std::swap(DATA1->target, DATA2->target);

    m_scrollingData->recalculate();
}

void CScrollingLayout::moveTargetInDirection(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) {
    moveTargetTo(t, dir, silent);
}

void CScrollingLayout::moveTargetTo(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) {
    const auto DATA = dataFor(t);

    if (!DATA)
        return;

    auto COL = DATA->column.lock();
    if (!COL)
        return;

    // Pick the slot in the destination column from the window's own position,
    // not the cursor: this path is keyboard-driven (drags go via movedTarget).
    const double MID_Y    = DATA->layoutBox.y + DATA->layoutBox.h / 2.0;
    const bool   WAS_ONLY = COL->windowDatas.size() == 1;

    // A window that gets a column of its own keeps its old column's settings
    // if it was alone there; a window split off a stack gets its auto width.
    const auto inheritColumn = [&](const SP<SColumnData>& NEWCOL) {
        if (WAS_ONLY) {
            NEWCOL->columnWidth      = COL->columnWidth;
            NEWCOL->savedColumnWidth = COL->savedColumnWidth;
            NEWCOL->collapsed        = COL->collapsed;
            NEWCOL->pinned           = COL->pinned;
        } else
            NEWCOL->columnWidth = autoWidthForTarget(t);
    };

    if (dir == Math::DIRECTION_LEFT) {
        const auto PREVCOL = m_scrollingData->prev(COL);

        COL->remove(t);
        if (COL->windowDatas.empty())
            m_scrollingData->remove(COL);

        if (!PREVCOL) {
            const auto NEWCOL = m_scrollingData->add(-1);
            NEWCOL->add(DATA);
            inheritColumn(NEWCOL);
            m_scrollingData->centerOrFitCol(NEWCOL);
        } else {
            if (PREVCOL->windowDatas.size() > 0)
                PREVCOL->add(DATA, PREVCOL->idxForHeight(MID_Y));
            else
                PREVCOL->add(DATA);
            m_scrollingData->centerOrFitCol(PREVCOL);
        }
    } else if (dir == Math::DIRECTION_RIGHT) {
        const auto NEXTCOL = m_scrollingData->next(COL);

        COL->remove(t);
        if (COL->windowDatas.empty())
            m_scrollingData->remove(COL);

        if (!NEXTCOL) {
            const auto NEWCOL = m_scrollingData->add();
            NEWCOL->add(DATA);
            inheritColumn(NEWCOL);
            m_scrollingData->centerOrFitCol(NEWCOL);
        } else {
            if (NEXTCOL->windowDatas.size() > 0)
                NEXTCOL->add(DATA, NEXTCOL->idxForHeight(MID_Y));
            else
                NEXTCOL->add(DATA);
            m_scrollingData->centerOrFitCol(NEXTCOL);
        }

    } else if (dir == Math::DIRECTION_UP)
        COL->up(DATA);
    else if (dir == Math::DIRECTION_DOWN)
        COL->down(DATA);

    m_scrollingData->recalculate();

    // Honor the silent flag: a silent move must not steal focus or warp the
    // cursor (e.g. programmatic/rearrange moves).
    if (!silent) {
        focusTargetUpdate(t);
        auto w = t->window();
        if (w)
            Pointer::mgr()->warpTo(w->middle());
    }
}

SP<SScrollingWindowData> CScrollingLayout::dataFor(SP<Layout::ITarget> t) {
    if (!t)
        return nullptr;

    for (const auto& c : m_scrollingData->columns) {
        for (const auto& d : c->windowDatas) {
            if (d->target.lock() == t)
                return d;
        }
    }

    return nullptr;
}

SP<SScrollingWindowData> CScrollingLayout::focusedWindowData() {
    auto focusedWindow = Desktop::focusState()->window();
    if (!focusedWindow)
        return nullptr;

    auto parent = m_parent.lock();
    if (!parent || !parent->space())
        return nullptr;

    for (const auto& c : m_scrollingData->columns) {
        for (const auto& d : c->windowDatas) {
            auto t = d->target.lock();
            if (t && t->window() == focusedWindow)
                return d;
        }
    }

    return nullptr;
}

void CScrollingLayout::moveColToWorkspace(SP<SColumnData> col, const std::string& wsStr) {
    if (!col || col->windowDatas.empty())
        return;

    // Resolve workspace
    const auto WSIDNAME = getWorkspaceIDNameFromString(wsStr);
    if (WSIDNAME.id == WORKSPACE_INVALID)
        return;

    auto parent = m_parent.lock();
    if (!parent || !parent->space())
        return;

    auto CURRENTWS = parent->space()->workspace();
    if (!CURRENTWS)
        return;

    // Get or create target workspace
    auto TARGETWS = hyprscrolling_compat::workspaceByID(WSIDNAME.id);
    if (!TARGETWS) {
        TARGETWS = State::workspaceState()->create(WSIDNAME.id, CURRENTWS->m_monitor.lock() ? CURRENTWS->m_monitor.lock()->m_id : 0, WSIDNAME.name);
    }

    if (!TARGETWS || TARGETWS == CURRENTWS)
        return;

    // Collect all windows from the column
    std::vector<PHLWINDOW> windowsToMove;
    for (const auto& wd : col->windowDatas) {
        auto target = wd->target.lock();
        if (target && target->window())
            windowsToMove.push_back(target->window());
    }

    // Move each window to the target workspace
    for (const auto& w : windowsToMove) {
        Desktop::globalWindowController()->moveWindowToWorkspace(w, TARGETWS);
    }
}

void CScrollingLayout::pushFocusHistory(SP<Layout::ITarget> target) {
    if (!g_config.focus_history->value() || !target || m_focusHistoryNavigating)
        return;

    // If we're not at the end of history, truncate forward history
    if (m_focusHistoryIdx >= 0 && m_focusHistoryIdx < (int64_t)m_focusHistory.size() - 1) {
        m_focusHistory.erase(m_focusHistory.begin() + m_focusHistoryIdx + 1, m_focusHistory.end());
    }

    // Don't push duplicates
    if (!m_focusHistory.empty()) {
        auto last = m_focusHistory.back().lock();
        if (last == target)
            return;
    }

    m_focusHistory.push_back(target);

    // Limit history size
    constexpr size_t MAX_HISTORY = 50;
    while (m_focusHistory.size() > MAX_HISTORY) {
        m_focusHistory.pop_front();
    }

    m_focusHistoryIdx = (int64_t)m_focusHistory.size() - 1;
}

float CScrollingLayout::autoWidthForTarget(SP<Layout::ITarget> target) {
    if (!target || m_config.autoWidthRules.empty())
        return defaultColumnWidth();

    auto w = target->window();
    if (!w)
        return defaultColumnWidth();

    const std::string cls = w->m_class;
    auto it = m_config.autoWidthRules.find(cls);
    if (it != m_config.autoWidthRules.end())
        return it->second;

    return defaultColumnWidth();
}

bool CScrollingLayout::stackNewWindows() const {
    if (g_placementOverride >= 0)
        return g_placementOverride == 1;
    return g_config.new_window_placement->value() == "stack";
}

int CScrollingLayout::effectiveFitMethod() const {
    return m_fitMethodOverride >= 0 ? m_fitMethodOverride : (int)g_config.focus_fit_method->value();
}

void CScrollingLayout::updateHoverTick() {
    if (!g_config.follow_hover->value()) {
        // Feature disabled: don't keep a global tick listener around.
        m_tickCallback.reset();
        m_hoverTarget.reset();
        return;
    }

    if (m_tickCallback)
        return;

    m_tickCallback = Event::bus()->m_events.tick.listen([this]() {
        auto target = m_hoverTarget.lock();
        if (!target || m_hoverTimer.getMillis() < (double)g_config.hover_delay_ms->value())
            return;

        // verify cursor is still over this window and not over the shell / edge margins
        Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
        auto PMONITOR = hyprscrolling_compat::monitorFromCursor();
        if (!PMONITOR || pointerBlocked(PMONITOR, cursor))
            return;

        auto PWIN = target->window();
        if (!PWIN)
            return;
        CBox box = {PWIN->positionAnimation()->value().x, PWIN->positionAnimation()->value().y, PWIN->sizeAnimation()->value().x, PWIN->sizeAnimation()->value().y};
        if (cursor.x < box.x || cursor.x > box.x + box.w || cursor.y < box.y || cursor.y > box.y + box.h)
            return;

        auto WDATA = dataFor(target);
        if (!WDATA)
            return;
        auto COL = WDATA->column.lock();
        if (!COL)
            return;

        m_scrollingData->centerOrFitCol(COL);
        m_scrollingData->recalculate();

        m_hoverTimer.reset();
        m_hoverTarget.reset();
    });
}

void CScrollingLayout::parseConfig() {
    // Parse column widths from explicit_column_widths (fall back to column_widths alias).
    // explicit_column_widths defaults to empty so that the alias can take effect.
    std::string widthsStr = g_config.explicit_column_widths->value();
    if (widthsStr.empty())
        widthsStr = g_config.column_widths->value();

    m_config.configuredWidths.clear();
    if (!widthsStr.empty()) {
        const auto WIDTHS = CVarList(widthsStr, 0, ',');
        for (size_t i = 0; i < WIDTHS.size(); ++i) {
            try {
                float w = std::stof(std::string(WIDTHS[i]));
                w = std::clamp(w, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
                m_config.configuredWidths.push_back(w);
            } catch (...) {}
        }
    }
    if (m_config.configuredWidths.empty())
        m_config.configuredWidths = {0.333f, 0.5f, 0.667f, 1.0f};
    std::sort(m_config.configuredWidths.begin(), m_config.configuredWidths.end());

    // Parse window_heights
    const std::string heightsStr = g_config.window_heights->value();
    m_config.configuredHeights.clear();
    if (!heightsStr.empty()) {
        const auto HEIGHTS = CVarList(heightsStr, 0, ',');
        for (size_t i = 0; i < HEIGHTS.size(); ++i) {
            try {
                float h = std::stof(std::string(HEIGHTS[i]));
                h = std::clamp(h, MIN_ROW_HEIGHT, MAX_ROW_HEIGHT);
                m_config.configuredHeights.push_back(h);
            } catch (...) {}
        }
    }
    if (m_config.configuredHeights.empty())
        m_config.configuredHeights = {0.333f, 0.5f, 0.667f, 1.0f};
    std::sort(m_config.configuredHeights.begin(), m_config.configuredHeights.end());

    // Parse auto_width_rules: "class:fraction, class2:fraction2, ..."
    m_config.autoWidthRules.clear();
    const std::string rulesStr = g_config.auto_width_rules->value();
    if (!rulesStr.empty()) {
        const auto trim = [](const std::string& s) -> std::string {
            const auto start = s.find_first_not_of(" \t");
            if (start == std::string::npos) return "";
            const auto end = s.find_last_not_of(" \t");
            return s.substr(start, end - start + 1);
        };

        const auto RULES = CVarList(rulesStr, 0, ',');
        for (size_t i = 0; i < RULES.size(); ++i) {
            const std::string rule     = std::string(RULES[i]);
            const auto        colonPos = rule.find(':');
            if (colonPos == std::string::npos)
                continue;
            const std::string cls      = trim(rule.substr(0, colonPos));
            const std::string widthStr = trim(rule.substr(colonPos + 1));
            if (cls.empty() || widthStr.empty())
                continue;
            try {
                float w = std::stof(widthStr);
                w = std::clamp(w, MIN_COLUMN_WIDTH, MAX_COLUMN_WIDTH);
                m_config.autoWidthRules[cls] = w;
            } catch (...) {}
        }
    }

    // A config reload re-applies focus_fit_method and new_window_placement,
    // dropping togglefit/placement overrides, and re-positions the view so
    // changed fit/width options show immediately.
    m_fitMethodOverride = -1;
    g_placementOverride = -1;
    if (const auto WDATA = focusedWindowData()) {
        m_scrollingData->centerOrFitCol(WDATA->column.lock());
        m_scrollingData->recalculate();
    }

    // Register/tear down the hover tick listener according to follow_hover.
    updateHoverTick();
}
