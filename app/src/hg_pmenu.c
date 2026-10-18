/*
 * hg_pmenu implementation. See hg_pmenu.h for why this exists as C.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_pmenu.h"

#include "hg_lang.h"   /* the row-label and value language keys (HG_T_*) */

#include <stdio.h>
#include <string.h>

/* The normal step is 5 rather than wiliwili's 0.05-of-200 (= 10): a short drama is
 * watched on a TV panel, where a 5-point move is already clearly visible, and 20
 * presses cover the whole range instead of 10. The fine step (1) exists because the
 * last few points are the ones that matter when matching two episodes' look. */
#define HG_PMENU_STEP_NORMAL 5
#define HG_PMENU_STEP_FINE 1

static const char *const ROW_LABELS[HG_PMENU_ROW_COUNT] = {
  "Brightness",   /* HG_PMENU_ROW_BRIGHTNESS */
  "Contrast",     /* HG_PMENU_ROW_CONTRAST */
  "Saturation",   /* HG_PMENU_ROW_SATURATION */
  "Gamma",        /* HG_PMENU_ROW_GAMMA */
  "Hue",          /* HG_PMENU_ROW_HUE */
  "Quality",      /* HG_PMENU_ROW_QUALITY */
  "Aspect",       /* HG_PMENU_ROW_ASPECT */
  "Mirror",       /* HG_PMENU_ROW_MIRROR */
  "Reset all",    /* HG_PMENU_ROW_RESET */
};

/* Sentence case (except the proper-noun-free "Auto"): these are shown as the detail
 * text on the right of a row and written into the log, where "auto" next to
 * "Aspect" reads as a capital-less orphan. */
static const char *const ASPECT_LABELS[HG_PMENU_ASPECT_COUNT] = {
  "Auto",     /* HG_PMENU_ASPECT_AUTO */
  "Crop",     /* HG_PMENU_ASPECT_CROP */
  "Stretch",  /* HG_PMENU_ASPECT_STRETCH */
};

/* Modulo with a strictly non-negative result. Written out rather than a loop
 * because a loop over a hostile n is a hang, and this is reached from a keypad. */
static int wrap(int v, int n) {
  if (n <= 0) return 0;
  v %= n;
  if (v < 0) v += n;
  return v;
}

/* Copy a label into the caller's buffer and ALWAYS terminate it.
 *
 * This exists because snprintf's truncation behaviour is not portable enough to
 * rely on here: under TinyCC -- the host compiler this project's test suite uses --
 * snprintf(buf, 4, "A / left / right") leaves buf as "A /" with NO NUL in the four
 * bytes it was given. That is a documented C99 promise being broken, and it was
 * caught by the test that checks exactly this ("a 4-byte buffer is
 * NUL-terminated"), which is why the promise is now kept here instead of assumed.
 * A label that runs off the end of its buffer is the kind of bug that shows up as
 * garbage in a UI and nowhere else. */
static void putStr(char *buf, int bufsize, const char *s) {
  int i = 0;
  if (!buf || bufsize <= 0) return;
  if (!s) s = "";
  while (i < bufsize - 1 && s[i] != '\0') {
    buf[i] = s[i];
    i++;
  }
  buf[i] = '\0';
}

int hg_pmenu_clamp(int v) {
  if (v < HG_PMENU_VALUE_MIN) return HG_PMENU_VALUE_MIN;
  if (v > HG_PMENU_VALUE_MAX) return HG_PMENU_VALUE_MAX;
  return v;
}

int hg_pmenu_step_normal(void) { return HG_PMENU_STEP_NORMAL; }
int hg_pmenu_step_fine(void) { return HG_PMENU_STEP_FINE; }

int hg_pmenu_row_is_adjust(int row) {
  return row >= HG_PMENU_ROW_FIRST_ADJUST && row <= HG_PMENU_ROW_LAST_ADJUST;
}

const char *hg_pmenu_row_label(int row) {
  if (row < 0 || row >= HG_PMENU_ROW_COUNT) return NULL;
  return ROW_LABELS[row];
}

