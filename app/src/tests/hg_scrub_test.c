/*
 * Tests for hg_scrub.c -- the drag-to-seek model.
 *
 * The checks that matter most:
 *
 *   - THE BAR AND THE COMMIT AGREE. Whatever fraction the cursor is drawn at is the
 *     fraction that gets seeked to. If those two disagree the preview is a lie, and the
 *     user sees the player land somewhere other than where the bar said.
 *   - RELEASING WITHOUT MOVING DOES NOT SEEK. mpv reloads the stream for a seek to where
 *     it already is, so a tap that "seeks to the current position" is pure cost.
 *   - A DRAG WITH NO DURATION DOES NOTHING. mpv reports duration 0 or -1 for a while
 *     after a load; a model that scaled by it would seek to 0.
 *   - THE STATE IS IN SECONDS. The first version of hg_scrub.c added a step in SECONDS to
 *     a field holding a FRACTION, and "the target moved by 60 s of 6000" failed. That
 *     check is first for a reason.
 *
 * ASCII-only source file.
 */

#include "hg_scrub.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

static float absf_(float v) { return v < 0.0f ? -v : v; }
static int nearf(float a, float b) { return absf_(a - b) < 0.01f; }
static int neard(double a, double b, double tol) {
  double d = a - b;
  if (d < 0) d = -d;
  return d < tol;
}

/* ---- begin / step / end --------------------------------------------------- */

static void test_lifecycle(void) {
  HgScrub s;
  double out = -1.0;

  printf("\n-- a drag begins, moves, and commits once --\n");

  hg_scrub_init(&s);
  ok("a fresh model is inactive", s.active == 0);
  ok("step before begin does nothing", hg_scrub_step(&s, 5.0) == 0);
  ok("end before begin reports no seek", hg_scrub_end(&s, &out) == 0);

  /* 600 seconds in to a 6000-second video, dragging right. */
  ok("begin succeeds with a real duration", hg_scrub_begin(&s, 600.0, 6000.0, +1, 0.0) == 1);
  ok("the drag is active", s.active == 1);
  ok("the anchor is the playback position", neard(s.anchorSeconds, 600.0, 1e-9));
  ok("the target starts at the anchor", neard(s.posSeconds, 600.0, 1e-9));
  ok("the duration is kept for the clamp", neard(s.duration, 6000.0, 1e-9));
  ok("nothing has moved yet", neard(s.movedSeconds, 0.0, 1e-9));
  ok("the fraction at the start is 0.1", neard(hg_scrub_fraction(&s), 0.1, 1e-9));

  /* THIS IS THE CHECK THE FIRST VERSION FAILED: a step is SECONDS, so 60 more seconds on a
   * 6000-second video is 660 s, which is 11% of the bar. */
  ok("a step reports that the target changed", hg_scrub_step(&s, 60.0) == 1);
  ok("the target moved 60 s, to 660 s", neard(s.posSeconds, 660.0, 1e-9));
  ok("the anchor did not move", neard(s.anchorSeconds, 600.0, 1e-9));
  ok("the distance travelled is 60 s", neard(s.movedSeconds, 60.0, 1e-9));
  ok("the fraction is now 0.11", neard(hg_scrub_fraction(&s), 0.11, 1e-9));

  ok("releasing reports a seek", hg_scrub_end(&s, &out) == 1);
  ok("the committed position is the target, in seconds", neard(out, 660.0, 1e-9));
  ok("the model is reset after the commit", s.active == 0);
  ok("a second release does not seek again", hg_scrub_end(&s, &out) == 0);
}

static void test_no_seek_without_movement(void) {
  HgScrub s;
  double out = -1.0;

  printf("\n-- releasing without moving does not seek --\n");

  hg_scrub_init(&s);
  hg_scrub_begin(&s, 1800.0, 3600.0, +1, 0.0);
  ok("a drag with no step commits nothing", hg_scrub_end(&s, &out) == 0);
  ok("but the position is still reported", neard(out, 1800.0, 1e-9));

  /* Stepping out and back to exactly where it started is the same thing: the position did
   * not change, so there is nothing to seek to. */
  hg_scrub_init(&s);
  hg_scrub_begin(&s, 1800.0, 3600.0, +1, 0.0);
  hg_scrub_step(&s, 30.0);
  hg_scrub_step(&s, -30.0);
  ok("a step out and back commits nothing", hg_scrub_end(&s, &out) == 0);
  ok("but it counts as having dragged 60 s", neard(s.movedSeconds, 60.0, 1e-9) == 0);
}

