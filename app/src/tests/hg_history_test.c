/*
 * Tests for hg_history.c -- local watch history and the watchlist.
 *
 * THE CHECKS THAT MATTER MOST, in order:
 *
 *   1. A TITLE CONTAINING A NEWLINE OR A TAB MUST NOT SPLIT A RECORD. That is the one invariant that
 *      holds the file format together. Without it a drama whose name contains a line break would
 *      write two lines, the second half would parse as a different series, and the symptom would be
 *      "history sometimes shows the wrong drama" -- a corruption with no error message anywhere.
 *   2. A ROUND TRIP THROUGH THE FILE IS LOSSLESS. Written, read back, same fields.
 *   3. THE LIST IS CAPPED AND THE OLDEST GOES, not the newest.
 *   4. THE WATCHLIST REFUSES TO DROP THINGS. A full list stops accepting rather than silently
 *      forgetting something the user explicitly saved.
 *   5. A MALFORMED LINE IS SKIPPED AND COUNTED, never fatal, and it does not stop the good lines
 *      around it from loading.
 *
 * File I/O goes through the real functions, writing into the scratch directory. Unlike the other
 * suites this one touches the filesystem; it uses a relative filename so it works wherever the suite
 * is run from, and removes its files afterwards.
 *
 * ASCII-only source file.
 */

#include "hg_history.h"
#include "hg_lang.h"   /* the age strings come from the interface language table */

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* ---- fixtures ------------------------------------------------------------- */

static HgHistoryEntry mkHist(const char* series, const char* title, const char* ep,
                             const char* label, double pos, double dur, long long when) {
  HgHistoryEntry e;
  memset(&e, 0, sizeof e);
  snprintf(e.seriesId, sizeof e.seriesId, "%s", series);
  snprintf(e.title, sizeof e.title, "%s", title);
  snprintf(e.episodeId, sizeof e.episodeId, "%s", ep);
  snprintf(e.episodeLabel, sizeof e.episodeLabel, "%s", label);
  e.position = pos;
  e.duration = dur;
  e.when = when;
  return e;
}

static HgFavEntry mkFav(const char* series, const char* title, long long when) {
  HgFavEntry e;
  memset(&e, 0, sizeof e);
  snprintf(e.seriesId, sizeof e.seriesId, "%s", series);
  snprintf(e.title, sizeof e.title, "%s", title);
  e.when = when;
  return e;
}

/* Write a raw text file, so malformed content can be tested deliberately. */
static void writeRaw(const char* path, const char* text) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

/* ---- the record-splitting invariant -------------------------------------- */

static void test_roundtrip(void) {
  const char* path = "hg_hist_rt.txt";
  HgHistoryEntry list[8];
  HgHistoryEntry back[8];
  int n = 0, got, skipped = 0;

  printf("\n-- a round trip through the file --\n");

  n = hg_history_put(list, n, 8, &mkHist("S1", "\xE8\x81\x9A\xE5\xAE\x9D\xE4\xBB\x99\xE7\x9B\x86\xE4\xB9\x8B\xE6\x9D\x82\xE7\x81\xB5\xE6\xA0\xB9\xE6\x89\x8D\xE6\x98\xAF\xE7\x9C\x9F\x42\x4F\x53\x53", "E13", "13", 754.5, 1200.0, 1000));
  ok("one entry inserted", n == 1);
  ok("the write succeeded", hg_history_save(path, list, n) == 1);

  got = hg_history_load(path, back, 8, &skipped);
  ok("one entry read back", got == 1);
  ok("nothing was skipped", skipped == 0);
  ok("the series id survived", strcmp(back[0].seriesId, "S1") == 0);
  ok("the title survived", strcmp(back[0].title, "\xE8\x81\x9A\xE5\xAE\x9D\xE4\xBB\x99\xE7\x9B\x86\xE4\xB9\x8B\xE6\x9D\x82\xE7\x81\xB5\xE6\xA0\xB9\xE6\x89\x8D\xE6\x98\xAF\xE7\x9C\x9F\x42\x4F\x53\x53") == 0);
  ok("the episode id survived", strcmp(back[0].episodeId, "E13") == 0);
  ok("the episode label survived", strcmp(back[0].episodeLabel, "13") == 0);
  ok("the position survived", back[0].position > 754.0 && back[0].position < 755.0);
  ok("the duration survived", back[0].duration > 1199.0 && back[0].duration < 1201.0);
  ok("the timestamp survived", back[0].when == 1000);

  /* THE ONE THAT HOLDS THE FORMAT TOGETHER. A title with a newline and a title with a tab must each
   * stay on one line; otherwise the record count changes after a round trip. */
  {
    const char* nasty = "line one\nline two\twith tab\rand CR";
    n = 0;
    n = hg_history_put(list, n, 8, &mkHist("S2", nasty, "E1", "1", 1.0, 2.0, 2000));
    n = hg_history_put(list, n, 8, &mkHist("S3", "after the nasty one", "E1", "1", 1.0, 2.0, 1999));
    ok("two entries written", n == 2);
    ok("the write succeeded", hg_history_save(path, list, n) == 1);

    got = hg_history_load(path, back, 8, &skipped);
    ok("STILL TWO entries after a round trip (no record split)", got == 2);
    ok("and nothing was skipped", skipped == 0);
    ok("the neighbours are in the right order", strcmp(back[1].seriesId, "S3") == 0);
    /* The characters are replaced, not dropped: the requirement is that the record stays on one
     * line, not that the title is mangled into something unrecognisable. */
    ok("the newline became a space", strstr(back[0].title, "line one line two") != NULL);
    ok("the tab became a space", strstr(back[0].title, "with tab") != NULL);
  }

  remove(path);
}

