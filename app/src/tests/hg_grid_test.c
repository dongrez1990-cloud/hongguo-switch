/*
 * Tests for hg_grid.c -- where each item lands in a poster or button grid.
 *
 * The grid arithmetic is the kind that fails silently: a wrong column draws a card
 * one slot over and still looks like a grid, a wrong total height clips the last row,
 * a wrong span count just looks like a different resolution. So every check here is
 * about a coordinate, a count or a boundary.
 *
 * The checks that matter most:
 *
 *   - THE CONTENT HEIGHT COVERS THE LAST ROW. A content box shorter than its contents
 *     loses the bottom row, and the symptom ("some cards are missing") does not point
 *     at the height arithmetic.
 *   - A HEADING RESETS THE COLUMN. If it did not, each tier would start wherever the
 *     previous tier happened to end and the whole screen would be visibly ragged.
 *   - A BOOK CARD'S HEIGHT DOES NOT DEPEND ON THE COLUMN WIDTH. That is what makes the
 *     episode picker's numbered buttons the same size at 544p and at 1080p.
 *
 * ASCII-only source file.
 */

#include "hg_grid.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* Exact float comparison is wrong for derived geometry, so everything is compared
 * with a tolerance far smaller than one logical pixel -- below what the screen can
 * show, so a difference that large is a real bug and not rounding.
 *
 * WRITTEN BY HAND RATHER THAN WITH fabsf(), and that is not an accident: including
 * <math.h> under TinyCC -- the host compiler this suite runs on -- fails outright
 * ("unknown constraint 't'" from an inline-asm declaration at math.h:217), so a
 * dependency on libm here would mean the grid geometry could not be tested on the
 * host at all, which is the entire reason this arithmetic lives in C. */
static float absf_(float v) { return v < 0.0f ? -v : v; }
static int near(float a, float b) { return absf_(a - b) < 0.01f; }

/* Build the flat kind array the layout takes: n tiers of `perSection` cards, each
 * preceded by a heading. Returns the item count. */
static size_t buildTiers(int* kinds, size_t cap, int sections, int perSection) {
  size_t n = 0;
  for (int s = 0; s < sections; s++) {
    if (n < cap) kinds[n++] = HG_GRID_ITEM_HEADING;
    for (int c = 0; c < perSection; c++) {
      if (n < cap) kinds[n++] = HG_GRID_ITEM_CARD;
    }
  }
  return n;
}

/* ---- the row step: what the D-pad's up and down mean ----------------------
 *
 * THE SUITE THAT EXISTS BECAUSE GETTING THIS WRONG IS INVISIBLE. Up/down used to be handled by a
 * per-frame poll of the pad in the view; a poll and the framework's own key dispatch both moved the
 * cursor, and the result was one press moving two cells. The movement is now answered where the
 * framework asks for it (`getNextFocus`), and the arithmetic that decides WHERE lives in hg_grid.c
 * so it can be checked here instead of on a console.
 *
 * The four shapes that matter, and each one is a case a naive `index + columns` gets wrong:
 *   - a full grid, where the answer is the same column;
 *   - a SHORT last row, where the same column does not exist and the nearest one has to be found;
 *   - TIER HEADINGS, which own a row number and hold no card, so the row numbering has gaps;
 *   - the two ends, where the answer must be "nowhere" rather than wrapping around. */

