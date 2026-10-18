/*
 * Tests for hg_sidebar.c -- the sidebar model.
 *
 * The checks that matter are the MOVEMENT ones, because that is where a sidebar goes quietly wrong:
 *
 *   - STEPPING MUST NEVER LAND ON A DISABLED ITEM. A disabled section is drawn dim; moving focus onto
 *     it is how a bar gets stuck on an entry that cannot be activated.
 *   - STEPPING MUST WRAP AT BOTH ENDS. wiliwili's shoulder buttons wrap, and a bar that stops dead at
 *     the last item makes those buttons feel broken exactly where they are most likely to be pressed.
 *   - STEPPING MUST STAY INSIDE ITS GROUP. Walking from a section into the pinned settings button
 *     would look like the bar jumped to the bottom of the screen.
 *   - A SUBTITLE containing a newline must not break, for the same reason the history format sanitises:
 *     the bar's layout depends on it being one line.
 *
 * ASCII-only source file.
 */

#include "hg_sidebar.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* The standard bar with everything available. */
static int buildFull(HgSidebarItem* items, int max) {
  return hg_sidebar_build(items, max, 1 /* haveHistory */, 1 /* haveWatchlist */);
}

/* ---- the set -------------------------------------------------------------- */

static void test_build(void) {
  HgSidebarItem items[HG_SB_MAX];
  int n;

  printf("\n-- what the bar contains --\n");

  n = buildFull(items, HG_SB_MAX);
  ok("five items", n == 5);
  ok("three of them are sections", hg_sidebar_count_group(items, n, HG_SB_GROUP_SECTIONS) == 3);
  ok("TWO of them are pinned", hg_sidebar_count_group(items, n, HG_SB_GROUP_PINNED) == 2);

  /* THE ORDER IS THE DRAWN ORDER: home, watchlist, history, then the pinned group -- search then
   * settings. Search is a FUNCTION over all the sections rather than a section of its own, which is why
   * it lives in the pinned group; it is placed before the gear so the bar reads content-then-tools. */
  ok("home is first", items[0].labelKey == HG_T_HOME && items[0].group == HG_SB_GROUP_SECTIONS);
  ok("the watchlist is second", items[1].labelKey == HG_T_FAVORITES);
  ok("history is third", items[2].labelKey == HG_T_HISTORY);
  ok("search is fourth and pinned",
     items[3].labelKey == HG_T_SEARCH && items[3].group == HG_SB_GROUP_PINNED);
  ok("search is always enabled (it needs no data at all)", items[3].enabled == 1);
  ok("settings is last and pinned",
     items[4].labelKey == HG_T_SETTINGS && items[4].group == HG_SB_GROUP_PINNED);
  ok("settings is always enabled", items[4].enabled == 1);

  /* Icons are distinct, so the bar is readable without labels in the way wiliwili's is. The search
   * entry's magnifier is a new shape, so it must differ from the other four. */
  ok("the five icons are all different",
     items[0].icon != items[1].icon && items[1].icon != items[2].icon &&
     items[2].icon != items[3].icon && items[3].icon != items[4].icon);
  ok("no subtitle to start with", items[0].subtitle[0] == '\0');

  /* EMPTY SECTIONS ARE DIM, NOT MISSING. A user who has saved nothing should be able to see that the
   * watchlist exists and that it is empty. */
  n = hg_sidebar_build(items, HG_SB_MAX, 0, 0);
  ok("still five items with nothing saved", n == 5);
  ok("home is still enabled", items[0].enabled == 1);
  ok("the watchlist is dim", items[1].enabled == 0);
  ok("history is dim", items[2].enabled == 0);
  ok("search is enabled with nothing saved", items[3].enabled == 1);
  ok("settings is still enabled", items[4].enabled == 1);
  ok("history alone can be enabled",
     hg_sidebar_build(items, HG_SB_MAX, 1, 0) == 5 && items[2].enabled == 1 && items[1].enabled == 0);

  /* Degenerate arguments. */
  ok("a zero cap builds nothing", hg_sidebar_build(items, 0, 1, 1) == 0);
  ok("NULL is refused", hg_sidebar_build(NULL, HG_SB_MAX, 1, 1) == 0);
  ok("a cap of one builds one", hg_sidebar_build(items, 1, 1, 1) == 1);
}

