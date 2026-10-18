/*
 * hg_grid -- where each home item goes in the poster grid, as pure logic.
 *
 * GPL-3.0-only.
 *
 * WHY THIS IS C AND NOT C++, for the third time in this project and for the same
 * reason each time: it is arithmetic whose mistakes are SILENT. A wrong column index
 * draws a card one slot to the left and still looks like a grid; a wrong span count
 * looks like a slightly different resolution; a wrong cell height makes the last row
 * clip by 20 pixels. None of those crash and none of them log. The host suite can
 * check every one of them here, and the C++ side is then left with nothing but
 * "put a view at the rectangle this function returned".
 *
 * ---------------------------------------------------------------------------
 * WHAT THE GRID IS, AND HOW IT RELATES TO wiliwili's.
 *
 * wiliwili's home is a `RecyclingGrid` (include/view/recycling_grid.hpp, 8 KB, plus
 * source/view/recycling_grid.cpp, 29 KB) -- a recycling grid with a column count, a
 * prefetch distance and its own focus handling. That component is the design
 * reference and this file reproduces only its LAYOUT half, deliberately:
 *
 *   - its scroll/focus half is borealis' job here. A brls::ScrollingFrame already
 *     scrolls to keep the focused view on screen, and Box::getNextFocus already
 *     walks children in order (box.cpp:409-443). Borrowing those means the grid
 *     inherits whatever borealis gets right, instead of a second implementation that
 *     can disagree with it.
 *   - the column count is NOT hard-coded to 4. wiliwili computes the cell width from
 *     `spanCount`; the request was explicitly that the column count be derived from the
 *     ui.scale logical width rather than fixed, and the app's logical width genuinely
 *     changes (544p = 960x544 up to 1080p = 1920x1080, see the ui.scale note in
 *     hongguo.cfg). So the caller passes its logical width and a target poster width,
 *     and the column count falls out.
 *
 * ---------------------------------------------------------------------------
 * THE ONE STRUCTURAL DIFFERENCE FROM wiliwili, AND WHY.
 *
 * wiliwili's grid is a flat grid of video cards. This screen has TIER HEADINGS --
 * four named tiers of nine cards, from hg_api_parse_sections -- and a heading is
 * not a card: it is a full-width label that interrupts the flow and starts a new
 * row. So an item is either
 *
 *   HG_GRID_ITEM_HEADING  full width, its own row, and it RESETS the column cursor
 *   HG_GRID_ITEM_CARD     one cell wide, placed in the next column
 *
 * which is exactly the "sectioned grid" shape and is what makes a heading land on
 * a row boundary rather than after whatever card happened to be in column 3.
 *
 * A section whose card count is not a multiple of the column count therefore leaves
 * a gap at the end of its last row, and the next heading starts a fresh row. That is
 * intended: it is how every sectioned grid behaves, and it is what keeps the tiers
 * visually separate.
 *
 * ---------------------------------------------------------------------------
 * THE POSTER ASPECT, WHICH IS THE ONE PLACE THIS DELIBERATELY DEPARTS FROM wiliwili.
 *
 * wiliwili's thumbnails are 16:9 landscape (Bilibili video frames) and its cards are
 * landscape. This service's covers are PORTRAIT posters (series_cover), so forcing a
 * 16:9 cell would either stretch them or letterbox them. The deviation is recorded in
 * docs/wiliwili-align-steps: keep wiliwili's grid STRUCTURE, change the RATIO to the
 * poster's own. So a cell is `cellW` wide and `cellW * 3 / 2` tall -- 2:3, the
 * poster's aspect -- plus room for the title lines under it.
 *
 * ASCII-only source file.
 */

#ifndef HG_GRID_H
#define HG_GRID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HG_GRID_ITEM_HEADING 0
#define HG_GRID_ITEM_CARD 1

/* Bounds on the column count. The floor is 2 because a one-column "grid" is the list this
 * replaces, and the point of the change is more than one card per row; the DEFAULT ceiling is 8
 * because beyond that a poster is too small to recognise on a TV. A caller that asks for
 * something outside this range, or passes 0, is CLAMPED to it rather than refused -- a clamped
 * grid still works, and a refusal here would blank the screen over a layout preference.
 *
 * THE CEILING IS A PARAMETER, NOT A WALL, and that was a bug this file shipped with for one
 * round. It was hard-coded at 8, so a caller that WANTED 12 got 8 -- and the home screen's
 * small/large toggle asks for a smaller poster precisely so that more fit per row. MEASURED at
 * the four ui.scale widths with a 200 pt large poster and a 110 pt small one:
 *
 *     544p    large 4  ->  small 8
 *     720p    large 6  ->  small 8       <- capped: without this the answer is 11
 *     900p    large 7  ->  small 8       <- capped: without this the answer is 14
 *     1080p   large 8  ->  small 8       <- capped
 *
 * so at three of the four resolutions the toggle changed NOTHING about the columns: it shrank
 * the posters and left the layout identical, which is the worst kind of broken toggle -- it looks
 * like it worked. HG_GRID_MAX_COLUMNS_HARD is what a caller may now ask up to, and
 * HG_GRID_COMPACT_MAX_COLUMNS (12) is what the small layout actually requests. */
