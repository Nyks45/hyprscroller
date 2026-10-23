#pragma once

#include <vector>
#include <deque>
#include <unordered_map>
#include <string>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/config/shared/ConfigErrors.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

class CScrollingLayout;
class COverview;
struct SColumnData;

enum ePinState : uint8_t {
    PIN_NONE = 0,
    PIN_LEFT,
    PIN_RIGHT
};

struct SScrollingWindowData {
    SScrollingWindowData(SP<Layout::ITarget> t, SP<SColumnData> col, float ws = 1.F) : target(t), column(col), windowSize(ws) {
        ;
    }

    WP<Layout::ITarget> target;
    WP<SColumnData>     column;
    float               windowSize = 1.F;

    CBox                layoutBox;
    // Moved off-screen (zen mode); warped back instead of animated.
    bool                parked = false;
};

struct SColumnData {
    SColumnData(CScrollingLayout* pLayout) : layout(pLayout) {
        ;
    }

    void                                  add(SP<Layout::ITarget> t);
    void                                  add(SP<Layout::ITarget> t, int after);
    void                                  add(SP<SScrollingWindowData> w);
    void                                  add(SP<SScrollingWindowData> w, int after);
    void                                  remove(SP<Layout::ITarget> t);
    // Collapse to collapsed_width, or restore the width it had before.
    void                                  setCollapsed(bool collapse);
    bool                                  has(SP<Layout::ITarget> t);
    size_t                                idx(SP<Layout::ITarget> t);

    // index of lowest window that is above y.
    size_t                                idxForHeight(float y);

    void                                  up(SP<SScrollingWindowData> w);
    void                                  down(SP<SScrollingWindowData> w);

    SP<SScrollingWindowData>              next(SP<SScrollingWindowData> w);
    SP<SScrollingWindowData>              prev(SP<SScrollingWindowData> w);

    std::vector<SP<SScrollingWindowData>> windowDatas;
    float                                 columnSize       = 1.F;
    float                                 columnWidth      = 1.F;
    float                                 savedColumnWidth = 0.F;
    // column_scroll: vertical scroll offset of this column, in px.
    double                                topOffset        = 0.0;
    bool                                  collapsed        = false;
    ePinState                             pinned           = PIN_NONE;
    CScrollingLayout*                     layout           = nullptr;
    WP<SScrollingWindowData>              lastFocusedWindow;

    WP<SColumnData>                       self;
};

struct SScrollingLayoutData {
    SScrollingLayoutData(CScrollingLayout* l) : layout(l) {
        ;
    }

        std::vector<SP<SColumnData>> columns;
        float                        leftOffset                  = 0;

    SP<SColumnData>              add();
    SP<SColumnData>              add(int after);
    int64_t                      idx(SP<SColumnData> c);
    void                         remove(SP<SColumnData> c);
    double                       maxWidth();
    double                       pinnedWidthLeft();
    double                       pinnedWidthRight();
    // Rendered width of a column in px (collapsed / fullscreen-on-one / peek / fill aware).
    double                       colWidth(const SP<SColumnData>& c, double usableW);
    // peek_width clamped to a sane range for this usable width.
    double                       peekWidth(double usableW);
    // stretch_to_fill factor for scrollable columns (1 when not stretching).
    double                       fillScale(double usableW);
    // Width of the scrollable viewport, i.e. usable width minus pinned columns.
    double                       scrollWidth();
    // Left edge of an unpinned column in scroll space; nullopt if pinned/absent.
    std::optional<double>        scrollLeft(const SP<SColumnData>& c);
    // Pixel heights of a column's windows (column_scroll aware); returns the total.
    double                       rowHeights(const SP<SColumnData>& c, double usableH, std::vector<double>& out);
    // Lay out one column's windows at x with the given width, applying its vertical offset.
    void                         placeColumn(const SP<SColumnData>& c, double x, double width, const Vector2D& origin, double usableH, bool forceInstant);
    // column_scroll: scroll a column vertically so `w` is in view.
    void                         fitRow(const SP<SColumnData>& c, const SP<SScrollingWindowData>& w);
    SP<SColumnData>              next(SP<SColumnData> c);
    SP<SColumnData>              prev(SP<SColumnData> c);
    SP<SColumnData>              atCenter();

    bool                         visible(SP<SColumnData> c);
    void                         centerCol(SP<SColumnData> c);
    void                         fitCol(SP<SColumnData> c);
    void                         fitCol2(SP<SColumnData> c);
    void                         centerOrFitCol(SP<SColumnData> c);

