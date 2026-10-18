/*
 * Home activity: the home page's tiers and cards, in a poster grid or a list.
 *
 * GPL-3.0-only.
 *
 * WHAT IS SHOWN. hg_api_parse_home_sections() is measured to yield 4 named tiers of 9
 * cards (36 cards, after de-duplication WITHIN each tier), and each card carries a
 * title, a cover URL, a genre-tag string and an episode-count badge.
 *
 * TWO LAYOUTS, AND WHY BOTH STILL EXIST.
 *
 * The screen shipped as a LIST (one card per row, cover on the left) and that version
 * is the one that has been verified on hardware. wiliwili's home is a poster grid, and
 * matching it is what the work is for -- so the grid is now the DEFAULT and the list
 * is kept, because the rule this project follows for every unverified change to a
 * screen that already works is that it must ship with a way back that costs no
 * rebuild. That way back is one line:
 *
 *   sdmc:/switch/hongguo.cfg    home.layout=list
 *
 * (An earlier version of this header claimed covers are not drawn at all. That has
 * been false since the image loader landed. The note is corrected rather than
 * deleted, because the wrong version was quoted in the handover document once.)
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hg_alive.hpp"
#include "hg_client.hpp"

namespace hg {

/* THE CEILING ON HOW MANY CELLS A POSTER GRID MAY BUILD -- the user's instruction: "in the search results and the
 * 更多 pages too, if there are too many, then after however many pages make the earlier ones static; the principle
 * is to use as few resources as possible and never crash."
 *
 * IT IS A SANITY STOP RATHER THAN A PAGE LIMIT: the user first asked for a bound and then asked for the endless
 * scroll back, and their later instruction wins. 4000 cells is 166 pages -- unreachable in practice -- and the real
 * cost is handled where it was: the cover fetches back off instead of stopping (see tickCoverRetries). */
#define HG_POSTER_GRID_MAX_CELLS 4000

class HomeGridView;
class SidebarView;

/* THE HOME PAGE'S POSTER GRID, HANDED TO OTHER SCREENS.
 *
 * WHY THIS EXISTS. The search screen and the category screen both need what this grid already is: a
 * poster grid whose column count comes from hg_grid_layout (tested on the host), whose UP/DOWN answers
 * come from hg_grid_row_step (also tested), whose focus is followed by the framework's own hook, and
 * whose covers are loaded through the image loader's view locks. The alternative was a second grid in
 * another file, and this project has already paid for "a second implementation that can disagree with
 * the first" more than once.
 *
 * `rows` is the same row model the home page uses -- a heading row, or a card row carrying a Series --
 * so a caller with a flat list passes card rows only, and gets the home page's grid. `onCard` is
 * called when a poster is activated, with the series; when it is empty the home page's own behaviour
 * (open the detail page) is used.
 *
 * The caller owns the returned view and must add it to a laid-out parent: the grid's column count is
 * computed from its own width, so it needs a width to have a layout at all. */
brls::View* makePosterGrid(std::vector<HomeRow> rows,
                           std::function<void(const Series&)> onCard = nullptr);

/* Re-fill a grid made by makePosterGrid. The class itself stays private to home_activity.cpp, so a
 * screen that shows a grid it does not own says "these are my rows now" through this instead of
 * knowing the type. Passing anything else is a programming error and is logged rather than
 * dereferenced. */
void setPosterGridRows(brls::View* grid, std::vector<HomeRow> rows);

/* ADD ROWS TO A GRID THAT IS ALREADY SHOWING SOME, without recreating the cells that are on screen.
 *
 * WHY THIS IS NOT setPosterGridRows(allTheRows): a recreated cell starts with an empty cover slot, so
 * rebuilding would re-fetch every cover already loaded -- page 2 would re-download page 1's pictures,
 * page 3 would re-download both, and a 34-page category would make thousands of requests for images it
 * already had. This appends.
 *
 * The grid's layout is recomputed for the taller content, so the caller can append and forget. */
void appendPosterGridRows(brls::View* grid, std::vector<HomeRow> rows);

/* CALLED WHEN THE GRID IS SCROLLED NEAR ITS BOTTOM -- how a screen loads the next page without a "load
 * more" button. It fires repeatedly while the user sits at the bottom (a frame runs whether or not
 * anything changed), so the callback is expected to be idempotent: check "already loading" and "no more
 * pages" itself. It does NOT fire when the content fits on screen (there is no bottom to reach). */
void setPosterGridNearBottom(brls::View* grid, std::function<void()> cb);

