/*
 * Tests for hg_quality.c -- the quality ladder.
 *
 * The measured rows from app/src/tests/fixtures/video_model.json drive every check here:
 *
 *     quality_type 40 -> 360p 640x360     quality_type 10 -> 720p 1280x720
 *     quality_type 30 -> 480p 854x480     quality_type  1 -> 1080p 1920x1080
 *     quality_type 20 -> 540p 1024x576
 *
 * The check that matters most is THE ORDER. Sorting by quality_type would put 1080p LAST
 * (it is 1) and 360p first (it is 40), which produces a menu that looks entirely reasonable
 * and offers the worst picture as the default. That is the failure this suite exists for.
 *
 * ASCII-only source file.
 */

#include "hg_quality.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* The fixture's five rows, worst-first as upstream sends them. */
static const int FIX_QT[5] = {40, 30, 20, 10, 1};
static const char* const FIX_DEF[5] = {"360p", "480p", "540p", "720p", "1080p"};
static const int FIX_W[5] = {640, 854, 1024, 1280, 1920};
static const int FIX_H[5] = {360, 480, 576, 720, 1080};
static const int FIX_BR[5] = {300000, 500000, 700000, 1200000, 2500000};
static const long long FIX_SZ[5] = {1000, 2000, 3000, 4000, 5000};
static const int FIX_OK[5] = {1, 1, 1, 1, 1};

static int buildFixture(HgQualityTier* out, int cap) {
  return hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, FIX_BR, FIX_SZ, FIX_OK, NULL, 5, out, cap);
}

/* ---- the order ----------------------------------------------------------- */

static void test_order(void) {
  HgQualityTier t[8];
  int n;

  printf("\n-- the ladder is best-first, not by quality_type --\n");

  n = buildFixture(t, 8);
  ok("all five measured tiers are present", n == 5);

  /* THE MEASURED TRAP. quality_type is 40 for 360p and 1 for 1080p, so anything ordering by
   * it ascending gets this backwards. */
  ok("the first tier is 1080p, not the smallest quality_type",
     strcmp(t[0].definition, "1080p") == 0);
  ok("the last tier is 360p, not the largest quality_type",
     strcmp(t[4].definition, "360p") == 0);
  ok("quality_type is NOT ascending in menu order",
     t[0].quality_type == 1 && t[4].quality_type == 40);
  ok("the heights descend", t[0].height == 1080 && t[1].height == 720 && t[2].height == 576 &&
                                t[3].height == 480 && t[4].height == 360);

  /* The whole ladder, spelled out, so a change to the order is a failing check and not a
   * line in a diff nobody reads. */
  {
    char want[64];
    snprintf(want, sizeof want, "%s>%s>%s>%s>%s", t[0].definition, t[1].definition,
             t[2].definition, t[3].definition, t[4].definition);
    ok("the order is 1080p>720p>540p>480p>360p", strcmp(want, "1080p>720p>540p>480p>360p") == 0);
  }

  /* source_index survives the sort: it must still point at the ORIGINAL offered position,
   * because that is how a switch finds the right entry in the parsed response. */
  ok("1080p remembers it was entry 4", t[0].source_index == 4);
  ok("360p remembers it was entry 0", t[4].source_index == 0);
  ok("720p remembers it was entry 3", t[1].source_index == 3);
}

static void test_index_of(void) {
  HgQualityTier t[8];
  int n = buildFixture(t, 8);

  printf("\n-- picking the tier that is playing --\n");

  ok("1080p is at index 0", hg_quality_index_of(t, n, 1) == 0);
  ok("720p is at index 1", hg_quality_index_of(t, n, 10) == 1);
  ok("360p is at index 4", hg_quality_index_of(t, n, 40) == 4);
  ok("an unknown quality_type is -1", hg_quality_index_of(t, n, 999) == -1);
  ok("0 (absent) is not found", hg_quality_index_of(t, n, 0) == -1);
  ok("a negative count is -1", hg_quality_index_of(t, 0, 10) == -1);
  ok("a NULL ladder is -1", hg_quality_index_of(NULL, 5, 10) == -1);
}

/* ---- labels -------------------------------------------------------------- */

