/*
 * Detail activity implementation.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "detail_activity.hpp"
#include "hg_haptic.hpp"
#include "hg_store.hpp"

extern "C" {
#include "hg_lang.h"
}
#include "player_activity.hpp"
#include "hg_cfg.hpp"
#include "hg_diag_view.hpp"
#include "hg_grid.h"
#include "hg_image.hpp"
#include "hg_touch.hpp"
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <cstdlib>
#include <utility>
namespace hg {

/* ---- the episode picker grid (detail.episodes=grid, the default) ----------
 *
 * 76 episodes as a 76-row list is a lot of scrolling for entries that are just
 * numbers. wiliwili's detail page uses a grid for the same job, so this does too: a
 * grid of numbered buttons, each one cell tall.
 *
 * THE GEOMETRY IS hg_grid.c, NOT A SECOND IMPLEMENTATION. The same function that
 * places home posters places these -- with `aspectNum = 0`, which means "there is no
 * image, the cell is just `extraH` tall". That is why the shape is a parameter rather
 * than a constant: a grid whose cells were always 2:3 posters would make every episode
 * button 200 pt tall. See hg_grid.h.
 *
 * NAVIGATION IS THE FRAMEWORK'S, WITH ONE ANSWER ADDED. Children are added in episode order and
 * absolutely positioned, so Box::getNextFocus walks them left-to-right (one button per press of
 * left/right) with no code of ours. What that walk cannot do is "one row down in the same column",
 * so the grid answers UP/DOWN in its own getNextFocus and lets the framework move the focus. See
 * HomeGridView's declaration in home_activity.cpp for the full reasoning. */

/* HOW MANY EPISODES ARE OPEN AT ONCE. See the note in applyEpisodes: a 200-episode series used to build 200
 * focusable cells and destroy them all in one frame on the way out, which crashed the console every time. The user
 * asked for groups of 20, and only one group is ever built. */
#define HG_EPISODE_GROUP 20

#define HG_EPISODE_HEADING_H 44.0f
#define HG_EPISODE_ROW_SPACE 12.0f
#define HG_EPISODE_ITEM_SPACE 12.0f
#define HG_EPISODE_BUTTON_H 56.0f
#define HG_EPISODE_DEFAULT_CELL_W 120

/* The episode buttons' shape: no image, a fixed 56 pt tall cell. Named once so the
 * tests and the view cannot disagree about it. */
static const HgGridMetrics EPISODE_BUTTON_METRICS = {0.0f, 1.0f, HG_EPISODE_BUTTON_H};

static int cfgEpisodeInt(const char* key, int fallback) {
    std::string raw = hg::cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || v <= 0) {
        hg::logPrintf("EPGRID: cfg %s='%s' is not a positive number -- using %d", key,
                      raw.c_str(), fallback);
        return fallback;
    }
    return (int)v;
}

static bool episodesAreGrid() {
    static const bool grid = !hg::cfgIs("detail.episodes", "list");
    static bool said = false;
    if (!said) {
        said = true;
        hg::logPrintf("DETAIL: episode layout = %s (cfg detail.episodes; grid is the default, "
                      "list is the way back)",
                      grid ? "GRID" : "LIST");
    }
    return grid;
}

/* One numbered episode button. No image: upstream sends no per-episode artwork and no
 * per-episode title, so a button is a number and nothing else. The id is NOT printed
 * on it (unlike the list, which had room) -- it goes into the log when the episode is
 * chosen, which is where a mismatch against the detail page is actually diagnosable. */
class EpisodeGridButton : public brls::Box {
  public:
    EpisodeGridButton() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setCornerRadius(6.0f);
        this->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));

        label = new brls::Label();
        label->setFontSize(22.0f);
        this->addView(label);
    }

    void setNumber(size_t oneBased) {
        char b[16];
        snprintf(b, sizeof b, "%u", (unsigned)oneBased);
        label->setText(b);
    }

    static brls::View* create() { return new EpisodeGridButton(); }

  private:
    brls::Label* label = nullptr;
};

class EpisodeGridView : public brls::ScrollingFrame {
  public:
    EpisodeGridView() {
        this->setFocusable(true);
        this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

        contentBox = new brls::Box();
        /* ROW, NOT COLUMN -- same bug as the home grid, and the same fix. MEASURED on hardware:
         * the episode picker drew eight columns and the D-pad stepped up and down through them
         * while left and right did nothing.
         *
         * Box::getNextFocus is an insertion-order walk whose AXIS decides which direction traverses
         * the children; the other direction is handed to the parent. The buttons are stored in
         * reading order, so ROW is the axis that traverses them with ONE CELL per press for
         * left/right, and UP/DOWN -- the direction this axis hands upwards -- is answered by this
         * grid's own getNextFocus instead of by the scroller. See the long note in
         * home_activity.cpp's HomeGridView for the exact source lines. */
        contentBox->setAxis(brls::Axis::ROW);
        contentBox->setPositionType(brls::PositionType::ABSOLUTE);
        this->setContentView(contentBox);
    }

    /* `full` is the whole episode list and `offset` is where this slice starts in it -- see the note on the
     * definition. Both are needed because a group is a slice: the labels and the episode number come from the
     * offset, and the player needs the FULL list so auto-next can leave the group. */
    void setEpisodes(std::vector<Episode>* items, const Series* series, std::vector<Episode>* full, int offset);
    void relayout();
    void onLayout() override;
    brls::View* getDefaultFocus() override;

    /* THE GRID ANSWERS UP/DOWN; THE FRAMEWORK DOES THE REST -- the same arrangement as the home
     * grid, and the same reason. See the long note on HomeGridView's declaration in
     * home_activity.cpp: what is here is only the half the framework cannot do, the arithmetic lives
     * in hg_grid.c where the host suite checks it, and nothing in this class ever moves the focus
     * itself. That last point is the one that matters: the version of this screen that called
     * giveFocus() from its own per-frame input poll had two things moving the cursor and therefore
     * moved two cells per press. */
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;
    /* The index into `buttons` of the view that has focus, or -1. See HomeGridView::cellIndexOf for
     * why the live focus is read instead of `currentView`. */
    int cellIndexOf(brls::View* view) const;
    /* Scroll so item `index` is visible. Called from the framework's own focus hook -- see the note on
     * the home grid's followFocus and onChildFocusGained. */
    void followFocus(int index);
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;

    int columnCount() const { return this->columns; }
    size_t buttonCount() const { return this->buttons.size(); }

    /* PUT THE CURSOR ON CELL `index` AND SCROLL IT INTO VIEW (build169a/b).
     *
     * WHY THIS IS A METHOD ON THE GRID AND NOT A giveFocus() AT THE CALL SITE. Three callers need it, for
     * two different reasons -- L/R and the group row want "the first cell of the group I just opened", the
     * history wants "the cell for the episode that was last watched" -- and all of them need the SAME two
     * things done in the SAME order: move the cursor, then scroll. Doing it at the call site would have
     * meant writing that order three times, and the order is the part that matters (see the note on the
     * definition).
     *
     * IT DOES NOT RUN EVERY FRAME. It is called from action callbacks (the L/R registration, the group
     * buttons) and once from applyEpisodes when the data arrives, which is the same place the screen has
     * always put its initial focus. The version of this screen that called giveFocus() from a per-frame
     * input poll had two things moving the cursor and moved two cells per press -- see the note on
     * getNextFocus above. Nothing here is reachable from draw(), and nothing here runs on a timer. */
    void focusCell(int index);

  private:
    brls::Box* contentBox = nullptr;
    std::vector<EpisodeGridButton*> buttons;
    std::vector<Episode>* items = nullptr;
    /* THE WHOLE LIST AND THIS SLICE'S OFFSET INTO IT -- see the note on setEpisodes. */
    std::vector<Episode>* full = nullptr;
    int episodeOffset = 0;
    const Series* series = nullptr;

    std::vector<HgGridPlace> places;
    float laidOutWidth = -1.0f;
    int columns = 0;
    int layoutCount = 0;

    void openEpisode(size_t index);
};