/* ---- ordering and the cap ------------------------------------------------ */

static void test_ordering_and_cap(void) {
  HgHistoryEntry list[HG_HIST_MAX];
  int n = 0, i;

  printf("\n-- newest first, and a full list drops the oldest --\n");

  n = hg_history_put(list, n, 4, &mkHist("A", "A", "E1", "1", 0, 0, 100));
  n = hg_history_put(list, n, 4, &mkHist("B", "B", "E1", "1", 0, 0, 300));
  n = hg_history_put(list, n, 4, &mkHist("C", "C", "E1", "1", 0, 0, 200));
  ok("three entries", n == 3);
  ok("newest is first", strcmp(list[0].seriesId, "B") == 0);
  ok("then the middle one", strcmp(list[1].seriesId, "C") == 0);
  ok("oldest is last", strcmp(list[2].seriesId, "A") == 0);

  /* Updating an existing episode moves it to the front and does NOT add a second row. */
  n = hg_history_put(list, n, 4, &mkHist("A", "A", "E1", "1", 500.0, 900.0, 400));
  ok("an update does not add a row", n == 3);
  ok("and it moves to the front", strcmp(list[0].seriesId, "A") == 0);
  ok("with the new position", list[0].position > 499.0 && list[0].position < 501.0);

  /* A FULL LIST DROPS THE OLDEST, not the newest: this is a "recently watched" queue. */
  n = hg_history_put(list, n, 4, &mkHist("D", "D", "E1", "1", 0, 0, 500));
  ok("the list grew to its cap", n == 4);
  n = hg_history_put(list, n, 4, &mkHist("E", "E", "E1", "1", 0, 0, 600));
  ok("it stays at the cap", n == 4);
  ok("the newest is present", strcmp(list[0].seriesId, "E") == 0);
  {
    int foundOldest = 0;
    for (i = 0; i < n; i++) {
      if (strcmp(list[i].seriesId, "C") == 0) foundOldest = 1;   /* C was the oldest (200) */
    }
    ok("the oldest was the one dropped", foundOldest == 0);
  }

  /* AN UPDATED RECORD KEEPS ITS OLD LABEL when the new one is empty: a progress tick may not know
   * which episode it is, and blanking the label would turn "(13)" into "()". */
  {
    HgHistoryEntry partial;
    memset(&partial, 0, sizeof partial);
    snprintf(partial.seriesId, sizeof partial.seriesId, "E");
    snprintf(partial.episodeId, sizeof partial.episodeId, "E1");
    partial.position = 10.0;
    partial.when = 700;
    n = hg_history_put(list, n, 4, &partial);
    ok("a partial update does not add a row", n == 4);
    ok("the label was kept", strcmp(list[0].episodeLabel, "1") == 0);
    ok("the title was kept", strcmp(list[0].title, "E") == 0);
    ok("but the position was updated", list[0].position > 9.0);
  }

  /* find / remove by series. */
  ok("a series can be found", hg_history_find_series(list, n, "E") == 0);
  ok("a missing series is -1", hg_history_find_series(list, n, "ZZZ") < 0);
  {
    int before = n;
    n = hg_history_remove_series(list, n, "E");
    ok("removing a series shrinks the list", n == before - 1);
    ok("and it is gone", hg_history_find_series(list, n, "E") < 0);
  }
}

/* ---- malformed input ----------------------------------------------------- */

