// hg_history implementation. See hg_history.h for why the format is a line-per-record text file.
//
// GPL-3.0-only.
//
// THE FILE FORMAT, exactly:
//
//     seriesId \t title \t episodeId \t episodeLabel \t position \t duration \t when \t cover
//
// One record per line, fields separated by TAB, no header. A field that is absent is an empty
// string; a numeric field that cannot be read becomes 0 rather than failing the record, because a
// record with an unknown position is still a useful "you watched this" row.
//
// The invariant that makes this safe: SANITISE ON WRITE. Every string goes through `field()` which
// replaces TAB, CR and LF with a space. Without it a title containing a newline would split one
// record into two and the second half would parse as a different series -- a corruption that would
// look like "history sometimes shows the wrong drama".
//
// The watchlist uses the same shape with four fields: seriesId, title, when, cover.
//
// ASCII-only source file.

#include "hg_history.h"

#include "hg_lang.h"

#include <stdio.h>
#include <string.h>

/* ---- small string helpers ------------------------------------------------ */

/* Copy `src` into `dst` (capacity `cap`), replacing TAB/CR/LF with a space. Always NUL-terminates.
 * This is the function that keeps a record on one line. */
static void field(char *dst, int cap, const char *src) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (!src) src = "";
  while (src[i] != '\0' && i < cap - 1) {
    char c = src[i];
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    dst[i] = c;
    i++;
  }
  dst[i] = '\0';
}

/* Compare with the trailing spaces the sanitiser may have left, and case-insensitively, so an id
 * that differs only in case is not treated as a different series. */