        void                         recalculate(bool forceInstant = false);

    CScrollingLayout*            layout = nullptr;
    WP<SScrollingLayoutData>     self;
};

class CScrollingLayout : public Layout::ITiledAlgorithm {
  public:
    CScrollingLayout();
    virtual ~CScrollingLayout();

    virtual void                                          newTarget(SP<Layout::ITarget> target) override;
    virtual void                                          movedTarget(SP<Layout::ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override;
    virtual void                                          removeTarget(SP<Layout::ITarget> target) override;

    virtual void                                          resizeTarget(const Vector2D& delta, SP<Layout::ITarget> target, Layout::eRectCorner corner = Layout::CORNER_NONE) override;
    virtual void                                          recalculate(Layout::eRecalculateReason reason = Layout::RECALCULATE_REASON_UNKNOWN) override;

    virtual SP<Layout::ITarget>                           getNextCandidate(SP<Layout::ITarget> old) override;

    virtual Config::ErrorResult                           layoutMsg(const std::string_view& sv) override;
    virtual std::optional<Vector2D>                       predictSizeForNewTarget() override;

    virtual void                                          swapTargets(SP<Layout::ITarget> a, SP<Layout::ITarget> b) override;
    virtual void                                          moveTargetInDirection(SP<Layout::ITarget> t, Math::eDirection dir, bool silent) override;

    CBox                                                  usableArea();

  private:
    SP<SScrollingLayoutData> m_scrollingData;

    CHyprSignalListener      m_configCallback;
    CHyprSignalListener      m_focusCallback;
    CHyprSignalListener      m_buttonCallback;

    // One-shot timer for follow_hover: armed when the pointer settles on a
    // window, fires after hover_delay_ms (event-loop timer: the animation
    // tick stops when nothing animates, so it can't drive this).
    SP<CEventLoopTimer>      m_hoverEventTimer;
    CTimer                   m_debounceTimer;
    WP<Layout::ITarget>      m_hoverTarget;
    // Window being removed and the neighbour chosen to take focus; Hyprland
    // asks getNextCandidate() only after removeTarget().
    WP<Layout::ITarget>      m_removedTarget;
    WP<Layout::ITarget>      m_removedNext;
    // Set around focus changes that must not scroll the view (pixel move).
    bool                     m_suppressFollow = false;
    int                      m_fitMethodOverride = -1;

    struct {
        std::vector<float>                           configuredWidths;
        std::vector<float>                           configuredHeights;
        std::unordered_map<std::string, float>        autoWidthRules;
    } m_config;

    // Focus history
    std::deque<WP<Layout::ITarget>>                  m_focusHistory;
    int64_t                                          m_focusHistoryIdx = -1;
    bool                                             m_focusHistoryNavigating = false;

    // Zen mode
    bool                                             m_zenMode = false;
    SP<SColumnData>                                  m_zenColumn;

    // Overview (zoomed-out strip, see Overview.hpp)
    UP<COverview>                                    m_overview;

    SP<SScrollingWindowData> findBestNeighbor(SP<SScrollingWindowData> pCurrent, SP<SColumnData> pTargetCol);
    SP<SScrollingWindowData> dataFor(SP<Layout::ITarget> t);
    // Returns the layout data for the currently focused window in this layout's
    // space, or nullptr if there is none. Replaces a block that was duplicated
    // across nearly every layoutMsg branch.
    SP<SScrollingWindowData> focusedWindowData();

    void                     focusTargetUpdate(SP<Layout::ITarget> target);
    // In zen mode, switch the shown column to `col` (e.g. on focus change or
    // a new window) so focused windows are never left parked off-screen.
    void                     syncZenColumn(SP<SColumnData> col);
    void                     pushFocusHistory(SP<Layout::ITarget> target);
    void                     moveTargetTo(SP<Layout::ITarget> t, Math::eDirection dir, bool silent);
    void                     moveColToWorkspace(SP<SColumnData> col, const std::string& wsStr);
    float                    autoWidthForTarget(SP<Layout::ITarget> target);

    float                    defaultColumnWidth();
    // Parse config and listen for reloads, once.
    void                     ensureConfig();
    int                      effectiveFitMethod() const;
    // Whether new windows open in the focused column (new_window_placement / placement msg).
    bool                     stackNewWindows() const;
    void                     parseConfig();
    // Creates the follow_hover timer while follow_hover is on and removes it
    // otherwise.
    void                     updateHoverTimer();
    void                     onHoverTimer();

    friend struct SScrollingLayoutData;
    friend class COverview;
};