static void test_malformed(void) {
  const char* path = "hg_hist_bad.txt";
  HgHistoryEntry back[8];
  int got, skipped = -1;

  printf("\n-- a broken file must not stop the app --\n");

  /* A short record, a record with no series id, a comment, a blank line, and GOOD records either
   * side of them. The good ones must all load. */
  writeRaw(path,
           "# a comment line\n"
           "\n"
           "S1\tGood one\tE1\t1\t10.0\t100.0\t1000\tcoverA\n"
           "too\tfew\tfields\n"
           "\t\tE2\t2\t10.0\t100.0\t1001\n"
           "S3\tAnother good one\tE3\t3\t20.0\t200.0\t1002\tcoverC\n");

  got = hg_history_load(path, back, 8, &skipped);
  ok("the two good records loaded", got == 2);
  ok("the two bad ones were counted", skipped == 2);
  ok("the first good record is first", strcmp(back[0].seriesId, "S1") == 0);
  ok("its cover was read", strcmp(back[0].cover, "coverA") == 0);
  ok("the second good record is second", strcmp(back[1].seriesId, "S3") == 0);

  /* A record with only six fields: `when` is missing, so it is a short record. */
  writeRaw(path, "S1\tT\tE1\t1\t10.0\t100.0\n");
  got = hg_history_load(path, back, 8, &skipped);
  ok("six fields is refused", got == 0 && skipped == 1);

  /* Seven fields (no cover) is a file written by an older build: it must load. */
  writeRaw(path, "S1\tT\tE1\t1\t10.0\t100.0\t1000\n");
  got = hg_history_load(path, back, 8, &skipped);
  ok("seven fields loads with an empty cover", got == 1 && back[0].cover[0] == '\0');

  /* Garbage numbers become 0 rather than failing the record: "you watched this" is still useful
   * without a position. */
  writeRaw(path, "S1\tT\tE1\t1\tnot-a-number\t-\t1000\n");
  got = hg_history_load(path, back, 8, &skipped);
  ok("a bad position still loads the record", got == 1);
  ok("and the position is 0", back[0].position == 0.0);
  ok("and a negative duration is 0 too", back[0].duration == 0.0);

  /* A missing file is not an error: it is a first run. */
  got = hg_history_load("hg_hist_does_not_exist.txt", back, 8, &skipped);
  ok("a missing file reads as empty", got == 0 && skipped == 0);

  /* Degenerate arguments must not crash. */
  ok("NULL path is refused", hg_history_load(NULL, back, 8, &skipped) == 0);
  ok("NULL out is refused", hg_history_load(path, NULL, 8, &skipped) == 0);
  ok("a zero cap is refused", hg_history_load(path, back, 0, &skipped) == 0);
  ok("a NULL skip counter is tolerated", hg_history_load(path, back, 8, NULL) >= 0);

  remove(path);
}

/* ---- the watchlist ------------------------------------------------------- */

static void test_favorites(void) {
  const char* path = "hg_fav_rt.txt";
  HgFavEntry list[4];
  HgFavEntry back[4];
  int n = 0, got, skipped = 0;

  printf("\n-- the watchlist --\n");

  n = hg_fav_add(list, n, 4, &mkFav("S1", "First", 100));
  n = hg_fav_add(list, n, 4, &mkFav("S2", "Second", 200));
  ok("two added", n == 2);
  ok("the first is found", hg_fav_find(list, n, "S1") == 0);
  ok("a missing one is -1", hg_fav_find(list, n, "S9") < 0);

  /* ADDING TWICE IS NOT AN ERROR, and it does not reshuffle: the original `when` is kept so the
   * list does not jump around when a user re-taps the button. */
  {
    long long before = list[0].when;
    n = hg_fav_add(list, n, 4, &mkFav("S1", "First renamed", 999));
    ok("adding an existing series does not grow the list", n == 2);
    ok("and its timestamp is unchanged", list[0].when == before);
    ok("and its title is unchanged too", strcmp(list[0].title, "First") == 0);
  }

  ok("writing succeeds", hg_fav_save(path, list, n) == 1);
  got = hg_fav_load(path, back, 4, &skipped);
  ok("both read back", got == 2);
  ok("nothing skipped", skipped == 0);
  ok("titles survived", strcmp(back[0].title, "First") == 0 && strcmp(back[1].title, "Second") == 0);

  /* A FULL LIST REFUSES RATHER THAN EVICTING. Dropping the oldest would silently forget something
   * the user explicitly saved, which is a worse failure than saying "full". */
  n = hg_fav_add(list, n, 4, &mkFav("S3", "Third", 300));
  n = hg_fav_add(list, n, 4, &mkFav("S4", "Fourth", 400));
  ok("the list reached its cap", n == 4);
  n = hg_fav_add(list, n, 4, &mkFav("S5", "Fifth", 500));
  ok("it stays at the cap", n == 4);
  ok("the new one was NOT added", hg_fav_find(list, n, "S5") < 0);
  ok("and nothing old was lost", hg_fav_find(list, n, "S1") >= 0 && hg_fav_find(list, n, "S4") >= 0);

  /* Remove. */
  n = hg_fav_remove(list, n, "S2");
  ok("removing shrinks the list", n == 3);
  ok("and it is gone", hg_fav_find(list, n, "S2") < 0);
  n = hg_fav_remove(list, n, "S2");
  ok("removing twice is a no-op", n == 3);

  /* A record with no series id is not a record. */
  n = hg_fav_add(list, n, 4, &mkFav("", "No id", 600));
  ok("an empty series id is refused", n == 3);

  /* Malformed file for the watchlist too. */
  writeRaw(path, "S1\tGood\t100\tcoverA\nbroken\n\n# comment\nS2\tAlso good\t200\n");
  got = hg_fav_load(path, back, 4, &skipped);
  ok("two watchlist records loaded", got == 2);
  ok("the broken one was counted", skipped == 1);

  ok("NULL arguments are refused", hg_fav_save(NULL, list, n) == 0);
  ok("a missing watchlist reads as empty",
     hg_fav_load("hg_fav_nope.txt", back, 4, &skipped) == 0);

  remove(path);
}