/* Loads the home page on a worker thread and reports progress/errors through
 * borealis' notification system, because both the page fetch and the JSON walk
 * are slow enough to be visible on a console. */
class HomeActivity : public brls::Activity {
  public:
    HomeActivity();
    ~HomeActivity() override;

    brls::View* createContentView() override;

    /* Input on this screen. Registered on the frame rather than globally so the
     * settings key fires only while this activity is on top. */
    void onContentAvailable() override;

  private:
    /* Applies a finished home fetch: header text, then the grid or the list. It runs
     * on the main thread in BOTH fetch modes (the async one hands it over through
     * brls::sync), so this is the single place the success/failure policy lives --
     * and the single place either layout is populated. It is also the single place a
     * REFRESH lands, because reloadHome() calls it too. */
    void applyHome(std::vector<HomeRow> got, const Result& r);

    /* Re-run the home fetch and apply the result. Bound to the header's refresh
     * button. Guarded against a second press while one is in flight -- two concurrent
     * fetches would race to install their rows, and the loser would win for no reason
     * the user could see. */
    void reloadHome();

    /* A SHORT LIVE LINE UNDER THE BAR'S 主页 ITEM -- where this screen's status lives now that the header
     * row is gone (the "N cards in N sections" count, "refreshing", or a failure). It is the sidebar
     * model's own `subtitle` feature, which is what that field was built for; the item is found by its
     * language key rather than by position. No-op when the bar is missing (home.sidebar=no). */
    void setHomeSubtitle(const std::string& text);

    /* THE LEFT SIDEBAR (hg_sidebar_view), or null when home.sidebar=no.
     *
     * It is built by createContentView and populated in onContentAvailable, and it is a SIBLING of
     * the grid inside a ROW box -- which is the whole integration: with the two side by side, the
     * framework's own walk moves focus between them (LEFT/RIGHT across the row) and up and down
     * inside whichever one has it. No key is intercepted for this, and neither the grid nor the bar
     * moves the focus itself.
     *
     * `home.sidebar=no` in hongguo.cfg takes it out again. That escape hatch exists for the same
     * reason home.layout=list does: this is a change to the shape of a screen that works, and an
     * unverified change to a working screen ships with a way back that costs no rebuild. */
    SidebarView* sidebar = nullptr;

    /* What activating a sidebar item means. Called from that item's click action (A, or a tap) with
     * the model's index, and mapped back to the item by its language key rather than by position --
     * see the implementation. */
    void onSidebarActivate(int index);

    /* Build the sidebar's items and wire its activation callback. Called once from
     * onContentAvailable, and it is a no-op when home.sidebar=no removed the bar. */
    void setupSidebar();

    /* True while a refresh is in flight. */
    bool reloading = false;

    /* The LIST view (home.layout=list). It EXISTS FROM createContentView -- borealis
     * gives focus to the activity's content view when the activity is pushed, and if
     * nothing focusable is in the tree at that moment focus stays on the previous
     * activity (that was a real regression: pressing A on the detail page kept
     * re-opening detail pages because a HomeCell still owned focus). It is created
     * here but given NO data source until rows arrive, which is also what keeps the
     * zero-row RecyclerFrame path unreachable: with a null data source, reloadData()
     * skips the entire block that contains selectRowAt.
     *
     * Exactly one of `frame` and `grid` is non-null; which one is decided once, in
     * createContentView, from home.layout. */
    brls::RecyclerFrame* frame = nullptr;

    /* The GRID view (home.layout=grid, the default). Same reasoning about existing
     * before the data: it is a focusable ScrollingFrame from the start, so focus has
     * somewhere to land, and it simply has no cells until setRows() is called. */
    HomeGridView* grid = nullptr;

    /* THE ROW MODEL IS SHARED BY BOTH LAYOUTS, WHICH IS THE POINT.
     *
     * Measured: the home page carries 4 named tiers of 9 cards each. Both layouts draw
     * one heading followed by that tier's cards, and the row arithmetic lives in C
     * (hg_api_home_rows) where it can be tested on the host -- index errors here are
     * silent, and this project has already paid for one silent index error with a
     * console crash that produced no message.
     *
     * The grid then converts the same rows into columns with hg_grid_layout(), which
     * is ALSO tested on the host (hg_grid_test). So neither layout re-derives the page
     * shape; they both consume one tested description of it. */
    std::vector<HomeRow> rows;
    bool loaded = false;

    /* See hg_alive.hpp: every async callback captures this and checks dead() before touching this screen. */
    AliveToken alive_ = makeAlive();
};

} // namespace hg
