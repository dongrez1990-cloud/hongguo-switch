/*
 * Tests for hg_pmenu.c -- the player picture menu's arithmetic.
 *
 * WHY THESE AND NOT "does the menu draw". The drawing is a borealis Box and cannot
 * be tested on the host; the arithmetic can, and the arithmetic is where this
 * project has actually been burned (a silent index error took the console down
 * with no message at all). So every check below is about a value, a bound, a wrap,
 * a clamp or a mapping -- the places where being wrong is silent on screen.
 *
 * The mapping checks are the important ones. "Aspect" is one integer here and three
 * mpv properties there; if the mapping were wrong in a way that left one of the
 * three describing the PREVIOUS mode, the picture would be both stretched and
 * letterboxed, which reads as a broken player rather than as a wrong setting.
 *
 * ASCII-only source file.
 */

#include "hg_pmenu.h"
#include "hg_lang.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-56s\n", what);
  else { failures++; printf("  FAIL  %-56s\n", what); }
}

/* Render a row into a caller-provided buffer. Used instead of repeating the
 * snprintf dance at each site, because the buffer is what the on-screen label gets
 * and a truncated value is a wrong value. */
static const char *valueOf(const HgPMenu *m, int row) {
  static char buf[64];
  return hg_pmenu_row_value(m, row, buf, (int)sizeof buf);
}

/* ---- defaults ------------------------------------------------------------- */

static void test_defaults(void) {
  HgPMenu m;
  int i;

  printf("\n-- a fresh menu is neutral --\n");
  memset(&m, 0xAB, sizeof m);   /* deliberately not zero: a stale heap pattern must
                                 * not be able to look like a default */
  m.open = 0;
  m.row = 0;
  hg_pmenu_reset(&m);

  for (i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
    if (m.values[i] != 0) {
      printf("  FAIL  row %d default is %d\n", i, m.values[i]);
      failures++; checks++;
      return;
    }
  }
  ok("every picture value defaults to 0", 1);
  ok("a zero value renders as '+0'", strcmp(valueOf(&m, HG_PMENU_ROW_BRIGHTNESS), "+0") == 0);
  ok("aspect defaults to Auto", m.aspect == HG_PMENU_ASPECT_AUTO);
  ok("mirror defaults to off", m.mirror == 0);

  /* Reset must NOT touch the highlight or the open flag, or pressing reset would
   * make the highlight jump under the user's finger. */
  m.row = HG_PMENU_ROW_HUE;
  m.values[HG_PMENU_ROW_HUE] = 40;
  m.open = 1;
  hg_pmenu_reset(&m);
  ok("reset leaves the highlighted row alone", m.row == HG_PMENU_ROW_HUE);
  ok("reset leaves the menu open", m.open == 1);
  ok("reset clears the value on the highlighted row", m.values[HG_PMENU_ROW_HUE] == 0);

  hg_pmenu_reset(NULL);
  ok("reset(NULL) is tolerated", 1);
}

/* ---- clamping ------------------------------------------------------------- */