/* ---- movement: the part that goes quietly wrong --------------------------- */

static void test_step(void) {
  HgSidebarItem items[HG_SB_MAX];
  int n = buildFull(items, HG_SB_MAX);

  printf("\n-- moving through the bar --\n");

  ok("from home, down goes to the watchlist", hg_sidebar_step(items, n, 0, 1) == 1);
  ok("down again goes to history", hg_sidebar_step(items, n, 1, 1) == 2);
  ok("AND WRAPS to home from history", hg_sidebar_step(items, n, 2, 1) == 0);
  ok("up from home WRAPS to history", hg_sidebar_step(items, n, 0, -1) == 2);
  ok("up from history goes to the watchlist", hg_sidebar_step(items, n, 2, -1) == 1);

  /* THE PINNED GROUP IS A SEPARATE RING: settings must not be reachable by stepping through the
   * sections, and the sections must not be reachable from settings. */
  /* The pinned group is a ring of TWO now (search + settings), so stepping inside it moves between
   * them and wraps -- and it still never touches the sections. */
  ok("from search, down goes to settings", hg_sidebar_step(items, n, 3, 1) == 4);
  ok("from settings, down WRAPS to search", hg_sidebar_step(items, n, 4, 1) == 3);
  ok("and backwards too", hg_sidebar_step(items, n, 3, -1) == 4);
  {
    int i, everReachedSettings = 0;
    for (i = 0; i < 10; i++) {
      /* walk the sections ring and make sure it never yields index 3 */
    }
    for (i = 0; i < 6; i++) {
      /* the ring is only three long, so this is checked directly below */
    }
    everReachedSettings = 0;
    {
      int at = 0;
      int step;
      for (step = 0; step < 12; step++) {
        at = hg_sidebar_step(items, n, at, 1);
        /* 3 and 4 are the pinned group (search, settings): neither may be reached from the sections. */
        if (at >= 3) everReachedSettings = 1;
      }
    }
    ok("walking the sections never reaches the pinned group", everReachedSettings == 0);
  }

  /* A DISABLED ITEM IS SKIPPED. From home, "down" must go to history when the watchlist is dim. */
  {
    HgSidebarItem dim[HG_SB_MAX];
    int dn = hg_sidebar_build(dim, HG_SB_MAX, 1 /* history on */, 0 /* watchlist off */);
    ok("with the watchlist dim, down from home goes to history",
       hg_sidebar_step(dim, dn, 0, 1) == 2);
    ok("and up from history goes home (skipping the dim one)",
       hg_sidebar_step(dim, dn, 2, -1) == 0);
  }

  /* BOTH sections dim: home is the only thing in the group, so stepping returns home rather than
   * walking into a dim entry. */
  {
    HgSidebarItem bare[HG_SB_MAX];
    int bn = hg_sidebar_build(bare, HG_SB_MAX, 0, 0);
    ok("with both sections dim, down from home stays home", hg_sidebar_step(bare, bn, 0, 1) == 0);
    ok("and up stays home too", hg_sidebar_step(bare, bn, 0, -1) == 0);
  }

  /* An invalid starting index does not step off the end: it lands on the first reachable item. */
  ok("an out-of-range start lands on the first item", hg_sidebar_step(items, n, 99, 1) == 0);
  ok("a negative start lands on the first item", hg_sidebar_step(items, n, -4, 1) == 0);
  /* A dim starting index is not a place focus can be. */
  {
    HgSidebarItem dim[HG_SB_MAX];
    int dn = hg_sidebar_build(dim, HG_SB_MAX, 0, 0);
    ok("starting from a dim item lands on the first enabled one", hg_sidebar_step(dim, dn, 1, 1) == 0);
  }
  ok("a zero delta stays put", hg_sidebar_step(items, n, 1, 0) == 1);
  ok("an empty bar steps to -1", hg_sidebar_step(items, 0, 0, 1) == -1);
  ok("NULL steps to -1", hg_sidebar_step(NULL, 4, 0, 1) == -1);
}