static void test_row_step(void) {
  /* The numbered-button shape, so the fixtures below are the sizes the episode picker really uses.
   * Only the ROW and COLUMN fields are read by hg_grid_row_step, so the shape does not change any
   * expectation here -- it just keeps the fixtures identical to the screen. */
  const HgGridMetrics buttons = {0.0f, 1.0f, 56.0f};

  printf("\n-- up/down: one ROW, nearest column, and nowhere at the ends --\n");

  /* A full 4x2 grid: items 0-3 in row 0, 4-7 in row 1. */
  {
    int kinds[8];
    HgGridPlace p[8];
    float total = 0.0f;
    int i;
    for (i = 0; i < 8; i++) kinds[i] = HG_GRID_ITEM_CARD;
    ok("the 4x2 fixture lays out",
       hg_grid_layout(kinds, 8, 1000.0f, 240.0f, 4, 44.0f, 18.0f, 16.0f, &buttons, p, 8,
                      &total) == 8);
    ok("it really is 4 columns wide (the fixture is what the checks assume)",
       p[4].row == 1 && p[4].column == 0 && p[3].column == 3);

    ok("DOWN from the first card is the card below it", hg_grid_row_step(p, 8, 0, 1) == 4);
    ok("DOWN keeps the column", hg_grid_row_step(p, 8, 2, 1) == 6);
    ok("UP from the second row is the card above it", hg_grid_row_step(p, 8, 6, -1) == 2);
    ok("UP keeps the column too", hg_grid_row_step(p, 8, 7, -1) == 3);

    /* THE ENDS DO NOT WRAP. A grid that wrapped would send UP from the first card to the LAST
     * one, which on a 222-episode page is a jump the user cannot undo by pressing the other key. */
    ok("UP from the first row is nowhere", hg_grid_row_step(p, 8, 0, -1) == -1);
    ok("DOWN from the last row is nowhere", hg_grid_row_step(p, 8, 5, 1) == -1);

    /* Malformed input: refused rather than answered with a plausible index. */
    ok("a step of zero rows is refused", hg_grid_row_step(p, 8, 1, 0) == -1);
    ok("an index past the end is refused", hg_grid_row_step(p, 8, 8, 1) == -1);
    ok("a negative index is refused", hg_grid_row_step(p, 8, -1, 1) == -1);
    ok("a null array is refused", hg_grid_row_step(NULL, 8, 1, 1) == -1);
  }

  /* A SHORT LAST ROW: 6 cards in 4 columns -> row 0 holds 0-3, row 1 holds only 4 and 5. */
  {
    int kinds[6];
    HgGridPlace p[6];
    float total = 0.0f;
    int i;
    for (i = 0; i < 6; i++) kinds[i] = HG_GRID_ITEM_CARD;
    ok("the 6-in-4 fixture lays out",
       hg_grid_layout(kinds, 6, 1000.0f, 240.0f, 4, 44.0f, 18.0f, 16.0f, &buttons, p, 6,
                      &total) == 6);

    /* THIS IS WHAT "NEAREST COLUMN" IS FOR. Column 3 exists on row 0 and NOT on row 1, so a plain
     * index + 4 walks off the end of the grid; the answer has to be the closest card that is
     * actually there, or DOWN from the last column of a tier would be a dead key. */
    ok("DOWN from column 3 of a full row lands on the short row's last card",
       hg_grid_row_step(p, 6, 3, 1) == 5);
    ok("DOWN from column 0 lands on column 0", hg_grid_row_step(p, 6, 0, 1) == 4);
    ok("UP comes back to the same column", hg_grid_row_step(p, 6, 5, -1) == 1);
    ok("UP from the short row's first card is column 0", hg_grid_row_step(p, 6, 4, -1) == 0);
    ok("DOWN from the short row is nowhere", hg_grid_row_step(p, 6, 4, 1) == -1);
  }

  /* TIER HEADINGS: two sections of four cards, each with a heading. The layout gives the headings
   * row numbers of their own (0 and 2) and the cards rows 1 and 3, so a step that walked rows by
   * one would land on a heading -- a row with nothing focusable in it. */
  {
    int kinds[10];
    HgGridPlace p[10];
    float total = 0.0f;
    size_t n = buildTiers(kinds, 10, 2, 4);
    ok("the tier fixture has 10 items (2 headings + 8 cards)", n == 10);
    ok("the tier fixture lays out",
       hg_grid_layout(kinds, n, 1000.0f, 240.0f, 4, 44.0f, 18.0f, 16.0f, &buttons, p, 10,
                      &total) == 10);
    ok("headings have a row of their own, with no card in it",
       p[0].kind == HG_GRID_ITEM_HEADING && p[0].row == 0 && p[1].kind == HG_GRID_ITEM_CARD &&
           p[1].row == 1 && p[5].kind == HG_GRID_ITEM_HEADING && p[6].row == 3);

    ok("DOWN from the first tier reaches the second tier",
       hg_grid_row_step(p, 10, 1, 1) == 6);
    ok("DOWN from the first tier keeps the column",
       hg_grid_row_step(p, 10, 4, 1) == 9);
    ok("UP from the second tier reaches the first", hg_grid_row_step(p, 10, 8, -1) == 3);
    ok("UP from the first tier is nowhere (the heading above it is not a destination)",
       hg_grid_row_step(p, 10, 1, -1) == -1);
    ok("a HEADING is not a starting point", hg_grid_row_step(p, 10, 0, 1) == -1);
    ok("nor the second heading", hg_grid_row_step(p, 10, 5, -1) == -1);
  }

  /* A REALISTIC PICKER: 76 episodes at 720p's width, which is 8 columns and a last row of four.
   * The property under test is the one the user feels: DOWN holds the column and UP returns it. */
  {
    int kinds[76];
    HgGridPlace p[76];
    float total = 0.0f;
    int cols;
    int i;
    int downOk = 1;
    int upOk = 1;
    for (i = 0; i < 76; i++) kinds[i] = HG_GRID_ITEM_CARD;
    ok("the 76-episode fixture lays out",
       hg_grid_layout(kinds, 76, 1232.0f, 120.0f, 0, 44.0f, 12.0f, 12.0f, &buttons, p, 76,
                      &total) == 76);
    cols = hg_grid_columns_for(1232.0f, 120.0f, 0);
    ok("it is the 8-column, 10-row page the fixture assumes",
       cols == 8 && p[8].row == 1 && p[72].row == 9 && p[75].column == 3);

    /* Rows 0..7 are full, so DOWN from them is exactly eight items down. */
    for (i = 0; i < 64; i++) {
      if (hg_grid_row_step(p, 76, i, 1) != i + 8) downOk = 0;
    }
    /* Every row except the first has a full row above it, so UP is exactly eight items up --
     * including from the short last row, where the column does exist above. */
    for (i = 8; i < 76; i++) {
      if (hg_grid_row_step(p, 76, i, -1) != i - 8) upOk = 0;
    }
    ok("DOWN holds the column for all 8 full rows", downOk);
    ok("UP holds the column for every row", upOk);

    ok("from the last full row, DOWN lands on the nearest card of the short row",
       hg_grid_row_step(p, 76, 68, 1) == 75);
    ok("DOWN from the short last row is nowhere", hg_grid_row_step(p, 76, 72, 1) == -1);
    ok("UP from the first row of the picker is nowhere",
       hg_grid_row_step(p, 76, 3, -1) == -1);
  }
}

