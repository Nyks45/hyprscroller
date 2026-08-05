#include "Scrolling.hpp"
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


//
void SColumnData::add(SP<Layout::ITarget> t) {
    const size_t n       = windowDatas.size();
    const float  pref    = std::clamp((float)g_config.window_default_height->value(), MIN_ROW_HEIGHT, 1.0f);
    const float  newSize = (n == 0 || pref >= 1.0f) ? 1.0f / (float)(n + 1) : pref;
    const float  scale   = (n == 0 || pref >= 1.0f) ? (float)n / (float)(n + 1) : (1.0f - pref);

    for (auto& wd : windowDatas)
        wd->windowSize *= scale;

    windowDatas.emplace_back(makeShared<SScrollingWindowData>(t, self.lock(), newSize));
}

void SColumnData::add(SP<Layout::ITarget> t, int after) {
    const size_t n       = windowDatas.size();
    const float  pref    = std::clamp((float)g_config.window_default_height->value(), MIN_ROW_HEIGHT, 1.0f);
    const float  newSize = (n == 0 || pref >= 1.0f) ? 1.0f / (float)(n + 1) : pref;
    const float  scale   = (n == 0 || pref >= 1.0f) ? (float)n / (float)(n + 1) : (1.0f - pref);

    for (auto& wd : windowDatas)
        wd->windowSize *= scale;

    windowDatas.insert(windowDatas.begin() + after + 1, makeShared<SScrollingWindowData>(t, self.lock(), newSize));
}

void SColumnData::add(SP<SScrollingWindowData> w) {
    for (auto& wd : windowDatas) {
        wd->windowSize *= (float)windowDatas.size() / (float)(windowDatas.size() + 1);
    }

    windowDatas.emplace_back(w);
    w->column     = self;
    w->windowSize = 1.F / (float)(windowDatas.size());
}

