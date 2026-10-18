/*
 * hg_grid implementation. See hg_grid.h for why this is C and what it models.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_grid.h"

/* The poster's own aspect and title allowance, used when the caller passes no metrics.
 * Covers are 2:3 portraits (series_cover), so a cell is 1.5x its width tall. These are
 * only the default; HG_GRID_POSTER_METRICS in the header is what a caller that cares
 * should use, and the tests check that the two agree. */
#define HG_GRID_POSTER_NUM 3.0f
#define HG_GRID_POSTER_DEN 2.0f
#define HG_GRID_POSTER_EXTRA 56.0f

/* The narrowest cell that is still a poster rather than a smear. A cell has to hold a
 * 2:3 cover AND a two-line title at a readable size, so anything under roughly an
 * inch of logical space is not a layout, it is a rendering accident.
 *
 * This is what makes the "gap so wide the cells vanish" case a REFUSAL instead of a
 * grid: with 100 pt of content and a 90 pt gap, two columns leave 5 pt per cell, and
 * drawing 45 five-pixel slivers is worse than drawing nothing and saying so. The
 * first version of this file only rejected cellW <= 1, and the test caught it. */
#define HG_GRID_MIN_CELL_W 64.0f

int hg_grid_columns_for(float contentW, float targetCellW, int maxColumns) {
  int cols;
  int ceiling = (maxColumns > 0) ? maxColumns : HG_GRID_MAX_COLUMNS;

  /* The ceiling is clamped to the HARD maximum, not to the default one -- see the note on
   * HG_GRID_MAX_COLUMNS_HARD for why a caller is allowed to ask for more than 8 at all. */
  if (ceiling > HG_GRID_MAX_COLUMNS_HARD) ceiling = HG_GRID_MAX_COLUMNS_HARD;
  if (ceiling < HG_GRID_MIN_COLUMNS) ceiling = HG_GRID_MIN_COLUMNS;
  if (contentW <= 0.0f || targetCellW <= 0.0f) return HG_GRID_MIN_COLUMNS;

  /* floor(contentW / targetCellW), computed in float then narrowed. A target of 0
   * or a negative width was handled above, so the division is safe. */
  cols = (int)(contentW / targetCellW);
  if (cols < HG_GRID_MIN_COLUMNS) cols = HG_GRID_MIN_COLUMNS;
  if (cols > ceiling) cols = ceiling;
  return cols;
}