/* ---- the column count ----------------------------------------------------- */

static void test_columns(void) {
  printf("\n-- column count comes from the width, not a constant --\n");

  /* The four logical widths this app can run at (ui.scale in hongguo.cfg), minus a
   * 24 pt margin each side, with a 200 pt target poster. */
  ok("960 wide -> 4 columns", hg_grid_columns_for(960.0f - 48.0f, 200.0f, 0) == 4);
  ok("1280 wide -> 6 columns", hg_grid_columns_for(1280.0f - 48.0f, 200.0f, 0) == 6);
  ok("1600 wide -> 7 columns", hg_grid_columns_for(1600.0f - 48.0f, 200.0f, 0) == 7);
  ok("1920 wide -> 8 columns (the ceiling)",
     hg_grid_columns_for(1920.0f - 48.0f, 200.0f, 0) == 8);

  /* The floor: a width that would give one column gets two, because a one-column
   * grid IS the list this replaces. */
  ok("a very narrow width still gives 2 columns", hg_grid_columns_for(100.0f, 200.0f, 0) == 2);
  ok("zero width gives the floor, not a crash", hg_grid_columns_for(0.0f, 200.0f, 0) == 2);
  ok("a zero target gives the floor", hg_grid_columns_for(1280.0f, 0.0f, 0) == 2);

  /* maxColumns is a request, and it is clamped rather than obeyed blindly. */
  ok("maxColumns=3 is honoured", hg_grid_columns_for(1280.0f, 200.0f, 3) == 3);
  ok("maxColumns=1 is raised to the floor", hg_grid_columns_for(1280.0f, 200.0f, 1) == 2);
  ok("maxColumns=99 is lowered to the HARD ceiling",
     hg_grid_columns_for(4000.0f, 100.0f, 99) == HG_GRID_MAX_COLUMNS_HARD);
  ok("a negative maxColumns means the default",
     hg_grid_columns_for(1280.0f, 200.0f, -1) == 6);

  /* ---- the density toggle's arithmetic ------------------------------------
   *
   * THE BUG THIS SECTION EXISTS FOR. The ceiling used to be hard-coded at 8, so a caller asking
   * for 12 got 8 -- and the home screen's small/large toggle asks for a SMALLER poster precisely
   * so that more fit per row. MEASURED at the four ui.scale widths, that made the toggle change
   * the poster size and NOT the column count at 720p, 900p and 1080p: a key that looks like it
   * worked and did not. These checks pin the RELATIONSHIP, not just the individual numbers. */
  {
    const float large = 200.0f;
    const float small = large * 55.0f / 100.0f;   /* 110, the compact target */
    const int compactCeiling = HG_GRID_MAX_COLUMNS_HARD;
    struct {
      float w;
      const char* name;
    } sizes[4] = {{960.0f - 48.0f, "544p"},
                  {1280.0f - 48.0f, "720p"},
                  {1600.0f - 48.0f, "900p"},
                  {1920.0f - 48.0f, "1080p"}};
    int allDenser = 1;
    int allSane = 1;
    for (int i = 0; i < 4; i++) {
      int big = hg_grid_columns_for(sizes[i].w, large, 0);
      int smallCols = hg_grid_columns_for(sizes[i].w, small, compactCeiling);
      if (smallCols <= big) allDenser = 0;
      if (smallCols < HG_GRID_MIN_COLUMNS || smallCols > compactCeiling) allSane = 0;
      printf("        %-6s large=%d  small=%d\n", sizes[i].name, big, smallCols);
    }
    ok("compact has MORE columns than large at every ui.scale", allDenser);
    ok("and stays inside its own ceiling and the floor", allSane);

    /* The specific numbers, so moving either constant fails a check. */
    ok("544p: 4 large -> 8 small",
       hg_grid_columns_for(912.0f, 200.0f, 0) == 4 &&
           hg_grid_columns_for(912.0f, 110.0f, 12) == 8);
    ok("720p: 6 large -> 11 small",
       hg_grid_columns_for(1232.0f, 200.0f, 0) == 6 &&
           hg_grid_columns_for(1232.0f, 110.0f, 12) == 11);
    ok("1080p: 8 large -> 12 small (its own ceiling)",
       hg_grid_columns_for(1872.0f, 200.0f, 0) == 8 &&
           hg_grid_columns_for(1872.0f, 110.0f, 12) == 12);
  }
}

