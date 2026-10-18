/* hg_ctl.c -- see hg_ctl.h for why these three values have their own model.
 *
 * ASCII-only source file. */

#include "hg_ctl.h"

#include <math.h>
#include <stdio.h>

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* ---- volume ------------------------------------------------------------------ */

int hg_ctl_volume_clamp(int volume) {
  return clamp_int(volume, HG_CTL_VOLUME_MIN, HG_CTL_VOLUME_MAX);
}

int hg_ctl_volume_step(int volume, int delta) {
  /* A single clamp at the end, and the sum cannot overflow on the way there: the volume is clamped into
   * 0..100 first and the delta is a key press, not a number from a file. */
  long long next = (long long)hg_ctl_volume_clamp(volume) + (long long)delta * HG_CTL_VOLUME_STEP;
  if (next > HG_CTL_VOLUME_MAX) next = HG_CTL_VOLUME_MAX;
  if (next < HG_CTL_VOLUME_MIN) next = HG_CTL_VOLUME_MIN;
  return (int)next;
}

/* ---- speed ------------------------------------------------------------------- */

static const double SPEEDS[HG_CTL_SPEED_COUNT] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};

double hg_ctl_speed_at(int index) {
  return SPEEDS[clamp_int(index, 0, HG_CTL_SPEED_COUNT - 1)];
}

int hg_ctl_speed_index(double speed) {
  int best = HG_CTL_SPEED_DEFAULT_INDEX;
  double bestDist = 1e9;
  int i;
  for (i = 0; i < HG_CTL_SPEED_COUNT; i++) {
    double d = SPEEDS[i] - speed;
    if (d < 0) d = -d;
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

int hg_ctl_speed_step(int index, int delta) {
  return clamp_int(clamp_int(index, 0, HG_CTL_SPEED_COUNT - 1) + delta, 0, HG_CTL_SPEED_COUNT - 1);
}

void hg_ctl_speed_label(int index, char *buf, int bufsize) {
  const double s = hg_ctl_speed_at(index);
  /* "0.75x" needs two decimals and "1.5x" needs one; the difference is whether the hundredths digit is a
   * five. Deciding it from the value (rather than a table of strings) means the ladder and its labels
   * cannot drift apart. */
  const int hundredths = (int)(s * 100.0 + 0.5);
  if (buf == NULL || bufsize <= 0) return;
  if (hundredths % 10 == 5) snprintf(buf, (size_t)bufsize, "%.2fx", s);
  else snprintf(buf, (size_t)bufsize, "%.1fx", s);
}

/* ---- zoom -------------------------------------------------------------------- */

static const double ZOOMS[HG_CTL_ZOOM_COUNT] = {1.0, 1.25, 1.5, 2.0};

double hg_ctl_zoom_factor_at(int index) {
  return ZOOMS[clamp_int(index, 0, HG_CTL_ZOOM_COUNT - 1)];
}

int hg_ctl_zoom_index(double factor) {
  int best = HG_CTL_ZOOM_DEFAULT_INDEX;
  double bestDist = 1e9;
  int i;
  for (i = 0; i < HG_CTL_ZOOM_COUNT; i++) {
    double d = ZOOMS[i] - factor;
    if (d < 0) d = -d;
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

int hg_ctl_zoom_step(int index, int delta) {
  return clamp_int(clamp_int(index, 0, HG_CTL_ZOOM_COUNT - 1) + delta, 0, HG_CTL_ZOOM_COUNT - 1);
}

void hg_ctl_zoom_label(int index, char *buf, int bufsize) {
  double z = hg_ctl_zoom_factor_at(index);
  if (buf == NULL || bufsize <= 0) return;
  if (z == (double)(int)z) snprintf(buf, (size_t)bufsize, "%.0fx", z);
  else snprintf(buf, (size_t)bufsize, "%.2fx", z);
}

double hg_ctl_zoom_property(int index) {
  /* mpv's `video-zoom` scales by 2^value: a factor of 1.0 is 0.0, a factor of 2.0 is 1.0. log2 is the
   * conversion, and it is the only place in the app that knows it. */
  return log(hg_ctl_zoom_factor_at(index)) / log(2.0);
}

/* ---- the player's own settings ----------------------------------------------- */

static const int SLEEP_MINUTES[HG_CTL_SLEEP_COUNT] = {0, 15, 30, 60, 90};

int hg_ctl_sleep_minutes_at(int index) {
  return SLEEP_MINUTES[clamp_int(index, 0, HG_CTL_SLEEP_COUNT - 1)];
}

int hg_ctl_sleep_step(int index, int delta) {
  /* A CLAMP, NOT A WRAP. Stepping down from "off" wrapping to 90 minutes would start a timer the user was
   * trying to switch off, and the next thing they would see is the player stopping an hour and a half later. */
  return clamp_int(clamp_int(index, 0, HG_CTL_SLEEP_COUNT - 1) + delta, 0, HG_CTL_SLEEP_COUNT - 1);
}

static const int CACHE_MB[HG_CTL_CACHE_COUNT] = {0, 32, 64, 128, 256};

int hg_ctl_cache_mb_at(int index) { return CACHE_MB[clamp_int(index, 0, HG_CTL_CACHE_COUNT - 1)]; }

int hg_ctl_cache_step(int index, int delta) {
  return clamp_int(clamp_int(index, 0, HG_CTL_CACHE_COUNT - 1) + delta, 0, HG_CTL_CACHE_COUNT - 1);
}

int hg_ctl_cache_index(int mb) {
  /* THE NEAREST SIZE, and 0 wins ties: a cfg value that is not on the ladder must not silently become a bigger
   * cache than the user asked for. */
  int best = 0;
  int bestDist = -1;
  int i;
  for (i = 0; i < HG_CTL_CACHE_COUNT; i++) {
    int d = CACHE_MB[i] - mb;
    if (d < 0) d = -d;
    if (bestDist < 0 || d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

int hg_ctl_flag_step(int value, int delta) {
  /* Any positive value is on, any negative is off, and a zero delta is a no-op -- so a row that starts from a
   * cfg value of 5 still steps to a clean 0/1. */
  if (delta == 0) return value ? 1 : 0;
  return delta > 0 ? 1 : 0;
}
