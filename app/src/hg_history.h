// hg_history.h -- local watch history and the watchlist, both stored on the SD card.
//
// GPL-3.0-only.
//
// WHY THIS IS LOCAL AND PARSED BY HAND. The service offers no favourite and no watch-progress
// endpoint -- verified, like the search endpoint: the detail payload's only image field is
// `series_cover` and its episode list is bare ids, and there is nothing account-shaped anywhere in
// the API this app talks to. So "continue watching" and "my list" are the CLIENT's own data, and
// they have to survive a reboot.
//
// The format is a line-per-record text file, because the alternative was worse in three ways:
//
//   * A binary struct dump breaks the moment a field is added, and this struct WILL gain fields.
//   * A real JSON writer would be a new dependency for six fields.
//   * A line-based text file can be repaired with a text editor on the SD card, which matters for a
//     file that is written by a console whose only other interface is an FTP connection.
//
// The encoding rule that makes it safe: every string field has its tabs and newlines replaced by
// spaces when written, so a title can never split a record. That is the one invariant that matters,
// and hg_history_test checks it with a title that contains a literal newline.
//
// The parser is TOTAL: a malformed line is skipped and counted, never fatal. A history file that has
// been hand-edited into nonsense must still let the app start -- the alternative is an app that
// will not launch until its user finds and deletes a file they have never heard of.
//
// ASCII-only source file.

#ifndef HG_HISTORY_H
#define HG_HISTORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enough for the longest title seen upstream (36 characters) with room to spare, and enough for a
 * URL-bearing id. Fixed sizes rather than heap: these structs are held in a flat array. */
#define HG_HIST_ID_MAX 64
#define HG_HIST_TITLE_MAX 128
#define HG_HIST_EP_MAX 32
#define HG_HIST_URL_MAX 512

/* How many "continue watching" entries are kept. Older ones are dropped when the list is full, and
 * the one being updated is always kept -- see hg_history_put. */
#define HG_HIST_MAX 64

/* How many series the watchlist holds. A list longer than this stops accepting new entries rather
 * than dropping old ones: silently forgetting something the user explicitly saved is worse than
 * telling them the list is full. */
#define HG_FAV_MAX 128

typedef struct {
  char seriesId[HG_HIST_ID_MAX];   /* the series this episode belongs to */
  char title[HG_HIST_TITLE_MAX];  /* its name, so the History screen needs no network */
  char episodeId[HG_HIST_ID_MAX]; /* the episode that was playing */
  char episodeLabel[HG_HIST_EP_MAX]; /* what to show, e.g. "13" -- upstream's own numbering */
  double position;                /* seconds into the episode, 0 when unknown */
  double duration;                /* seconds, 0 when unknown */
  long long when;                 /* UNIX seconds of the last update; newest first in the list */
  char cover[HG_HIST_URL_MAX];    /* the series cover, so History shows a picture offline */
} HgHistoryEntry;

typedef struct {
  char seriesId[HG_HIST_ID_MAX];
  char title[HG_HIST_TITLE_MAX];
  long long when;                 /* UNIX seconds when it was added */
  char cover[HG_HIST_URL_MAX];
} HgFavEntry;

/* ---- history ------------------------------------------------------------- */

/* Load `path`. Returns the number of entries read, or 0 when the file is missing or empty. A
 * malformed line is skipped; `outSkipped` (may be NULL) receives how many were skipped, so the
 * caller can say so in the log rather than silently losing records. */
int hg_history_load(const char *path, HgHistoryEntry *out, int max, int *outSkipped);

/* Write `n` entries to `path`, ATOMICALLY: a temporary file in the same directory is written,
 * flushed, closed, and then replaces the target.
 *
 * The atomicity is not decoration. This file is rewritten every time playback reports progress --
 * which is every few seconds -- and a power loss or a crash mid-write would otherwise leave a
 * truncated file whose last line is half a record. The next load would drop it, and a user would
 * lose history without ever seeing an error. Returns 1 on success. */
int hg_history_save(const char *path, const HgHistoryEntry *in, int n);

/* Insert or update the record for `episodeId`, then keep the list ordered newest-first and trimmed
 * to `max`. `n` is the current count and the return value is the new count. */
int hg_history_put(HgHistoryEntry *list, int n, int max, const HgHistoryEntry *entry);

/* The most recent entry whose seriesId matches, or -1. Used to offer "continue from 12:34" when the
 * user reopens a series. */
int hg_history_find_series(const HgHistoryEntry *list, int n, const char *seriesId);

/* Remove every entry for one series. Returns the new count. */
int hg_history_remove_series(HgHistoryEntry *list, int n, const char *seriesId);

/* 0 when the file has no usable content; used to decide whether to show the empty state. */

/* ---- the watchlist ------------------------------------------------------- */

int hg_fav_load(const char *path, HgFavEntry *out, int max, int *outSkipped);
int hg_fav_save(const char *path, const HgFavEntry *in, int n);
/* Add `entry` if its seriesId is not already present. Returns the new count, which is unchanged when
 * it was already there (adding twice is not an error). */
int hg_fav_add(HgFavEntry *list, int n, int max, const HgFavEntry *entry);
/* Remove one series. Returns the new count, unchanged when it was not there. */
int hg_fav_remove(HgFavEntry *list, int n, const char *seriesId);
/* Index of a series, or -1. The watchlist button's on/off state depends on it. */
int hg_fav_find(const HgFavEntry *list, int n, const char *seriesId);

/* ---- shared helpers ------------------------------------------------------ */

/* "12:34" / "1:02:03" from seconds. Always NUL-terminates within `bufsize`; never writes a partial
 * character sequence. Returns the number of characters written, excluding the terminator. */
int hg_fmt_clock(double seconds, char *buf, int bufsize);

/* "3 minutes ago" style age, in the language in force (the strings come from hg_lang, which has both
 * languages). An unknown or future timestamp reads as "just now" rather than a negative age.
 * Returns the number of characters written. */
int hg_fmt_age(long long when, long long now, char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* HG_HISTORY_H */