/* THE ROW LABELS AS LANGUAGE KEYS, in the same order as ROW_LABELS above -- the ONLY place that mapping
 * exists, which is why a test walks every row and requires a key that resolves. */
int hg_pmenu_row_label_key(int row) {
  static const int KEYS[HG_PMENU_ROW_COUNT] = {
    HG_T_ROW_BRIGHTNESS, HG_T_ROW_CONTRAST, HG_T_ROW_SATURATION, HG_T_ROW_GAMMA, HG_T_ROW_HUE,
    HG_T_ROW_QUALITY,    HG_T_ROW_ASPECT,   HG_T_ROW_MIRROR,     HG_T_ROW_RESET
  };
  if (row < 0 || row >= HG_PMENU_ROW_COUNT) return -1;
  return KEYS[row];
}

int hg_pmenu_row_value_key(const HgPMenu *m, int row) {
  if (!m || row < 0 || row >= HG_PMENU_ROW_COUNT) return -1;
  /* Numbers and composite strings ("3 of 5") have no key: the caller formats them with
   * hg_pmenu_row_value(). Only the words below are translated. */
  if (hg_pmenu_row_is_adjust(row)) return -1;
  switch (row) {
    case HG_PMENU_ROW_QUALITY:
      return m->qualityCount <= 0 ? HG_T_NOT_AVAILABLE : -1;
    case HG_PMENU_ROW_ASPECT:
      switch (wrap(m->aspect, HG_PMENU_ASPECT_COUNT)) {
        case HG_PMENU_ASPECT_CROP: return HG_T_VALUE_CROP;
        case HG_PMENU_ASPECT_STRETCH: return HG_T_VALUE_STRETCH;
        default: return HG_T_VALUE_AUTO;
      }
    case HG_PMENU_ROW_MIRROR:
      return m->mirror ? HG_T_VALUE_ON : HG_T_VALUE_OFF;
    default:
      return -1;
  }
}

void hg_pmenu_reset(HgPMenu *m) {
  int i;
  if (!m) return;
  /* The open flag and the highlighted row are NOT part of "reset": the user pressed
   * reset on a row, and having the highlight jump elsewhere while the menu stays
   * open would be a surprise. */
  for (i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
    m->values[i] = 0;
  }
  m->aspect = HG_PMENU_ASPECT_AUTO;
  m->mirror = 0;
}

int hg_pmenu_move(HgPMenu *m, int delta) {
  int before;
  if (!m || delta == 0) return 0;
  before = m->row;
  m->row = wrap(m->row + delta, HG_PMENU_ROW_COUNT);
  return m->row != before;
}