void SColumnData::add(SP<SScrollingWindowData> w, int after) {
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

void SScrollingLayoutData::centerCol(SP<SColumnData> c) {
    if (!c)
        return;

    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    double       currentLeft  = 0;

    for (const auto& COL : columns) {
        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX :
            (g_config.fullscreen_on_one_column->value() && columns.size() == 1 ? USABLE.w : USABLE.w * COL->columnWidth);

        if (COL != c)
            currentLeft += ITEM_WIDTH;
        else {
            const double target = currentLeft - (USABLE.w - ITEM_WIDTH) / 2.0;
            if (std::abs(target - leftOffset) < 0.5)
                return;
            leftOffset = target;
            return;
        }
    }
}

void SScrollingLayoutData::fitCol(SP<SColumnData> c) {
    if (!c)
        return;

    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    double       currentLeft  = 0;

    for (const auto& COL : columns) {
        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX :
            (g_config.fullscreen_on_one_column->value() && columns.size() == 1 ? USABLE.w : USABLE.w * COL->columnWidth);

        if (COL != c)
            currentLeft += ITEM_WIDTH;
        else {
            const double clamped = std::clamp((double)leftOffset, currentLeft - USABLE.w + ITEM_WIDTH, currentLeft);
            if (std::abs(clamped - leftOffset) < 0.5)
                return;
            leftOffset = clamped;
            return;
        }
    }
}

void SScrollingLayoutData::fitCol2(SP<SColumnData> c) {
    if (!c)
        return;

    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    double       currentLeft  = 0;
    double       maxExtent    = maxWidth();

    for (const auto& COL : columns) {
        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX :
            (g_config.fullscreen_on_one_column->value() && columns.size() == 1 ? USABLE.w : USABLE.w * COL->columnWidth);

        if (COL != c)
            currentLeft += ITEM_WIDTH;
        else {
            double idealOffset = currentLeft - (USABLE.w - ITEM_WIDTH) / 2.0;

            double target;
            if (maxExtent < USABLE.w) {
                target = (maxExtent - USABLE.w) / 2.0;
            } else {
                double lo = 0.0;
                double hi = maxExtent - USABLE.w;
                target = std::clamp(idealOffset, lo, std::max(lo, hi));
            }
            if (std::abs(target - leftOffset) < 0.5)
                return;
            leftOffset = target;
            return;
        }
    }
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
}

SP<SColumnData> SScrollingLayoutData::atCenter() {
    double       currentLeft  = 0;
    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    const double threshold    = leftOffset + USABLE.w / 2.0 - 2;

    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            continue;

        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX :
            (g_config.fullscreen_on_one_column->value() && columns.size() == 1 ? USABLE.w : USABLE.w * COL->columnWidth);

        currentLeft += ITEM_WIDTH;

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
            double currentTop = 0.0;
            for (const auto& WDATA : zenCol->windowDatas) {
                WDATA->layoutBox = CBox{0, currentTop, USABLE.w, WDATA->windowSize * USABLE.h}
                                       .translate(WORKAREA.pos());
                currentTop += WDATA->windowSize * USABLE.h;

                auto target = WDATA->target.lock();
                if (target) {
                    CBox box = WDATA->layoutBox;
                    box.round();
                    target->setPositionGlobal(box);
                    if (forceInstant) { target->warpPositionSize(); target->damageEntire(); }
                }
            }

            // Move all other columns off-screen
            for (const auto& COL : columns) {
                if (COL == zenCol)
                    continue;
                for (const auto& WDATA : COL->windowDatas) {
                    WDATA->layoutBox = CBox{-9999, -9999, 1, 1};
                    auto target = WDATA->target.lock();
                    if (target) {
                        target->setPositionGlobal(WDATA->layoutBox);
                        if (forceInstant) { target->warpPositionSize(); target->damageEntire(); }
                    }
                }
            }
            return;
        }
    }

    // Overview mode: flat thumbnail grid (Win+Tab style).
    // All windows are arranged as equal-sized tiles in a grid that fills the
    // screen, ignoring column structure. Navigation (focus l/r/u/d) still works
    // in terms of the underlying column layout.
    if (layout->m_overviewActive) {
        // Collect all windows in layout order (column-major)
        std::vector<SP<SScrollingWindowData>> allWindows;
        for (const auto& COL : columns) {
            for (const auto& WDATA : COL->windowDatas)
                allWindows.push_back(WDATA);
        }
        if (allWindows.empty()) return;

        constexpr double GAP  = 16.0;
        const size_t     total = allWindows.size();
        const size_t     gridCols = std::max(size_t{1}, (size_t)std::ceil(std::sqrt((double)total)));
        const size_t     gridRows = (total + gridCols - 1) / gridCols;
        const double     cellW    = std::max(1.0, (USABLE.w - GAP * (gridCols + 1)) / (double)gridCols);
        const double     cellH    = std::max(1.0, (USABLE.h - GAP * (gridRows + 1)) / (double)gridRows);

        for (size_t i = 0; i < allWindows.size(); ++i) {
            const auto&  WDATA = allWindows[i];
            const size_t row   = i / gridCols;
            const size_t col   = i % gridCols;
            const double x     = GAP + col * (cellW + GAP);
            const double y     = GAP + row * (cellH + GAP);

            WDATA->layoutBox = CBox{x, y, cellW, cellH}.translate(WORKAREA.pos());

            auto target = WDATA->target.lock();
            if (target) {
                CBox box = WDATA->layoutBox;
                box.round();
                target->setPositionGlobal(box);
                if (forceInstant) { target->warpPositionSize(); target->damageEntire(); }
            }
        }
        return;
    }

    const double COLLAPSED_PX = g_config.collapsed_width->value();
    const bool   FSO          = g_config.fullscreen_on_one_column->value() && columns.size() == 1;

    // Single pass: pinned widths + scrollable total (replaces 3 separate iterations)
    // FSO (fullscreen single) only applies to unpinned scrollable columns.
    double PINNED_L = 0, PINNED_R = 0, scrollableMaxWidth = 0;
    for (const auto& COL : columns) {
        const double nominalW = COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
        if (COL->pinned == PIN_LEFT)       PINNED_L += nominalW;
        else if (COL->pinned == PIN_RIGHT) PINNED_R += nominalW;
        else                               scrollableMaxWidth += (FSO ? USABLE.w : nominalW);
    }
    const double SCROLL_W = USABLE.w - PINNED_L - PINNED_R;

    const double cameraLeft = scrollableMaxWidth < SCROLL_W ? std::round((scrollableMaxWidth - SCROLL_W) / 2.0) : leftOffset;

    // Render pinned-left columns first
    double pinnedLeftX = 0;
    for (const auto& COL : columns) {
        if (COL->pinned != PIN_LEFT)
            continue;

        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
        double       currentTop = 0.0;

        for (const auto& WDATA : COL->windowDatas) {
            WDATA->layoutBox = CBox{pinnedLeftX, currentTop, ITEM_WIDTH, WDATA->windowSize * USABLE.h}
                                   .translate(WORKAREA.pos());
            currentTop += WDATA->windowSize * USABLE.h;

            auto target = WDATA->target.lock();
            if (target) {
                CBox box = WDATA->layoutBox;
                box.round();
                target->setPositionGlobal(box);
                if (forceInstant) { target->warpPositionSize(); target->damageEntire(); }
            }
        }
        pinnedLeftX += ITEM_WIDTH;
    }

    // Render pinned-right columns
    double pinnedRightX = USABLE.w;
    for (int64_t i = (int64_t)columns.size() - 1; i >= 0; --i) {
        const auto& COL = columns[i];
        if (COL->pinned != PIN_RIGHT)
            continue;

        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
        pinnedRightX -= ITEM_WIDTH;
        double currentTop = 0.0;

        for (const auto& WDATA : COL->windowDatas) {
            WDATA->layoutBox = CBox{pinnedRightX, currentTop, ITEM_WIDTH, WDATA->windowSize * USABLE.h}
                                   .translate(WORKAREA.pos());
            currentTop += WDATA->windowSize * USABLE.h;

            auto target = WDATA->target.lock();
            if (target) {
                CBox box = WDATA->layoutBox;
                box.round();
                target->setPositionGlobal(box);
                if (forceInstant) { target->warpPositionSize(); target->damageEntire(); }
            }
        }
    }

    // Render scrollable (unpinned) columns
    double currentLeft = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto&  COL = columns[i];
        if (COL->pinned != PIN_NONE)
            continue;

        double       currentTop = 0.0;
        const double ITEM_WIDTH = COL->collapsed ? COLLAPSED_PX : (FSO ? USABLE.w : USABLE.w * COL->columnWidth);

        for (const auto& WDATA : COL->windowDatas) {
            WDATA->layoutBox = CBox{currentLeft, currentTop, ITEM_WIDTH, WDATA->windowSize * USABLE.h}
                                   .translate(WORKAREA.pos() + Vector2D{PINNED_L - cameraLeft, 0.0});

            currentTop += WDATA->windowSize * USABLE.h;

            auto target = WDATA->target.lock();
            if (target) {
                CBox box = WDATA->layoutBox;
                box.round();
                target->setPositionGlobal(box);

                if (forceInstant) {
                    target->warpPositionSize();
                    target->damageEntire();
                }
            }
        }

        currentLeft += ITEM_WIDTH;
        if (currentLeft >= SCROLL_W)
            currentLeft++;
    }
}