int hg_grid_layout(const int* kinds, size_t count, float contentW, float targetCellW,
                   int maxColumns, float headingH, float rowSpace, float itemSpace,
                   const HgGridMetrics* metrics, HgGridPlace* out, size_t cap,
                   float* outTotalHeight) {
  int cols;
  float cellW;
  float cellH;
  float aspectNum;
  float aspectDen;
  float extraH;
  float cursorY = 0.0f;    /* the top of the row currently being filled */
  float rowH = 0.0f;       /* the height of that row, 0 until something lands in it */
  int column = 0;
  int row = 0;
  size_t placed = 0;
  size_t i;

  if (outTotalHeight) *outTotalHeight = 0.0f;
  if (!out || cap == 0 || contentW <= 0.0f) return 0;

  /* THE CELL SHAPE. A caller that passes nothing gets the poster shape, so the
   * original call sites keep their meaning; a caller that passes a zero numerator
   * gets a plain box `extraH` tall, which is the numbered-button case. */
  if (metrics) {
    aspectNum = metrics->aspectNum;
    aspectDen = metrics->aspectDen;
    extraH = metrics->extraH;
  } else {
    aspectNum = HG_GRID_POSTER_NUM;
    aspectDen = HG_GRID_POSTER_DEN;
    extraH = HG_GRID_POSTER_EXTRA;
  }
  /* A denominator of zero would divide by zero, and a negative image height is not a
   * cell. Refuse rather than emit infinities into a layout. */
  if (aspectDen <= 0.0f || aspectNum < 0.0f || extraH < 0.0f) return 0;

  cols = hg_grid_columns_for(contentW, targetCellW, maxColumns);

  /* THE CELL WIDTH IS DERIVED FROM THE COLUMN COUNT, NOT FROM targetCellW.
   *
   * This is the point of the whole computation. targetCellW is only a REQUEST -- "I
   * would like posters about this wide" -- and the actual width is whatever divides
   * the content evenly. Using targetCellW directly would leave a strip of unused
   * space on the right, and the grid would not line up with the screen edges, which
   * is the most visible way a grid can look wrong. */
  cellW = (contentW - itemSpace * (float)(cols - 1)) / (float)cols;

  /* A pathological caller could ask for a gap wider than the content, leaving a cell
   * too narrow to be a poster. Saying "no grid" is better than emitting 45 entries
   * full of slivers for the view to draw: the caller shows nothing and logs why,
   * instead of the screen filling with unreadable noise. */
  if (cellW < HG_GRID_MIN_CELL_W) return 0;

  /* The image part plus the space its label needs. extraH is a fixed count rather than
   * proportional: text does not scale with the cell, and a proportional allowance would
   * clip the label on a narrow column and waste space on a wide one. With aspectNum 0
   * the cell IS extraH, which is the numbered-button case. */
  cellH = cellW * aspectNum / aspectDen + extraH;

  if (count > cap) count = cap;

  for (i = 0; i < count; i++) {
    int kind = kinds[i];

    if (kind == HG_GRID_ITEM_HEADING) {
      /* A HEADING CLOSES THE OPEN ROW FIRST.
       *
       * It cannot share a row with cards: `cursorY` advances past whatever row was
       * being filled, and only up to the height of an item that actually landed in it
       * (`rowH`), so a heading straight after a full row does not add a phantom empty
       * row. Then the column cursor resets, which is what guarantees the cards after
       * it start at column 0 -- the property that keeps every tier aligned. */
      if (rowH > 0.0f) {
        cursorY += rowH + rowSpace;
        rowH = 0.0f;
        row++;
      }
      column = 0;

      out[placed].kind = HG_GRID_ITEM_HEADING;
      out[placed].row = row;
      out[placed].column = 0;
      out[placed].x = 0.0f;
      out[placed].y = cursorY;
      out[placed].w = contentW;
      out[placed].h = headingH;
      out[placed].cellW = cellW;
      out[placed].cellH = 0.0f;
      placed++;

      cursorY += headingH;
      row++;
      continue;
    }

    /* A card. If the previous card filled the last column, start a new row. */
    if (column >= cols) {
      cursorY += rowH + rowSpace;
      rowH = 0.0f;
      column = 0;
      row++;
    }

    out[placed].kind = HG_GRID_ITEM_CARD;
    out[placed].row = row;
    out[placed].column = column;
    out[placed].x = (cellW + itemSpace) * (float)column;
    out[placed].y = cursorY;
    out[placed].w = cellW;
    out[placed].h = cellH;
    out[placed].cellW = cellW;
    out[placed].cellH = cellH;
    placed++;

    /* Every card in a row has the same height here (cellH is constant), so rowH can
     * simply be assigned. It is still written as a "max" because the moment a heading
     * or a variable-height cell is introduced, an assignment would be the bug. */
    if (cellH > rowH) rowH = cellH;
    column++;
  }

  /* A trailing row still counts towards the content height, or the last row would be
   * clipped by the scroll view -- the classic "last row is half cut off" symptom. */
  if (rowH > 0.0f) cursorY += rowH;

  if (outTotalHeight) *outTotalHeight = cursorY;
  return (int)placed;
}

int hg_grid_row_step(const HgGridPlace* places, size_t count, int from, int deltaRow) {
  int fromRow;
  int fromColumn;
  int targetRow = -1;
  int bestDistance = 0;
  int bestColumn = -1;
  int found = -1;
  size_t i;

  /* A row step of zero is not a no-op to be tolerated: it means the caller did not decide which
   * direction it is asking about, and answering "the item you are already on" would look exactly
   * like a working key that does nothing. */
  if (!places || deltaRow == 0 || from < 0 || (size_t)from >= count) return -1;
  if (places[from].kind != HG_GRID_ITEM_CARD) return -1;

  fromRow = places[from].row;
  fromColumn = places[from].column;

  /* (1) the nearest row in the asked-for direction that HOLDS A CARD. Scanning for the best row
   * rather than incrementing `row` by one is what makes heading rows and empty rows harmless: a
   * heading has a row number of its own and no card in it, so it is simply never a candidate. */
  for (i = 0; i < count; i++) {
    if (places[i].kind != HG_GRID_ITEM_CARD) continue;
    if (deltaRow < 0) {
      if (places[i].row < fromRow && (targetRow < 0 || places[i].row > targetRow))
        targetRow = places[i].row;
    } else {
      if (places[i].row > fromRow && (targetRow < 0 || places[i].row < targetRow))
        targetRow = places[i].row;
    }
  }
  if (targetRow < 0) return -1;

  /* (2) the nearest column in that row, ties to the left. */
  for (i = 0; i < count; i++) {
    int distance;
    if (places[i].kind != HG_GRID_ITEM_CARD || places[i].row != targetRow) continue;
    distance = places[i].column - fromColumn;
    if (distance < 0) distance = -distance;
    if (found < 0 || distance < bestDistance ||
        (distance == bestDistance && places[i].column < bestColumn)) {
      bestDistance = distance;
      bestColumn = places[i].column;
      found = (int)i;
    }
  }

  return found;
}