/* ---- clamping ------------------------------------------------------------ */

static void test_clamping(void) {
  HgScrub s;
  double out = -1.0;
  int i;

  printf("\n-- the target never leaves the video --\n");

  hg_scrub_init(&s);
  hg_scrub_begin(&s, 900.0, 1000.0, +1, 0.0);
  for (i = 0; i < 50; i++) hg_scrub_step(&s, 10.0);   /* 500 s past the end */
  ok("dragging right past the end clamps at the duration", neard(s.posSeconds, 1000.0, 1e-9));
  ok("the fraction is 1.0", neard(hg_scrub_fraction(&s), 1.0, 1e-9));
  ok("a step at the end reports no change", hg_scrub_step(&s, 10.0) == 0);
  hg_scrub_end(&s, &out);
  ok("the commit is the clamped position", neard(out, 1000.0, 1e-9));

  hg_scrub_init(&s);
  hg_scrub_begin(&s, 100.0, 1000.0, -1, 0.0);
  for (i = 0; i < 50; i++) hg_scrub_step(&s, -10.0);
  ok("dragging left past the start clamps at 0", neard(s.posSeconds, 0.0, 1e-9));
  hg_scrub_end(&s, &out);
  ok("the commit is 0", neard(out, 0.0, 1e-9));

  /* An out-of-range STARTING position is clamped too: it comes from mpv, which can report
   * a position past the duration on a stream whose duration it estimated. */
  hg_scrub_init(&s);
  ok("begin clamps a position past the end",
     hg_scrub_begin(&s, 9999.0, 1000.0, +1, 0.0) == 1 && neard(s.posSeconds, 1000.0, 1e-9));
  hg_scrub_init(&s);
  ok("begin clamps a negative position",
     hg_scrub_begin(&s, -30.0, 1000.0, +1, 0.0) == 1 && neard(s.posSeconds, 0.0, 1e-9));
}

static void test_refusals(void) {
  HgScrub s;

  printf("\n-- no duration means no drag --\n");

  hg_scrub_init(&s);
  ok("begin refuses a zero duration", hg_scrub_begin(&s, 300.0, 0.0, +1, 0.0) == 0);
  ok("nothing is active after the refusal", s.active == 0);
  ok("a step after the refusal does nothing", hg_scrub_step(&s, 5.0) == 0);
  ok("and it does not become active", s.active == 0);
  ok("its fraction is 0", neard(hg_scrub_fraction(&s), 0.0, 1e-9));

  hg_scrub_init(&s);
  ok("begin refuses a negative duration", hg_scrub_begin(&s, 300.0, -1.0, +1, 0.0) == 0);

  hg_scrub_init(&s);
  ok("begin refuses a zero direction", hg_scrub_begin(&s, 300.0, 100.0, 0, 0.0) == 0);

  /* NULL tolerance: these are called from a draw path where a null would otherwise be a
   * crash on the console with no message. */
  ok("begin(NULL) is refused", hg_scrub_begin(NULL, 300.0, 100.0, 1, 0.0) == 0);
  ok("step(NULL) is refused", hg_scrub_step(NULL, 1.0) == 0);
  ok("end(NULL) is refused", hg_scrub_end(NULL, NULL) == 0);
  ok("a NULL out pointer is tolerated", hg_scrub_end(&s, NULL) == 0);
  ok("fraction(NULL) is 0", neard(hg_scrub_fraction(NULL), 0.0, 1e-9));
  hg_scrub_init(NULL);
  ok("init(NULL) is tolerated", 1);
}

/* ---- the acceleration input ---------------------------------------------- */

