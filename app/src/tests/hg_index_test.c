/* hg_index_test.c -- the local index's merge policy.
 *
 * WHY THESE CHECKS EXIST. The merge replaced a nested loop that had no tests because it lived in a C++ member
 * function that talks to the network. The three things it decides can all be wrong SILENTLY:
 *
 *   - a duplicate that is not detected makes a search show the same drama twice;
 *   - a missing cap turns an upstream paging bug into "the console runs out of memory", a crash with no visible
 *     connection to its cause;
 *   - a merge that reorders the pages makes the newest results appear in the middle of the list.
 *
 * ASCII-only source file. */

#include <stdio.h>
#include <string.h>

#include "hg_index.h"

static int g_checks = 0;
static int g_failures = 0;

static void ok(const char *what, int cond) {
  g_checks++;
  if (!cond) {
    g_failures++;
    printf("  FAIL  %s\n", what);
  } else {
    printf("  PASS  %-58s\n", what);
  }
}

static void mk(HgSeries *s, const char *id) {
  memset(s, 0, sizeof *s);
  snprintf(s->id, sizeof s->id, "%s", id);
  snprintf(s->title, sizeof s->title, "title-%s", id);
}

static void test_append(void) {
  HgSeries dst[8];
  HgSeries in[4];
  size_t count = 0;
  HgIndexMerge m;
  int i;

  printf("\nmerging pages into the index\n");

  mk(&in[0], "1");
  mk(&in[1], "2");
  ok("a merge into an empty list succeeds", hg_index_append(dst, &count, 8, in, 2, &m) == 0);
  ok("both entries were added", count == 2 && m.added == 2);
  ok("nothing was skipped", m.duplicate == 0 && m.overflow == 0);
  ok("the order is the order they arrived", strcmp(dst[0].id, "1") == 0 && strcmp(dst[1].id, "2") == 0);

  /* THE SAME SERIES IN A SECOND CATEGORY: measured, so it happens. */
  mk(&in[0], "2");
  mk(&in[1], "3");
  ok("a second page merges", hg_index_append(dst, &count, 8, in, 2, &m) == 0);
  ok("the duplicate is skipped, the new one is added", m.added == 1 && m.duplicate == 1 && count == 3);
  ok("and the list has no repeated id", strcmp(dst[2].id, "3") == 0);

  /* WITHIN ONE PAGE: a page that repeats an entry adds it once. */
  mk(&in[0], "4");
  mk(&in[1], "4");
  ok("a page that repeats itself adds one entry",
     hg_index_append(dst, &count, 8, in, 2, &m) == 0 && m.added == 1 && m.duplicate == 1 && count == 4);

  /* THE CAP: normal, not an error. */
  mk(&in[0], "5");
  mk(&in[1], "6");
  ok("a full list is not an error", hg_index_append(dst, &count, 4, in, 2, &m) == 0);
  ok("nothing was added and both were counted as overflow", m.added == 0 && m.overflow == 2 && count == 4);
  ok("the list is unchanged", strcmp(dst[3].id, "4") == 0);

  /* A FULL LIST STILL SKIPS DUPLICATES CORRECTLY, not as overflow: the entry is already there. */
  mk(&in[0], "1");
  mk(&in[1], "9");
  ok("a duplicate of something already present is a duplicate, not overflow",
     hg_index_append(dst, &count, 4, in, 2, &m) == 0 && m.duplicate == 1 && m.overflow == 1);

  /* THE CAP IS EXACTLY THE CAP: appending to count == cap - 1 with one entry fills it. */
  {
    HgSeries small[2];
    size_t c = 1;
    mk(&small[0], "a");
    mk(&in[0], "b");
    ok("the last free slot is used", hg_index_append(small, &c, 2, in, 1, &m) == 0 && c == 2 && m.added == 1);
  }

  /* Degenerate arguments. */
  mk(&in[0], "x");
  ok("a NULL destination is refused", hg_index_append(NULL, &count, 8, in, 1, &m) == -1);
  ok("a NULL count is refused", hg_index_append(dst, NULL, 8, in, 1, &m) == -1);
  ok("a NULL input is refused", hg_index_append(dst, &count, 8, NULL, 1, &m) == -1);
  ok("a zero cap is refused", hg_index_append(dst, &count, 0, in, 1, &m) == -1);
  {
    size_t bad = 9;
    ok("a count past the cap is refused", hg_index_append(dst, &bad, 8, in, 1, &m) == -1);
  }
  ok("a NULL result struct is allowed", hg_index_append(dst, &count, 8, in, 1, NULL) == 0);
  ok("and zero entries merge into nothing", hg_index_append(dst, &count, 8, in, 0, &m) == 0 && m.added == 0);

  /* An EMPTY ID is a value: two entries with none are the same entry as far as this policy is concerned. */
  {
    HgSeries e[2];
    size_t c = 0;
    memset(&e[0], 0, sizeof e[0]);
    memset(&e[1], 0, sizeof e[1]);
    (void)i;
    ok("two entries with no id are treated as one",
       hg_index_append(dst, &c, 8, e, 2, &m) == 0 && m.added == 1 && m.duplicate == 1);
  }
}

static void test_count_new(void) {
  HgSeries dst[4];
  HgSeries in[4];

  printf("\ncounting what a page would add\n");

  mk(&dst[0], "1");
  mk(&dst[1], "2");
  mk(&in[0], "2");
  mk(&in[1], "3");
  mk(&in[2], "3");
  ok("a page that repeats one entry and repeats itself counts two new",
     hg_index_count_new(dst, 2, in, 3) == 1);
  mk(&in[0], "3");
  mk(&in[1], "4");
  ok("two genuinely new entries count two", hg_index_count_new(dst, 2, in, 2) == 2);
  ok("nothing new counts zero", hg_index_count_new(dst, 2, dst, 2) == 0);
  ok("an empty page counts zero", hg_index_count_new(dst, 2, in, 0) == 0);
  ok("NULL is zero, not a crash", hg_index_count_new(NULL, 2, in, 2) == 0 && hg_index_count_new(dst, 2, NULL, 2) == 0);
  ok("counting does not modify anything", strcmp(dst[0].id, "1") == 0 && strcmp(dst[1].id, "2") == 0);
}

int main(void) {
  printf("hg_index tests (i386 host build)\n");
  test_append();
  test_count_new();
  printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