/* `full` and `offset` ARE THE FIX FOR A BUG THE GROUPING INTRODUCED, and the user's report is what exposed it:
 *
 * "it still shows 21 to 40, 41 to 60 like that -- what I meant is the SMALL CELLS BELOW, change the numbers on
 * them to the corresponding episode number."
 *
 * The cells were labelled from their index in the list they were given, and after grouping that list is a SLICE
 * (episodes 21..40). So every group was numbered 1..20. Worse, the same slice was handed to PlayerActivity, which
 * meant the player believed it had only twenty episodes: "第 N 集" was wrong and auto-next stopped at the group's
 * end, with ZL/ZR unable to leave it.
 *
 * So the grid now knows BOTH: the slice it draws, and the whole list with the slice's offset into it. Labels and
 * episode numbers come from the offset; playback gets the FULL list so advancing works across groups. */
void EpisodeGridView::setEpisodes(std::vector<Episode>* items, const Series* series, std::vector<Episode>* full,
                                  int offset) {
    this->items = items;
    this->full = full ? full : items;
    this->episodeOffset = offset > 0 ? offset : 0;
    this->series = series;
    this->places.clear();
    this->laidOutWidth = -1.0f;

    /* MOVE THE CURSOR OFF THE BUTTONS WE ARE ABOUT TO DESTROY -- AND THIS IS A CRASH FIX, NOT TIDYING.
     *
     * MEASURED BY READING THE FRAMEWORK, because the failure it prevents is the kind that leaves nothing
     * in the log (the process is gone before a logger can run -- the same shape as the B-key crash
     * documented in onContentAvailable):
     *
     *   * Box::addView APPENDS (box.cpp:161). It does not replace, and this function used to only ever
     *     `addView`, never `removeView` -- so reopening a group LEAKED the previous group's 20 buttons
     *     into the content box. They stayed positioned at their old coordinates, VISIBLE, and FOCUSABLE,
     *     stacked under the new ones. With the group buttons that was already wrong: switching groups
     *     three times left 60 focusable cells in the tree. With L/R on top -- a key the user holds and
     *     mashes -- it is 20 more per press, and the framework's own left/right walk
     *     (Box::getNextFocus, which is what makes one press move one cell) walks the CONTENT BOX'S
     *     CHILDREN, so the duplicate cells are exactly the "two things moving the cursor" that the note
     *     on getNextFocus says this screen was already burned by.
     *   * Application::giveFocus calls `oldFocus->onFocusLost()` on whatever was focused
     *     (application.cpp:890). So FREEING the cell that currently has the cursor and only then asking
     *     for a new focus is a use-after-free on the next focus change.
     *
     * THE FOUR STEPS BELOW ARE IN THIS ORDER FOR A REASON, and the third one is the one that is easy to
     * get wrong:
     *
     *   1. ASK WHERE THE CURSOR IS, while `buttons` still describes the old group (cellIndexOf walks it);
     *   2. FORGET THE OLD CELLS (`buttons.clear()`), so that step 3 cannot name one of them;
     *   3. MOVE THE CURSOR TO THIS FRAME -- and this must come after step 2, because giveFocus resolves its
     *      argument through getDefaultFocus() (application.cpp:885) and this grid's getDefaultFocus answers
     *      "the cell that currently has the cursor". With `buttons` still holding the old cells, step 3
     *      would resolve straight back to the very button that is about to be freed -- a no-op that leaves
     *      the dangling focus exactly where it was. Empty, it falls through to ScrollingFrame's answer.
     *   4. FREE THEM.
     *
     * The cursor lands on the grid frame for the width of one function call: both callers either pass
     * `initial` (showGroup re-focuses the grid so the framework picks cell 0) or call focusCell right after,
     * which lands on a real cell of the new group. */
    /* ---- STEP 3 IS A NO-OP, AND THAT WAS MEASURED BY READING THE FRAMEWORK (build172) ----------------
     *
     * The reasoning above assumed that emptying `buttons` makes step 3 resolve somewhere safe. It does not.
     * `EpisodeGridView::getDefaultFocus()` answers "the focused cell" only while `cellIndexOf` can still name
     * it; with `buttons` empty it falls through to ScrollingFrame, whose Box-level answer is
     * `lastFocusedView` -- THE VERY CELL THAT IS ABOUT TO BE FREED. So `giveFocus(this)` is called with
     * newFocus == oldFocus and application.cpp:887 skips the whole block: THE CURSOR DOES NOT MOVE.
     *
     * WHAT ACTUALLY KEEPS THIS SAFE, since step 3 does not: `freeView()` only QUEUES the deletion
     * (view.cpp:2569) and the queue drains at the END of the frame (application.cpp:209-223), while BOTH
     * callers focus a real cell of the new group inside the SAME call stack (showGroup's
     * `if (initial) giveFocus(episodeGrid)`, or focusCell immediately after). The state in which the tree
     * holds a freed-but-still-focused cell therefore never crosses a frame boundary. That is the whole
     * guarantee, and it rests on those two facts -- not on this call.
     *
     * The call is kept rather than deleted because it is free and it is the right thing to ask for: if
     * getDefaultFocus() ever grows a "return buttons[0]" branch, this line becomes the one that saves it.
     * Do not cite it as the reason the screen is safe. */
    const bool cellWasFocused =
        brls::Application::getCurrentFocus() != nullptr &&
        this->cellIndexOf(brls::Application::getCurrentFocus()) >= 0;
    this->buttons.clear();
    if (cellWasFocused) brls::Application::giveFocus(this);
    /* FREE THE PREVIOUS GROUP, which is the "drop the previous group's cache" half of the user's design:
     * opening a group REPLACES the previous one rather than adding to it. `true` frees; without this the
     * boxes accumulated for the life of the screen. */
    this->contentBox->clearViews(true);

    if (!items || items->empty()) {
        hg::logPrintf("EPGRID: no episodes -- nothing to build");
        return;
    }

    this->buttons.reserve(items->size());
    for (size_t i = 0; i < items->size(); i++) {
        auto* b = new EpisodeGridButton();
        b->setPositionType(brls::PositionType::ABSOLUTE);
        /* THE CELL SHOWS ITS REAL EPISODE NUMBER: offset + position in this slice. The user's exact words:
         * "change the numbers on them to the corresponding episode number". */
        b->setNumber((size_t)this->episodeOffset + i + 1);
        /* Touch: a tap plays that episode, the same action A runs. See hg_touch.hpp. */
        hg::installTapSelect(b);
        b->registerClickAction([this, i](brls::View*) {
            this->openEpisode(i);
            return true;
        });
        this->contentBox->addView(b);
        this->buttons.push_back(b);
    }

    hg::logPrintf("EPGRID: built %u episode buttons", (unsigned)this->buttons.size());
    this->relayout();
}

void EpisodeGridView::openEpisode(size_t index) {
    if (!this->items || !this->series || index >= this->items->size()) {
        hg::logPrintf("EPGRID: select of episode %u is out of range -- ignored",
                      (unsigned)index);
        return;
    }
    const Episode& e = (*this->items)[index];
    /* The id is logged, not drawn: a button shows "N", and N mapping to the wrong id
     * is exactly the silent failure this log line makes visible. */
    hg::logPrintf("EPGRID: episode %u selected, id=%s", (unsigned)(this->episodeOffset + (int)index + 1), e.id.c_str());
    /* THE FULL LIST, AND AN ABSOLUTE INDEX: with a slice the player would think it had twenty episodes,
     * so auto-next would stop at the group boundary and "第 N 集" would be wrong. */
    brls::Application::pushActivity(
        new PlayerActivity(*this->series, *this->full, this->episodeOffset + (int)index + 1));
}

