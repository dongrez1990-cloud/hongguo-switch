// hg_sidebar.h -- the left sidebar, modelled on wiliwili's.
//
// GPL-3.0-only.
//
// WHY THIS FILE IS A MODEL AND NOT A WIDGET. wiliwili implements its sidebar as
// `view/auto_tab_frame.hpp`, and the obvious move was to copy that class in. That was tried and it
// FAILS on this toolchain: it pulls in `view/button_refresh.hpp` -> `view/svg_image.hpp` ->
// `<lunasvg.h>`, and this devkitPro has no lunasvg. Its icons are SVG, so the class cannot be
// adopted without adding a library to the toolchain.
//
// So the sidebar is written here, but its STRUCTURE is copied from wiliwili's rather than invented,
// because that structure is the part that makes it feel right:
//
//   * ONE GROUP AT THE TOP, which is the content sections. wiliwili calls these HomeTab /
//     DynamicTab / MineTab and gives each a large icon plus a live-updating subtitle
//     (a small piece of text under the icon showing something about that section).
//   * ONE GROUP AT THE BOTTOM, pinned, holding functions rather than sections -- in wiliwili
//     that is the inbox and the settings button, and it is a SEPARATE group, not more tabs.
//   * Each item carries TWO things that a plain list item does not: an icon that changes when the
//     item is the active one, and a "subtitle" that the owning view can update at any time. The
//     subtitle is what makes the bar informative instead of decorative, and it is why this model
//     stores a string per item rather than just a label.
//
// WHAT IS DELIBERATELY NOT HERE: any drawing, any borealis type, any focus logic. The model is a
// plain C array with pure functions so the host suite can check it -- the geometry decisions are the
// ones that go wrong silently (an off-by-one in "which item is active", a move that walks off the
// end of the list), and they are exactly the decisions that are testable without a console.
//
// ASCII-only source file.

#ifndef HG_SIDEBAR_H
#define HG_SIDEBAR_H

#include "hg_lang.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which group an item belongs to. The two behave differently: the sections group is the main
 * navigation, the pinned group sits at the bottom and is not reached by scrolling. */
typedef enum {
  HG_SB_GROUP_SECTIONS = 0,
  HG_SB_GROUP_PINNED = 1
} HgSidebarGroup;

/* How to draw an item's icon, WITHOUT an SVG rasteriser.
 *
 * wiliwili uses SVG files; we do not have lunasvg, so each item says which of a small set of shapes
 * it wants and the view draws that shape with nanovg. A named shape rather than a raw drawing keeps
 * the model free of coordinates, and it keeps the set of icons visible in one place -- a new section
 * has to pick one of these rather than growing its own drawing code. */
typedef enum {
  HG_SB_ICON_HOME = 0,     /* a house */
  HG_SB_ICON_STAR,         /* the watchlist */
  HG_SB_ICON_CLOCK,        /* history */
  HG_SB_ICON_GEAR,         /* settings */
  HG_SB_ICON_SEARCH,       /* a magnifier: the search screen */
  HG_SB_ICON_COUNT
} HgSidebarIcon;

/* One sidebar entry. `labelKey` is an hg_lang key, so the bar follows the interface language --
 * wiliwili uses i18n keys in XML for the same reason. `subtitle` is the small live text under the
 * label; it is a plain buffer because it is written by whichever view owns the section (for example
 * "3 sections" once the home fetch lands) and read while drawing. */
typedef struct {
  int labelKey;            /* an HG_T_* key */
  int icon;                /* HgSidebarIcon */
  int group;               /* HgSidebarGroup */
  int enabled;             /* 0 = drawn dim and not focusable (a section with nothing behind it) */
  char subtitle[24];       /* live text, "" for none */
} HgSidebarItem;

/* How many sections the bar can hold. Four, because that is what the app has: home, watchlist,
 * history, settings. A fifth would need a real reason. */
#define HG_SB_MAX 8

/* Build the standard set of items for the app and return how many there are.
 *
 * `haveHistory` and `haveWatchlist` exist because a section with no data behind it should not
 * pretend: with no history file the History item is built DISABLED, which the view draws dim and
 * skips when moving focus. That is the difference between "the feature is empty" and "the feature
 * looks broken", and it is the reason `enabled` is in the model rather than being decided while
 * drawing. */
int hg_sidebar_build(HgSidebarItem *items, int max, int haveHistory, int haveWatchlist);

/* How many items are in one group. Used to size the pinned block and to know where the sections
 * end when moving focus between groups. */
int hg_sidebar_count_group(const HgSidebarItem *items, int n, int group);

/* The index of the item `delta` steps from `from`, WITHIN THE SAME GROUP, wrapping at the ends.
 *
 * WRAPPING IS THE POINT. wiliwili's LB/RB wraps between sections, and a bar that stops dead at the
 * last item makes the shoulder buttons feel broken at exactly the two places a user is most likely
 * to press them (the first and last section). It also means there is no "nothing happened" state to
 * explain. Disabled items are SKIPPED, so a wrap cannot land on one.
 *
 * Returns `from` when the group has no reachable other item, which is the honest answer for a group
 * of one -- moving is impossible rather than moving to itself. */
int hg_sidebar_step(const HgSidebarItem *items, int n, int from, int delta);

/* The first selectable item of `group`, or -1 when that group is empty or entirely disabled.
 * This is what a "move to the pinned group" key needs: it should land on the FIRST pinned item, not
 * on whatever the last position was. */
int hg_sidebar_first_of_group(const HgSidebarItem *items, int n, int group);

/* The first selectable item overall, or -1. The bar needs a starting focus even when the sections
 * group is somehow empty. */
int hg_sidebar_first(const HgSidebarItem *items, int n);

/* 1 when `index` is a valid, enabled item. The view calls this before trusting an index it has
 * carried across a rebuild. */
int hg_sidebar_valid(const HgSidebarItem *items, int n, int index);

/* Replace an item's subtitle. Truncates rather than failing, and sanitises newlines to spaces: the
 * subtitle is drawn on ONE line under the label, and a subtitle with a line break in it would push
 * the whole bar's layout apart. Returns 1 when the text was actually different. */
int hg_sidebar_set_subtitle(HgSidebarItem *item, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* HG_SIDEBAR_H */
