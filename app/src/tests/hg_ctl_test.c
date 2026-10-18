/* hg_ctl_test.c -- the control bar's numbers: volume, speed, zoom.
 *
 * WHY THESE CHECKS EXIST. Each of the three can be wrong silently:
 *   - a volume that wraps instead of clamping (100 -> 0 is a jump-scare);
 *   - a speed ladder that cannot return to exactly 1.0x, which is the one value the user needs;
 *   - a zoom whose LABEL and whose mpv property disagree (mpv's video-zoom is a power of two, so a factor
 *     of 2.0 must be sent as 1.0 -- the classic off-by-a-doubling).
 *
 * ASCII-only source file. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hg_ctl.h"

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

static void test_volume(void) {
  printf("\nvolume\n");
  ok("a plain step up", hg_ctl_volume_step(50, 1) == 55);
  ok("a plain step down", hg_ctl_volume_step(50, -1) == 45);
  ok("IT CLAMPS AT 100, it does not wrap", hg_ctl_volume_step(100, 1) == 100);
  ok("and at 0", hg_ctl_volume_step(0, -1) == 0);
  ok("a large step up still clamps", hg_ctl_volume_step(90, 10) == 100);
  ok("a large step down still clamps", hg_ctl_volume_step(10, -10) == 0);
  ok("a zero delta changes nothing", hg_ctl_volume_step(45, 0) == 45);
  ok("an out-of-range start is clamped first", hg_ctl_volume_step(999, 1) == 100);
  ok("a negative start is clamped first", hg_ctl_volume_step(-999, -1) == 0);
  ok("clamp keeps 0", hg_ctl_volume_clamp(0) == 0);
  ok("clamp keeps 100", hg_ctl_volume_clamp(100) == 100);
  ok("clamp lifts -1 to 0", hg_ctl_volume_clamp(-1) == 0);
  ok("clamp drops 101 to 100", hg_ctl_volume_clamp(101) == 100);
}

static void test_speed(void) {
  char buf[16];
  printf("\nspeed\n");
  ok("1.0x IS on the ladder and is the default",
     fabs(hg_ctl_speed_at(HG_CTL_SPEED_DEFAULT_INDEX) - 1.0) < 1e-9);
  ok("the ladder starts at 0.5x", fabs(hg_ctl_speed_at(0) - 0.5) < 1e-9);
  ok("the ladder ends at 2.0x", fabs(hg_ctl_speed_at(HG_CTL_SPEED_COUNT - 1) - 2.0) < 1e-9);
  ok("stepping up from 1.0x gives 1.25x",
     fabs(hg_ctl_speed_at(hg_ctl_speed_step(HG_CTL_SPEED_DEFAULT_INDEX, 1)) - 1.25) < 1e-9);
  ok("stepping back down returns to exactly 1.0x",
     fabs(hg_ctl_speed_at(hg_ctl_speed_step(HG_CTL_SPEED_DEFAULT_INDEX + 1, -1)) - 1.0) < 1e-9);
  ok("the ladder clamps at the top", hg_ctl_speed_step(HG_CTL_SPEED_COUNT - 1, 1) == HG_CTL_SPEED_COUNT - 1);
  ok("and at the bottom", hg_ctl_speed_step(0, -1) == 0);
  ok("an index out of range is clamped", hg_ctl_speed_step(99, 0) == HG_CTL_SPEED_COUNT - 1);

  /* The cfg seed path: a speed from the file becomes the nearest ladder index. */
  ok("1.0 finds the default index", hg_ctl_speed_index(1.0) == HG_CTL_SPEED_DEFAULT_INDEX);
  ok("1.24 finds 1.25x", fabs(hg_ctl_speed_at(hg_ctl_speed_index(1.24)) - 1.25) < 1e-9);
  ok("0.4 finds the bottom of the ladder", hg_ctl_speed_index(0.4) == 0);
  ok("9.0 finds the top of the ladder", hg_ctl_speed_index(9.0) == HG_CTL_SPEED_COUNT - 1);

  hg_ctl_speed_label(HG_CTL_SPEED_DEFAULT_INDEX, buf, sizeof buf);
  ok("1.0x is labelled with one decimal", strcmp(buf, "1.0x") == 0);
  hg_ctl_speed_label(1, buf, sizeof buf);
  ok("0.75x is labelled with two", strcmp(buf, "0.75x") == 0);
  hg_ctl_speed_label(0, buf, sizeof buf);
  ok("0.5x is labelled with one", strcmp(buf, "0.5x") == 0);
  hg_ctl_speed_label(HG_CTL_SPEED_COUNT, buf, sizeof buf);
  ok("an out-of-range index still labels sanely", strcmp(buf, "2.0x") == 0);
}