void EpisodeGridView::relayout() {
    if (!this->items || this->items->empty()) return;

    const float width = this->getWidth();
    if (width <= 0.0f) return;
    if (width == this->laidOutWidth) return;

    const float target = (float)cfgEpisodeInt("detail.cellW", HG_EPISODE_DEFAULT_CELL_W);

    std::vector<int> kinds(this->items->size(), HG_GRID_ITEM_CARD);

    this->places.assign(this->items->size(), HgGridPlace());
    float total = 0.0f;
    /* headingH is 0 and there are no headings: an episode picker has nothing to label,
     * and passing a non-zero height for a kind that never occurs would be misleading. */
    int placed = hg_grid_layout(kinds.data(), kinds.size(), width, target, 0, 0.0f,
                                HG_EPISODE_ROW_SPACE, HG_EPISODE_ITEM_SPACE,
                                &EPISODE_BUTTON_METRICS, this->places.data(),
                                this->places.size(), &total);

    if (placed != (int)this->items->size()) {
        hg::logPrintf("EPGRID: layout placed %d of %u episodes at width %.0f -- NOT showing a "
                      "partial picker",
                      placed, (unsigned)this->items->size(), width);
        for (size_t i = 0; i < this->buttons.size(); i++) {
            this->buttons[i]->setVisibility(brls::Visibility::GONE);
        }
        this->laidOutWidth = width;
        return;
    }

    this->columns = hg_grid_columns_for(width, target, 0);
    this->laidOutWidth = width;
    this->contentBox->setHeight(total);

    for (size_t i = 0; i < this->places.size(); i++) {
        const HgGridPlace& p = this->places[i];
        auto* b = this->buttons[i];
        b->setPositionTop(p.y);
        b->setPositionLeft(p.x);
        b->setWidth(p.w);
        b->setHeight(p.h);
        b->setVisibility(brls::Visibility::VISIBLE);
    }

    this->layoutCount++;
    hg::logPrintf("EPGRID: layout #%d w=%.0f -> %d columns, button %.1fx%.1f, contentH=%.0f "
                  "(%u episodes)",
                  this->layoutCount, width, this->columns, this->places[0].cellW,
                  this->places[0].cellH, total, (unsigned)this->buttons.size());
}

void EpisodeGridView::onLayout() {
    brls::ScrollingFrame::onLayout();
    this->relayout();
}

/* WHERE FOCUS GOES WHEN THE PAD IS PUSHED. See HomeGridView::getNextFocus in home_activity.cpp:
 * this is the same arrangement, for the same reason, and the comment there carries the full history.
 *
 * LEFT/RIGHT -- nullptr, "not my question": the framework's own walk through the content box already
 * steps one button and is the half the user confirmed working on hardware.
 *
 * UP/DOWN -- one row, nearest column, from hg_grid_row_step. The episode picker is the screen that
 * needs this most: 76 buttons in 8 columns means two presses of DOWN used to leave the visible area. */
brls::View* EpisodeGridView::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    (void)currentView;   /* the live focus is what matters here; see HomeGridView::getNextFocus */

    if (direction != brls::FocusDirection::UP && direction != brls::FocusDirection::DOWN) {
        /* LEFT/RIGHT: the framework's walk, with its dead end handed to the parent -- see the same
         * block in HomeGridView::getNextFocus. On this screen there is nothing to the left of the
         * episode grid, so the parent finds nothing and the walk ends in the shake it always did; it
         * is written the same way in both grids so the two cannot drift apart. */
        if (this->getParent()) return this->getParent()->getNextFocus(direction, this);
        return nullptr;
    }

    const int at = this->cellIndexOf(brls::Application::getCurrentFocus());
    if (at < 0) return nullptr;

    const int delta = (direction == brls::FocusDirection::UP) ? -1 : 1;
    const int target = hg_grid_row_step(this->places.data(), this->places.size(), at, delta);

    /* One line per movement, so "did one press move one row?" is countable in the log instead of
     * being inferred. See the same line on the home grid. */
    {
        static int lines = 0;
        if (lines < 200) {
            if (target >= 0) {
                hg::logPrintf("NAVROW: ep %s button %d (row %d col %d) -> button %d (row %d col %d) "
                              "[build32]",
                              delta < 0 ? "UP" : "DOWN", at + 1, this->places[(size_t)at].row,
                              this->places[(size_t)at].column, target + 1,
                              this->places[(size_t)target].row, this->places[(size_t)target].column);
            } else {
                hg::logPrintf("NAVROW: ep %s button %d -- no row that way, handed to the parent "
                              "[build32]",
                              delta < 0 ? "UP" : "DOWN", at + 1);
            }
            lines++;
        }
    }

    if (target >= 0 && this->buttons[(size_t)target]) {
        return this->buttons[(size_t)target]->getDefaultFocus();
    }

    /* Nothing that way inside the grid: the parent decides, exactly as Box::getNextFocus does for a
     * direction it cannot serve. */
    if (this->getParent()) return this->getParent()->getNextFocus(direction, this);
    return nullptr;
}

int EpisodeGridView::cellIndexOf(brls::View* view) const {
    if (!view) return -1;
    for (brls::View* v = view; v != nullptr; v = v->getParent()) {
        for (size_t i = 0; i < this->buttons.size(); i++) {
            if (this->buttons[i] && this->buttons[i] == v) return (int)i;
        }
    }
    return -1;
}

void EpisodeGridView::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    /* Scroll first, then let the framework finish its own bookkeeping -- the same order and the same
     * reasoning as the home grid: followFocus sets the offset, and the framework's updateScrolling
     * then aims at the same target (getDefaultFocus answers with the focused button) and does
     * nothing. */
    const int at = this->cellIndexOf(focusedView);
    if (at >= 0) this->followFocus(at);
    brls::ScrollingFrame::onChildFocusGained(directChild, focusedView);
}

/* SCROLL SO THE FOCUSED BUTTON IS ON SCREEN. Called from onChildFocusGained, and see the long note on
 * the home grid's followFocus for the one correction that matters: this framework DOES have
 * follow-focus (ScrollingFrame::onChildFocusGained calls updateScrolling on every focus change), and
 * this grid used to defeat it by answering getDefaultFocus() with "button 1, always".
 *
 * This is the episode picker, where the need is the most obvious: 76 buttons in rows of eight means
 * the focused button is off screen after a couple of presses of DOWN. */
void EpisodeGridView::followFocus(int index) {
    if (index < 0 || index >= (int)this->places.size()) return;
    if (this->buttons.empty()) return;

    const float viewH = this->getHeight();
    const float contentH = this->getContentHeight();
    if (viewH <= 0.0f) return;
    if (contentH <= viewH) {
        this->setContentOffsetY(0.0f, false);
        return;
    }

    const HgGridPlace& p = this->places[(size_t)index];
    const float centre = p.y + p.cellH * 0.5f;
    float want = centre - viewH * 0.5f;
    const float bottomLimit = contentH - viewH;
    if (want < 0.0f) want = 0.0f;
    if (want > bottomLimit) want = bottomLimit;
    this->setContentOffsetY(want, false);
}

/* MOVE THE CURSOR ONTO CELL `index` AND SCROLL IT INTO VIEW. See the declaration in the class for who
 * calls this and why it is not a giveFocus() at the call site; what the body has to say is the ORDER.
 *
 * FOCUS FIRST, SCROLL SECOND, and the reverse is the bug this is written against. followFocus reads
 * this->places, which setEpisodes has already recomputed for the new group, so the arithmetic works either
 * way -- but the framework's own ScrollingFrame::updateScrolling runs on the focus change and aims at
 * getDefaultFocus(), and getDefaultFocus answers "the cell that currently has the cursor". Scrolling first
 * would therefore be not just redundant but actively wrong: the framework would scroll back to wherever
 * the cursor still was, undoing it. Focus first, and the two agree.
 *
 * THE RANGE CHECK IS THE USER-FACING ONE: index is an offset within the CURRENT group, so a caller that
 * hands over an absolute episode number lands here and is refused with a line in the log rather than
 * scrolling somewhere arbitrary. Both callers pass offsets, and both are the reason that distinction is
 * spelled out rather than assumed. */
void EpisodeGridView::focusCell(int index) {
    if (index < 0 || index >= (int)this->buttons.size()) {
        hg::logPrintf("EPGRID: focusCell %d is outside this group of %u cells -- ignored [build169]",
                      index, (unsigned)this->buttons.size());
        return;
    }
    if (!this->buttons[(size_t)index]) return;

    brls::Application::giveFocus(this->buttons[(size_t)index]);
    this->followFocus(index);
}

brls::View* EpisodeGridView::getDefaultFocus() {
    /* Episode 1, so A right after the page opens plays the first episode -- the same
     * thing the list did.
     *
     * WITH ONE BRANCH ADDED, and it is the one that fixes the scrolling: when the focus is inside this
     * grid, the answer is the FOCUSED button, because that is also the answer the framework's
     * follow-focus wants (ScrollingFrame::updateScrolling scrolls to whatever this returns). A plain
     * Box answers "the child that last had focus" for the same reason. Before this branch, every
     * focus change asked the framework to scroll back to button 1. */
    const int at = this->cellIndexOf(brls::Application::getCurrentFocus());
    if (at >= 0) {
        brls::View* focused = this->buttons[(size_t)at]->getDefaultFocus();
        if (focused) return focused;
    }

    if (!this->buttons.empty()) {
        auto* view = this->buttons[0]->getDefaultFocus();
        if (view) return view;
    }
    return brls::ScrollingFrame::getDefaultFocus();
}