/* ---- the layout of a real home page --------------------------------------- */

static void test_tiered_home(void) {
  int kinds[64];
  HgGridPlace places[64];
  float total = 0.0f;
  size_t n;
  int placed;

  const float contentW = 1280.0f - 48.0f;
  const float headingH = 44.0f;
  const float rowSpace = 18.0f;
  const float itemSpace = 16.0f;

  printf("\n-- the real page: 4 tiers of 9 cards --\n");
  n = buildTiers(kinds, 64, 4, 9);
  ok("4 tiers of 9 build 40 items (4 headings + 36 cards)", n == 40);

  placed = hg_grid_layout(kinds, n, contentW, 200.0f, 0, headingH, rowSpace, itemSpace,
                          NULL, places, 64, &total);
  ok("all 40 items are placed", placed == 40);

  {
    int cols = hg_grid_columns_for(contentW, 200.0f, 0);
    float expectedCellW = (contentW - itemSpace * (float)(cols - 1)) / (float)cols;
    ok("the cell width divides the content exactly", near(places[1].cellW, expectedCellW));
    ok("the last column's right edge reaches the content width",
       near(places[cols].x + places[cols].w, contentW));
    ok("cell height is the 2:3 poster plus a title block",
       near(places[1].cellH, expectedCellW * 1.5f + 56.0f));
  }

  /* Every heading is full width, on its own row, and at column 0. */
  {
    int headings = 0;
    int badWidth = 0, badCol = 0, badCellH = 0;
    for (int i = 0; i < placed; i++) {
      if (places[i].kind != HG_GRID_ITEM_HEADING) continue;
      headings++;
      if (!near(places[i].w, contentW)) badWidth++;
      if (places[i].column != 0) badCol++;
      if (!near(places[i].cellH, 0.0f)) badCellH++;
    }
    ok("all four headings are present", headings == 4);
    ok("every heading spans the full content width", badWidth == 0);
    ok("every heading sits at column 0", badCol == 0);
    ok("a heading reports no cell height (there is no poster under it)", badCellH == 0);
  }

  /* THE PROPERTY THAT MATTERS MOST: the first card of each tier is in column 0.
   * Without the heading's column reset, tier 2 would start in whatever column tier 1
   * ended in -- 9 cards in a 6-column grid ends in column 3 -- and every tier after
   * the first would be visibly ragged. */
  {
    int startsAtZero = 1;
    int tiersSeen = 0;
    for (int i = 0; i < placed; i++) {
      if (places[i].kind != HG_GRID_ITEM_HEADING) continue;
      if (i + 1 < placed && places[i + 1].column != 0) startsAtZero = 0;
      tiersSeen++;
    }
    ok("the card after every heading starts at column 0", startsAtZero);
    ok("all four tiers were examined", tiersSeen == 4);
  }

  /* Cards in a row share a y and are spaced by exactly one gap; a row never has more
   * columns than the grid has. */
  {
    int cols = hg_grid_columns_for(contentW, 200.0f, 0);
    int badColSpan = 0, badGap = 0, badY = 0;
    for (int i = 0; i < placed; i++) {
      if (places[i].kind != HG_GRID_ITEM_CARD) continue;
      if (places[i].column < 0 || places[i].column >= cols) badColSpan++;
      if (places[i].column > 0) {
        for (int j = i - 1; j >= 0; j--) {
          if (places[j].row == places[i].row && places[j].column == places[i].column - 1) {
            if (!near(places[i].x - places[j].x, places[j].cellW + itemSpace)) badGap++;
            if (!near(places[i].y, places[j].y)) badY++;
            break;
          }
        }
      }
    }
    ok("no card is placed beyond the last column", badColSpan == 0);
    ok("cards in one row are one cell width plus one gap apart", badGap == 0);
    ok("cards in one row share the same y", badY == 0);
  }

  /* THE LAST ROW IS INSIDE THE CONTENT HEIGHT. */
  {
    float lowest = 0.0f;
    for (int i = 0; i < placed; i++) {
      float bottom = places[i].y + places[i].h;
      if (bottom > lowest) lowest = bottom;
    }
    ok("the content height covers the lowest item's bottom edge", total >= lowest - 0.01f);
    ok("the content height is not wildly larger than the contents", total <= lowest + 0.01f);
    ok("four headings each advance the cursor by headingH + rowSpace", total > headingH * 4.0f);
  }
}