static void test_zoom(void) {
  char buf[16];
  printf("\nzoom\n");
  ok("1.0x is the default and means no zoom", fabs(hg_ctl_zoom_factor_at(HG_CTL_ZOOM_DEFAULT_INDEX) - 1.0) < 1e-9);
  ok("THE PROPERTY FOR 1.0x IS 0.0 (mpv scales by 2^value)",
     fabs(hg_ctl_zoom_property(HG_CTL_ZOOM_DEFAULT_INDEX) - 0.0) < 1e-9);
  ok("THE PROPERTY FOR 2.0x IS 1.0, not 2.0", fabs(hg_ctl_zoom_property(3) - 1.0) < 1e-9);
  ok("1.25x maps to log2(1.25)", fabs(hg_ctl_zoom_property(1) - log(1.25) / log(2.0)) < 1e-9);
  ok("stepping up from 1.0x gives 1.25x", hg_ctl_zoom_step(0, 1) == 1);
  ok("stepping back down returns to 1.0x", hg_ctl_zoom_step(1, -1) == 0);
  ok("the ladder clamps at both ends",
     hg_ctl_zoom_step(0, -1) == 0 && hg_ctl_zoom_step(HG_CTL_ZOOM_COUNT - 1, 1) == HG_CTL_ZOOM_COUNT - 1);
  ok("a factor finds the nearest index", hg_ctl_zoom_index(1.4) == 2);

  hg_ctl_zoom_label(0, buf, sizeof buf);
  ok("1x is labelled without decimals", strcmp(buf, "1x") == 0);
  hg_ctl_zoom_label(1, buf, sizeof buf);
  ok("1.25x keeps two", strcmp(buf, "1.25x") == 0);
  hg_ctl_zoom_label(3, buf, sizeof buf);
  ok("2x is labelled without decimals", strcmp(buf, "2x") == 0);

  /* The labels and the property must agree about the same index, or the picture and the text disagree. */
  ok("label 2x goes with property 1.0", fabs(hg_ctl_zoom_property(hg_ctl_zoom_index(2.0)) - 1.0) < 1e-9);
}

/* ---- the player's own settings ---------------------------------------------- */

static void test_settings(void) {
  printf("\nplayer settings (sleep timer, always-show-progress)\n");
  ok("off is index 0", hg_ctl_sleep_minutes_at(0) == 0);
  ok("15 minutes is index 1", hg_ctl_sleep_minutes_at(1) == 15);
  ok("the ladder ends at 90", hg_ctl_sleep_minutes_at(HG_CTL_SLEEP_COUNT - 1) == 90);
  ok("stepping up from off gives 15", hg_ctl_sleep_step(0, 1) == 1);
  ok("IT CLAMPS AT OFF, it does not wrap to 90",
     hg_ctl_sleep_step(0, -1) == 0 && hg_ctl_sleep_minutes_at(hg_ctl_sleep_step(0, -1)) == 0);
  ok("and it clamps at 90", hg_ctl_sleep_step(HG_CTL_SLEEP_COUNT - 1, 1) == HG_CTL_SLEEP_COUNT - 1);
  ok("a zero delta is a no-op", hg_ctl_sleep_step(2, 0) == 2);
  ok("an out-of-range index is clamped", hg_ctl_sleep_minutes_at(99) == 90 && hg_ctl_sleep_minutes_at(-5) == 0);

  ok("a flag steps up to 1", hg_ctl_flag_step(0, 1) == 1);
  ok("a flag steps down to 0", hg_ctl_flag_step(1, -1) == 0);
  ok("a flag left alone stays what it was", hg_ctl_flag_step(1, 0) == 1 && hg_ctl_flag_step(0, 0) == 0);
  ok("a dirty cfg value still steps to a clean 0/1",
     hg_ctl_flag_step(5, 0) == 1 && hg_ctl_flag_step(5, -1) == 0);
}
/* ---- the decode cache (C3) --------------------------------------------------- */

static void test_cache(void) {
  printf("\ndecode cache ladder\n");
  ok("index 0 means mpv's own default", hg_ctl_cache_mb_at(0) == 0);
  ok("32 MiB is index 1", hg_ctl_cache_mb_at(1) == 32);
  ok("the ladder ends at 256 MiB", hg_ctl_cache_mb_at(HG_CTL_CACHE_COUNT - 1) == 256);
  ok("stepping up from the default gives 32", hg_ctl_cache_step(0, 1) == 1);
  ok("it clamps at the default, it does not wrap to 256",
     hg_ctl_cache_step(0, -1) == 0 && hg_ctl_cache_mb_at(hg_ctl_cache_step(0, -1)) == 0);
  ok("and it clamps at the top", hg_ctl_cache_step(HG_CTL_CACHE_COUNT - 1, 1) == HG_CTL_CACHE_COUNT - 1);
  ok("an out-of-range index is clamped", hg_ctl_cache_mb_at(99) == 256 && hg_ctl_cache_mb_at(-3) == 0);

  /* The cfg seed path. */
  ok("64 finds its own index", hg_ctl_cache_mb_at(hg_ctl_cache_index(64)) == 64);
  ok("100 finds 128 (the nearest)", hg_ctl_cache_mb_at(hg_ctl_cache_index(100)) == 128);
  ok("a tiny value finds the default rather than a big cache",
     hg_ctl_cache_mb_at(hg_ctl_cache_index(1)) == 0);
  ok("a huge value finds the top of the ladder", hg_ctl_cache_mb_at(hg_ctl_cache_index(9999)) == 256);
}
int main(void) {
  printf("hg_ctl tests (i386 host build)\n");
  test_volume();
  test_speed();
  test_zoom();
  test_settings();
  test_cache();
  printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