static void test_clamp(void) {
  HgPMenu m;
  int i;

  printf("\n-- values are clamped, never wrapped --\n");
  ok("clamp(-101) == -100", hg_pmenu_clamp(-101) == HG_PMENU_VALUE_MIN);
  ok("clamp(101) == 100", hg_pmenu_clamp(101) == HG_PMENU_VALUE_MAX);
  ok("clamp(0) == 0", hg_pmenu_clamp(0) == 0);
  ok("the range is mpv's own -100..100",
     HG_PMENU_VALUE_MIN == -100 && HG_PMENU_VALUE_MAX == 100);

  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);
  m.row = HG_PMENU_ROW_BRIGHTNESS;

  /* The step size is a measurable claim, not a comment: five normal steps down
   * from neutral land exactly on -25. */
  for (i = 0; i < 5; i++) hg_pmenu_adjust(&m, -1, 0);
  ok("five normal steps down from 0 reach -25", m.values[HG_PMENU_ROW_BRIGHTNESS] == -25);

  /* Drive it past the end and check it STOPS rather than wrapping to +100. */
  for (i = 0; i < 40; i++) hg_pmenu_adjust(&m, -1, 0);
  ok("down past the bottom clamps at -100",
     m.values[HG_PMENU_ROW_BRIGHTNESS] == HG_PMENU_VALUE_MIN);

  /* At the bound the key must report "nothing changed": a caller that logs and
   * re-applies on every keypress would otherwise talk to mpv forever at the end of
   * the range. */
  ok("a step that changes nothing returns 0", hg_pmenu_adjust(&m, -1, 0) == 0);
  ok("a step at the bound leaves the value at the bound",
     m.values[HG_PMENU_ROW_BRIGHTNESS] == HG_PMENU_VALUE_MIN);

  for (i = 0; i < 40; i++) hg_pmenu_adjust(&m, +1, 0);
  ok("up past the top clamps at +100",
     m.values[HG_PMENU_ROW_BRIGHTNESS] == HG_PMENU_VALUE_MAX);

  /* A fine step is 1 and lands exactly. */
  ok("one fine step down is 1",
     hg_pmenu_adjust(&m, -1, 1) == 1 && m.values[HG_PMENU_ROW_BRIGHTNESS] == 99);
  ok("one fine step back up is 1",
     hg_pmenu_adjust(&m, +1, 1) == 1 && m.values[HG_PMENU_ROW_BRIGHTNESS] == 100);
  ok("hg_pmenu_step_normal() == 5", hg_pmenu_step_normal() == 5);
  ok("hg_pmenu_step_fine() == 1", hg_pmenu_step_fine() == 1);

  ok("adjust(NULL) is tolerated", hg_pmenu_adjust(NULL, 1, 0) == 0);
  ok("adjust(dir=0) reports no change", hg_pmenu_adjust(&m, 0, 0) == 0);
}

/* ---- row navigation ------------------------------------------------------- */

static void test_navigation(void) {
  HgPMenu m;
  int i;

  printf("\n-- row navigation wraps --\n");
  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);

  ok("starts on the first row", m.row == HG_PMENU_ROW_BRIGHTNESS);
  ok("up from the first row wraps to Reset",
     hg_pmenu_move(&m, -1) == 1 && m.row == HG_PMENU_ROW_RESET);
  ok("down from Reset wraps back to Brightness",
     hg_pmenu_move(&m, +1) == 1 && m.row == HG_PMENU_ROW_BRIGHTNESS);

  /* A full lap must return to the start exactly -- the classic off-by-one. */
  for (i = 0; i < HG_PMENU_ROW_COUNT; i++) hg_pmenu_move(&m, +1);
  ok("a full lap of HG_PMENU_ROW_COUNT moves returns to row 0",
     m.row == HG_PMENU_ROW_BRIGHTNESS);

  for (i = 0; i < HG_PMENU_ROW_COUNT - 1; i++) hg_pmenu_move(&m, +1);
  ok("the last reachable row is Reset", m.row == HG_PMENU_ROW_RESET);

  ok("move(0) reports no change", hg_pmenu_move(&m, 0) == 0);
  ok("move(NULL) is tolerated", hg_pmenu_move(NULL, 1) == 0);

  ok("row_is_adjust accepts Brightness", hg_pmenu_row_is_adjust(HG_PMENU_ROW_BRIGHTNESS) == 1);
  ok("row_is_adjust accepts Hue", hg_pmenu_row_is_adjust(HG_PMENU_ROW_HUE) == 1);
  ok("row_is_adjust rejects Aspect", hg_pmenu_row_is_adjust(HG_PMENU_ROW_ASPECT) == 0);
  ok("row_is_adjust rejects Mirror", hg_pmenu_row_is_adjust(HG_PMENU_ROW_MIRROR) == 0);
  ok("row_is_adjust rejects Reset", hg_pmenu_row_is_adjust(HG_PMENU_ROW_RESET) == 0);
  ok("row_is_adjust rejects a negative row", hg_pmenu_row_is_adjust(-1) == 0);
  ok("row_is_adjust rejects HG_PMENU_ROW_COUNT", hg_pmenu_row_is_adjust(HG_PMENU_ROW_COUNT) == 0);

  /* Row order is part of the contract: the renderer indexes the label table by it,
   * and the C++ side maps each row to an mpv property by it. */
  ok("row 0 is Brightness", HG_PMENU_ROW_BRIGHTNESS == 0);
  ok("Hue is the last numeric row", HG_PMENU_ROW_HUE + 1 == HG_PMENU_ROW_QUALITY);
  ok("Quality sits between the numbers and the named values",
     HG_PMENU_ROW_QUALITY < HG_PMENU_ROW_ASPECT);
  ok("Reset is the last row", HG_PMENU_ROW_RESET + 1 == HG_PMENU_ROW_COUNT);
  ok("Quality is NOT a numeric row", hg_pmenu_row_is_adjust(HG_PMENU_ROW_QUALITY) == 0);
}