class EpisodeCell : public brls::RecyclerCell {
  public:
    EpisodeCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(60.0f);
        this->setMarginBottom(6.0f);
        this->setPadding(10.0f, 16.0f, 10.0f, 16.0f);
        this->setCornerRadius(8.0f);
        this->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));

        label = new brls::Label();
        label->setFontSize(22.0f);
        label->setGrow(1.0f);
        this->addView(label);
    }

    void setData(size_t i, const Episode& e) {
        char b[160];
        /* "episode N of M" is positional because upstream sends no title. The
         * raw id is appended so a mismatch against the detail page is visible
         * here instead of only inside a network trace. */
        snprintf(b, sizeof b, "Episode %u   id=%s", (unsigned)(i + 1), e.id.c_str());
        label->setText(b);
    }

    static brls::RecyclerCell* create() { return new EpisodeCell(); }

  private:
    brls::Label* label = nullptr;
};

class EpisodeDataSource : public brls::RecyclerDataSource {
  public:
    EpisodeDataSource(std::vector<Episode>* items, const Series* series)
        : items(items), series(series) {}

    int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }

    int numberOfRows(brls::RecyclerFrame* recycler, int section) override {
        return (int)items->size();
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = (EpisodeCell*)recycler->dequeueReusableCell("EpisodeCell");
        cell->setData(index.row, (*items)[index.row]);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        /* A on an episode (or a tap) starts playback. The player resolves the
         * signed video model itself, so it needs the episode id and a label.
         *
         * This is the ONLY path that starts playback: it is reached through the
         * click action RecyclerCell registers in its constructor, so it fires
         * only when a CELL holds focus. If A appears to do nothing, the first
         * thing to check is whether focus is on a cell -- which is why the log
         * line below exists. */
        hg::logPrintf("DETAIL didSelectRowAt: row=%d id=%s", index.row,
                      (*items)[index.row].id.c_str());
        brls::Application::pushActivity(
            new PlayerActivity(*series, *items, (int)index.row + 1));
        hg::logPrintf("DETAIL didSelectRowAt: PlayerActivity pushed");
    }

  private:
    std::vector<Episode>* items;
    const Series* series;
};

DetailActivity::DetailActivity(const Series& s) : series(s) { }

DetailActivity::~DetailActivity() {
    /* Tell every callback in flight that this screen is gone -- see hg_alive.hpp. */
    this->alive_->kill();
}

/* B LEAVES THIS SCREEN, AND IT HAS TO BE REGISTERED TO DO SO.
 *
 * MEASURED ON HARDWARE: "in the episode picker I cannot get back to the preview". borealis registers
 * no default B action -- the only ones in the library are in dialog.cpp -- so a screen that wants B
 * to go back must say so. Registered on whichever view is holding focus (the grid, or the list
 * frame), because Application::handleAction looks the action up starting at the focused view and
 * walking up its parents; registering on the activity's content view alone left B dead once focus
 * had moved into the grid. */