/* ---- headings and rows ---------------------------------------------------- */

static void test_boundaries(void) {
  const int kinds[6] = {HG_GRID_ITEM_HEADING, HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD,
                        HG_GRID_ITEM_HEADING, HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD};
  HgGridPlace places[16];
  float total = 0.0f;
  int placed;

  printf("\n-- a heading straight after a heading --\n");
  {
    /* Two headings in a row must NOT create an empty card row between them: rowH is
     * 0 when nothing landed in the row, and the code only advances when it is not. */
    const int hh[2] = {HG_GRID_ITEM_HEADING, HG_GRID_ITEM_HEADING};
    placed = hg_grid_layout(hh, 2, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 16,
                            &total);
    ok("two headings place two items", placed == 2);
    ok("the second heading is exactly one heading-height below the first",
       near(places[1].y - places[0].y, 44.0f));
    ok("no phantom empty row was inserted", near(total, 88.0f));
  }

  printf("\n-- a full row, then a heading --\n");
  {
    int full[16];
    int cols = hg_grid_columns_for(1000.0f, 200.0f, 0);
    for (int i = 0; i < cols; i++) full[i] = HG_GRID_ITEM_CARD;
    full[cols] = HG_GRID_ITEM_HEADING;
    placed = hg_grid_layout(full, (size_t)cols + 1, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f,
                            NULL, places, 16, &total);
    ok("a full row plus a heading places everything", placed == cols + 1);
    ok("the heading starts a NEW row below the full row",
       places[cols].kind == HG_GRID_ITEM_HEADING && places[cols].row > places[cols - 1].row);
    ok("the heading is at the top of its row",
       near(places[cols].y, places[cols - 1].y + places[cols - 1].h + 18.0f));
  }

  printf("\n-- a partial row, then a heading --\n");
  placed = hg_grid_layout(kinds, 6, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 16,
                          &total);
  ok("six items place", placed == 6);
  ok("the second heading follows the two-card row",
     near(places[3].y, places[1].y + places[1].h + 18.0f));
}