/* ---- the non-numeric rows ------------------------------------------------- */

static void test_switches_and_reset(void) {
  HgPMenu m;

  printf("\n-- aspect / mirror / reset --\n");
  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);
  m.row = HG_PMENU_ROW_ASPECT;

  ok("aspect renders 'Auto'", strcmp(valueOf(&m, HG_PMENU_ROW_ASPECT), "Auto") == 0);
  ok("right moves Auto -> Crop",
     hg_pmenu_adjust(&m, +1, 0) == 1 && m.aspect == HG_PMENU_ASPECT_CROP);
  ok("aspect renders 'Crop'",
     strcmp(valueOf(&m, HG_PMENU_ROW_ASPECT), "Crop") == 0);
  ok("right moves Crop -> Stretch",
     hg_pmenu_adjust(&m, +1, 0) == 1 && m.aspect == HG_PMENU_ASPECT_STRETCH);
  ok("right wraps Stretch -> Auto",
     hg_pmenu_adjust(&m, +1, 0) == 1 && m.aspect == HG_PMENU_ASPECT_AUTO);
  ok("left wraps Auto -> Stretch",
     hg_pmenu_adjust(&m, -1, 0) == 1 && m.aspect == HG_PMENU_ASPECT_STRETCH);
  ok("aspect ignores the fine flag",
     hg_pmenu_adjust(&m, -1, 1) == 1 && m.aspect == HG_PMENU_ASPECT_CROP);

  m.row = HG_PMENU_ROW_MIRROR;
  ok("mirror starts off and renders 'Off'",
     m.mirror == 0 && strcmp(valueOf(&m, HG_PMENU_ROW_MIRROR), "Off") == 0);
  ok("mirror toggles on", hg_pmenu_adjust(&m, +1, 0) == 1 && m.mirror == 1);
  ok("mirror renders 'On' when on",
     strcmp(valueOf(&m, HG_PMENU_ROW_MIRROR), "On") == 0);
  /* A switch has no left and right: EITHER direction toggles it. */
  ok("mirror toggles on a LEFT too", hg_pmenu_adjust(&m, -1, 0) == 1 && m.mirror == 0);

  /* The aspect moves above left it on Crop, which is itself something for reset to
   * clean up -- so the "nothing to do" case has to be established explicitly.
   * (Getting this wrong is what the first run of this test did, and the failure was
   * real: reset reported a change because there genuinely was one.) */
  m.aspect = HG_PMENU_ASPECT_AUTO;
  m.row = HG_PMENU_ROW_RESET;
  ok("reset with nothing to do reports no change", hg_pmenu_adjust(&m, +1, 0) == 0);

  m.values[HG_PMENU_ROW_GAMMA] = 35;
  m.values[HG_PMENU_ROW_SATURATION] = -10;
  m.aspect = HG_PMENU_ASPECT_CROP;
  m.mirror = 1;
  ok("reset with something to do reports a change", hg_pmenu_adjust(&m, -1, 0) == 1);
  ok("reset cleared gamma", m.values[HG_PMENU_ROW_GAMMA] == 0);
  ok("reset cleared saturation", m.values[HG_PMENU_ROW_SATURATION] == 0);
  ok("reset returned aspect to Auto", m.aspect == HG_PMENU_ASPECT_AUTO);
  ok("reset turned mirror off", m.mirror == 0);
  ok("reset kept the highlight on the reset row", m.row == HG_PMENU_ROW_RESET);
}

