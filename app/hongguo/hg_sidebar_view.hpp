// hg_sidebar_view.hpp -- the left sidebar, drawn with nanovg.
//
// GPL-3.0-only.
//
// THE MODEL IS SEPARATE ON PURPOSE. hg_sidebar.h holds what the bar CONTAINS and how focus MOVES,
// with no borealis type and no drawing in it, which is why the host suite can check the movement
// rules (never land on a disabled item, wrap at both ends, stay inside the group). This file is the
// other half: it owns the views and paints them.
//
// WHY THE ICONS ARE DRAWN IN CODE RATHER THAN LOADED. wiliwili's sidebar uses SVG files, which is
// why its class cannot be adopted here: it needs lunasvg, which this devkitPro does not have. So each
// icon is a few nanovg paths. That is more code than calling setImageFromRes, and it is the only
// option -- but it has one real advantage: the icons scale cleanly to any ui.scale without a second
// asset per size.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO: it does not push activities, does not own a screen, and
// does not decide what a section means. It reports "the user activated item N" through a callback, so
// the same bar can drive the settings screen today and the home screen later without either of them
// knowing how the bar is drawn.
//
// ASCII-only source file.

#ifndef HG_SIDEBAR_VIEW_HPP
#define HG_SIDEBAR_VIEW_HPP

#include <functional>
#include <string>

#include <borealis.hpp>

#include "hg_sidebar.h"

namespace hg {

/* The item counts, as a struct rather than three loose ints: the bar needs them to dim a section
 * that has nothing behind it, and passing them separately invites a call site where one is forgotten
 * -- which would show a section as available when it is not. */
struct SidebarContent {
    bool haveHome = true;
    bool haveWatchlist = false;
    bool haveHistory = false;
};

class SidebarView : public brls::Box {
  public:
    SidebarView();
    ~SidebarView() override;

    /* Rebuild the items. Called once at construction and again when the content flags change (the
     * watchlist becomes non-empty, say). Keeping it explicit rather than doing it in a timer is what
     * makes "why is that item dim" answerable. */
    void rebuild(const SidebarContent& content);

    /* Move focus by `delta` items inside the CURRENT group, wrapping. This is what the shoulder
     * buttons are wired to; it is deliberately not "move by one row", because the two groups are two
     * separate rings. */
    void moveFocus(int delta);

    /* Jump to the first item of a group. Used to leave the sections ring for the pinned one. */
    void focusGroup(HgSidebarGroup group);

    /* Which item is active, or -1. The active item is drawn highlighted and is NOT the same thing as
     * "has focus": the bar can show the current SECTION while focus is somewhere else entirely. */
    void setActive(int index);
    int getActive() const { return this->activeIndex; }

    /* What to do when an item is activated (A, or a tap). */
    void setOnActivate(std::function<void(int)> cb) { this->onActivate = std::move(cb); }

    /* The label of an item, for a screen that wants to name the section it is showing. */
    std::string labelOf(int index) const;

    /* How many items the bar holds, and WHICH item is which, both for a screen that has to decide
     * what activating an item means.
     *
     * THE KEY, NOT THE POSITION, and that is the point: an owner that switched on `index` would be
     * encoding the bar's order into the screen that uses it, and the two would silently disagree the
     * day an item is inserted. `labelKeyOf` also lets a screen find an item it has something to say
     * about without knowing where it sits. */
    int itemCount() const { return this->count; }
    int labelKeyOf(int index) const;

    /* Update an item's small live line under the label -- wiliwili's `setSubtitle`. Returns 1 when the
     * text actually changed, so a caller can skip a redraw. */
    int setSubtitle(int index, const std::string& text);

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;
    void onLayout() override;

  private:
    /* One row: a focusable Box holding an icon area and a label. It reports focus so the bar can keep
     * its highlight in step without polling. */
    class Item;

    void layoutItems();
    Item* itemAt(int index);

    HgSidebarItem items[HG_SB_MAX];
    /* THE VIEW FOR EACH MODEL INDEX, filled while rebuilding.
     *
     * Not a positional walk of the children: the drawn order is [sections..., spacer, pinned...],
     * which happens to match the model's order today and would stop matching the moment the model
     * puts a pinned item before a section. An index -> view table says what it means and cannot drift
     * from the drawing code. */
    Item* views[HG_SB_MAX] = {};
    int count = 0;
    int activeIndex = 0;
    std::function<void(int)> onActivate;

    /* The bar's HEIGHT in logical pixels, and the layout changed so that this is what matters.
     *
     * MEASURED TWICE, and the second measurement is why this bar is at the TOP and horizontal. The
     * first version was a left column 120 px wide (icon beside label); at 720p that took the poster
     * grid from 8 columns to 7, which pushed each tier's eighth card -- the "more" card -- onto a
     * second row. It was narrowed to 72 px with the icon OVER the label, and the user's answer was:
     * "还是影响排版，更多还在第二排，把它放到顶部去，横排摆放".
     *
     * A horizontal bar costs the grid NOTHING horizontally: the grid keeps all 1232 px (1280 minus the
     * page padding) and therefore its 8 columns, whatever the bar contains. The cost moves to the
     * vertical, where there is room -- the header row is above it and the grid is ~600 px tall.
     *
     * 54 px = 8 px padding each side + a 26 px icon + room for the label's line box. */
    static constexpr float HEIGHT = 54.0f;
};

} // namespace hg

#endif