void DetailActivity::onContentAvailable() {
    auto pop = [](brls::View*) {
        hg::logPrintf("DETAIL: B pressed -> popActivity (deferred one frame)");
        /* DEFERRED BY ONE FRAME, AND THAT IS THE CRASH FIX, NOT A STYLE CHOICE.
         *
         * The first version called `brls::Application::popActivity()` directly from this callback. The
         * callback is registered ON `episodeGrid`, and popActivity destroys this Activity -- and with it
         * the grid -- while `Application::handleAction` is still walking that view's action list on the
         * stack. That is a use-after-free on an object the dispatcher is iterating, which is exactly why
         * the app died with a software error and left NOTHING in the log: the process was gone before any
         * logger could run.
         *
         * `brls::sync` runs on the main thread within the current frame, so by the time this executes the
         * action dispatch has fully unwound and no view is mid-use. It is the same mechanism every worker
         * in this app already uses to return to the main thread. */
        brls::sync([]() { brls::Application::popActivity(); });
        return true;
    };

    if (this->episodeGrid) {
        /* ---- B WORKS ANYWHERE ON THIS SCREEN (build128, the user's report) ---------------------------------
     *
     * THEIR WORDS: "on the episode page, on the '1 to 20 / 21 to 40' group buttons there is no way out with B --
     * only on the concrete episode numbers is there a B that goes back. B should work anywhere on this screen."
     *
     * It was registered on the GRID (and on the list), and borealis dispatches an action from the focused view
     * upwards -- so the group buttons, which are a sibling of the grid, had nothing to answer B. Registering it on
     * the ACTIVITY as well means every child of this screen reaches it, whatever holds the focus. */
    this->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, pop);
        hg::logPrintf("DETAIL: B registered on the episode grid");
    } else if (this->frame) {
        /* Y IS REGISTERED ON THE ACTIVITY NOW -- see the note after this if/else, and build129 in the log. */
        this->frame->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, pop);
        hg::logPrintf("DETAIL: B registered on the episode list");
    } else {
        hg::logPrintf("DETAIL: no view to register B on -- B will not leave this screen");
    }

    /* ---- L / R SWITCH EPISODE GROUP (build169a, the user's request) --------------------------------------
     *
     * THEIR WORDS: "the episode page's 1-to-20 / 21-to-40 row should be switchable with R / L."
     *
     * THE ROW ALREADY EXISTED AND ALREADY WORKED -- by clicking it, or by walking the cursor onto a group
     * button and pressing A (build85). What was missing is the binding: a shoulder-button shortcut, which is
     * what the user actually reaches for when the cursor is down among the episode numbers and the fifteen
     * other things on this screen. So this is the same `showGroup` the row calls, reached from a key.
     *
     * REGISTERED ON THE ACTIVITY, OUTSIDE THE GRID/LIST BRANCH -- for the reason the Y note two blocks down
     * spells out at length, and which cost a whole build when it was got wrong: borealis dispatches an action
     * from the FOCUSED view upwards, so an action registered on the grid is unreachable while the cursor is
     * on the group row. L/R has to work from the row, from a cell, and from the list, so it is registered
     * where every child of this screen can reach it.
     *
     * ONLY IN THE GRID LAYOUT, because in the LIST layout there are no groups at all -- the list is handed
     * the whole episode vector. Binding L/R there would be a key that logs a refusal, which is worse than an
     * unbound key. `detail.episodes=list` therefore keeps L/R exactly as dead as it was before this build.
     *
     * ---- AND THE ORDER OF THE THREE THINGS IT DOES IS THE PART THAT MATTERS ------------------------------
     *
     *  1. work out the target group and REFUSE if it is off either end;
     *  2. showGroup(that group) -- which is what BUILDS the cells;
     *  3. focusCell(0) -- the first cell OF THE GROUP JUST BUILT.
     *
     * Doing (3) before (2) would put the cursor on a cell of the previous group, and the user's requirement
     * is the opposite: "after switching, the focus lands on the first cell of the NEW group". Doing (2)
     * without (3) -- which is what the group row does, deliberately, because a click is a pointer -- would
     * leave the cursor on cell 7 of a group of 20 when the user asked for the next one.
     *
     * WHY THIS IS SAFE TO DO FROM AN ACTION CALLBACK, when the note on getNextFocus says this screen was
     * burned by calling giveFocus from a per-frame poll: this runs ONCE PER PRESS, from the input
     * dispatcher, and it is the only thing that moves the cursor during that press. It is not on a timer and
     * it is not reachable from draw(). The old bug was two independent things moving the cursor every frame.
     *
     * WHY IT DOES NOT USE `allowRepeating`. That is build160's lesson, in the L/R seek of the player:
     * repeating a group switch at 10 presses a second rebuilds twenty views per press. A held L/R therefore
     * switches once, which is also what the boundary log line is for. */
    if (this->episodeGrid) {
        auto step = [this](int delta) {
            const int total = (int)this->episodes.size();
            if (total <= 0) {
                hg::logPrintf("DETAIL: L/R pressed before the episode list arrived -- ignored [build169a]");
                return true;
            }
            const int groups = (total + HG_EPISODE_GROUP - 1) / HG_EPISODE_GROUP;
            const int want = this->groupIndex + delta;

            /* THE BOUNDARY: "at the first group L and at the last group R must not go out of bounds and
             * must not crash". Refused here rather than letting showGroup's own check do it, so that the
             * line the user will read says what was pressed and where it stopped. A negative or overshooting
             * `want` is never passed on: nothing is rebuilt, no cell is focused, nothing is freed. */
            if (want < 0 || want >= groups) {
                hg::logPrintf("DETAIL: L/R -> group %d of %d already at the %s end -- no group that way "
                              "[build169a]",
                              this->groupIndex + 1, groups, want < 0 ? "first" : "last");
                return true;
            }

            /* The interval in the log is 1-based and inclusive, matching the group buttons' own labels
             * ("21-40"), because that is the text the user is comparing it against. */
            const int firstEp = want * HG_EPISODE_GROUP + 1;
            int lastEp = firstEp + HG_EPISODE_GROUP - 1;
            if (lastEp > total) lastEp = total;
            hg::logPrintf("DETAIL: L/R -> group %d of %d (episodes %d..%d) [build169a]", want + 1, groups,
                          firstEp, lastEp);

            this->showGroup(want, false);
            this->episodeGrid->focusCell(0);
            return true;
        };
        /* WHICH ENUM MEMBER IS THE PHYSICAL L BUTTON, CHECKED RATHER THAN GUESSED, because the names are
         * the opposite way round from what they look like: borealis' enum follows an Xbox pad, where LB is
         * the UPPER shoulder button and the lower one is LT. On the Switch the mapping table is
         * switch_input.cpp:26-48,
         *
         *     HidNpadButton_ZL -> BUTTON_LT        HidNpadButton_ZR -> BUTTON_RT
         *     HidNpadButton_L  -> BUTTON_LB        HidNpadButton_R  -> BUTTON_RB
         *
         * so BUTTON_LB / BUTTON_RB ARE the console's L and R, which is what the user named. (The player
         * screen's own LB/RB are its seek actions -- the two screens do not share a binding, which is the
         * same division home_activity.cpp makes about its LB refresh key.) */
        this->registerAction("Previous group", brls::ControllerButton::BUTTON_LB, [step](brls::View*) {
            return step(-1);
        });
        this->registerAction("Next group", brls::ControllerButton::BUTTON_RB, [step](brls::View*) {
            return step(+1);
        });
        hg::logPrintf("DETAIL: L/R registered on the activity -> previous/next episode group [build169a]");
    } else {
        hg::logPrintf("DETAIL: list layout -- no L/R group binding (there are no groups in this layout) "
                      "[build169a]");
    }

    /* ---- Y TOGGLES THE WATCHLIST, IN BOTH LAYOUTS (build129, and this is why favourites "did nothing") ----
     *
     * MEASURED ON HARDWARE, from the user: "the favourite feature has not been made." It had been -- it was
     * registered one branch too deep, inside the `else if (frame)` that builds the LIST layout, so on this console
     * (which uses the grid) Y was never bound at all. The log proves it: "Y pressed -> watchlist" appears ZERO
     * times in a session where the key was pressed, while the grid branch's own line ("B registered on the episode
     * grid") is there.
     *
     * So it is registered on the ACTIVITY, outside the branch: borealis dispatches an action from the focused view
     * upwards, so every child of this screen -- the grid, the list, the group buttons -- reaches it.
     *
     * The toggle writes the same file the 收藏 screen reads (hg_store), the notification states the RESULT rather
     * than the action ("added" / "removed"), and the label on this screen follows it. */
    this->registerAction(hg_tr(HG_T_FAVORITES), brls::ControllerButton::BUTTON_Y,
        [this](brls::View*) {
            const bool now = Store::toggleFavorite(this->series);
            hg::logPrintf("DETAIL: Y pressed -> watchlist is now %s for '%s' (%d entries)",
                          now ? "ON" : "OFF", this->series.title.c_str(), Store::favoritesCount());
            /* A CONFIRMATION THE CONSOLE CAN DELIVER WITHOUT LOOKING UP (C15). One of the two places the rumble
             * setting is wired to; see hg_haptic.hpp for why the scope is this narrow. */
            hg::hapticPulse(0x8000, 0x4000, 120);
            brls::Application::notify(now ? hg_tr(HG_T_FAV_ADDED) : hg_tr(HG_T_FAV_REMOVED));
            if (this->favLabel) {
                this->favLabel->setText(now ? hg_tr(HG_T_ADDED_TO_FAV) : hg_tr(HG_T_FAVORITES));
            }
            return true;
        });
    hg::logPrintf("DETAIL: Y registered on the activity -> the watchlist works in both layouts [build129]");

    /* ---- X CLEARS THIS SERIES FROM THE CONTINUE-WATCHING LIST (build129, the user's request) ------------
     *
     * THEIR WORDS: "for this one series I need a way to clear the history, because I never really watched 222
     * episodes of it, but now it says 222."
     *
     * X is the one key this screen still had free (A plays, Y is the watchlist, B goes back). It removes the rows
     * for THIS series and nothing else, and reports the count -- a key that silently edits stored data is
     * indistinguishable from one that did nothing. */
    this->registerAction("Clear history", brls::ControllerButton::BUTTON_X,
        [this](brls::View*) {
            /* ---- TWO PRESSES, BECAUSE THIS DELETES SOMETHING THE USER CANNOT GET BACK (build130) ----------
             *
             * THEIR WORDS: "clearing the history should ask me to confirm once, so I do not do it by accident."
             *
             * The first press only ARMS the action for three seconds and says so; a second press inside that
             * window does the work. That is the confirmation pattern a console uses for a destructive key, it needs
             * no new interface, and it cannot fire twice by accident because the window closes the moment it is
             * used.
             *
             * B leaves the screen, which cancels it just as well: the window is a deadline, not a mode. The clock
             * is std::chrono rather than an app helper, because this is the only place on this screen that needs
             * one and steady_clock is always there. */
            const double nowMs3 = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            if (nowMs3 >= this->clearArmedUntil) {
                this->clearArmedUntil = nowMs3 + 3000.0;
                hg::logPrintf("DETAIL: X pressed -> ARMED; press X again within 3 s to clear the history of '%s'",
                              this->series.title.c_str());
                brls::Application::notify(std::string(hg_tr(HG_T_OK)) + "  X x2   /   " +
                                          std::string(hg_tr(HG_T_CANCEL)) + "  B");
                return true;
            }
            this->clearArmedUntil = 0.0;

            const int removed = Store::clearHistoryFor(this->series.id);
            hg::logPrintf("DETAIL: X pressed (confirmed) -> cleared %d history row(s) for '%s' (%d entries left)",
                          removed, this->series.title.c_str(), Store::historyCount());
            brls::Application::notify(removed > 0 ? hg_tr(HG_T_HISTORY) : hg_tr(HG_T_HISTORY_EMPTY));
            if (this->histLabel) {
                this->histLabel->setText(hg_tr(HG_T_HISTORY_EMPTY));
            }
            return true;
        });
}