static void test_moved_distance(void) {
  HgScrub s;

  printf("\n-- movedSeconds is distance, not displacement --\n");

  hg_scrub_init(&s);
  hg_scrub_begin(&s, 300.0, 600.0, +1, 0.0);
  hg_scrub_step(&s, 30.0);
  hg_scrub_step(&s, 30.0);
  ok("two steps right accumulate", neard(s.movedSeconds, 60.0, 1e-9));

  /* Dragging back the way you came still counts as having dragged: the acceleration curve
   * is fed the effort, not the net position. A signed sum would drop to 0 after a reversal
   * and the drag would suddenly slow down, which is the opposite of what the curve is for. */
  hg_scrub_step(&s, -60.0);
  ok("a reversal does not cancel the distance", neard(s.movedSeconds, 120.0, 1e-9));
  ok("but the position did reverse", neard(s.posSeconds, 300.0, 1e-9));
}

/* ---- preview scheduling -------------------------------------------------- */

static void test_preview_scheduling(void) {
  HgScrub s;

  printf("\n-- the preview is rate limited by the model --\n");

  hg_scrub_init(&s);
  hg_scrub_begin(&s, 200.0, 1000.0, +1, 1000.0);
  ok("the first preview is due immediately", hg_scrub_preview_due(&s, 1000.0) == 1);
  hg_scrub_preview_done(&s, 1000.0);
  ok("a second one in the same millisecond is not", hg_scrub_preview_due(&s, 1000.0) == 0);
  ok("nor one 100 ms later", hg_scrub_preview_due(&s, 1100.0) == 0);
  ok("one at the floor is due", hg_scrub_preview_due(&s, 1000.0 + HG_SCRUB_PREVIEW_MS) == 1);
  ok("one well past the floor is due", hg_scrub_preview_due(&s, 5000.0) == 1);

  /* A backwards clock must not bypass the floor. */
  hg_scrub_preview_done(&s, 9000.0);
  ok("a stale timestamp is not adopted as newer", hg_scrub_preview_due(&s, 9100.0) == 0);
  hg_scrub_preview_done(&s, 9100.0);

  /* An inactive model never asks for a preview. */
  hg_scrub_init(&s);
  ok("an inactive drag never wants a preview", hg_scrub_preview_due(&s, 100000.0) == 0);
}

/* ---- the bar geometry ---------------------------------------------------- */

static void test_geometry(void) {
  HgScrubBar bar;
  int i;

  printf("\n-- the bar and the commit agree --\n");

  bar.barX = 16.0f;
  bar.barW = 1248.0f;
  bar.duration = 3600.0;

  ok("fraction 0 lands at the left inset",
     nearf(hg_scrub_x_for(&bar, 0.0), bar.barX + HG_SCRUB_INSET * 0.5f));
  ok("fraction 1 lands at the right inset",
     nearf(hg_scrub_x_for(&bar, 1.0), bar.barX + bar.barW - HG_SCRUB_INSET * 0.5f));
  ok("fraction 0.5 lands in the middle",
     nearf(hg_scrub_x_for(&bar, 0.5), bar.barX + bar.barW * 0.5f));
  ok("an out-of-range fraction is clamped into the bar",
     nearf(hg_scrub_x_for(&bar, 2.0), hg_scrub_x_for(&bar, 1.0)));

  /* THE INVARIANT: x_for and fraction_at are inverses. This is the check that stops the
   * preview showing one moment while the seek lands on another. */
  {
    int worst = 0;
    for (i = 0; i <= 64; i++) {
      double f = (double)i / 64.0;
      float x = hg_scrub_x_for(&bar, f);
      double back = hg_scrub_fraction_at(&bar, x);
      double d = back - f;
      if (d < 0) d = -d;
      if (d > 1e-4) worst = 1;
    }
    ok("every 1/64th of the bar round-trips through x and back", worst == 0);
  }

  ok("a click left of the bar means 0", neard(hg_scrub_fraction_at(&bar, -500.0f), 0.0, 1e-9));
  ok("a click right of the bar means 1", neard(hg_scrub_fraction_at(&bar, 5000.0f), 1.0, 1e-9));
  ok("a click at the middle means 0.5",
     neard(hg_scrub_fraction_at(&bar, bar.barX + bar.barW * 0.5f), 0.5, 1e-3));

  /* The end-to-end property, stated in the terms the user sees: the x the cursor is drawn
   * at, turned back into a time, is the time the seek lands on. */
  {
    HgScrub s;
    double out = -1.0;
    double f = hg_scrub_fraction_at(&bar, hg_scrub_x_for(&bar, 0.37));
    hg_scrub_init(&s);
    hg_scrub_begin(&s, 0.0, bar.duration, +1, 0.0);
    hg_scrub_step(&s, f * bar.duration);
    hg_scrub_end(&s, &out);
    ok("a position drawn at 0.37 commits 0.37 of the duration",
       neard(out / bar.duration, 0.37, 1e-3));
  }

  /* Degenerate bars must not divide by zero. */
  {
    HgScrubBar zero = {0.0f, 0.0f, 0.0};
    ok("a zero-width bar maps everything to 0",
       neard(hg_scrub_fraction_at(&zero, 100.0f), 0.0, 1e-9));
    ok("a zero-width bar draws at its x", nearf(hg_scrub_x_for(&zero, 0.5), 0.0f));
    ok("a NULL bar is tolerated", hg_scrub_fraction_at(NULL, 1.0f) == 0.0);
    ok("a NULL bar draws at 0", nearf(hg_scrub_x_for(NULL, 0.5), 0.0f));
  }
}