/* ---- degradation and refusal ---------------------------------------------- */

static void test_edge_cases(void) {
  HgGridPlace places[8];
  float total = -1.0f;
  const int cards[4] = {HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD,
                        HG_GRID_ITEM_CARD};

  printf("\n-- zero items, tiny buffers and impossible widths --\n");

  ok("zero items place nothing",
     hg_grid_layout(cards, 0, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 8,
                    &total) == 0);
  ok("zero items report zero content height", near(total, 0.0f));

  /* Truncation is REPORTED, not silent: the caller compares its own count. */
  ok("more items than the buffer places only what fits",
     hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 2,
                    &total) == 2);

  total = -1.0f;
  ok("cap 0 places nothing",
     hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 0,
                    &total) == 0);
  ok("cap 0 still writes the height out-parameter", near(total, 0.0f));

  total = -1.0f;
  ok("a zero content width is refused",
     hg_grid_layout(cards, 4, 0.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 8,
                    &total) == 0);
  ok("a refused layout reports zero height", near(total, 0.0f));

  total = -1.0f;
  ok("a null output array is refused",
     hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, NULL, 8,
                    &total) == 0);

  /* A gap wider than the content would make the cell width collapse; refusing beats
   * emitting 45 unreadable slivers. The threshold is a MINIMUM USABLE CELL, not
   * "greater than zero": with 100 pt of content and a 90 pt gap, two columns leave
   * 5 pt each, which is not a grid. */
  total = -1.0f;
  ok("a gap wider than the content is refused",
     hg_grid_layout(cards, 4, 100.0f, 200.0f, 8, 44.0f, 18.0f, 90.0f, NULL, places, 8,
                    &total) == 0);
  ok("the refusal reports zero height", near(total, 0.0f));

  /* The boundary itself, checked from both sides so the threshold cannot drift. */
  total = 0.0f;
  ok("a cell of exactly the minimum usable width is accepted",
     hg_grid_layout(cards, 2, 200.0f, 200.0f, 2, 44.0f, 18.0f, 0.0f, NULL, places, 8,
                    &total) == 2 &&
         near(places[0].cellW, 100.0f));
  total = -1.0f;
  ok("a cell narrower than the minimum is refused",
     hg_grid_layout(cards, 2, 100.0f, 100.0f, 2, 44.0f, 18.0f, 60.0f, NULL, places, 8,
                    &total) == 0);

  /* A NULL total-height pointer is allowed: the caller may not care. */
  ok("a NULL out-parameter is tolerated",
     hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 8,
                    NULL) == 4);

  /* One card: the degenerate grid still has a sensible height. */
  total = 0.0f;
  hg_grid_layout(cards, 1, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places, 8, &total);
  ok("a single card's height is its cell height", near(total, places[0].cellH));
}

/* ---- a non-multiple count leaves a gap, on purpose ------------------------ */

static void test_ragged_section(void) {
  int kinds[16];
  HgGridPlace places[16];
  float total = 0.0f;

  printf("\n-- a tier whose count is not a multiple of the column count --\n");
  {
    size_t n = buildTiers(kinds, 16, 2, 3);
    int cols = hg_grid_columns_for(1400.0f, 200.0f, 0);
    int placed = hg_grid_layout(kinds, n, 1400.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, places,
                                16, &total);
    ok("both tiers are placed", placed == 8);
    ok("the grid holds more than 3 cards per row, so a gap is possible", cols > 3);
    ok("the second heading is on a row of its own",
       places[4].kind == HG_GRID_ITEM_HEADING && places[4].row > places[3].row);
    ok("the second heading starts at column 0", places[4].column == 0);
    ok("the second tier's first card is below its heading",
       places[5].y >= places[4].y + 44.0f - 0.01f);
  }
}

/* ---- the numbered-button shape (the episode picker) ----------------------- */