brls::View* DetailActivity::createContentView() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(20.0f);

    /* THE COVER, RENDERED RATHER THAN PRINTED.
     *
     * The previous version showed the cover URL as text, on the grounds that
     * borealis has no HTTP image loader and no decoder for this CDN's images.
     * Half of that is right -- there is no HTTP image loader and no loader
     * registry to extend (views/image.cpp hard-codes its two paths) -- but the
     * decode half is wrong: Image::setImageFromMem hands ENCODED bytes to
     * nanovg, and the deko3d nanovg backend compiles stb_image in
     * (stbi_load_from_memory is defined in the built libborealis.a). So the
     * loader is a fetch plus one API call, which is what hg_image.cpp does.
     *
     * A thumbnail and the series identity sit side by side: the image alone
     * would not say WHICH series it belongs to if the cover were wrong, and this
     * screen exists to make upstream mismatches visible. */
    auto* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setMarginBottom(10.0f);

    this->coverImage = new brls::Image();
    this->coverImage->setWidth(96.0f);
    this->coverImage->setHeight(128.0f);
    this->coverImage->setMarginRight(14.0f);
    this->coverImage->setScalingType(brls::ImageScalingType::FIT);
    headerRow->addView(this->coverImage);

    auto* infoCol = new brls::Box();
    infoCol->setAxis(brls::Axis::COLUMN);
    infoCol->setGrow(1.0f);

    auto* idLabel = new brls::Label();
    idLabel->setFontSize(16.0f);
    idLabel->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    idLabel->setText("series_id: " + series.id);
    idLabel->setMarginBottom(6.0f);
    infoCol->addView(idLabel);

    /* The cover state is stated on screen: "loading", the byte count on success,
     * or the reason on failure. A silently blank thumbnail is the one outcome
     * that tells the user nothing -- and "the image is missing" and "the loader
     * never ran" look identical without this line. */
    this->coverStatus = new brls::Label();
    this->coverStatus->setFontSize(15.0f);
    this->coverStatus->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    this->coverStatus->setText(series.cover.empty() ? "cover: none upstream" : "cover: loading...");
    infoCol->addView(this->coverStatus);

    headerRow->addView(infoCol);
    box->addView(headerRow);

    /* THE SYNOPSIS. wiliwili's detail page has a scrollable text box here
     * (view/text_box.hpp); this is the simpler half of it -- a Label that wraps --
     * because the synopsis on this service is one short paragraph on the runs that
     * have it at all.
     *
     * THE FALLBACK IS THE POINT. `desc` is unreliable (measured: absent on one whole
     * capture, present on 2 of 36 cards in another), while `tags` is present on 36/36.
     * So the block shows the synopsis when there is one and the genre tags otherwise,
     * and when BOTH are empty it is hidden -- an empty bordered box reads as a loading
     * failure, which is worse than not showing a block at all. */
    {
        std::string text = series.desc;
        if (text.empty()) text = series.tags;
        if (text.empty()) {
            hg::logPrintf("DETAIL: no synopsis and no tags upstream -- the block is hidden");
        } else {
            this->synopsis = new brls::Label();
            this->synopsis->setFontSize(17.0f);
            this->synopsis->setHorizontalAlign(brls::HorizontalAlign::LEFT);
            this->synopsis->setTextColor(brls::Application::getTheme().getColor("brls/text"));
            this->synopsis->setMarginBottom(10.0f);
            this->synopsis->setText(text);
            box->addView(this->synopsis);
            hg::logPrintf("DETAIL: synopsis from %s (%u bytes)",
                          series.desc.empty() ? "tags (desc was empty)" : "series_intro",
                          (unsigned)text.size());
        }
    }

    if (!series.cover.empty()) {
        hg::loadImageAsync(this->coverImage, series.cover);
    }

    auto* header = new brls::Label();
    header->setFontSize(24.0f);
    header->setText("Loading episodes...");
    header->setMarginBottom(12.0f);
    box->addView(header);
    this->headerLabel = header;

    /* ---- THE WATCHLIST STATE, ON SCREEN (build124, the user's suggestion) ------------------------------
     *
     * THEIR WORDS: "I suggest adding a favourite button on the episode page, one tap adds it to the list."
     *
     * The toggle has existed for a while -- Y on this screen -- but nothing on screen said so, so the only way to
     * find it was to read the log. This label states the state and names the key, and the Y handler below updates
     * it, so a favourite can be added and seen without leaving the screen.
     *
     * The text comes from the language table rather than from a literal, because the source files in this project
     * are ASCII-only: HG_T_ADDED_TO_FAV is exactly the string the notification already shows. */
    this->favLabel = new brls::Label();
    this->favLabel->setFontSize(18.0f);
    this->favLabel->setMarginBottom(10.0f);
    this->favLabel->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    this->favLabel->setText(Store::isFavorite(this->series.id) ? hg_tr(HG_T_ADDED_TO_FAV)
                                                              : hg_tr(HG_T_FAVORITES));
    box->addView(this->favLabel);

    /* ---- WHAT HISTORY SAYS ABOUT THIS SERIES (build125, the user's request) ----------------------------
     *
     * THEIR WORDS: "when you open the episode page, show how far the history got and which episode."
     *
     * The store has kept this all along -- recordPlayback writes the episode id, its label, the position and the
     * duration on every update -- but nothing ever showed it, so the only way to know where you were was to open
     * the player and look. This line says it before anything is fetched or played, and the resume itself is the
     * next step (the same lookup, used to open that episode at that position).
     *
     * The entry is matched on seriesId, not on the title: the id is what the history file keys on (see
     * hg_history.h), and two series can share a name. The episodeNumber comes from upstream's own numbering,
     * which is what `episodeLabel` carries, so it reads the way the episode buttons do. */
    {
        const std::vector<HgHistoryEntry> hist = Store::history();
        for (size_t i = 0; i < hist.size(); i++) {
            if (this->series.id != hist[i].seriesId) continue;

            char mmss[16];
            char line[192];
            auto hms = [](double seconds, char* out, size_t n) {
                if (seconds < 0.0) seconds = 0.0;
                const int total = (int)(seconds + 0.5);
                if (total >= 3600) {
                    snprintf(out, n, "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
                } else {
                    snprintf(out, n, "%d:%02d", total / 60, total % 60);
                }
            };
            char pos[16];
            char dur[16];
            hms(hist[i].position, pos, sizeof pos);
            hms(hist[i].duration, dur, sizeof dur);

            if (hist[i].duration > 0.0) {
                snprintf(line, sizeof line, "%s %s  %s / %s", hg_tr(HG_T_WATCHED_TO), hist[i].episodeLabel, pos,
                         dur);
            } else {
                /* A record with no duration is one that was opened and left before mpv reported one: the episode
                 * is worth naming, the position is not worth claiming. */
                snprintf(line, sizeof line, "%s %s", hg_tr(HG_T_WATCHED_TO), hist[i].episodeLabel);
            }
            (void)mmss;

            this->histLabel = new brls::Label();
            this->histLabel->setFontSize(18.0f);
            this->histLabel->setMarginBottom(10.0f);
            this->histLabel->setTextColor(brls::Application::getTheme().getColor("brls/text"));
            this->histLabel->setText(line);
            box->addView(this->histLabel);
            hg::logPrintf("DETAIL: history for this series -- episode '%s', %s / %s (from %s) [build125]",
                          hist[i].episodeLabel, pos, dur, Store::historyPath());
            break;
        }
    }

    /* ONE OF THE TWO LAYOUTS, decided once, here. Both exist before the data for the
     * reason home_activity.cpp documents at length: focus is assigned when the
     * activity is pushed, so something focusable has to be in the tree already. */
    if (episodesAreGrid()) {
        /* ---- THE GROUP SELECTOR, ABOVE THE GRID (the user's design) ---------------------------------------
         *
         * "Group them 20 at a time, only one group open at a time, and drop the previous group's cache." The row
         * is built empty here and filled when the episode list arrives (the number of groups is not known until
         * then), and it sits between the header and the grid so the two read as one control.
         *
         * IT IS MADE OF brls::Button ON PURPOSE: that gives the group buttons the framework's focus handling AND
         * touch for free, which matters because the user asked for touch on this screen's furniture as well. */
        auto* groups = new brls::Box();
        groups->setAxis(brls::Axis::ROW);
        groups->setMarginBottom(10.0f);
        groups->setFocusable(false);
        box->addView(groups);
        this->groupRow = groups;

        auto* grid = new EpisodeGridView();
        grid->setGrow(1.0f);
        box->addView(grid);
        this->episodeGrid = grid;
        hg::logPrintf("DETAIL: ENTER createContentView; empty episode GRID attached (grouped %d at a time)",
                      HG_EPISODE_GROUP);
    } else {
        auto* frame = new hg::LoggingRecyclerFrame();
        frame->registerCell("EpisodeCell", []() { return EpisodeCell::create(); });
        frame->setGrow(1.0f);
        box->addView(frame);
        this->frame = frame;
        hg::logPrintf("DETAIL: ENTER createContentView; empty episode LIST attached");
    }

    auto* hint = new brls::Label();
    hint->setFontSize(18.0f);
    hint->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    /* Stating the control mapping on screen matters: B and + are global, but
     * "A plays" is specific to this list and there is no other affordance. */
    hint->setText("A plays the highlighted episode    B goes back    + exits");
    hint->setMarginTop(12.0f);
    box->addView(hint);

    DetailActivity* self = this;
    const AliveToken alive = this->alive_;
    std::string sid = this->series.id;

    /* The worker comes from hg_thread (libnx threadCreate), not std::thread:
     * on this toolchain std::thread's constructor throws ENOSYS and aborts the
     * process. See hg_thread.hpp for the on-hardware evidence. */
    hg::logPrintf("DETAIL: ENTER hg::runDetached(fetchEpisodes)");
    bool started = hg::runDetached([self, alive, sid]() {
        hg::logPrintf("DTH: thread started for series %s", sid.c_str());
        std::vector<Episode> got;
        Result r = Client::fetchEpisodes(sid, got);
        hg::logPrintf("DTH: fetchEpisodes returned ok=%d n=%u err='%s'", (int)r.ok,
                      (unsigned)got.size(), r.error.c_str());
        brls::sync([self, alive, got, r]() {
            HG_GUARD_ALIVE(alive);
            hg::logPrintf("DETAIL SYNC: ENTER apply");
            self->applyEpisodes(got, r);
            hg::logPrintf("DETAIL SYNC: OK apply");
        });
    });

    if (!started) {
        /* No worker: fetch inline rather than leaving "Loading episodes..."
         * on screen forever. */
        hg::logPrintf("DETAIL: no worker available -- fetching inline instead");
        std::vector<Episode> got;
        Result r = Client::fetchEpisodes(sid, got);
        this->applyEpisodes(std::move(got), r);
    }
    hg::logPrintf("DETAIL: OK createContentView returning view");

    return hg::withHintBar(box);
}