int hg_pmenu_adjust(HgPMenu *m, int dir, int fine) {
  int step;
  int before;
  if (!m || dir == 0) return 0;

  if (!hg_pmenu_row_is_adjust(m->row)) {
    /* The non-numeric rows ignore the sign of `dir` on purpose: a switch has no
     * left and right, so MIRROR toggles either way, and either direction is the
     * same action for RESET. */
    if (m->row == HG_PMENU_ROW_ASPECT) {
      before = m->aspect;
      m->aspect = wrap(m->aspect + (dir > 0 ? 1 : -1), HG_PMENU_ASPECT_COUNT);
      return m->aspect != before;
    }
    if (m->row == HG_PMENU_ROW_MIRROR) {
      before = m->mirror;
      m->mirror = m->mirror ? 0 : 1;
      return m->mirror != before;
    }
    /* The quality row is NOT part of "reset": resetting the picture does not change which
   * stream is playing, and a reset that silently switched quality would be a surprise on a
   * different scale from the other seven rows. Its presence is stated in the header. */
  if (m->row == HG_PMENU_ROW_QUALITY) {
    int before;
    if (m->qualityCount <= 0) return 0;   /* no ladder yet: nothing to step */
    before = m->qualityIndex;
    /* WRAPS, like aspect. The alternative -- clamping -- would make the ends of the ladder
     * dead keys, and a menu where right does nothing at the best quality reads as broken. */
    m->qualityIndex = wrap(m->qualityIndex + (dir > 0 ? 1 : -1), m->qualityCount);
    return m->qualityIndex != before;
  }

  if (m->row == HG_PMENU_ROW_RESET) {
      /* "Nothing changed" is a real answer here: pressing reset when everything is
       * already neutral must not report a change, or the caller would log and
       * re-apply five properties for nothing.
       *
       * The dirty flag is computed against a probe COPY first, then the real state
       * is reset unconditionally. The obvious alternative -- reset, then compare --
       * cannot work, because after the reset there is nothing left to compare
       * against. */
      HgPMenu probe = *m;
      hg_pmenu_reset(&probe);
      before = ((m->values[HG_PMENU_ROW_BRIGHTNESS] != probe.values[HG_PMENU_ROW_BRIGHTNESS]) ||
                (m->values[HG_PMENU_ROW_CONTRAST] != probe.values[HG_PMENU_ROW_CONTRAST]) ||
                (m->values[HG_PMENU_ROW_SATURATION] != probe.values[HG_PMENU_ROW_SATURATION]) ||
                (m->values[HG_PMENU_ROW_GAMMA] != probe.values[HG_PMENU_ROW_GAMMA]) ||
                (m->values[HG_PMENU_ROW_HUE] != probe.values[HG_PMENU_ROW_HUE]) ||
                (m->aspect != probe.aspect) || (m->mirror != probe.mirror));
      hg_pmenu_reset(m);
      return before;
    }
    return 0;
  }

  step = fine ? HG_PMENU_STEP_FINE : HG_PMENU_STEP_NORMAL;
  before = m->values[m->row];
  m->values[m->row] = hg_pmenu_clamp(before + (dir > 0 ? step : -step));
  return m->values[m->row] != before;
}

const char *hg_pmenu_row_value(const HgPMenu *m, int row, char *buf, int bufsize) {
  if (!buf || bufsize <= 0) return buf;
  buf[0] = '\0';
  if (!m || row < 0 || row >= HG_PMENU_ROW_COUNT) {
    putStr(buf, bufsize, "?");
    return buf;
  }
  if (hg_pmenu_row_is_adjust(row)) {
    /* Signed, always: "+5" and "-5" are the same number of characters, so the
     * detail text does not shift left and right as the value crosses zero. */
    snprintf(buf, (size_t)bufsize, "%+d", m->values[row]);
    buf[bufsize - 1] = '\0';   /* see putStr: do not rely on snprintf for this */
    return buf;
  }
  switch (row) {
    case HG_PMENU_ROW_QUALITY:
      /* The label for the selected tier lives in the caller's ladder, not here -- see
       * hg_pmenu.h. What this renders is the ROW'S STATE, and the distinction that matters is
       * "the ladder has not arrived" versus "the ladder has one entry", because both would
       * otherwise read as a tier called zero. */
      if (m->qualityCount <= 0) {
        putStr(buf, bufsize, "(not available)");
      } else if (m->qualityApplied != m->qualityIndex) {
        /* THE CURSOR HAS MOVED AND NOTHING HAS BEEN APPLIED YET. The marker is the whole point of the
         * two-step: the row shows what A would switch to, so a user who moved it and then closed the menu
         * with B can see that nothing changed. See hg_pmenu.h. */
        snprintf(buf, (size_t)bufsize, "%d of %d -> A", m->qualityIndex + 1, m->qualityCount);
        buf[bufsize - 1] = '\0';
      } else {
        snprintf(buf, (size_t)bufsize, "%d of %d", m->qualityIndex + 1, m->qualityCount);
        buf[bufsize - 1] = '\0';
      }
      break;
    case HG_PMENU_ROW_ASPECT:
      putStr(buf, bufsize, ASPECT_LABELS[wrap(m->aspect, HG_PMENU_ASPECT_COUNT)]);
      break;
    case HG_PMENU_ROW_MIRROR:
      putStr(buf, bufsize, m->mirror ? "On" : "Off");
      break;
    case HG_PMENU_ROW_RESET:
      putStr(buf, bufsize, "A / left / right");
      break;
    default:
      putStr(buf, bufsize, "?");
      break;
  }
  return buf;
}