/* ---- the mapping to mpv --------------------------------------------------- */

static void test_mpv_mapping(void) {
  HgPMenu m;
  const char *keep;
  const char *over;
  const char *pan;

  printf("\n-- the aspect triple is always fully written --\n");
  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);

  /* Pre-fill with a sentinel that is not a legal property value: if the function
   * ever returns without writing one of the three, the check names it. */
  m.aspect = HG_PMENU_ASPECT_AUTO;
  keep = over = pan = "UNSET";
  hg_pmenu_aspect_props(&m, &keep, &over, &pan);
  ok("auto: keepaspect=yes", strcmp(keep, "yes") == 0);
  ok("auto: override=-1 (the file's own aspect)", strcmp(over, "-1") == 0);
  ok("auto: panscan=0.0", strcmp(pan, "0.0") == 0);

  m.aspect = HG_PMENU_ASPECT_CROP;
  keep = over = pan = "UNSET";
  hg_pmenu_aspect_props(&m, &keep, &over, &pan);
  ok("crop: keepaspect=yes", strcmp(keep, "yes") == 0);
  ok("crop: panscan=1.0 (fills, then pans)", strcmp(pan, "1.0") == 0);
  ok("crop: override=-1", strcmp(over, "-1") == 0);

  m.aspect = HG_PMENU_ASPECT_STRETCH;
  keep = over = pan = "UNSET";
  hg_pmenu_aspect_props(&m, &keep, &over, &pan);
  ok("stretch: keepaspect=no (this is what stretches)", strcmp(keep, "no") == 0);
  ok("stretch: panscan back to 0.0", strcmp(pan, "0.0") == 0);
  ok("stretch: override still -1", strcmp(over, "-1") == 0);

  /* Every mode must leave keepaspect AND panscan describing THAT mode. This is the
   * check that catches a mapping which only ever writes two of the three -- the
   * case that shows a stretched picture with black bars around it. */
  m.aspect = HG_PMENU_ASPECT_AUTO;
  hg_pmenu_aspect_props(&m, &keep, NULL, &pan);
  ok("auto after stretch restores keepaspect=yes and panscan=0.0",
     strcmp(keep, "yes") == 0 && strcmp(pan, "0.0") == 0);

  /* An out-of-range aspect must not leave any of the three unset. */
  m.aspect = 42;
  keep = over = pan = "UNSET";
  hg_pmenu_aspect_props(&m, &keep, &over, &pan);
  ok("an out-of-range aspect falls back to auto and still fills all three",
     strcmp(keep, "yes") == 0 && strcmp(over, "-1") == 0 && strcmp(pan, "0.0") == 0);

  /* NULL tolerance: a caller may care about only one of the three. */
  hg_pmenu_aspect_props(&m, NULL, NULL, NULL);
  hg_pmenu_aspect_props(NULL, &keep, &over, &pan);
  ok("NULL out-parameters and a NULL menu are tolerated", 1);

  printf("\n-- mirror filter --\n");
  m.mirror = 0;
  ok("mirror off clears vf", strcmp(hg_pmenu_mirror_filter(&m), "") == 0);
  m.mirror = 1;
  ok("mirror on is hflip", strcmp(hg_pmenu_mirror_filter(&m), "hflip") == 0);
  ok("a NULL menu has no filter", strcmp(hg_pmenu_mirror_filter(NULL), "") == 0);
}

/* ---- labels and rendering ------------------------------------------------- */