static void test_button_metrics(void) {
  HgGridPlace places[76];
  float total = 0.0f;
  int placed;
  /* 76 episodes, no headings, aspectNum 0: every cell is exactly `extraH` tall
   * regardless of how wide the column is. */
  const HgGridMetrics buttons = {0.0f, 1.0f, 56.0f};
  const int cards[4] = {HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD, HG_GRID_ITEM_CARD,
                        HG_GRID_ITEM_CARD};

  printf("\n-- numbered buttons: no image, fixed height --\n");

  placed = hg_grid_layout(cards, 4, 1000.0f, 120.0f, 0, 44.0f, 12.0f, 12.0f, &buttons, places,
                          76, &total);
  ok("four buttons place", placed == 4);
  ok("a button cell is exactly extraH tall", near(places[0].cellH, 56.0f));
  ok("a button cell is shorter than its own width (not a square)",
     places[0].cellH < places[0].cellW);

  {
    HgGridPlace wide[4];
    float wideTotal = 0.0f;
    hg_grid_layout(cards, 4, 2000.0f, 120.0f, 0, 44.0f, 12.0f, 12.0f, &buttons, wide, 4,
                   &wideTotal);
    ok("the button height is the same on a much wider screen",
       near(wide[0].cellH, places[0].cellH));
    ok("but the cell IS wider there", wide[0].cellW > places[0].cellW);
  }

  /* A zero numerator must not be read as "square". */
  ok("cell height is not cellW for aspectNum 0", !near(places[0].cellH, places[0].cellW));

  /* The default (no metrics) is the poster shape, and it must match the macro the
   * views use -- otherwise the home grid and its tests would disagree. */
  {
    HgGridPlace poster[4];
    HgGridPlace byMacro[4];
    float t1 = 0.0f, t2 = 0.0f;
    hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, NULL, poster, 4, &t1);
    hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, &HG_GRID_POSTER_METRICS,
                   byMacro, 4, &t2);
    ok("a NULL metrics means the poster shape", near(t1, t2));
    ok("HG_GRID_POSTER_METRICS is 3:2 plus 56",
       near(byMacro[0].cellH, byMacro[0].cellW * 1.5f + 56.0f));
  }

  /* Malformed metrics are refused rather than turned into infinities. */
  {
    const HgGridMetrics badDen = {3.0f, 0.0f, 56.0f};
    const HgGridMetrics negNum = {-3.0f, 2.0f, 56.0f};
    const HgGridMetrics negExtra = {3.0f, 2.0f, -10.0f};
    total = -1.0f;
    ok("a zero denominator is refused",
       hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, &badDen, places, 76,
                      &total) == 0);
    ok("a negative aspect numerator is refused",
       hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, &negNum, places, 76,
                      &total) == 0);
    ok("a negative extra height is refused",
       hg_grid_layout(cards, 4, 1000.0f, 200.0f, 0, 44.0f, 18.0f, 16.0f, &negExtra, places, 76,
                      &total) == 0);
  }

  /* A realistic episode page: 76 buttons, no headings, at 720p's width. */
  {
    int kinds[76];
    HgGridPlace big[76];
    float bigTotal = 0.0f;
    int i;
    for (i = 0; i < 76; i++) kinds[i] = HG_GRID_ITEM_CARD;
    placed = hg_grid_layout(kinds, 76, 1232.0f, 120.0f, 0, 44.0f, 12.0f, 12.0f, &buttons, big,
                            76, &bigTotal);
    ok("76 episodes all place", placed == 76);
    {
      /* 1232 / 120 would be 10, but the ceiling is 8 columns -- so the button is
       * WIDER than requested and the count is capped. Both are visible in one check:
       * the assertion states the whole result rather than just the part I expected.
       * (The first version of this test asserted 10 columns, which is what the raw
       * division gives and what the code deliberately does not do.) */
      int cols = hg_grid_columns_for(1232.0f, 120.0f, 0);
      int rows = (76 + cols - 1) / cols;
      float expected = (float)rows * 56.0f + (float)(rows - 1) * 12.0f;
      ok("the column count is capped at HG_GRID_MAX_COLUMNS", cols == HG_GRID_MAX_COLUMNS);
      ok("76 buttons in 8 columns make 10 rows", rows == 10);
      ok("a capped grid gives cells WIDER than the request", big[0].cellW > 120.0f);
      ok("the content height is rows * cellH plus the gaps", near(bigTotal, expected));
      ok("the whole picker fits in a fraction of the old 76-row list", bigTotal < 800.0f);
    }
  }
}

int main(void) {
  printf("hg_grid tests: poster and button grid geometry\n");
  printf("========================================\n");

  test_columns();
  test_tiered_home();
  test_boundaries();
  test_edge_cases();
  test_ragged_section();
  test_button_metrics();
  test_row_step();

  printf("\n========================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