#define HG_GRID_MIN_COLUMNS 2
#define HG_GRID_MAX_COLUMNS 8
#define HG_GRID_MAX_COLUMNS_HARD 12

/* Where one item goes. All values are in the caller's logical pixels, and `x`/`y`
 * are relative to the scroll content's top-left -- the view adds its own padding.
 *
 * `cellW`/`cellH` describe a CARD's slot and are written for every item, including
 * headings, because the caller needs them for the titles under the covers and for
 * the row-height arithmetic. A heading uses `w` = content width and `h` = its own
 * height; its `cellH` is 0, which is the caller's signal that there is no poster
 * under it.
 */
typedef struct {
  int kind;      /* HG_GRID_ITEM_HEADING | HG_GRID_ITEM_CARD */
  int row;       /* which layout row this landed on, 0-based */
  int column;    /* 0-based column within that row; 0 for a heading */
  float x;
  float y;
  float w;
  float h;
  float cellW;   /* the card slot's width (column width) */
  float cellH;   /* the card slot's height: poster + title block */
} HgGridPlace;

/* THE CELL SHAPE IS A PARAMETER, BECAUSE THE TWO GRIDS ON THIS APP ARE NOT THE SAME
 * SHAPE.
 *
 * The home grid holds 2:3 POSTERS (series_cover) with two lines of title under them.
 * The episode picker holds 76 SHORT numbered buttons -- a clickable "N" with a few
 * pixels of padding, no image at all. Forcing one shape on both would either make the
 * episode buttons 340 px tall or squash the posters, so the caller says what it wants:
 *
 *   aspectNum / aspectDen   the image's aspect; the image part is cellW * num / den.
 *                           SET aspectNum TO 0 FOR "NO IMAGE" -- then the whole cell
 *                           is `extraH` tall, which is the numbered-button case. (0
 *                           and not 1, because 1 would be a square, and a caller that
 *                           forgot to set it should get a square cell rather than a
 *                           zero-height one.)
 *   extraH                  the height below the image: title lines for a poster, and
 *                           the label's own box for a numbered button.
 *
 * Both are per-layout, not per-item: within one grid every cell is the same size,
 * which is what keeps the rows aligned. A grid whose cells varied per item would be
 * wiliwili's `isFlowMode` (recycling_grid.hpp:191), which it only enables for a single
 * column -- and a single column is the list this project already has. */
typedef struct {
  float aspectNum;
  float aspectDen;
  float extraH;
} HgGridMetrics;

/* The home grid's shape: the cover's own 2:3 plus 56 px for two lines of title. This
 * is the one place that constant lives, so the view and the tests cannot disagree. */
#define HG_GRID_POSTER_METRICS ((HgGridMetrics){3.0f, 2.0f, 56.0f})

/* Lay out `count` items into a grid.
 *
 * `kinds` is an array of HG_GRID_ITEM_* values, one per item -- NOT inferred, because
 * the caller already knows which is which (the row model in hg_api.h says so) and
 * inferring it here would be a second source of truth for the same fact.
 *
 * `contentW` is the width available for cards, excluding the caller's own margins.
 * The column count is chosen from it and `targetCellW`; `maxColumns` caps it (pass
 * 0 for the built-in ceiling). `headingH` is the height of a heading block including
 * the space under it; `rowSpace` is the vertical gap between card rows; `itemSpace`
 * is the horizontal gap between columns. `metrics` is the cell shape (see above); a
 * NULL pointer means HG_GRID_POSTER_METRICS.
 *
 * WRITES AT MOST `cap` entries and returns the number of items PLACED. If `count`
 * exceeds `cap` this places `cap` of them and the remainder is NOT laid out -- the
 * return value is how the caller detects it. (Contrast with hg_api_home_rows, which
 * refuses rather than truncating: there, a partial list is indistinguishable from a
 * short page and the caller would show the wrong thing. Here the caller has the item
 * array in hand and can compare its own count against this return value, so a
 * truncation is detectable rather than silent.)
 *
 * `outTotalHeight` receives the content height needed to show everything, which is
 * what the scroll content box must be sized to. It is a pointer to write rather than
 * an entry in the array because with zero items there is no entry to hold it.
 *
 * Returns 0 when the arguments cannot describe a grid (null arrays, cap 0, a
 * non-positive contentW, a cell too narrow to be usable, or a metrics with a
 * non-positive denominator); in that case `*outTotalHeight` is set to 0 and the caller
 * shows nothing rather than guessing at a layout. */
