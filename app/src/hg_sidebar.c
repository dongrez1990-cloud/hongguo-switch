// hg_sidebar implementation. See hg_sidebar.h for why this is a model with no drawing in it.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_sidebar.h"

#include <string.h>

/* THE STANDARD SET, in wiliwili's order: content sections first (its HomeTab / DynamicTab / MineTab),
 * then the pinned group (its inbox and settings buttons).
 *
 * The labels are Home, Favorites, History and Settings -- the four the user asked for. There is no inbox
 * entry: this service publishes no message endpoint at all (checked, not assumed), so a section for
 * it could never have content. The user dropped it for the same reason. */
int hg_sidebar_build(HgSidebarItem *items, int max, int haveHistory, int haveWatchlist) {
  int n = 0;
  if (!items || max <= 0) return 0;

  /* ---- the sections group ---- */

  /* HOME, always available: it is the screen the app opens on. */
  if (n < max) {
    memset(&items[n], 0, sizeof items[n]);
    items[n].labelKey = HG_T_HOME;
    items[n].icon = HG_SB_ICON_HOME;
    items[n].group = HG_SB_GROUP_SECTIONS;
    items[n].enabled = 1;
    n++;
  }

  /* THE WATCHLIST. With nothing saved it is built DISABLED rather than hidden: a user who has saved
   * nothing sees a dim entry they can read and understand, instead of walking into an empty page to
   * find out it is empty. It becomes enabled the moment the first series is saved. */
  if (n < max) {
    memset(&items[n], 0, sizeof items[n]);
    items[n].labelKey = HG_T_FAVORITES;
    items[n].icon = HG_SB_ICON_STAR;
    items[n].group = HG_SB_GROUP_SECTIONS;
    items[n].enabled = haveWatchlist ? 1 : 0;
    n++;
  }

  /* HISTORY, on the same rule. */
  if (n < max) {
    memset(&items[n], 0, sizeof items[n]);
    items[n].labelKey = HG_T_HISTORY;
    items[n].icon = HG_SB_ICON_CLOCK;
    items[n].group = HG_SB_GROUP_SECTIONS;
    items[n].enabled = haveHistory ? 1 : 0;
    n++;
  }

  /* ---- the pinned group, at the bottom of the bar ----
   *
   * Settings is a FUNCTION, not a section, which is exactly why wiliwili puts it down here with the
   * inbox button instead of adding it as another tab. Same division here -- and SEARCH belongs to this
   * group for the same reason: it is not a section of the catalogue, it is a tool over all of them.
   * It is placed before settings so the bar reads left-to-right as content, then tools, with the
   * settings gear last. */
  if (n < max) {
    memset(&items[n], 0, sizeof items[n]);
    items[n].labelKey = HG_T_SEARCH;
    items[n].icon = HG_SB_ICON_SEARCH;
    items[n].group = HG_SB_GROUP_PINNED;
    /* ALWAYS ENABLED, unlike the watchlist and history: the search screen works with nothing saved and
     * nothing watched -- it is the one section that has no data dependency at all. */
    items[n].enabled = 1;
    n++;
  }

  if (n < max) {
    memset(&items[n], 0, sizeof items[n]);
    items[n].labelKey = HG_T_SETTINGS;
    items[n].icon = HG_SB_ICON_GEAR;
    items[n].group = HG_SB_GROUP_PINNED;
    items[n].enabled = 1;
    n++;
  }

  return n;
}

int hg_sidebar_count_group(const HgSidebarItem *items, int n, int group) {
  int i, c = 0;
  if (!items || n <= 0) return 0;
  for (i = 0; i < n; i++) {
    if (items[i].group == group) c++;
  }
  return c;
}

/* Is this index a position focus may occupy? */
static int selectable(const HgSidebarItem *items, int n, int i) {
  if (!items || i < 0 || i >= n) return 0;
  return items[i].enabled ? 1 : 0;
}

int hg_sidebar_valid(const HgSidebarItem *items, int n, int index) {
  return selectable(items, n, index);
}

int hg_sidebar_step(const HgSidebarItem *items, int n, int from, int delta) {
  int group, at, seen;

  if (!items || n <= 0) return -1;
  if (!selectable(items, n, from)) return hg_sidebar_first(items, n);
  if (delta == 0) return from;

  group = items[from].group;

  /* WALK THE ARRAY IN ITS NATURAL ORDER, WRAPPING, and take only members of this group.
   *
   * The array order IS the drawn order, so moving along it moves the way the bar looks. Deriving the
   * movement order differently from the drawing order is how a bar ends up stepping "down" into a dim
   * item or jumping into the pinned group. Wrapping at both ends is what makes the shoulder buttons
   * behave at the first and last section instead of doing nothing there. */
  at = from;
  for (seen = 0; seen < n; seen++) {
    at = (delta > 0) ? (at + 1) % n : (at - 1 + n) % n;
    if (items[at].group != group) continue;
    if (!items[at].enabled) continue;
    if (at == from) break;   /* all the way round and nothing else to move to */
    return at;
  }
  return from;
}

int hg_sidebar_first_of_group(const HgSidebarItem *items, int n, int group) {
  int i;
  if (!items || n <= 0) return -1;
  for (i = 0; i < n; i++) {
    if (items[i].group == group && items[i].enabled) return i;
  }
  return -1;
}

int hg_sidebar_first(const HgSidebarItem *items, int n) {
  int i;
  if (!items || n <= 0) return -1;
  for (i = 0; i < n; i++) {
    if (items[i].enabled) return i;
  }
  return -1;
}

int hg_sidebar_set_subtitle(HgSidebarItem *item, const char *text) {
  char buf[sizeof item->subtitle];
  int i = 0;
  if (!item) return 0;
  if (!text) text = "";

  /* The same sanitiser as the history file, for the same reason: the subtitle is drawn on ONE line
   * under the label, so a newline in it would break the bar's layout rather than merely look odd. */
  while (text[i] != '\0' && i < (int)sizeof buf - 1) {
    char c = text[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    buf[i] = c;
    i++;
  }
  buf[i] = '\0';

  if (strcmp(item->subtitle, buf) == 0) return 0;
  memcpy(item->subtitle, buf, (size_t)i + 1);
  return 1;
}