/* ---- formatting ---------------------------------------------------------- */

static void test_formatting(void) {
  char b[64];

  printf("\n-- the clock and the age --\n");

  hg_fmt_clock(0.0, b, sizeof b);
  ok("0 seconds is 0:00", strcmp(b, "0:00") == 0);
  hg_fmt_clock(65.0, b, sizeof b);
  ok("65 seconds is 1:05", strcmp(b, "1:05") == 0);
  hg_fmt_clock(754.0, b, sizeof b);
  ok("754 seconds is 12:34", strcmp(b, "12:34") == 0);
  hg_fmt_clock(3723.0, b, sizeof b);
  ok("over an hour gets an hours field", strcmp(b, "1:02:03") == 0);
  hg_fmt_clock(-5.0, b, sizeof b);
  ok("a negative duration reads as 0:00", strcmp(b, "0:00") == 0);
  /* A huge value must not overflow the buffer: it is clamped to 99:59:59. */
  hg_fmt_clock(1.0e12, b, sizeof b);
  ok("an absurd duration is clamped", strcmp(b, "99:59:59") == 0);
  ok("and the buffer stayed terminated", b[sizeof b - 1] == '\0' || strlen(b) < sizeof b);
  {
    char tiny[4];
    hg_fmt_clock(754.0, tiny, sizeof tiny);
    ok("a tiny buffer is still NUL-terminated", tiny[sizeof tiny - 1] == '\0');
  }
  ok("a NULL buffer is tolerated", hg_fmt_clock(1.0, NULL, 8) == 0);
  ok("a zero size is tolerated", hg_fmt_clock(1.0, b, 0) == 0);

  /* The age strings go through the language table, so both languages are exercised. */
  hg_lang_set(HG_LANG_ZH);
  hg_fmt_age(1000, 1000 + 30, b, sizeof b);
  ok("under a minute is 'just now'", strstr(b, "\xE5\x88\x9A") != NULL);
  hg_fmt_age(1000, 1000 + 300, b, sizeof b);
  ok("minutes are reported", strstr(b, "5") != NULL);
  hg_fmt_age(1000, 1000 + 7200, b, sizeof b);
  ok("hours are reported", strstr(b, "2") != NULL);
  hg_fmt_age(1000, 1000 + 3 * 86400, b, sizeof b);
  ok("days are reported", strstr(b, "3") != NULL);

  hg_lang_set(HG_LANG_EN);
  hg_fmt_age(1000, 1000 + 300, b, sizeof b);
  ok("the English age string is used in English mode", strstr(b, "min ago") != NULL);
  hg_lang_set(HG_LANG_ZH);

  /* A future timestamp means the console clock was wrong when the entry was written. It must not
   * render as a negative age. */
  hg_fmt_age(2000, 1000, b, sizeof b);
  ok("a future timestamp reads as 'just now'", strstr(b, "-") == NULL && b[0] != '\0');
  hg_fmt_age(0, 1000, b, sizeof b);
  ok("an unknown timestamp reads as 'just now'", b[0] != '\0');
  hg_fmt_age(1000, 0, b, sizeof b);
  ok("an unknown 'now' reads as 'just now'", b[0] != '\0');
}

int main(void) {
  printf("hg_history tests: local watch history and watchlist\n");
  printf("================================================\n");

  test_roundtrip();
  test_ordering_and_cap();
  test_malformed();
  test_favorites();
  test_formatting();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