double SScrollingLayoutData::maxWidth() {
    double            currentLeft  = 0;
    const auto        USABLE       = layout->usableArea();
    const double      COLLAPSED_PX = g_config.collapsed_width->value();

    for (const auto& COL : columns) {
        if (COL->pinned != PIN_NONE)
            continue;
        if (COL->collapsed) {
            currentLeft += COLLAPSED_PX;
        } else {
            currentLeft += g_config.fullscreen_on_one_column->value() && columns.size() == 1 ? USABLE.w : USABLE.w * COL->columnWidth;
        }
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

    const auto   USABLE       = layout->usableArea();
    const double COLLAPSED_PX = g_config.collapsed_width->value();
    const double SCROLL_W     = USABLE.w - pinnedWidthLeft() - pinnedWidthRight();
    double       totalLeft    = 0;

    for (const auto& col : columns) {
        if (col->pinned != PIN_NONE)
            continue;

        const double colWidth = col->collapsed ? COLLAPSED_PX : col->columnWidth * USABLE.w;

        if (col == c)
            return totalLeft < leftOffset + SCROLL_W && leftOffset < totalLeft + colWidth;

        totalLeft += colWidth;
    }

    return false;
}

// ======================
// CScrollingLayout
// ======================

CScrollingLayout::CScrollingLayout() {
    m_scrollingData = makeShared<SScrollingLayoutData>(this);
    m_scrollingData->self = m_scrollingData;
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

                if (!g_config.follow_focus->value())
                    return;

                if (reason == Desktop::FOCUS_REASON_FFM) {
                    // Always check edge exclusion for hover/always-follow
                    Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
                    auto PMONITOR = hyprscrolling_compat::monitorFromCursor();
                    if (PMONITOR) {
                        const auto MON_BOX = PMONITOR->logicalBox();
                        if (cursor.x < MON_BOX.x + (double)g_config.click_edge_left->value() ||
                            cursor.x > MON_BOX.x + MON_BOX.w - (double)g_config.click_edge_right->value())
                            return;
                    }

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
        m_buttonCallback = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo& info) {
            if (ev.button != BTN_LEFT || ev.state != WL_POINTER_BUTTON_STATE_PRESSED)
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

            // exclude clicks on left/right screen edges (caelestia)
            const auto MON_BOX = PMONITOR->logicalBox();
            if (cursor.x < MON_BOX.x + (double)g_config.click_edge_left->value() ||
                cursor.x > MON_BOX.x + MON_BOX.w - (double)g_config.click_edge_right->value())
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

    if (!m_tickCallback) {
        m_tickCallback = Event::bus()->m_events.tick.listen([this]() {
            auto target = m_hoverTarget.lock();
            if (!target || m_hoverTimer.getMillis() < (double)g_config.hover_delay_ms->value())
                return;

            // verify cursor is still over this window and not in exclusion zone
            Vector2D cursor = g_pInputManager->getMouseCoordsInternal();
            auto PMONITOR = hyprscrolling_compat::monitorFromCursor();
            if (!PMONITOR)
                return;
            const auto MON_BOX = PMONITOR->logicalBox();
            if (cursor.x < MON_BOX.x + (double)g_config.click_edge_left->value() ||
                cursor.x > MON_BOX.x + MON_BOX.w - (double)g_config.click_edge_right->value())
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

    if (!droppingColumn) {
        auto col = m_scrollingData->add();
        col->add(target);
        col->columnWidth = autoWidthForTarget(target);
        m_scrollingData->centerOrFitCol(col);
    } else {
        auto idx = m_scrollingData->idx(droppingColumn);
        auto col = idx == -1 ? m_scrollingData->add() : m_scrollingData->add(idx);
        col->add(target);
        col->columnWidth = autoWidthForTarget(target);
        m_scrollingData->centerOrFitCol(col);
    }

    m_scrollingData->recalculate();
}

void CScrollingLayout::movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint) {
    newTarget(target);
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

    if (!m_scrollingData->next(COL)) {
        // move the view if this is the last column
        const auto   USABLE       = usableArea();
        const double COLLAPSED_PX = g_config.collapsed_width->value();
        m_scrollingData->leftOffset -= COL->collapsed ? COLLAPSED_PX : USABLE.w * COL->columnWidth;
    }

    COL->remove(target);

    // Remove empty columns
    if (COL->windowDatas.empty()) {
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

void CScrollingLayout::recalculate(Layout::eRecalculateReason reason) {
    m_scrollingData->recalculate();
}

void CScrollingLayout::resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner) {
    if (!target)
        return;

    const auto DATA = dataFor(target);

    if (!DATA || !DATA->column.lock())
        return;

    const auto USABLE        = usableArea();
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

    if (DATA->column.lock()->windowDatas.size() > 1) {
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
            auto focusedTarget = Desktop::focusState()->window();
            if (!focusedTarget)
                return {};

            SP<SScrollingWindowData> WDATA = nullptr;
            auto                     parent = m_parent.lock();
            if (parent && parent->space()) {
                for (auto& wt : parent->space()->targets()) {
                    auto t = wt.lock();
                    if (t && t->window() == focusedTarget) {
                        WDATA = dataFor(t);
                        break;
                    }
                }
            }
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
            auto focusedTarget = Desktop::focusState()->window();

            SP<SScrollingWindowData> WDATA = nullptr;
            auto                     parent = m_parent.lock();
            if (parent && parent->space()) {
                for (auto& wt : parent->space()->targets()) {
                    auto t = wt.lock();
                    if (t && t->window() == focusedTarget) {
                        WDATA = dataFor(t);
                        break;
                    }
                }
            }

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
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }

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

        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }

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
            if (COL->windowDatas.size() > 1) {
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

        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        auto parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    moveTargetTo(t, Math::fromChar(ARGS[1][0]), false);
                    break;
                }
            }
        }
    } else if (ARGS[0] == "fit") {

        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }

        // Helper: sum scrollable width of columns[0..i-1], skipping pinned, accounting for collapsed.
        const auto scrollableOffsetBefore = [&](size_t count) -> double {
            const auto   USABLE       = usableArea();
            const double COLLAPSED_PX = g_config.collapsed_width->value();
            double       off          = 0;
            for (size_t j = 0; j < count; ++j) {
                const auto& col = m_scrollingData->columns[j];
                if (col->pinned != PIN_NONE)
                    continue;
                off += col->collapsed ? COLLAPSED_PX : USABLE.w * col->columnWidth;
            }
            return off;
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
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }


        if (!WDATA || ARGS[1].empty())
            return {};

        switch (ARGS[1][0]) {
            case 'u':
            case 't': {
                auto PREV = WDATA->column.lock()->prev(WDATA);
                if (!PREV) {
                    if (!g_config.focus_wrap->value())
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
                    if (!g_config.focus_wrap->value())
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
                    if (!g_config.focus_wrap->value()) {
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
                    if (!g_config.focus_wrap->value()) {
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
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }

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

        m_scrollingData->recalculate();
    } else if (ARGS[0] == "swapcol") {
        if (ARGS.size() < 2)
            return {};

        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }

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
    } else if (ARGS[0] == "togglefit") {
        m_fitMethodOverride = (effectiveFitMethod() + 1) % 3;

        if (m_scrollingData->columns.empty())
            return {};

        auto focusedWindow = Desktop::focusState()->window();
        SP<SScrollingWindowData> focusedData = nullptr;
        auto parent = m_parent.lock();
        if (parent && parent->space() && focusedWindow) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    focusedData = dataFor(t);
                    break;
                }
            }
        }

        const auto columnToUse = (focusedData && focusedData->column.lock()) ? focusedData->column.lock() : m_scrollingData->atCenter();
        if (!columnToUse)
            return {};

        m_scrollingData->centerOrFitCol(columnToUse);
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "pin") {
        // pin left / pin right — pin the focused column to a screen edge
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
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
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
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

        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL)
            return {};

        moveColToWorkspace(COL, ARGS[1]);
    } else if (ARGS[0] == "collapse") {
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
        if (!WDATA)
            return {};

        auto COL = WDATA->column.lock();
        if (!COL || COL->collapsed)
            return {};

        COL->savedColumnWidth = COL->columnWidth;
        COL->collapsed = true;
        m_scrollingData->recalculate();
    } else if (ARGS[0] == "expand") {
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
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
        auto focusedWindow = Desktop::focusState()->window();
        if (!focusedWindow)
            return {};

        SP<SScrollingWindowData> WDATA = nullptr;
        auto                     parent = m_parent.lock();
        if (parent && parent->space()) {
            for (auto& wt : parent->space()->targets()) {
                auto t = wt.lock();
                if (t && t->window() == focusedWindow) {
                    WDATA = dataFor(t);
                    break;
                }
            }
        }
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
            auto focusedWindow = Desktop::focusState()->window();
            if (!focusedWindow)
                return {};

            SP<SScrollingWindowData> WDATA = nullptr;
            auto                     parent = m_parent.lock();
            if (parent && parent->space()) {
                for (auto& wt : parent->space()->targets()) {
                    auto t = wt.lock();
                    if (t && t->window() == focusedWindow) {
                        WDATA = dataFor(t);
                        break;
                    }
                }
            }
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
        if (!m_overviewActive)
            return {};

        SP<SColumnData> focusedCol;
        auto focusedWindow = Desktop::focusState()->window();
        if (focusedWindow) {
            auto parent = m_parent.lock();
            if (parent && parent->space()) {
                for (auto& wt : parent->space()->targets()) {
                    auto t = wt.lock();
                    if (t && t->window() == focusedWindow) {
                        auto WDATA = dataFor(t);
                        if (WDATA)
                            focusedCol = WDATA->column.lock();
                        break;
                    }
                }
            }
        }

        m_overviewActive = false;
        m_overviewClickHook.reset();

        if (focusedCol)
            m_scrollingData->centerOrFitCol(focusedCol);
        else {
            m_scrollingData->leftOffset = m_overviewSavedOffset;
        }

        m_scrollingData->recalculate(!g_config.overview_animate->value());
    } else if (ARGS[0] == "overview") {
        if (!m_overviewActive) {
            if (m_scrollingData->columns.empty())
                return {};

            // Save only the offset — column widths are not touched; the
            // overview grid is rendered entirely inside recalculate().
            m_overviewSavedOffset = m_scrollingData->leftOffset;
            m_overviewActive = true;
            m_scrollingData->recalculate(!g_config.overview_animate->value());

            m_overviewClickHook = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent ev, Event::SCallbackInfo&) {
                if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || !m_overviewActive)
                    return;

                const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
                auto parent = m_parent.lock();
                SP<SColumnData>      clickedCol;
                SP<Layout::ITarget>  clickedTarget;

                if (parent && parent->space()) {
                    for (auto& wt : parent->space()->targets()) {
                        auto t = wt.lock();
                        if (!t || !t->window()) continue;
                        auto w = t->window();
                        CBox box{w->positionAnimation()->value().x, w->positionAnimation()->value().y,
                                 w->sizeAnimation()->value().x, w->sizeAnimation()->value().y};
                        if (MOUSECOORDS.x >= box.x && MOUSECOORDS.x <= box.x + box.w &&
                            MOUSECOORDS.y >= box.y && MOUSECOORDS.y <= box.y + box.h) {
                            clickedTarget = t;
                            auto WDATA = dataFor(t);
                            if (WDATA)
                                clickedCol = WDATA->column.lock();
                            break;
                        }
                    }
                }

                m_overviewActive = false;
                m_overviewClickHook.reset();

                if (clickedCol)
                    m_scrollingData->centerOrFitCol(clickedCol);
                else {
                    m_scrollingData->leftOffset = m_overviewSavedOffset;
                        }

                m_scrollingData->recalculate(!g_config.overview_animate->value());

                if (clickedTarget)
                    focusTargetUpdate(clickedTarget);
            });

        } else {
            m_overviewActive = false;
            m_overviewClickHook.reset();
            m_scrollingData->leftOffset = m_overviewSavedOffset;
            m_scrollingData->recalculate(!g_config.overview_animate->value());
        }
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

    if (dir == Math::DIRECTION_LEFT) {
        const auto PREVCOL = m_scrollingData->prev(COL);

        COL->remove(t);
        if (COL->windowDatas.empty())
            m_scrollingData->remove(COL);

        if (!PREVCOL) {
            const auto NEWCOL = m_scrollingData->add(-1);
            NEWCOL->add(DATA);
            m_scrollingData->centerOrFitCol(NEWCOL);
        } else {
            if (PREVCOL->windowDatas.size() > 0)
                PREVCOL->add(DATA, PREVCOL->idxForHeight(g_pInputManager->getMouseCoordsInternal().y));
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
            m_scrollingData->centerOrFitCol(NEWCOL);
        } else {
            if (NEXTCOL->windowDatas.size() > 0)
                NEXTCOL->add(DATA, NEXTCOL->idxForHeight(g_pInputManager->getMouseCoordsInternal().y));
            else
                NEXTCOL->add(DATA);
            m_scrollingData->centerOrFitCol(NEXTCOL);
        }

    } else if (dir == Math::DIRECTION_UP)
        COL->up(DATA);
    else if (dir == Math::DIRECTION_DOWN)
        COL->down(DATA);

    m_scrollingData->recalculate();
    focusTargetUpdate(t);
    auto w = t->window();
    if (w)
        Pointer::mgr()->warpTo(w->middle());
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

int CScrollingLayout::effectiveFitMethod() const {
    return m_fitMethodOverride >= 0 ? m_fitMethodOverride : (int)g_config.focus_fit_method->value();
}

void CScrollingLayout::parseConfig() {
    // Parse column widths from explicit_column_widths (fall back to column_widths alias)
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
}