static void test_labels(void) {
  HgPMenu m;
  char b[8];
  int i;

  printf("\n-- labels and value rendering --\n");
  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);

  for (i = 0; i < HG_PMENU_ROW_COUNT; i++) {
    const char *label = hg_pmenu_row_label(i);
    if (!label || label[0] == '\0') {
      printf("  FAIL  row %d has no label\n", i);
      failures++; checks++;
      return;
    }
  }
  ok("every row has a non-empty label", 1);
  ok("label past the end is NULL", hg_pmenu_row_label(HG_PMENU_ROW_COUNT) == NULL);
  ok("label before the start is NULL", hg_pmenu_row_label(-1) == NULL);

  /* An out-of-range row must render something visible rather than leaving the
   * caller's buffer untouched: stale text in a label is a silent wrong value. */
  b[0] = 'X';
  b[1] = '\0';
  hg_pmenu_row_value(&m, 999, b, (int)sizeof b);
  ok("an out-of-range row renders '?'", strcmp(b, "?") == 0);
  hg_pmenu_row_value(NULL, 0, b, (int)sizeof b);
  ok("a NULL menu renders '?'", strcmp(b, "?") == 0);
  hg_pmenu_row_value(&m, 0, NULL, 0);
  ok("a NULL buffer is tolerated", 1);

  /* A small buffer must be truncated and still NUL-terminated, not overrun. */
  memset(b, 'X', sizeof b);
  hg_pmenu_row_value(&m, HG_PMENU_ROW_RESET, b, 4);
  ok("a 4-byte buffer is NUL-terminated", b[3] == '\0');

  m.values[HG_PMENU_ROW_CONTRAST] = -7;
  ok("a negative value renders with its sign",
     strcmp(valueOf(&m, HG_PMENU_ROW_CONTRAST), "-7") == 0);

  /* Every numeric row renders its OWN value, not a neighbour's -- the one bug that
   * would put a plausible number on the wrong row. */
  for (i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
    m.values[i] = 10 + i;
  }
  {
    int distinct = 1;
    for (i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
      char want[16];
      snprintf(want, sizeof want, "%+d", 10 + i);
      if (strcmp(valueOf(&m, i), want) != 0) distinct = 0;
    }
    ok("each numeric row renders its own value", distinct);
  }
}

/* ---- the quality row ----------------------------------------------------- */

static void test_quality_row(void) {
  HgPMenu m;
  char b[48];

  printf("\n-- quality: a ladder the caller owns --\n");

  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);
  m.row = HG_PMENU_ROW_QUALITY;

  /* BEFORE THE LADDER ARRIVES. This is the state the player is in for the first second of
   * playback, and it must not read as "tier 0 of 0" -- an empty menu and a wrong menu look the
   * same unless the row says which it is. */
  ok("with no ladder the row is inactive", m.qualityCount == 0);
  ok("stepping it does nothing", hg_pmenu_adjust(&m, +1, 0) == 0);
  ok("and the index stays at 0", m.qualityIndex == 0);
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, b, (int)sizeof b);
  ok("it renders as not available, not as '1 of 0'", strcmp(b, "(not available)") == 0);

  /* WITH A LADDER. The five measured tiers of a real response. */
  m.qualityCount = 5;
  m.qualityIndex = 0;
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, b, (int)sizeof b);
  ok("the best tier reads as 1 of 5", strcmp(b, "1 of 5") == 0);

  ok("right steps down the ladder", hg_pmenu_adjust(&m, +1, 0) == 1 && m.qualityIndex == 1);
  /* THE CURSOR AND THE TIER IN FORCE ARE NOW TWO NUMBERS (the user's two-step, §3.1), so a readout check
   * has to say which of the two it means. Marking this cursor as applied gives the plain readout; the
   * PENDING form is checked in test_lang_keys, where the marker is the thing under test. */
  m.qualityApplied = m.qualityIndex;
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, b, (int)sizeof b);
  ok("and the readout follows", strcmp(b, "2 of 5") == 0);
  ok("left steps back up", hg_pmenu_adjust(&m, -1, 0) == 1 && m.qualityIndex == 0);

  /* WRAPS, like aspect. A ladder whose ends are dead keys reads as a broken menu. */
  ok("left from the best wraps to the worst",
     hg_pmenu_adjust(&m, -1, 0) == 1 && m.qualityIndex == 4);
  ok("right from the worst wraps to the best",
     hg_pmenu_adjust(&m, +1, 0) == 1 && m.qualityIndex == 0);

  /* A ONE-TIER ladder: every step is a no-op rather than a wrap onto itself, so the caller
   * does not re-load the stream for a change that is not one. */
  m.qualityCount = 1;
  m.qualityIndex = 0;
  m.qualityApplied = 0;   /* the cursor and the tier in force agree, so no pending marker */
  ok("a single-tier ladder does not step", hg_pmenu_adjust(&m, +1, 0) == 0);
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, b, (int)sizeof b);
  ok("it reads as 1 of 1", strcmp(b, "1 of 1") == 0);

  /* RESET MUST NOT TOUCH IT: resetting the picture does not change which stream is playing, so
   * a reset that silently switched quality would be a surprise on a different scale from the
   * other seven rows. */
  memset(&m, 0, sizeof m);
  hg_pmenu_reset(&m);
  m.qualityCount = 5;
  m.qualityIndex = 3;
  m.row = HG_PMENU_ROW_RESET;
  m.values[HG_PMENU_ROW_GAMMA] = 40;
  hg_pmenu_adjust(&m, +1, 0);
  ok("reset leaves the quality selection alone", m.qualityIndex == 3);
  ok("reset still clears the picture values", m.values[HG_PMENU_ROW_GAMMA] == 0);
}