static void test_labels(void) {
  HgQualityTier t[8];
  char b[48];
  int n = buildFixture(t, 8);

  printf("\n-- labels --\n");

  hg_quality_label(&t[0], b, (int)sizeof b);
  ok("a tier with a definition uses it", strcmp(b, "1080p") == 0);

  /* An absent definition falls back to the measured pixel height, which is a fact rather
   * than a guess. */
  {
    HgQualityTier bare;
    memset(&bare, 0, sizeof bare);
    bare.quality_type = 10;
    bare.width = 1280;
    bare.height = 720;
    hg_quality_label(&bare, b, (int)sizeof b);
    ok("no definition falls back to '<height>p'", strcmp(b, "720p") == 0);

    bare.height = 0;
    hg_quality_label(&bare, b, (int)sizeof b);
    ok("no definition and no height falls back to quality_type",
       strcmp(b, "quality 10") == 0);

    memset(&bare, 0, sizeof bare);
    hg_quality_label(&bare, b, (int)sizeof b);
    ok("nothing at all renders 'unknown', never empty", strcmp(b, "unknown") == 0);
    ok("and the length is reported", hg_quality_label(&bare, b, (int)sizeof b) == 7);
  }

  /* A truncated buffer is terminated and a prefix, not garbage. */
  {
    memset(b, 'X', sizeof b);
    hg_quality_label(&t[0], b, 4);
    ok("a 4-byte buffer is NUL-terminated", b[3] == '\0');
    ok("and holds a prefix", strcmp(b, "108") == 0);
  }

  ok("a NULL tier renders 'unknown'", hg_quality_label(NULL, b, (int)sizeof b) == 7 &&
                                         strcmp(b, "unknown") == 0);
  ok("a NULL buffer is tolerated", hg_quality_label(&t[0], NULL, 32) == 0);
  ok("a zero-size buffer is tolerated", hg_quality_label(&t[0], b, 0) == 0);
}

static void test_describe(void) {
  HgQualityTier t[8];
  char b[320];
  int n = buildFixture(t, 8);

  printf("\n-- the log line --\n");

  hg_quality_describe(t, n, b, (int)sizeof b);
  ok("the summary reads best to worst",
     strcmp(b, "1080p > 720p > 540p > 480p > 360p") == 0);

  /* An undecodable tier is marked, so the log and the menu agree about which rows are
   * offers. */
  {
    const int okFlags[3] = {1, 0, 1};
    const int qt[3] = {40, 20, 1};
    const char* const def[3] = {"360p", "540p", "1080p"};
    const int w[3] = {640, 1024, 1920};
    const int h[3] = {360, 576, 1080};
    HgQualityTier m[4];
    int mn = hg_quality_build(qt, def, w, h, NULL, NULL, okFlags, NULL, 3, m, 4);
    hg_quality_describe(m, mn, b, (int)sizeof b);
    ok("an undecodable tier is marked in the summary",
       strcmp(b, "1080p > 540p (undecodable) > 360p") == 0);
  }

  hg_quality_describe(NULL, 0, b, (int)sizeof b);
  ok("an empty ladder says so", strcmp(b, "(no tiers)") == 0);
  hg_quality_describe(t, n, NULL, 32);
  ok("a NULL buffer is tolerated", 1);
  {
    memset(b, 'X', sizeof b);
    hg_quality_describe(t, n, b, 8);
    ok("a small buffer is NUL-terminated", b[7] == '\0');
  }
}

/* ---- degradation --------------------------------------------------------- */

