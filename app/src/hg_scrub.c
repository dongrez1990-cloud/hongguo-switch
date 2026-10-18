// hg_scrub implementation. See hg_scrub.h for why this is C and what it models.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_scrub.h"

#include <stdio.h>

// The state is SECONDS. This helper exists only to keep the clamp in one place; the two
// functions that deal in fractions (the bar geometry) do their own clamping of a value
// that is already a fraction.
static double clampSeconds(double v, double duration) {
  if (!(v > 0.0)) return 0.0;   // also catches NaN, which compares false to everything
  if (!(duration > 0.0)) return 0.0;
  if (v > duration) return duration;
  return v;
}

static double clamp01(double v) {
  if (!(v > 0.0)) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

void hg_scrub_init(HgScrub* s) {
  if (!s) return;
  s->active = 0;
  s->anchorSeconds = 0.0;
  s->posSeconds = 0.0;
  s->duration = 0.0;
  s->movedSeconds = 0.0;
  s->dir = 0;
  s->lastPreviewAt = 0.0;
}

int hg_scrub_begin(HgScrub* s, double playbackSeconds, double duration, int dir, double now) {
  if (!s) return 0;
  // A drag with no duration has no scale: the bar could not draw a target and there would
  // be no way to clamp the position to the end of the video. mpv reports duration 0 or
  // negative for a while after a load, so this is a normal state rather than an error.
  if (!(duration > 0.0)) {
    hg_scrub_init(s);
    return 0;
  }
  if (dir == 0) {
    hg_scrub_init(s);
    return 0;
  }

  s->active = 1;
  // The position is clamped INTO the video on the way in: it comes from mpv, which can
  // report a position slightly past the duration on a stream whose duration it estimated.
  s->anchorSeconds = clampSeconds(playbackSeconds, duration);
  s->posSeconds = s->anchorSeconds;
  s->duration = duration;
  s->movedSeconds = 0.0;
  s->dir = dir > 0 ? 1 : -1;
  // 0 means "no preview has happened yet", which makes the first due-check succeed
  // immediately: the preview should appear the moment the drag starts, not a quarter of a
  // second later.
  s->lastPreviewAt = 0.0;
  (void)now;
  return 1;
}

int hg_scrub_step(HgScrub* s, double deltaSeconds) {
  double before;
  if (!s || !s->active) return 0;
  if (!(deltaSeconds == deltaSeconds)) return 0;   // NaN guard

  before = s->posSeconds;
  s->posSeconds = clampSeconds(s->posSeconds + deltaSeconds, s->duration);

  // movedSeconds accumulates the DISTANCE, not the displacement: dragging right past the
  // end and then back left should still be "you have been dragging a while", which is what
  // the acceleration curve is fed. A signed sum would collapse to ~0 after a reversal at
  // the end of a video and the drag would suddenly become slow -- the opposite of what the
  // curve is for.
  s->movedSeconds += (deltaSeconds < 0.0 ? -deltaSeconds : deltaSeconds);

  return s->posSeconds != before;
}

int hg_scrub_end(HgScrub* s, double* outSeconds) {
  int moved;
  if (!s || !s->active) return 0;

  moved = (s->posSeconds != s->anchorSeconds) ? 1 : 0;
  if (outSeconds) *outSeconds = s->posSeconds;

  // Reset, so a second release without a new begin cannot seek again.
  hg_scrub_init(s);
  return moved;
}

double hg_scrub_fraction(const HgScrub* s) {
  if (!s || !(s->duration > 0.0)) return 0.0;
  return clamp01(s->posSeconds / s->duration);
}

double hg_scrub_position_seconds(const HgScrub* s) {
  if (!s || !(s->duration > 0.0)) return 0.0;
  return s->posSeconds;
}

int hg_scrub_preview_due(HgScrub* s, double now) {
  if (!s || !s->active) return 0;
  if (s->lastPreviewAt <= 0.0) return 1;   // never previewed: due immediately
  return (now - s->lastPreviewAt) >= (double)HG_SCRUB_PREVIEW_MS;
}

void hg_scrub_preview_done(HgScrub* s, double now) {
  if (!s) return;
  // `now` may legitimately be less than the previous stamp if the clock wrapped; refusing
  // to move the stamp backwards keeps the floor from being bypassed by a backwards clock.
  if (now > s->lastPreviewAt) s->lastPreviewAt = now;
  else if (s->lastPreviewAt <= 0.0) s->lastPreviewAt = now;
}

float hg_scrub_x_for(const HgScrubBar* bar, double fraction) {
  double f;
  if (!bar || !(bar->barW > 0.0f)) return bar ? bar->barX : 0.0f;
  f = clamp01(fraction);
  // Half the inset at each end: the range of x the cursor can reach is
  // [barX + inset/2, barX + barW - inset/2], and fraction 0..1 maps onto exactly that.
  return bar->barX + HG_SCRUB_INSET * 0.5f + (float)f * (bar->barW - HG_SCRUB_INSET);
}

double hg_scrub_fraction_at(const HgScrubBar* bar, float x) {
  double usable;
  if (!bar || !(bar->barW > 0.0f)) return 0.0;
  usable = (double)bar->barW - (double)HG_SCRUB_INSET;
  if (!(usable > 0.0)) return 0.0;
  return clamp01(((double)x - (double)bar->barX - (double)HG_SCRUB_INSET * 0.5) / usable);
}

// "H:MM:SS" or "M:SS". Written with a bounded copy rather than snprintf for the VALUE
// and a plain snprintf for the whole thing, because the return value of a truncating
// snprintf is not portable in this project's toolchain: MEASURED under TinyCC, the host
// compiler the test suite runs on,
//
//     char b[4]; snprintf(b, 4, "%d:%02d:%02d", 1, 1, 1);
//
// writes "1:01" (four bytes, NO terminator) and returns -1 -- where C99 requires "the
// number of characters that would have been written" and a terminated buffer. So the
// digits are formatted into a private buffer sized for the worst case (an hours field of
// up to 6 digits plus "HH:MM:SS" is at most 16 bytes), and the copy into the caller's
// buffer is bounded by hand. The returned length is then this function's own count, which
// is well defined whatever the C library does.
static void copyBounded(char* dst, int cap, const char* src, int* outLen) {
  int i = 0;
  while (src[i] != '\0' && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  if (cap > 0) dst[i] = '\0';
  // The length REPORTED is the full formatted length, not what fitted: a caller deciding
  // "does this fit" asks exactly that question, and answering it with the truncated count
  // would always say yes.
  *outLen = 0;
  while (src[*outLen] != '\0') (*outLen)++;
}

int hg_scrub_format_time(double seconds, char* buf, int bufsize) {
  long total;
  int h, m, s;
  char tmp[32];
  int len = 0;

  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';

  // Negative means "mpv does not know yet" (-1 is what it reports before a load settles).
  // Clamping to 0 keeps the bar from showing arithmetic instead of a time.
  if (!(seconds > 0.0)) {
    total = 0;
  } else {
    total = (long)(seconds + 0.5);   // round, so 59.6 s reads as 1:00 rather than 0:59
    if (total < 0) total = 0;
  }

  h = (int)(total / 3600);
  m = (int)((total / 60) % 60);
  s = (int)(total % 60);

  // 32 bytes is far more than "999999:59:59" needs; the format is fixed-width below the
  // hours field so it can never be longer than 7 + the hours digits.
  if (h > 0) snprintf(tmp, sizeof tmp, "%d:%02d:%02d", h, m, s);
  else snprintf(tmp, sizeof tmp, "%d:%02d", m, s);
  tmp[sizeof tmp - 1] = '\0';

  copyBounded(buf, bufsize, tmp, &len);
  return len;
}