static int idEq(const char *a, const char *b) {
  size_t i;
  if (!a || !b) return 0;
  for (i = 0; a[i] && b[i]; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return a[i] == '\0' && b[i] == '\0';
}

/* Fields on one line, split on TAB. Returns how many were found (up to `max`). Line is modified in
 * place. */
static int splitTabs(char *line, char **out, int max) {
  int n = 0;
  char *p = line;
  if (max <= 0) return 0;
  out[n++] = p;
  while (*p && n < max) {
    if (*p == '\t') {
      *p = '\0';
      out[n++] = p + 1;
    }
    p++;
  }
  if (n < max) {
    /* An empty trailing field is legal, so nothing to do -- the caller checks n against the number
     * of fields the record type needs. */
  }
  return n;
}

static double parseDouble(const char *s) {
  double v = 0.0;
  if (!s || !s[0]) return 0.0;
  if (sscanf(s, "%lf", &v) != 1) return 0.0;
  /* A negative position or duration is nonsense and a non-finite value would poison every
   * comparison downstream, so both are normalised to 0 here rather than at each use. */
  if (!(v >= 0.0)) return 0.0;
  if (v > 1.0e9) return 0.0;
  return v;
}

static long long parseLL(const char *s) {
  long long v = 0;
  if (!s || !s[0]) return 0;
  if (sscanf(s, "%lld", &v) != 1) return 0;
  if (v < 0) return 0;
  return v;
}

/* Strip a trailing newline and any CR, in place. */
static void chomp(char *s) {
  size_t n;
  if (!s) return;
  n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
}

/* A line is worth attempting only when it has at least one TAB for history (or is at least
 * non-empty for favourites). Blank lines and comments are skipped silently -- an empty file and a
 * file of blank lines are the same thing. */
static int isSkippable(const char *line) {
  if (!line) return 1;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0' || *line == '#') return 1;
  return 0;
}

/* ---- history ------------------------------------------------------------- */

int hg_history_load(const char *path, HgHistoryEntry *out, int max, int *outSkipped) {
  FILE *f;
  char line[1200];
  int n = 0;
  int skipped = 0;

  if (outSkipped) *outSkipped = 0;
  if (!path || !out || max <= 0) return 0;

  f = fopen(path, "rb");
  if (!f) return 0;

  while (fgets(line, (int)sizeof line, f)) {
    char *cols[8];
    int got;
    HgHistoryEntry e;

    if (isSkippable(line)) continue;
    chomp(line);
    if (line[0] == '\0') continue;

    memset(&e, 0, sizeof e);
    got = splitTabs(line, cols, 8);
    /* SEVEN FIELDS IS THE MINIMUM: through `when`. `cover` was added later, so a file written by an
     * older build of this app loads with an empty cover rather than being rejected -- that is the
     * whole reason the format is positional with an optional tail. */
    if (got < 7) {
      skipped++;
      continue;
    }
    field(e.seriesId, HG_HIST_ID_MAX, cols[0]);
    field(e.title, HG_HIST_TITLE_MAX, cols[1]);
    field(e.episodeId, HG_HIST_ID_MAX, cols[2]);
    field(e.episodeLabel, HG_HIST_EP_MAX, cols[3]);
    e.position = parseDouble(cols[4]);
    e.duration = parseDouble(cols[5]);
    e.when = parseLL(cols[6]);
    if (got >= 8) field(e.cover, HG_HIST_URL_MAX, cols[7]);

    /* A record with no series id cannot be shown or matched, so it is not a record. */
    if (e.seriesId[0] == '\0') {
      skipped++;
      continue;
    }
    if (n >= max) {
      skipped++;
      continue;
    }

    /* THE LOADER KEEPS THE FILE'S ORDER, ON PURPOSE (see hg_history_test: "the first good record is first").
     * Merging obsolete per-episode rows is the STORE's job, where the list is normalised once after loading --
     * doing it here would have silently re-sorted the file and broken that contract. */
    out[n++] = e;
  }
  fclose(f);

  if (outSkipped) *outSkipped = skipped;
  return n;
}

/* Write to a temporary file, then rename over the target. See the header for why. */
int hg_history_save(const char *path, const HgHistoryEntry *in, int n) {
  FILE *f;
  char tmp[600];
  int i;

  if (!path || !path[0]) return 0;
  /* The temp file lives beside the target so the rename is within one filesystem and therefore
   * atomic; a temp in a different directory could silently degrade to copy-then-delete. */
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  tmp[sizeof tmp - 1] = '\0';

  f = fopen(tmp, "wb");
  if (!f) return 0;

  for (i = 0; i < n; i++) {
    char a[HG_HIST_ID_MAX], b[HG_HIST_TITLE_MAX], c[HG_HIST_ID_MAX], d[HG_HIST_EP_MAX];
    char g[HG_HIST_URL_MAX];
    /* Sanitise every string field. See the note at the top: this is what keeps one record on one
     * line. */
    field(a, sizeof a, in[i].seriesId);
    field(b, sizeof b, in[i].title);
    field(c, sizeof c, in[i].episodeId);
    field(d, sizeof d, in[i].episodeLabel);
    field(g, sizeof g, in[i].cover);
    if (a[0] == '\0') continue;   /* nothing to write for a record with no series */
    if (fprintf(f, "%s\t%s\t%s\t%s\t%.1f\t%.1f\t%lld\t%s\n", a, b, c, d, in[i].position,
                in[i].duration, in[i].when, g) < 0) {
      fclose(f);
      remove(tmp);
      return 0;
    }
  }

  /* fflush AND fclose, both checked: a full SD card fails at flush time, and reporting success
   * after that would leave the target replaced by a short file. */
  if (fflush(f) != 0) {
    fclose(f);
    remove(tmp);
    return 0;
  }
  if (fclose(f) != 0) {
    remove(tmp);
    return 0;
  }

  /* remove-then-rename, because POSIX rename over an existing file is not portable to the Switch's
   * FAT layer in the way it is elsewhere. The window is one syscall wide and the file it can lose is
   * the one being replaced, which is already in memory. */
  remove(path);
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return 0;
  }
  return 1;
}