int hg_grid_layout(const int* kinds, size_t count, float contentW, float targetCellW,
                   int maxColumns, float headingH, float rowSpace, float itemSpace,
                   const HgGridMetrics* metrics, HgGridPlace* out, size_t cap,
                   float* outTotalHeight);

/* WHICH ITEM IS ONE ROW AWAY -- what the D-pad's up and down mean in a grid.
 *
 * `places`/`count` are exactly what hg_grid_layout wrote. `from` is the INDEX of the item that
 * currently has focus (an index into that array, not a row number) and `deltaRow` is -1 for up and
 * +1 for down. Returns the index of the item to focus, or -1 when there is no row in that
 * direction.
 *
 * WHY THIS IS HERE AND NOT IN THE VIEW. It is the same argument as the rest of this file: this
 * arithmetic is SILENT when it is wrong. A step that lands one column off looks like the selection
 * "jumping", and a step that misses a row looks like a dead key -- neither crashes and neither
 * logs. On the host every one of those is a checkable property; in the view it could only ever be
 * observed by pressing the D-pad on a console, which is the most expensive way this project has
 * found to discover a wrong index.
 *
 * THIS IS THE HALF OF NAVIGATION BOREALIS CANNOT DO. `Box::getNextFocus` (box.cpp:409-443) walks
 * its children in INSERTION ORDER, so it can step to the next card but it cannot express "the card
 * one row down in the same column" -- and that is the whole of a grid. wiliwili answers the same
 * question the same way, from the item INDEX and the column count (`RecyclingGrid::getNextCellFocus`,
 * recycling_grid.cpp:561-592: `index +/- spanCount`), and the view here answers it from the row and
 * column hg_grid_layout already produced. See the long note in home_activity.cpp for how the two
 * halves meet.
 *
 * THE THREE PROPERTIES A WRONG VERSION WOULD BREAK, and each is a check in hg_grid_test:
 *
 *   1. THE NEAREST ROW, NOT THE NEXT ARRAY SLOT. Rows are not all full: a tier whose card count is
 *      not a multiple of the column count has a short last row, and a HEADING takes a row of its own
 *      with no card in it. So the target row is the nearest one that holds a card, searched in the
 *      direction asked for -- which is also why this takes the whole array rather than a column
 *      count: the row numbering has gaps and the gaps are not all the same size.
 *   2. THE NEAREST COLUMN WITHIN THAT ROW. Landing on the same column where it exists and otherwise
 *      on the closest one is what keeps DOWN working when the row below is shorter. A tie goes to the
 *      LEFT column so the answer never depends on how the cards happen to be ordered in the array.
 *   3. A HEADING IS NOT A DESTINATION, and not a starting point either: `kind` is checked, because a
 *      heading is not focusable and aiming at one would ask the framework to focus a view that cannot
 *      take focus.
 *
 * Rows are compared by their row NUMBER (never by array position), so this does not require `places`
 * to be sorted. hg_grid_layout does produce them in order; a caller that reordered its items would
 * still get a correct answer from this rather than a plausible-looking wrong one. */
int hg_grid_row_step(const HgGridPlace* places, size_t count, int from, int deltaRow);

/* How many columns hg_grid_layout would choose for this content width. Exposed
 * because the caller logs it and because a "why are there 5 columns" question should
 * be answerable without reproducing the arithmetic by hand.
 *
 * `maxColumns` of 0 means HG_GRID_MAX_COLUMNS (8); a value in between is used as given; anything
 * above HG_GRID_MAX_COLUMNS_HARD (12) is lowered to it. This MUST be called with the same
 * maxColumns the layout was called with, or the logged count is not the count that was used --
 * hg_grid_test checks that the two agree. */
int hg_grid_columns_for(float contentW, float targetCellW, int maxColumns);

#ifdef __cplusplus
}
#endif

#endif /* HG_GRID_H */