void hg_pmenu_aspect_props(const HgPMenu *m, const char **keepaspect,
                           const char **aspectOverride, const char **panscan) {
  int mode = m ? wrap(m->aspect, HG_PMENU_ASPECT_COUNT) : HG_PMENU_ASPECT_AUTO;
  switch (mode) {
    case HG_PMENU_ASPECT_CROP:
      /* keepaspect yes + panscan 1.0 is wiliwili's crop-to-fill: mpv scales the
       * picture to FILL the screen and pans over whatever overflows. */
      if (keepaspect) *keepaspect = "yes";
      if (aspectOverride) *aspectOverride = "-1";
      if (panscan) *panscan = "1.0";
      break;
    case HG_PMENU_ASPECT_STRETCH:
      /* keepaspect no is what actually stretches: -1 means "use the file's own
       * aspect", and with keepaspect off mpv stretches that to the window. */
      if (keepaspect) *keepaspect = "no";
      if (aspectOverride) *aspectOverride = "-1";
      if (panscan) *panscan = "0.0";
      break;
    case HG_PMENU_ASPECT_AUTO:
    default:
      if (keepaspect) *keepaspect = "yes";
      if (aspectOverride) *aspectOverride = "-1";
      if (panscan) *panscan = "0.0";
      break;
  }
}

/* THE WHOLE vf CHAIN, IN ONE STRING. See hg_pmenu.h for why the five values need filters at all (I3), and for
 * the conversions used here. */
void hg_pmenu_vf(const HgPMenu *m, char *buf, int bufsize) {
  char part[128];
  int used = 0;
  int i;

  if (!buf || bufsize <= 0) return;
  buf[0] = '\0';
  if (!m) return;

  /* THE FIVE VALUES, ONLY WHEN THEY ARE NOT NEUTRAL. A chain that is always written is a chain that is always
   * rebuilt, and an `eq` filter with every parameter at its neutral value still costs a filter pass -- on the
   * console's software path that is exactly the CPU this app does not have. */
  if (m->values[HG_PMENU_ROW_BRIGHTNESS] != 0 || m->values[HG_PMENU_ROW_CONTRAST] != 0 ||
      m->values[HG_PMENU_ROW_SATURATION] != 0 || m->values[HG_PMENU_ROW_GAMMA] != 0) {
    snprintf(part, sizeof part, "eq=brightness=%.4f:contrast=%.4f:saturation=%.4f:gamma=%.4f",
             (double)m->values[HG_PMENU_ROW_BRIGHTNESS] / 100.0,
             1.0 + (double)m->values[HG_PMENU_ROW_CONTRAST] / 100.0,
             1.0 + (double)m->values[HG_PMENU_ROW_SATURATION] / 100.0,
             1.0 + (double)m->values[HG_PMENU_ROW_GAMMA] / 100.0);
    part[sizeof part - 1] = '\0';
    used += snprintf(buf + used, (size_t)(bufsize - used), "%s", part);
  }

  if (m->values[HG_PMENU_ROW_HUE] != 0) {
    snprintf(part, sizeof part, "hue=h=%.2f", (double)m->values[HG_PMENU_ROW_HUE] * 1.8);
    part[sizeof part - 1] = '\0';
    used += snprintf(buf + used, (size_t)(bufsize - used), "%s%s", used > 0 ? "," : "", part);
  }

  /* THE MIRROR IS PART OF THE SAME CHAIN, and that is the point of this function existing: the mirror used to
   * write `vf` on its own, so a second writer would silently drop one half. */
  if (m->mirror) {
    used += snprintf(buf + used, (size_t)(bufsize - used), "%shflip", used > 0 ? "," : "");
  }

  /* snprintf truncates safely but returns what it WOULD have written, so `used` can exceed the buffer; the
   * terminator is placed by hand rather than trusted. (TinyCC's snprintf returns -1 on truncation, which is why
   * the value is not compared against anything.) */
  buf[bufsize - 1] = '\0';
  (void)i;
}
const char *hg_pmenu_mirror_filter(const HgPMenu *m) {
  return (m && m->mirror) ? "hflip" : "";
}