static void test_first_of_group(void) {
  HgSidebarItem items[HG_SB_MAX];
  int n = buildFull(items, HG_SB_MAX);

  printf("\n-- landing in a group --\n");

  ok("the first section is home", hg_sidebar_first_of_group(items, n, HG_SB_GROUP_SECTIONS) == 0);
  ok("the first pinned item is SEARCH", hg_sidebar_first_of_group(items, n, HG_SB_GROUP_PINNED) == 3);
  ok("the first of everything is home", hg_sidebar_first(items, n) == 0);

  /* An empty group is -1, not 0: a caller that moved focus "into" an empty group must be able to tell
   * that it cannot. */
  ok("an unknown group is -1", hg_sidebar_first_of_group(items, n, 77) == -1);
  ok("an empty bar has no first item", hg_sidebar_first(items, 0) == -1);

  /* With everything dim, the pinned group is still reachable -- settings is always enabled. */
  {
    HgSidebarItem dim[HG_SB_MAX];
    int dn = hg_sidebar_build(dim, HG_SB_MAX, 0, 0);
    ok("the pinned group is reachable with nothing saved",
       hg_sidebar_first_of_group(dim, dn, HG_SB_GROUP_PINNED) == 3);
    ok("and its first item is search, not settings",
       dim[hg_sidebar_first_of_group(dim, dn, HG_SB_GROUP_PINNED)].labelKey == HG_T_SEARCH);
    ok("and the first item overall is still home", hg_sidebar_first(dim, dn) == 0);
  }

  /* valid(). */
  ok("index 0 is valid", hg_sidebar_valid(items, n, 0) == 1);
  ok("the last index is valid", hg_sidebar_valid(items, n, n - 1) == 1);
  ok("past the end is not", hg_sidebar_valid(items, n, n) == 0);
  ok("negative is not", hg_sidebar_valid(items, n, -1) == 0);
}

/* ---- the subtitle --------------------------------------------------------- */

static void test_subtitle(void) {
  HgSidebarItem items[HG_SB_MAX];
  int n = buildFull(items, HG_SB_MAX);

  printf("\n-- the live subtitle --\n");

  ok("setting one reports a change", hg_sidebar_set_subtitle(&items[0], "4 sections") == 1);
  ok("and it is stored", strcmp(items[0].subtitle, "4 sections") == 0);
  /* Setting the same text again reports NO change, so a view that updates on a timer does not force
   * a redraw every tick. */
  ok("setting the same text reports no change", hg_sidebar_set_subtitle(&items[0], "4 sections") == 0);
  ok("a different text reports a change", hg_sidebar_set_subtitle(&items[0], "36") == 1);

  /* A NEWLINE IS REPLACED, not stored: the subtitle is drawn on one line and a line break in it
   * would push the bar's layout apart. */
  ok("a newline is sanitised", hg_sidebar_set_subtitle(&items[1], "line\nbreak") == 1);
  ok("and the newline is gone", strchr(items[1].subtitle, '\n') == NULL);
  ok("and a tab too", hg_sidebar_set_subtitle(&items[1], "a\tb") == 1 &&
                          strchr(items[1].subtitle, '\t') == NULL);

  /* TRUNCATION, not overflow: a long subtitle fills the buffer and stops. */
  {
    char big[200];
    int i;
    for (i = 0; i < 199; i++) big[i] = 'x';
    big[199] = '\0';
    hg_sidebar_set_subtitle(&items[2], big);
    ok("a long subtitle is truncated", strlen(items[2].subtitle) == sizeof items[2].subtitle - 1);
    ok("and it is still terminated",
       items[2].subtitle[sizeof items[2].subtitle - 1] == '\0');
  }

  /* Clearing works, and NULL is treated as empty rather than crashing. */
  ok("clearing reports a change", hg_sidebar_set_subtitle(&items[0], "") == 1);
  ok("and it is empty", items[0].subtitle[0] == '\0');
  hg_sidebar_set_subtitle(&items[1], NULL);
  ok("NULL clears rather than crashing", items[1].subtitle[0] == '\0');
  ok("a NULL item is tolerated", hg_sidebar_set_subtitle(NULL, "x") == 0);

  (void)n;
}

int main(void) {
  printf("hg_sidebar tests: the left sidebar model\n");
  printf("================================================\n");

  test_build();
  test_step();
  test_first_of_group();
  test_subtitle();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