int hg_history_put(HgHistoryEntry *list, int n, int max, const HgHistoryEntry *entry) {
  int at = -1;
  int i;

  if (!list || !entry || max <= 0) return n < 0 ? 0 : n;

  for (i = 0; i < n; i++) {
    /* ---- ONE ROW PER SERIES, NOT ONE PER EPISODE (hongguo build127, the user's report) ------------------
     *
     * MEASURED ON HARDWARE, from the user: "the history has an entry for the same content on every episode, so
     * the list is full of things I have not watched that many of -- I only watched a few shows."
     *
     * That is this line as it was: the match required BOTH the episode id and the series id to be equal, so
     * watching twenty episodes of one drama wrote twenty rows. It is not what a "continue watching" list is for --
     * the list answers "where was I in THIS series", and the episode is part of that answer, not part of its
     * identity.
     *
     * So the series alone identifies the row now. The update path below already does the right thing with the
     * episode it is handed (it keeps the previous label only when the new record has none), so a row always names
     * the episode watched last. */
    if (idEq(list[i].seriesId, entry->seriesId)) {
      at = i;
      break;
    }
  }

  if (at < 0) {
    /* A new record. When the list is full the OLDEST is dropped -- the list is a "recently watched"
     * queue, so the least recently updated entry is the one to lose. */
    if (n >= max) {
      int oldest = 0;
      for (i = 1; i < n; i++) {
        if (list[i].when < list[oldest].when) oldest = i;
      }
      /* Shift down over the hole. */
      for (i = oldest; i < n - 1; i++) list[i] = list[i + 1];
      n--;
    }
    list[n] = *entry;
    n++;
  } else {
    /* An update keeps the ORIGINAL episode label unless the new one has one: a later progress tick
     * may not know which episode number it is, and overwriting a good label with an empty string
     * would make the row read "(13)" -> "()". */
    HgHistoryEntry merged = *entry;
    if (merged.title[0] == '\0') field(merged.title, HG_HIST_TITLE_MAX, list[at].title);
    if (merged.episodeLabel[0] == '\0') {
      field(merged.episodeLabel, HG_HIST_EP_MAX, list[at].episodeLabel);
    }
    if (merged.cover[0] == '\0') field(merged.cover, HG_HIST_URL_MAX, list[at].cover);
    list[at] = merged;
  }

  /* Newest first. A stable insertion sort, because n is at most 64 and this runs once per save --
   * and stability means two entries with the same timestamp keep their relative order instead of
   * swapping places on every write. */
  for (i = 1; i < n; i++) {
    HgHistoryEntry key = list[i];
    int j = i - 1;
    while (j >= 0 && list[j].when < key.when) {
      list[j + 1] = list[j];
      j--;
    }
    list[j + 1] = key;
  }
  return n;
}

int hg_history_find_series(const HgHistoryEntry *list, int n, const char *seriesId) {
  int i;
  if (!list || !seriesId) return -1;
  for (i = 0; i < n; i++) {
    if (idEq(list[i].seriesId, seriesId)) return i;
  }
  return -1;
}

int hg_history_remove_series(HgHistoryEntry *list, int n, const char *seriesId) {
  int i, w = 0;
  if (!list || !seriesId) return n;
  for (i = 0; i < n; i++) {
    if (!idEq(list[i].seriesId, seriesId)) list[w++] = list[i];
  }
  return w;
}

/* ---- the watchlist ------------------------------------------------------- */

int hg_fav_load(const char *path, HgFavEntry *out, int max, int *outSkipped) {
  FILE *f;
  char line[1200];
  int n = 0;
  int skipped = 0;

  if (outSkipped) *outSkipped = 0;
  if (!path || !out || max <= 0) return 0;

  f = fopen(path, "rb");
  if (!f) return 0;

  while (fgets(line, (int)sizeof line, f)) {
    char *cols[4];
    int got;
    HgFavEntry e;

    if (isSkippable(line)) continue;
    chomp(line);
    if (line[0] == '\0') continue;

    memset(&e, 0, sizeof e);
    got = splitTabs(line, cols, 4);
    if (got < 3) {   /* through `when`; `cover` may be absent */
      skipped++;
      continue;
    }
    field(e.seriesId, HG_HIST_ID_MAX, cols[0]);
    field(e.title, HG_HIST_TITLE_MAX, cols[1]);
    e.when = parseLL(cols[2]);
    if (got >= 4) field(e.cover, HG_HIST_URL_MAX, cols[3]);
    if (e.seriesId[0] == '\0') {
      skipped++;
      continue;
    }
    if (n >= max) {
      skipped++;
      continue;
    }
    out[n++] = e;
  }
  fclose(f);

  if (outSkipped) *outSkipped = skipped;
  return n;
}