/* ---- THE GROUP SELECTOR (the user's design: 20 episodes at a time, one group open) ----------------------
 *
 * Built when the episode list arrives, because the number of groups is not known before that. Each button is a
 * brls::Button, which gives it the framework's focus handling AND touch for free -- the user asked for touch on
 * this screen's furniture too, and a hand-rolled box would have needed both written by hand.
 *
 * ACT  =  one click opens that group. `showGroup` replaces the grid's contents, so the group that was open is
 * gone rather than hidden: that is the "clear the previous group's cache" half of their request. */
void DetailActivity::buildGroupSelector() {
    if (!this->groupRow) return;
    this->groupRow->clearViews(true);
    this->groupButtons.clear();

    const int total = (int)this->episodes.size();
    const int groups = (total + HG_EPISODE_GROUP - 1) / HG_EPISODE_GROUP;
    for (int g = 0; g < groups; g++) {
        auto* b = new brls::Button();
        const int first = g * HG_EPISODE_GROUP + 1;
        int last = first + HG_EPISODE_GROUP - 1;
        if (last > total) last = total;
        /* THE BUTTON SHOWS THE FIRST EPISODE NUMBERS, not a range. The user's instruction: "21~40 should be
         * labelled 21, 22, 23, 24 like that, and 41~60 should be 41, 42, 43, 44, and so on."
         *
         * That reads better than "21-40": the numbers look like the episodes the button opens rather than like a
         * page range. Four of them fit the width the range did, and four is a FIXED count so every button in the
         * row is the same size whatever the numbers are. */
        /* THE RANGE IS WHAT THEY ASKED FOR HERE, and my first attempt got it wrong: I had the GROUP buttons
         * show episode numbers, when the instruction was that the group buttons keep saying "21-40" and the
         * numbered CELLS BELOW them should carry the real episode numbers. Corrected back; the cells are
         * fixed through the offset that setEpisodes now takes. */
        char label[32];
        snprintf(label, sizeof label, "%d-%d", first, last);
        b->setText(label);
        b->setMarginRight(8.0f);
        DetailActivity* self = this;
        /* ---- ONE LAMBDA FOR BOTH THE CLICK AND THE A PRESS, AND IT MOVES THE CURSOR NOWHERE (build169a) --
         *
         * THE TWO HANDLERS WERE ALREADY THE SAME THREE LINES, so this is the same behaviour written once.
         * What is new is the reason they can share it at all: openGroupRowButton does showGroup and then
         * puts the cursor ON THIS BUTTON, which is where it already was for the A press and which is what
         * makes the row scroll for the click. See the note on that lambda for why a tap must not throw the
         * cursor down into the grid.
         *
         * The cursor landing back on the button is also what keeps the user's "B anywhere on the group
         * buttons" report fixed: the button stays focused, so B still resolves up through this row to the
         * activity's registration -- see onContentAvailable. */
        auto openGroupRowButton = [self, b, g](brls::View*) {
            self->showGroup(g, false);
            /* AFTER showGroup, so this is the last word on the cursor, exactly as in the L/R path. It is
             * also what scrolls the row: borealis scrolls a focused view into view, and a ten-button row
             * for a 200-episode series does not fit the line. */
            brls::Application::giveFocus(b);
            hg::logPrintf("DETAIL: group %d chosen from the row -- cursor stays on the row [build169a]", g + 1);
            return true;
        };
        b->registerClickAction(openGroupRowButton);
        b->registerAction(hg_tr(HG_T_OK), brls::ControllerButton::BUTTON_A, openGroupRowButton);
        this->groupRow->addView(b);
        this->groupButtons.push_back(b);
    }
    hg::logPrintf("DETAIL: %d episodes -> %d groups of up to %d (only one group is ever built) [build85]",
                  total, groups, HG_EPISODE_GROUP);
}

/* ---- THE ONE PLACE THAT DECIDES WHICH GROUP IS OPEN ---------------------------------------------------
 *
 * The signature is the header's and stays the header's (`void showGroup(int, bool)`); the "which cell does
 * the cursor land on" half of the job is done by the callers, which call this and then call the grid's
 * focusCell. That split is deliberate rather than a workaround: it is what keeps `initial` saying only the
 * thing it is really about -- "this is the first group of a freshly opened screen" -- while the three
 * different reasons to change group disagree about the cursor.
 *
 * `initial == true`  the data just arrived, cursor on the grid, unchanged from before;
 * `initial == false` the user asked for a group: via the row (a click, cursor stays where it is -- see the
 *                    note on the row's giveFocus below) or via L/R (a key, cursor goes to cell 0, done by
 *                    the caller). */
void DetailActivity::showGroup(int index, bool initial) {
    if (!this->episodeGrid) return;
    const int total = (int)this->episodes.size();
    const int groups = (total + HG_EPISODE_GROUP - 1) / HG_EPISODE_GROUP;
    /* THE BOUNDARY THE USER ASKED ABOUT: "at the first group L and at the last group R must not go out of
     * bounds and must not crash". Refused here as well as at the key, so that a click or a stale index
     * cannot get through either. Loud in the log, because a key that does nothing and a key that is not
     * bound look identical on screen and completely different here. */
    if (index < 0 || index >= groups) {
        hg::logPrintf("DETAIL: group %d refused -- the series has %d group(s), so there is nothing that way "
                      "[build169]",
                      index + 1, groups);
        return;
    }

    const int first = index * HG_EPISODE_GROUP;
    int last = first + HG_EPISODE_GROUP;
    if (last > total) last = total;

    /* THE SLICE IS COPIED, because the grid keeps a pointer to it. */
    this->groupItems.assign(this->episodes.begin() + first, this->episodes.begin() + last);
    this->groupIndex = index;
    this->episodeGrid->setEpisodes(&this->groupItems, &this->series, &this->episodes, first);

    /* Mark the open group on its button, so the row says which one is showing. THIS IS THE "SELECTED STATE"
     * OF THE CHIPS (build169a): the row has no focus ring of its own to rely on -- see the giveFocus note
     * below -- so alpha IS the selected state, and the two must be changed together or the row will name
     * the group you left. */
    for (size_t i = 0; i < this->groupButtons.size(); i++) {
        if (!this->groupButtons[i]) continue;
        this->groupButtons[i]->setAlpha((int)i == index ? 1.0f : 0.6f);
    }

    hg::logPrintf("DETAIL: group %d/%d open -- episodes %d..%d (%u cells built, %u cells in the series) [build85]",
                  index + 1, groups, first + 1, last, (unsigned)this->groupItems.size(), (unsigned)total);

    if (initial) brls::Application::giveFocus(this->episodeGrid);
}