/* ---- time formatting ----------------------------------------------------- */

static void test_time_format(void) {
  char b[32];
  int n;

  printf("\n-- time formatting --\n");

  hg_scrub_format_time(0.0, b, (int)sizeof b);
  ok("0 seconds is 0:00", strcmp(b, "0:00") == 0);
  hg_scrub_format_time(5.0, b, (int)sizeof b);
  ok("5 seconds is 0:05", strcmp(b, "0:05") == 0);
  hg_scrub_format_time(65.0, b, (int)sizeof b);
  ok("65 seconds is 1:05", strcmp(b, "1:05") == 0);
  hg_scrub_format_time(599.0, b, (int)sizeof b);
  ok("599 seconds is 9:59", strcmp(b, "9:59") == 0);
  hg_scrub_format_time(3600.0, b, (int)sizeof b);
  ok("3600 seconds is 1:00:00", strcmp(b, "1:00:00") == 0);
  hg_scrub_format_time(3661.0, b, (int)sizeof b);
  ok("3661 seconds is 1:01:01", strcmp(b, "1:01:01") == 0);

  /* Rounding, not truncation: 59.6 s must not read as 0:59 for ever. */
  hg_scrub_format_time(59.6, b, (int)sizeof b);
  ok("59.6 seconds rounds up to 1:00", strcmp(b, "1:00") == 0);

  /* mpv reports negative positions for "unknown"; the bar must not show arithmetic. */
  hg_scrub_format_time(-1.0, b, (int)sizeof b);
  ok("a negative time is 0:00", strcmp(b, "0:00") == 0);
  hg_scrub_format_time(-99999.0, b, (int)sizeof b);
  ok("a large negative time is 0:00", strcmp(b, "0:00") == 0);

  /* A truncated buffer is NUL-terminated (TinyCC's snprintf does not guarantee this).
   * 4 bytes cannot hold "1:01:01" (7 characters), so the result is a prefix. */
  memset(b, 'X', sizeof b);
  n = hg_scrub_format_time(3661.0, b, 4);
  ok("a 4-byte buffer is NUL-terminated", b[3] == '\0');
  ok("the truncated text is a prefix of the full one", strcmp(b, "1:0") == 0);
  ok("the reported length is still the full length snprintf would need", n == 7);
  ok("a NULL buffer is tolerated", hg_scrub_format_time(10.0, NULL, 32) == 0);
  ok("a zero-size buffer is tolerated", hg_scrub_format_time(10.0, b, 0) == 0);
}

int main(void) {
  printf("hg_scrub tests: drag-to-seek and the progress bar\n");
  printf("================================================\n");

  test_lifecycle();
  test_no_seek_without_movement();
  test_clamping();
  test_refusals();
  test_moved_distance();
  test_preview_scheduling();
  test_geometry();
  test_time_format();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