/* ---- the language keys, and the pending marker -------------------------------
 *
 * WHY THIS IS TESTED. The Chinese menu was not missing strings -- the keys (HG_T_ROW_*, HG_T_VALUE_*,
 * HG_T_NOT_AVAILABLE) already existed. What was missing was the WIRING: the panel drew the model's English
 * strings. So the thing that can now go wrong is the MAPPING, and these checks are that mapping:
 *
 *   - every row must report a label key that hg_tr() actually resolves (a key that returns "" would draw an
 *     empty row and look like a layout bug);
 *   - the value keys must follow the state (On vs Off, Auto vs Crop vs Stretch, available vs not);
 *   - the quality row must MARK a moved cursor, because that is the whole visible half of the two-step.
 */
static void test_lang_keys(void) {
  HgPMenu m;
  char buf[48];
  int i;

  printf("\nlanguage keys and the pending marker\n");

  for (i = 0; i < HG_PMENU_ROW_COUNT; i++) {
    int key = hg_pmenu_row_label_key(i);
    if (key < 0 || hg_tr(key)[0] == '\0') {
      printf("  FAIL  row %d has no label key that resolves\n", i);
      failures++;
    }
    checks++;
  }
  ok("every row has a label key that resolves", 1);
  ok("an out-of-range row has no label key", hg_pmenu_row_label_key(-1) == -1 &&
                                                hg_pmenu_row_label_key(HG_PMENU_ROW_COUNT) == -1);

  memset(&m, 0, sizeof m);   /* the suite's own way of starting from a known state */
  m.mirror = 0;
  ok("mirror off reports the Off key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_MIRROR) == HG_T_VALUE_OFF);
  m.mirror = 1;
  ok("mirror on reports the On key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_MIRROR) == HG_T_VALUE_ON);

  m.aspect = HG_PMENU_ASPECT_AUTO;
  ok("aspect auto reports the Auto key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_ASPECT) == HG_T_VALUE_AUTO);
  m.aspect = HG_PMENU_ASPECT_CROP;
  ok("aspect crop reports the Crop key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_ASPECT) == HG_T_VALUE_CROP);
  m.aspect = HG_PMENU_ASPECT_STRETCH;
  ok("aspect stretch reports the Stretch key",
     hg_pmenu_row_value_key(&m, HG_PMENU_ROW_ASPECT) == HG_T_VALUE_STRETCH);

  /* Numbers are the UI's own business: the key must say "format it yourself" with -1. */
  ok("an adjust row reports no key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_BRIGHTNESS) == -1);
  ok("the reset row reports no key", hg_pmenu_row_value_key(&m, HG_PMENU_ROW_RESET) == -1);

  /* The quality row: not available until the ladder arrives, and then a NUMBER the caller formats. */
  m.qualityCount = 0;
  ok("quality with no ladder reports the not-available key",
     hg_pmenu_row_value_key(&m, HG_PMENU_ROW_QUALITY) == HG_T_NOT_AVAILABLE);
  m.qualityCount = 5;
  ok("quality with a ladder reports no key (the caller has the tier's own name)",
     hg_pmenu_row_value_key(&m, HG_PMENU_ROW_QUALITY) == -1);

  /* THE PENDING MARKER: the cursor at 2 while 1 is playing must look different from the applied state. */
  m.qualityCount = 5;
  m.qualityIndex = 2;
  m.qualityApplied = 1;
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, buf, (int)sizeof buf);
  ok("a moved cursor is marked pending", strstr(buf, "-> A") != NULL);
  m.qualityApplied = 2;
  hg_pmenu_row_value(&m, HG_PMENU_ROW_QUALITY, buf, (int)sizeof buf);
  ok("an applied cursor is not marked", strstr(buf, "-> A") == NULL);
  ok("and it still says which of how many", strcmp(buf, "3 of 5") == 0);
}
/* ---- the vf chain (I3) -------------------------------------------------------
 *
 * REPORTED: the five picture values could be set, read back correctly, and change nothing. The cause is the
 * render path (see hg_pmenu.h), and the fix is to express them as filters. These checks are the CONVERSION and
 * the COMPOSITION, which are the two things that can be wrong silently: a wrong neutral point shifts the picture
 * the moment the menu is opened, and two writers of `vf` would drop one half of the chain.
 */
static void test_vf_chain(void) {
  HgPMenu m;
  char buf[192];

  printf("\nthe vf chain\n");

  memset(&m, 0, sizeof m);
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("a neutral menu writes NO filter at all", buf[0] == '\0');

  memset(&m, 0, sizeof m);
  m.values[HG_PMENU_ROW_BRIGHTNESS] = 50;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("brightness becomes eq brightness 0.5", strstr(buf, "eq=brightness=0.5000") != NULL);
  ok("and the other three stay neutral",
     strstr(buf, "contrast=1.0000") != NULL && strstr(buf, "saturation=1.0000") != NULL &&
         strstr(buf, "gamma=1.0000") != NULL);

  memset(&m, 0, sizeof m);
  m.values[HG_PMENU_ROW_CONTRAST] = -40;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("contrast -40 becomes 0.6 (neutral is 1.0)", strstr(buf, "contrast=0.6000") != NULL);
  ok("and brightness is still neutral", strstr(buf, "brightness=0.0000") != NULL);

  memset(&m, 0, sizeof m);
  m.values[HG_PMENU_ROW_HUE] = 50;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("hue becomes 90 degrees", strcmp(buf, "hue=h=90.00") == 0);

  /* ONE CHAIN, ONE WRITER: the mirror must be APPENDED, not overwrite. */
  memset(&m, 0, sizeof m);
  m.values[HG_PMENU_ROW_BRIGHTNESS] = 25;
  m.mirror = 1;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("the mirror is appended with a comma", strstr(buf, ",hflip") != NULL);
  ok("and the eq half survives", strstr(buf, "eq=brightness=0.2500") != NULL);

  memset(&m, 0, sizeof m);
  m.mirror = 1;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("the mirror alone is exactly hflip", strcmp(buf, "hflip") == 0);

  memset(&m, 0, sizeof m);
  m.values[HG_PMENU_ROW_HUE] = -50;
  m.mirror = 1;
  hg_pmenu_vf(&m, buf, (int)sizeof buf);
  ok("hue and mirror compose", strcmp(buf, "hue=h=-90.00,hflip") == 0);

  /* Degenerate arguments: no buffer, or no menu. */
  hg_pmenu_vf(NULL, buf, (int)sizeof buf);
  ok("a NULL menu writes an empty chain", buf[0] == '\0');
  memset(&m, 0, sizeof m);
  hg_pmenu_vf(&m, NULL, 0);
  ok("a NULL buffer does not crash", 1);
}
int main(void) {
  test_vf_chain();
  test_lang_keys();
  printf("hg_pmenu tests: the player picture menu\n");
  printf("========================================\n");

  test_defaults();
  test_clamp();
  test_navigation();
  test_switches_and_reset();
  test_quality_row();
  test_mpv_mapping();
  test_labels();

  printf("\n========================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