static void test_edge_cases(void) {
  HgQualityTier t[8];
  int n;

  printf("\n-- tiny and impossible inputs --\n");

  n = hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, FIX_BR, FIX_SZ, FIX_OK, NULL, 5, t, 3);
  ok("a cap smaller than the list truncates to the cap", n == 3);
  ok("and keeps the BEST three, because the sort happens after", strcmp(t[0].definition, "1080p") == 0);

  ok("zero entries builds nothing",
     hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, NULL, NULL, NULL, NULL, 0, t, 8) == 0);
  ok("a NULL output builds nothing",
     hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, NULL, NULL, NULL, NULL, 5, NULL, 8) == 0);
  ok("a zero cap builds nothing",
     hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, NULL, NULL, NULL, NULL, 5, t, 0) == 0);
  ok("missing width/height arrays build nothing",
     hg_quality_build(FIX_QT, FIX_DEF, NULL, NULL, NULL, NULL, NULL, NULL, 5, t, 8) == 0);

  /* Optional arrays may be NULL: quality_type, definition, bitrate and size are all things
   * a real response can omit. */
  n = hg_quality_build(NULL, NULL, FIX_W, FIX_H, NULL, NULL, NULL, NULL, 5, t, 8);
  ok("only width and height is enough to build a ladder", n == 5);
  ok("with no quality_type, index_of finds nothing meaningful",
     hg_quality_index_of(t, n, 10) == -1);
  ok("but the order is still correct", t[0].height == 1080 && t[4].height == 360);
  ok("and the label falls back to the pixel height", strcmp(t[0].definition, "") == 0);

  /* Ties are broken by bitrate, and a full tie keeps the offered order -- a stable sort, so
   * the menu does not shuffle between runs. */
  {
    const int h2[3] = {480, 480, 480};
    const int w2[3] = {854, 848, 852};
    const int br2[3] = {400000, 900000, 100000};
    const char* const d2[3] = {"low", "high", "lowest"};
    HgQualityTier m[4];
    int mn = hg_quality_build(NULL, d2, w2, h2, br2, NULL, NULL, NULL, 3, m, 4);
    ok("equal heights are ordered by bitrate", mn == 3 &&
                                                  strcmp(m[0].definition, "high") == 0 &&
                                                  strcmp(m[2].definition, "lowest") == 0);
  }
  {
    const int h3[3] = {480, 480};
    const int w3[3] = {854, 854};
    const int br3[3] = {500000, 500000};
    const char* const d3[3] = {"first", "second"};
    HgQualityTier m[4];
    int mn = hg_quality_build(NULL, d3, w3, h3, br3, NULL, NULL, NULL, 2, m, 4);
    ok("a full tie keeps the offered order (stable)", mn == 2 &&
                                                         strcmp(m[0].definition, "first") == 0 &&
                                                         strcmp(m[1].definition, "second") == 0);
  }
}

/* ---- the codec in the label (C5) --------------------------------------------
 *
 * MEASURED: the codec is per tier, not a choice (see HgQualityTier::codec). So what is tested here is the
 * DISPLAY: the token upstream sends must appear in the label the menu shows, the two known ones must be
 * spelled the way users know them, and an unknown token must be shown AS IS rather than guessed at.
 */
static void test_codec_label(void) {
  /* THE MEASURED LADDER, indexed the way FIX_* is: FIX_DEF runs 360p..1080p, so this array does too. MEASURED
   * (fixtures/video_model.json): 360p=bytevc2, 480p/540p/720p=bytevc2, 1080p=bytevc1; and the h264 capture
   * differs only at the lowest tier (360p=h264). The ladder is SORTED best-first, so t[0] is the 1080p entry --
   * an array written in the other order would have tested the wrong tiers, which is exactly what the first
   * version of this test did. */
  static const char* CODECS[5] = {"h264", "bytevc2", "bytevc2", "bytevc2", "bytevc1"};
  HgQualityTier t[8];
  char buf[64];

  int n = hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, FIX_BR, FIX_SZ, FIX_OK, CODECS, 5, t, 8);
  ok("the ladder still builds with codecs", n == 5);

  hg_quality_label(&t[0], buf, (int)sizeof buf);
  ok("the best tier's label names HEVC", strstr(buf, "HEVC") != NULL);
  ok("and still leads with the resolution", strncmp(buf, "1080p", 5) == 0);
  hg_quality_label(&t[4], buf, (int)sizeof buf);
  ok("a 360p h264 tier says H.264", strstr(buf, "H.264") != NULL);
  hg_quality_label(&t[2], buf, (int)sizeof buf);
  ok("an UNKNOWN token is shown as upstream spelled it, not guessed", strstr(buf, "bytevc2") != NULL);

  /* No codec: the label is exactly what it was before this feature existed. */
  n = hg_quality_build(FIX_QT, FIX_DEF, FIX_W, FIX_H, FIX_BR, FIX_SZ, FIX_OK, NULL, 5, t, 8);
  hg_quality_label(&t[0], buf, (int)sizeof buf);
  ok("no codec means no suffix", strstr(buf, "HEVC") == NULL && strstr(buf, "H.264") == NULL);
}
int main(void) {
  test_codec_label();
  printf("hg_quality tests: the quality ladder\n");
  printf("================================================\n");

  test_order();
  test_index_of();
  test_labels();
  test_describe();
  test_edge_cases();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