/* WHERE THE USER LEFT OFF, AS AN INDEX INTO `episodes`, OR -1 (build169b).
 *
 * MATCHED ON THE EPISODE ID, NOT ON THE NUMBER, AND THAT IS THE WHOLE POINT. The history row carries both
 * (`episodeId` and `episodeLabel`), and only one of them is safe to join on:
 *
 *   * `episodeLabel` is the 1-based position this app printed when it recorded the row
 *     (player_activity.cpp:115 writes `this->episodeIndex`). It is convenient and it is a number, which is
 *     exactly what makes it tempting -- and it is wrong, because it is only meaningful FOR THE LIST IT WAS
 *     RECORDED FROM. The notes on setEpisodes above are the record of a bug caused by treating an index as
 *     an episode number; joining two lists on a position would rebuild that bug in a place where it is much
 *     harder to see.
 *   * `episodeId` is the bare id upstream sent (player_activity.cpp:97 -- `episodes[i].id`), and the ids in
 *     the list fetched for THIS screen are the same strings. So the join is on the thing upstream actually
 *     uses to identify an episode.
 *
 * Given that, the label is not consulted at all. It is read in only one place -- the "watched to episode N"
 * line -- where showing the number the user saw before is the entire job.
 *
 * NEWEST FIRST, SO THE FIRST MATCH IS THE ANSWER. hg_history keeps the list newest-first and
 * Store::history() hands it over in that order, so the first row for this series is the most recent one and
 * the loop stops there. (hg_history_find_series does the same search in C; it is not used here because it
 * works on the flat array the store owns rather than on the vector it hands out, and this loop needs to walk
 * the SAME copy the id lookup uses.)
 *
 * AN EMPTY seriesId OR episodeId MATCHES NOTHING. An empty id is not "any id": without this, a history row
 * written before the series id was known would claim to match a series that also has no id. */
static int lastWatchedEpisodeIndex(const std::string& seriesId, const std::vector<Episode>& episodes) {
    if (seriesId.empty() || episodes.empty()) return -1;

    const std::vector<HgHistoryEntry> hist = Store::history();
    for (size_t i = 0; i < hist.size(); i++) {
        if (seriesId != hist[i].seriesId) continue;
        if (hist[i].episodeId[0] == '\0') continue;

        for (size_t j = 0; j < episodes.size(); j++) {
            if (episodes[j].id == hist[i].episodeId) return (int)j;
        }
        /* A ROW FOR THIS SERIES WHOSE EPISODE IS NOT IN THE LIST. Not an error and not a reason to keep
         * looking at older rows: the newest row is the user's most recent position, and if upstream no
         * longer offers that episode then the honest answer is "cannot place it", not "guess from an older
         * one". Say so, because the alternative -- silently opening at episode 1 -- is indistinguishable
         * from the feature not existing. */
        hg::logPrintf("DETAIL: history says episode id '%s' for this series, but the list just fetched "
                      "(%u episodes) does not contain it -- opening at episode 1 [build169b]",
                      hist[i].episodeId, (unsigned)episodes.size());
        return -1;
    }
    return -1;
}

void DetailActivity::applyEpisodes(std::vector<Episode> got, const Result& r) {
    if (!this->headerLabel) {
        hg::logPrintf("DETAIL apply: view already gone; nothing to do");
        return;
    }
    if (!r.ok) {
        this->headerLabel->setText("Episodes failed: " + r.error);
        brls::Application::notify("Episodes failed: " + r.error);
        hg::logPrintf("DETAIL apply: reported failure '%s'", r.error.c_str());
        return;
    }

    this->episodes = std::move(got);
    char b[96];
    snprintf(b, sizeof b, "%u episodes", (unsigned)this->episodes.size());
    this->headerLabel->setText(b);
    hg::logPrintf("DETAIL apply: %u rows", (unsigned)this->episodes.size());

    if (this->episodes.empty()) {
        hg::logPrintf("DETAIL apply: 0 rows -- data source NOT attached");
        this->headerLabel->setText("This series returned 0 episodes");
        return;
    }

    if (this->episodeGrid) {
        /* ---- ONLY ONE GROUP OF EPISODES IS EVER BUILT (the user's design, and it is a crash fix) ----------
         *
         * MEASURED ON HARDWARE, in their words: "on the episode page, with more than 200 episodes, it crashes
         * every time on the way out -- group them 20 at a time, only one group open at a time, and drop the
         * previous group's cache. It is probably opening too many boxes at once, and that would avoid it."
         *
         * They diagnosed it correctly. The grid used to be handed the WHOLE list, so a 200-episode series built
         * two hundred focusable buttons -- each a brls view with a label, a border and a click recogniser -- and
         * destroyed all of them when the screen was popped. Every one of those is a heap allocation and a deko3d
         * draw command, and the teardown happened in a single frame.
         *
         * So the episodes are cut into groups of HG_EPISODE_GROUP (20). A selector row above the grid opens one
         * group at a time, and opening a group REPLACES the previous group's cells rather than adding to them --
         * `setEpisodes` rebuilds the grid and borealis frees what it replaces. At most 20 cells exist at any
         * moment, whatever the series has. */
        this->buildGroupSelector();

        /* ---- #5: OPEN THE GROUP THAT HOLDS THE LAST WATCHED EPISODE, AND PUT THE CURSOR ON IT (build169b)
         *
         * THE USER'S REPORT: "for a series with a viewing history, the episode page should open on the
         * episode I last watched." The history was ALREADY being read on this screen -- the "watched to
         * episode N" line just above the episode picker (see createContentView, build125) -- and it was
         * only ever turned into a LABEL. This is that same lookup finally driving the cursor.
         *
         * WHY IT IS DONE HERE AND NOT EARLIER. The episode ids do not exist until `fetchEpisodes` has
         * answered, and the history is keyed by episode id; so this is the first moment at which "which
         * episode is the one from the history" is a question with an answer. That is also why the lookup is
         * not cached in a member of the activity: at the time the history line is written there is nothing
         * to match it against.
         *
         * REUSING THE STORE'S IN-MEMORY COPY, NOT RE-READING THE FILE. Store::history() returns the list
         * hg_store already loaded and holds under its own mutex (hg_store.cpp:135 -- it takes the lock and
         * copies the in-memory vector; it does not touch the SD card). So this costs no I/O on the path
         * that runs on every episode-page open, and it cannot disagree with the line printed above.
         *
         * NO HISTORY -> NOTHING HAPPENS HERE, and that is the user's requirement verbatim: "without a
         * history the behaviour must be completely unchanged". The `showGroup(0, true)` below is the old
         * code path, untouched, and it is reached by exactly the same condition that reached it before --
         * lastWatched == -1 whenever no row for this series exists, and also whenever the row's id is not
         * in the list upstream returned just now (a series whose episodes were re-issued, in which case
         * jumping to a guessed episode would be worse than staying at the top). */
        const int lastWatched = lastWatchedEpisodeIndex(this->series.id, this->episodes);
        if (lastWatched >= 0) {
            const int k = lastWatched / HG_EPISODE_GROUP;             /* which group holds it */
            const int cell = lastWatched - k * HG_EPISODE_GROUP;      /* and which cell inside that group */
            /* THE GROUP IS SWITCHED FIRST, and that ordering is the requirement rather than a preference:
             * a cursor put on a cell of a group that is not built is a cursor on a view that does not
             * exist -- showGroup is what builds the cells. See the same ordering note in onContentAvailable's
             * L/R handler. */
            hg::logPrintf("DETAIL: focus -> episode %d (last watched, from history) [build169b]",
                          lastWatched + 1);
            this->showGroup(k, false);
            this->episodeGrid->focusCell(cell);
            hg::logPrintf("DETAIL: history episode %d is group %d/%d, cell %d -- opened there instead of "
                          "episode 1 [build169b]",
                          lastWatched + 1, k + 1,
                          ((int)this->episodes.size() + HG_EPISODE_GROUP - 1) / HG_EPISODE_GROUP, cell + 1);
            return;
        }
        this->showGroup(0, true);
        return;
    }

    if (!this->frame) {
        hg::logPrintf("DETAIL apply: no frame; nothing to attach to");
        return;
    }
    hg::logPrintf("DETAIL apply: ENTER attach %u rows", (unsigned)this->episodes.size());
    this->frame->setDataSource(new EpisodeDataSource(&this->episodes, &this->series));
    this->frame->reloadData();

    /* See home_activity.cpp: the frame was empty at push time, so focus has to
     * be placed on it now that there are focusable cells. */
    brls::Application::giveFocus(this->frame);
    hg::logPrintf("DETAIL apply: rows attached, focus re-given to the list");
}

} // namespace hg