int hg_fav_save(const char *path, const HgFavEntry *in, int n) {
  FILE *f;
  char tmp[600];
  int i;

  if (!path || !path[0]) return 0;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  tmp[sizeof tmp - 1] = '\0';

  f = fopen(tmp, "wb");
  if (!f) return 0;

  for (i = 0; i < n; i++) {
    char a[HG_HIST_ID_MAX], b[HG_HIST_TITLE_MAX], g[HG_HIST_URL_MAX];
    field(a, sizeof a, in[i].seriesId);
    field(b, sizeof b, in[i].title);
    field(g, sizeof g, in[i].cover);
    if (a[0] == '\0') continue;
    if (fprintf(f, "%s\t%s\t%lld\t%s\n", a, b, in[i].when, g) < 0) {
      fclose(f);
      remove(tmp);
      return 0;
    }
  }

  if (fflush(f) != 0) {
    fclose(f);
    remove(tmp);
    return 0;
  }
  if (fclose(f) != 0) {
    remove(tmp);
    return 0;
  }
  remove(path);
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return 0;
  }
  return 1;
}

int hg_fav_add(HgFavEntry *list, int n, int max, const HgFavEntry *entry) {
  if (!list || !entry || max <= 0) return n < 0 ? 0 : n;
  if (entry->seriesId[0] == '\0') return n;

  /* Already there: keep the ORIGINAL `when`, so the list order does not reshuffle when a user
   * re-taps the button. */
  if (hg_fav_find(list, n, entry->seriesId) >= 0) return n;

  /* FULL IS AN ERROR, NOT AN EVICTION. Dropping the oldest would silently forget something the user
   * explicitly saved; the caller reports "the list is full" instead. */
  if (n >= max) return n;

  list[n] = *entry;
  return n + 1;
}

int hg_fav_remove(HgFavEntry *list, int n, const char *seriesId) {
  int i, w = 0;
  if (!list || !seriesId) return n;
  for (i = 0; i < n; i++) {
    if (!idEq(list[i].seriesId, seriesId)) list[w++] = list[i];
  }
  return w;
}

int hg_fav_find(const HgFavEntry *list, int n, const char *seriesId) {
  int i;
  if (!list || !seriesId) return -1;
  for (i = 0; i < n; i++) {
    if (idEq(list[i].seriesId, seriesId)) return i;
  }
  return -1;
}

/* ---- shared helpers ------------------------------------------------------ */

int hg_fmt_clock(double seconds, char *buf, int bufsize) {
  int total, h, m, s, written;
  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';
  if (!(seconds >= 0.0)) seconds = 0.0;
  if (seconds > 359999.0) seconds = 359999.0;   /* 99:59:59, so the buffer cannot overflow */
  total = (int)(seconds + 0.5);
  h = total / 3600;
  m = (total % 3600) / 60;
  s = total % 60;
  if (h > 0) written = snprintf(buf, (size_t)bufsize, "%d:%02d:%02d", h, m, s);
  else written = snprintf(buf, (size_t)bufsize, "%d:%02d", m, s);
  /* EXPLICIT TERMINATION, because the truncation case is not covered by snprintf's own guarantee in
   * the way the standard's wording suggests. The host suite caught this: with a 4-byte buffer and
   * "12:34" to write, snprintf fills three characters, and some implementations leave no NUL because
   * there was not room for one. A clock string that is not terminated is read past its end by the
   * next strlen -- a crash whose cause would be three call frames away from this line. */
  buf[bufsize - 1] = '\0';
  /* snprintf returns the length it WOULD have written, which can exceed the buffer. Reporting that
   * is the honest answer; the buffer is terminated either way now. */
  return written < 0 ? 0 : written;
}

int hg_fmt_age(long long when, long long now, char *buf, int bufsize) {
  long long d;
  int written;
  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';
  if (when <= 0 || now <= 0 || now < when) {
    /* An unknown or future timestamp is shown as "just now" rather than as a negative age. A clock
     * that was wrong when the entry was written must not produce "in -3 days". */
    written = snprintf(buf, (size_t)bufsize, "%s", hg_tr(HG_T_AGE_NOW));
    return written < 0 ? 0 : written;
  }
  d = now - when;
  if (d < 60) {
    written = snprintf(buf, (size_t)bufsize, "%s", hg_tr(HG_T_AGE_NOW));
  } else if (d < 3600) {
    written = snprintf(buf, (size_t)bufsize, hg_tr(HG_T_AGE_MIN), (int)(d / 60));
  } else if (d < 86400) {
    written = snprintf(buf, (size_t)bufsize, hg_tr(HG_T_AGE_HOUR), (int)(d / 3600));
  } else {
    written = snprintf(buf, (size_t)bufsize, hg_tr(HG_T_AGE_DAY), (int)(d / 86400));
  }
  return written < 0 ? 0 : written;
}
