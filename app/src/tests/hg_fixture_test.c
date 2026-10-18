/*
 * Tests for the parsers, driven by fixtures captured from LIVE upstream.
 *
 * WHY THIS FILE EXISTS SEPARATELY FROM hg_api_test.c
 * ------------------------------------------------
 * hg_api_test.c tests shapes that were WRITTEN to look like the upstream. Those
 * shapes were wrong in ways that mattered: the port read `title` / `cover` /
 * `video_desc`, and the upstream sends `series_name` / `series_cover` /
 * `series_intro`. A fixture that is silently edited is worse than no fixture, so
 * the bytes here come from app/src/tests/fixtures/ (the .json files there), which
 * app/src/tools/capture_fixtures.mjs wrote directly from a live fetch.
 *
 * WHAT IS VERBATIM AND WHAT IS TRIMMED -- stated per fixture, below. The rule
 * applied when trimming was: every field the parser READS is byte-for-byte
 * upstream, and only bulk that would bloat the repo was cut. Nothing that the
 * assertions touch was edited. The one exception is called out in capitals.
 *
 *   fixtures/home_router.json
 *     VERBATIM: the loaderData key names ("layout" / "page"), both banner arrays
 *       and homeSections[0]'s tab_type/tab_name, and every series_id, series_name,
 *       series_cover/background_cover, series_intro and tags value.
 *     TRIMMED: 2 of 4 bannerList, 2 of 5 mBannerList, 1 homeSections of 4 holding
 *       2 of its 9 video_list entries; covers and intros truncated to a prefix.
 *       Extra page fields (req, isSuccess) dropped.
 *
 *   fixtures/detail_router.json
 *     VERBATIM: the key names detail_layout / detail_page / seriesDetail, the
 *       series_* values, and the 4 episode ids -- the real list's first two and
 *       last two, in order.
 *     TRIMMED: vid_list held 222 ids; 218 were dropped. `_capture` records that.
 *
 *   fixtures/video_model.json
 *     VERBATIM: the whole double-encoded envelope. BaseResp, code, data, and the
 *       FACT THAT video_model IS A STRING; every entry key; every encrypt_info key
 *       and value (encrypt, kid, spade_a, encryption_method); every video_meta key
 *       and value the parser reads; all 5 quality_type numbers and their codecs.
 *     TRIMMED: big_thumbs deleted, main_url/backup_url shortened to a 118-char
 *       synthetic URL. The URL is NOT upstream bytes and no assertion checks it
 *       beyond it being non-empty and plausible; nothing else about the entry
 *       changed.
 *
 *   fixtures/video_model_h264.json
 *     *** THIS IS NOT UPSTREAM DATA. *** It is video_model.json with entry 0's
 *       video_meta.codec_type changed bytevc2 -> h264, definition 360p -> 720p and
 *       quality_type 40 -> 30, so the "skip bytevc2 and take the next playable
 *       entry" path can be exercised against a real envelope. Every other byte is
 *       the same as video_model.json. On the live sample ALL FIVE entries were
 *       bytevc2 (40/30/20/10/1), so no genuine non-bytevc2 entry was available to
 *       capture -- that limitation is real and is recorded here rather than hidden.
 *
 * This file is ASCII only. The fixtures carry UTF-8 Chinese READ FROM DISK, so no
 * Chinese literal is written in this source, and the expected strings are asserted
 * as octal byte escapes.
 */

#include "hg_api.h"
#include "hg_play.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* ---- fixture loading ----------------------------------------------------- */

/* Read a fixture into a static buffer. Returns its length, or 0 and a message if
 * it is missing. No allocation: this runs on the switch test harness too. */
#define FIX_MAX 131072

static int load(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  size_t n;
  if (f == NULL) { printf("  !! cannot open %s\n", path); return 0; }
  n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  return (int)n;
}

/* Where the fixtures live, relative to the test binary's working directory --
 * run.ps1 sets the working directory to the repo root before running this.
 * The paths themselves are written out at each call site rather than composed
 * from this prefix: under TinyCC a non-literal format or an unused static
 * would warn, and this suite must build warning-free on BOTH compilers. */

/* The UTF-8 expected for bannerList[0].series_name, written as octal escapes so
 * this file stays pure ASCII on a GBK(936) machine. The upstream text is the
 * Chinese title "Yan Lu, your white moonlight has returned home" followed by a
 * FULLWIDTH COMMA (U+FF0C, bytes 357 274 214), which is exactly the kind of
 * multi-byte punctuation a naive parser mangles:
 *   \345\256\264 \345\276\213  = U+5BB4 U+5F8B
 *   \357\274\214               = U+FF0C  (the fullwidth comma)
 * Asserted byte for byte, not by length: a length check passes for mojibake. */
static const char *BANNER0_NAME =
  "\345\256\264\345\276\213\357\274\214\344\275\240\347\232\204\347\231\275\346\234\210\345\205\211\345\233\236\345\233\275\344\272\206";

/* The detail fixture's series_name: a 16-character Chinese title that ends in the
 * ASCII letters "BOSS", so it also proves the decoder does not treat non-ASCII as
 * a terminator or vice versa. */
static const char *DETAIL_NAME =
  "\350\201\232\345\256\235\344\273\231\347\233\206\344\271\213\346\235\202\347\201\265\346\240\271"
  "\346\211\215\346\230\257\347\234\237BOSS\347\254\254\345\215\201\344\270\211\345\255\243";

/* ---- home ---------------------------------------------------------------- */

static void test_home_fixture(void) {
  static char buf[FIX_MAX];
  static HgSeries items[HG_API_MAX_SERIES];
  size_t count = 999;
  int n;

  printf("\nhome page (LIVE fixture: fixtures/home_router.json)\n");

  n = load("app/src/tests/fixtures/home_router.json", buf, sizeof buf);
  if (n <= 0) { failures++; checks++; return; }

  /* The loaderData key really is "page" here. Asserted so that if upstream
   * renames it the fixture test fails loudly rather than silently passing. */
  ok("fixture still carries the dynamic key loaderData.page",
     strstr(buf, "\"page\"") != NULL && strstr(buf, "\"layout\"") != NULL);

  n = hg_api_parse_home(buf, (size_t)n, items, HG_API_MAX_SERIES, &count);
  ok("home page parsed", n > 0);
  if (n <= 0) { printf("        rc=%d\n", n); return; }

  /* 2 banners + 2 mBanners + 2 section videos = 6, all distinct series_ids. */
  ok("all 6 cards collected (2 banner + 2 mBanner + 2 section)",
     n == 6 && count == 6);
  if (n != 6) { size_t i; printf("        got %d\n", n); for (i = 0; i < count; i++) printf("        [%u] %s %s\n", (unsigned)i, items[i].id, items[i].title); }

  /* series_name -- THE MEASURED NAME. title/cover/video_desc do not exist. */
  ok("banner[0].series_name decoded to the exact UTF-8 bytes",
     strcmp(items[0].title, BANNER0_NAME) == 0);
  if (strcmp(items[0].title, BANNER0_NAME) != 0)
    printf("        got  \"%s\"\n", items[0].title);

  ok("banner[0].series_id is the live id",
     strcmp(items[0].id, "7503745441910017049") == 0);

  /* Cover assertions check the INVARIANT, not a host name: the CDN host rotates
   * between fetches (the same series came back from p3-novel, p6-novel and
   * p11-novel-seo across captures), so asserting a prefix would make this suite
   * fail for reasons that have nothing to do with the parser. What IS stable is
   * that the value is a non-empty https URL that was read from the right key --
   * a wrong key yields "" and is caught by the non-empty test. */
  ok("banner[0].series_cover read from series_cover (non-empty https URL)",
     strncmp(items[0].cover, "https://", 8) == 0 &&
     strstr(items[0].cover, "series_cover") == NULL &&
     strstr(items[0].cover, "byteimg.com") != NULL);
  ok("banner[0].cover is not the literal string \"null\"",
     strcmp(items[0].cover, "null") != 0);

  /* series_intro, not video_desc -- non-empty proves the right key was read,
   * since a wrong key yields "". */
  ok("banner[0].series_intro read from series_intro",
     strncmp(items[0].desc, "\344\270\203\345\271\264\345\211\215", 9) == 0);

  /* mBannerList has NO series_cover; the fallback must find background_cover.
   * Asserted by identity plus a non-empty URL -- the host rotates, see above.
   * The entry is located by ID rather than by index: banner/mBanner/section
   * order is an implementation detail of collect order, and pinning it here
   * would make a reordering look like a cover bug. */
  {
    size_t i;
    int found = 0;
    for (i = 0; i < count; i++) {
      if (strcmp(items[i].id, "7574406343008734232") == 0) {
        found = (strncmp(items[i].cover, "https://", 8) == 0 &&
                 strstr(items[i].cover, "byteimg.com") != NULL);
        break;
      }
    }
    ok("mBanner[0] falls back to background_cover", found);
    if (!found) {
      size_t j;
      for (j = 0; j < count; j++)
        printf("        [%u] %s cover=\"%s\"\n", (unsigned)j, items[j].id, items[j].cover);
    }
  }

  /* homeSections[].video_list[] cards. MEASURED: these have NO series_name at
   * all -- all 36 of them carry series_title instead -- so this fixture is the
   * only place the fallback is exercised against real bytes. */
  ok("a section card (series_title, no series_name) lands with its id",
     strcmp(items[4].id, "7691228619774905368") == 0);
  ok("the section card's title came from series_title",
     strcmp(items[4].title, DETAIL_NAME) == 0);
  if (strcmp(items[4].title, DETAIL_NAME) != 0)
    printf("        got  \"%s\"\n", items[4].title);
  ok("section card cover read from series_cover",
     strncmp(items[4].cover, "https://", 8) == 0 &&
     strstr(items[4].cover, "byteimg.com") != NULL);
  ok("section card badge came from episode_right_text",
     items[4].badge[0] != '\0');

  /* No duplicates: the same series appears in a banner and a section upstream. */
  {
    size_t i, j;
    int dup = 0;
    for (i = 0; i < count; i++)
      for (j = i + 1; j < count; j++)
        if (strcmp(items[i].id, items[j].id) == 0) dup = 1;
    ok("series_id de-duplicated across banner and section", !dup);
  }

  /* A buffer too small refuses the WHOLE page rather than returning a prefix. */
  {
    size_t c = 999;
    ok("a 2-item buffer refuses the 6-card page",
       hg_api_parse_home(buf, strlen(buf), items, 2, &c) == HG_API_ERR_SMALL && c == 0);
  }
}

/* ---- dynamic loaderData keys --------------------------------------------- */

/* The whole point of walking rather than hardcoding: the same parser must find
 * content under keys it has never seen. These fixtures are SYNTHETIC and labelled
 * as such -- they exist to prove key-name independence, which the live home
 * fixture (always "page") cannot show on its own. The FIELD NAMES inside them are
 * the measured ones. */
static const char *CATEGORY_JSON =
  "{\"loaderData\":{\"category_layout\":null,\"category_page\":{"
  "\"video_list\":[{\"series_id\":\"111\",\"series_name\":\"\\u4e00\","
  "\"series_cover\":\"https://c/1.jpg\",\"series_intro\":\"d\"}]}}}";

static const char *SEARCH_PAGE_JSON =
  "{\"loaderData\":{\"search_layout\":null,\"search_(keyword)/page\":{"
  "\"video_list\":[{\"series_id\":\"222\",\"series_name\":\"\\u4e8c\","
  "\"series_cover\":\"https://c/2.jpg\",\"series_intro\":\"d\"}]}}}";

static const char *RANK_JSON =
  "{\"loaderData\":{\"rank_layout\":null,\"rank_hot-comic-drama/page\":{"
  "\"video_list\":[{\"series_id\":\"333\",\"series_name\":\"\\u4e09\","
  "\"series_cover\":\"https://c/3.jpg\",\"series_intro\":\"d\"}]}}}";

/* A page whose FIRST array is not series-shaped, as the live home page is:
 * req.semDeliveryConfig.items comes before bannerList and its entries carry a
 * `default` object. A parser that took the first array it saw would read those. */
static const char *DISTRACTOR_JSON =
  "{\"loaderData\":{\"page\":{"
  "\"req\":{\"semDeliveryConfig\":{\"items\":[{\"configKey\":\"x\",\"default\":{}}]}},"
  "\"bannerList\":[{\"series_id\":\"444\",\"series_name\":\"\\u56db\","
  "\"series_cover\":\"https://c/4.jpg\"}]}}}";

static void test_dynamic_keys(void) {
  static HgSeries items[HG_API_MAX_SERIES];
  static char keybuf[128];
  size_t count = 999, page = 0;
  int n;

  printf("\ndynamic loaderData keys (synthetic fixtures, measured field names)\n");

  n = hg_api_parse_home(CATEGORY_JSON, strlen(CATEGORY_JSON), items,
                        HG_API_MAX_SERIES, &count);
  ok("category_page found without a hardcoded key",
     n == 1 && strcmp(items[0].id, "111") == 0);

  count = 999;
  n = hg_api_parse_home(SEARCH_PAGE_JSON, strlen(SEARCH_PAGE_JSON), items,
                        HG_API_MAX_SERIES, &count);
  ok("search_(keyword)/page found, parentheses and slash included",
     n == 1 && strcmp(items[0].id, "222") == 0);

  count = 999;
  n = hg_api_parse_home(RANK_JSON, strlen(RANK_JSON), items,
                        HG_API_MAX_SERIES, &count);
  ok("rank_hot-comic-drama/page found",
     n == 1 && strcmp(items[0].id, "333") == 0);

  /* And the discovered key is reported back, so a caller can log WHAT it read. */
  ok("the discovered key name is reported",
     hg_api_find_page(RANK_JSON, strlen(RANK_JSON), &page, keybuf, sizeof keybuf) == 1 &&
     strcmp(keybuf, "rank_hot-comic-drama/page") == 0);

  count = 999;
  n = hg_api_parse_home(DISTRACTOR_JSON, strlen(DISTRACTOR_JSON), items,
                        HG_API_MAX_SERIES, &count);
  ok("a non-series array before bannerList is skipped",
     n == 1 && strcmp(items[0].id, "444") == 0);

  /* Refusals, each distinguishable. */
  ok("no loaderData is HG_API_ERR_ROUTER",
     hg_api_parse_home("{\"x\":1}", 7, items, HG_API_MAX_SERIES, &count) == HG_API_ERR_ROUTER);
  ok("empty loaderData is HG_API_ERR_ROUTER",
     hg_api_parse_home("{\"loaderData\":{}}", 17, items, HG_API_MAX_SERIES, &count)
       == HG_API_ERR_ROUTER);
  ok("a loaderData with no usable cards is HG_API_ERR_EMPTY",
     hg_api_parse_home("{\"loaderData\":{\"page\":{\"video_list\":[]}}}", 41,
                       items, HG_API_MAX_SERIES, &count) == HG_API_ERR_EMPTY);
}

/* ---- episode ids --------------------------------------------------------- */

static void test_episode_ids_fixture(void) {
  static char buf[FIX_MAX];
  static HgEpisode eps[HG_API_MAX_EPISODES];
  size_t count = 999;
  int n;

  printf("\nepisode ids (LIVE fixture: fixtures/detail_router.json)\n");

  n = load("app/src/tests/fixtures/detail_router.json", buf, sizeof buf);
  if (n <= 0) { failures++; checks++; return; }

  n = hg_api_parse_episode_ids(buf, (size_t)n, eps, HG_API_MAX_EPISODES, &count);
  ok("episode ids parsed from seriesDetail.vid_list", n == 4 && count == 4);
  if (n != 4) { printf("        rc=%d count=%u\n", n, (unsigned)count); return; }

  /* Entries are BARE STRINGS. Order is list order, first two then last two. */
  ok("entry 0 is the live first vid",
     strcmp(eps[0].id, "7691249364097829913") == 0);
  ok("entry 1 is the live second vid",
     strcmp(eps[1].id, "7691249358905281561") == 0);
  ok("entry 2 is the live 221st vid",
     strcmp(eps[2].id, "7692415709166439486") == 0);
  ok("entry 3 is the live last vid",
     strcmp(eps[3].id, "7692415709166472254") == 0);

  /* Every id is 19 digits: the real shape, and a length check the port's
   * [0-9]{1,30} validator must accept. */
  ok("all ids are 19 digits and pass the id validator",
     strlen(eps[0].id) == 19 && hg_api_valid_id(eps[0].id) &&
     hg_api_valid_id(eps[3].id));

  /* Truncation is refused, not reported as a short series. */
  {
    size_t c = 999;
    ok("a 3-slot buffer refuses the 4-id list",
       hg_api_parse_episode_ids(buf, strlen(buf), eps, 3, &c) == HG_API_ERR_SMALL && c == 0);
  }

  /* Shape drift: an OBJECT where a string belongs is refused, not skipped. */
  {
    static const char *OBJ = "{\"loaderData\":{\"page\":{\"seriesDetail\":"
      "{\"vid_list\":[{\"vid\":\"1\"}]}}}}";
    size_t c = 999;
    ok("an object vid_list entry is refused, not skipped",
       hg_api_parse_episode_ids(OBJ, strlen(OBJ), eps, 8, &c) == HG_API_ERR_EPISODE);
  }
  {
    static const char *DUP = "{\"loaderData\":{\"page\":{\"seriesDetail\":"
      "{\"vid_list\":[\"1001\",\"1002\",\"1001\"]}}}}";
    size_t c = 999;
    ok("a duplicate id is refused",
       hg_api_parse_episode_ids(DUP, strlen(DUP), eps, 8, &c) == HG_API_ERR_DUPLICATE);
  }
  {
    static const char *EMPTY = "{\"loaderData\":{\"page\":{\"seriesDetail\":"
      "{\"vid_list\":[]}}}}";
    size_t c = 999;
    ok("an empty vid_list is HG_API_ERR_EMPTY",
       hg_api_parse_episode_ids(EMPTY, strlen(EMPTY), eps, 8, &c) == HG_API_ERR_EMPTY);
  }
  {
    static const char *NOVL = "{\"loaderData\":{\"page\":{\"seriesDetail\":{}}}}";
    size_t c = 999;
    ok("a seriesDetail without vid_list is HG_API_ERR_MISSING",
       hg_api_parse_episode_ids(NOVL, strlen(NOVL), eps, 8, &c) == HG_API_ERR_MISSING);
  }
}

/* ---- playback plan ------------------------------------------------------- */

static void test_play_fixture(void) {
  static char buf[FIX_MAX];
  static HgPlayPlan plan;
  int n;

  printf("\nplayback plan (LIVE fixture: fixtures/video_model.json)\n");

  n = load("app/src/tests/fixtures/video_model.json", buf, sizeof buf);
  if (n <= 0) { failures++; checks++; return; }

  /* The envelope really is doubly encoded: video_model is a JSON STRING, i.e. it
   * appears as \"video_model\":\"{\\\"status\\\"... on the wire. */
  ok("fixture's video_model is a JSON string, not an object",
     strstr(buf, "\"video_model\":\"{") != NULL);

  /* Entries 0-3 of the live response are bytevc2, entry 4 is bytevc1. The parser
   * must skip the four and take the fifth -- bytevc1 is a DIFFERENT codec and is
   * playable, so treating the two names as one would wrongly reject the only
   * usable 1080p source in the response. */
  n = hg_play_plan_from_model(buf, strlen(buf), &plan);
  ok("the four bytevc2 entries are skipped, bytevc1 is taken", n == 0);
  if (n != 0) { printf("        rc=%d\n", n); return; }
  ok("the surviving entry is the 1080p bytevc1 one",
     strcmp(plan.codec_type, "bytevc1") == 0 &&
     plan.quality_type == 1 &&
     strcmp(plan.definition, "1080p") == 0);
  ok("its video_meta pixels are the live 1920x1080",
     plan.vwidth == 1920 && plan.vheight == 1080);
  ok("encrypt=true and the live kid were read from encrypt_info",
     plan.encrypt_flag == 1 &&
     strcmp(plan.kid, "6ac13d1b476d5a03cc1186750002ebeb") == 0 &&
     strcmp(plan.encryption_method, "cenc-aes-ctr") == 0);

  /* A response where EVERY entry is bytevc2 must say so, and must be
   * distinguishable from "there was no list". Built from the live envelope by
   * forcing the one bytevc1 entry to bytevc2 -- labelled derived, like the other
   * edited fixture. */
  {
    static char edited[FIX_MAX];
    char *hit;
    size_t len;
    strcpy(edited, buf);
    hit = strstr(edited, "\\\"codec_type\\\":\\\"bytevc1\\\"");
    ok("the bytevc1 entry is findable in the escaped text for editing", hit != NULL);
    if (hit != NULL) {
      /* Flip the LAST digit of "bytevc1" -> "bytevc2". Both are 7 chars, so the
       * document stays byte-identical in length and the edit stays inside the
       * string literal. Offset computed, not hardcoded, so a fixture change
       * cannot silently turn this into an edit of the wrong byte. */
      char *digit = strstr(hit, "bytevc1");
      ok("the codec literal is where expected", digit != NULL);
      if (digit != NULL) {
        digit[6] = '2';                      /* '1' -> '2' */
        len = strlen(edited);
        memset(&plan, 0, sizeof plan);
        n = hg_play_plan_from_model(edited, len, &plan);
        ok("all-bytevc2 reports HG_PLAY_ERR_CODEC, not HG_PLAY_ERR_NOMODEL",
           n == HG_PLAY_ERR_CODEC);
        if (n != HG_PLAY_ERR_CODEC) printf("        rc=%d\n", n);
      }
    }
  }

  /* No video_list anywhere: a DIFFERENT error, so a caller can tell an upstream
   * shape change from an undecodable response. */
  ok("a document with no video_list reports HG_PLAY_ERR_NOMODEL",
     hg_play_plan_from_model("{\"code\":0,\"data\":{}}", 20, &plan) == HG_PLAY_ERR_NOMODEL);

  /* The inner document is reachable at all -- this is the bug that mattered. */
  {
    static const char *INNER = "{\"status\":10,\"video_list\":[{\"main_url\":\"https://x/1.mp4\","
      "\"quality_type\":40,\"video_meta\":{\"definition\":\"360p\",\"codec_type\":\"h264\","
      "\"vwidth\":640,\"vheight\":360,\"bitrate\":216741,\"size\":5172264},"
      "\"encrypt_info\":{\"encrypt\":false,\"kid\":\"abc\"}}]}";
    n = hg_play_plan_from_model(INNER, strlen(INNER), &plan);
    ok("a bare inner document is still accepted", n == 0);
  }
}

/* ---- THE UNKEYED ENVELOPE (build156) ---------------------------------------------------------
 *
 * The live route that answers now is /novel/player/video_model/v1/ -- ONE video per request -- and it
 * returns `data` as the video's OWN object, with video_model directly in it, instead of `data` keyed
 * by the long numeric video id. The old batched route answers {"Code":110001} to every request that
 * reaches it, including one with no signing headers at all, which is what put "play_plan rc=-5" on
 * the screen for a whole round.
 *
 * This fixture is a REAL response captured from that route (24,124 bytes), not a hand-written one, so
 * the nesting it locks is the nesting the server actually sends. Its inner model carries the same
 * five tiers as the old fixture's: 360p/480p/540p/720p as bytevc2 and 1080p as bytevc1.
 */
static void test_play_fixture_v1(void) {
  static char buf[FIX_MAX];
  static HgPlayPlan plan;
  int n;

  printf("\nplayback plan (LIVE one-video envelope: fixtures/video_model_v1.json)\n");

  n = load("app/src/tests/fixtures/video_model_v1.json", buf, sizeof buf);
  if (n <= 0) { failures++; checks++; return; }

  /* The shape assertion that makes this fixture the RIGHT one for this case: `data` opens with a
   * scalar field, so it is not an object keyed by a video id. If a future fixture goes back to the
   * keyed envelope this check fails rather than the test quietly passing on the old path. */
  ok("the fixture's `data` is the video's own object, not keyed by a video id",
     strstr(buf, "\"data\":{\"disable_vsr\"") != NULL);
  ok("its video_model is still a doubly-encoded JSON string",
     strstr(buf, "\"video_model\":\"{") != NULL);

  n = hg_play_plan_from_model(buf, strlen(buf), &plan);
  ok("the unkeyed envelope is unwrapped and a plan is built", n == 0);
  if (n != 0) { printf("        rc=%d\n", n); return; }
  ok("the surviving entry is the 1080p bytevc1 one",
     strcmp(plan.codec_type, "bytevc1") == 0 &&
     plan.quality_type == 1 &&
     strcmp(plan.definition, "1080p") == 0);
  ok("its video_meta pixels are the live 1920x1080",
     plan.vwidth == 1920 && plan.vheight == 1080);
  ok("encrypt=true and the live kid were read from encrypt_info",
     plan.encrypt_flag == 1 &&
     strcmp(plan.encryption_method, "cenc-aes-ctr") == 0);

  /* The ladder reads the whole inner document through hg_play_model_inner, so the same unwrapping
   * has to serve it -- the port builds the quality menu from this, and a plan that worked while the
   * ladder did not would show as "no ladder for this episode". */
  {
    const char *inner = NULL;
    size_t innerLen = 0;
    n = hg_play_model_inner(buf, strlen(buf), &inner, &innerLen);
    ok("hg_play_model_inner reaches the inner model in the unkeyed envelope", n == 0);
    ok("the inner model is the video_list document",
       n == 0 && inner != NULL && innerLen > 0 &&
       strstr(inner, "\"video_list\"") != NULL);
    ok("all five tiers are in it, so the ladder can be built from one request",
       n == 0 && inner != NULL && innerLen > 0 &&
       strstr(inner, "\"1080p\"") != NULL && strstr(inner, "\"360p\"") != NULL);
  }
}

static void test_play_h264_fixture(void) {
  static char buf[FIX_MAX];
  static HgPlayPlan plan;
  int n;

  printf("\nplayback plan, mixable (DERIVED fixture: fixtures/video_model_h264.json)\n");

  n = load("app/src/tests/fixtures/video_model_h264.json", buf, sizeof buf);
  if (n <= 0) { failures++; checks++; return; }

  n = hg_play_plan_from_model(buf, strlen(buf), &plan);
  ok("plan built from the doubly-encoded envelope", n == 0);
  if (n != 0) { printf("        rc=%d\n", n); return; }

  /* ---- THE POLICY CHANGED HERE (build158), SO THESE ASSERTIONS DID TOO ----------------------------
   *
   * What this fixture is for: entry 0 is h264 (640x360) and entry 4 is bytevc1 (1920x1080), so it
   * exercises "skip the bytevc2 rows and take a playable one". Until build158 the parser returned the
   * FIRST usable entry -- entry 0 -- and the five assertions below described IT.
   *
   * build158 picks the BEST usable entry instead: the user asked for 1080p by default, and the
   * single-video route the app now calls returns every tier in the one response the plan already comes
   * from, so the best entry costs no extra request (hg_play.c, and docs/另见播放链路自检-第21轮.md).
   * With a decodable 1920x1080 entry present, entry 0 cannot win any more, so describing entry 0 here
   * would be asserting the policy this build replaced.
   *
   * THE ORIGINAL PROPERTY IS NOT LOST: the block after these lines removes the 1080p entry from the
   * same document in memory, and asserts that the h264 entry is then chosen -- i.e. that a codec which
   * is not bytevc1 is still perfectly playable, which is what this fixture was written to prove. */
  ok("the BEST entry wins, not the first decodable one: 1080p bytevc1",
     strcmp(plan.codec_type, "bytevc1") == 0 && plan.has_codec_type == 1 &&
     plan.quality_type == 1);
  ok("definition came from video_meta.definition",
     strcmp(plan.definition, "1080p") == 0);
  ok("vwidth / vheight came from video_meta",
     plan.vwidth == 1920 && plan.vheight == 1080);
  ok("bitrate and size came from video_meta",
     plan.bitrate == 1228051 && plan.size_bytes == 28974608);
  ok("kid came from encrypt_info.kid",
     strcmp(plan.kid, "6ac13d1b476d5a03cc1186750002ebeb") == 0);
  ok("encryption_method came from encrypt_info",
     strcmp(plan.encryption_method, "cenc-aes-ctr") == 0);
  ok("encrypt=true was read as a BOOLEAN, not as an absent key",
     plan.encrypt_flag == 1);

  /* spade_a IS present in this fixture, under encrypt_info. It is the live value,
   * so the key derivation runs for real -- and the live spade_a is a valid one. */
  ok("spade_a was found under encrypt_info and produced a CENC key",
     plan.encrypted == 1 &&
     strncmp(plan.decryption_key, "decryption_key=", 15) == 0 &&
     strlen(plan.decryption_key) == 47);

  /* key_seed DOES NOT EXIST upstream, so the URL must survive untouched (apart
   * from cleanUrl) rather than being run through a decryptor with a bogus seed. */
  ok("no key_seed: the URL is taken as already plaintext",
     strncmp(plan.url, "https://v5-dj-reading-video.example/", 36) == 0);

  ok("mpv header fields carry UA and Referer",
     strstr(plan.header_fields, "User-Agent: ") == plan.header_fields &&
     strstr(plan.header_fields, "Referer: https://novel.snssdk.com/") != NULL);

  /* ---- THE PROPERTY THIS FIXTURE WAS BUILT FOR, STILL PROVEN (build158) ---------------------------
   *
   * Entry 4 is the only bytevc1 row and it now wins on resolution, so "h264 is accepted" can no longer be
   * seen from the unedited document. Flipping that one row's last codec digit to bytevc2 leaves the h264
   * entry as the only decodable one -- and then the plan MUST come from it. If the codec filter ever
   * starts treating h264 as undecodable, this is where it shows. */
  {
    static char edited[FIX_MAX];
    char* hit;
    size_t len;
    strcpy(edited, buf);
    hit = strstr(edited, "\\\"codec_type\\\":\\\"bytevc1\\\"");
    ok("the bytevc1 entry is findable in the escaped text for editing", hit != NULL);
    if (hit != NULL) {
      char* digit = strstr(hit, "bytevc1");
      ok("the codec literal is where expected", digit != NULL);
      if (digit != NULL) {
        digit[6] = '2';                  /* "bytevc1" -> "bytevc2", same length */
        len = strlen(edited);
        memset(&plan, 0, sizeof plan);
        n = hg_play_plan_from_model(edited, len, &plan);
        ok("with only the h264 row decodable, a plan is still built", n == 0);
        if (n == 0) {
          ok("and it came from the h264 row: a non-bytevc1 codec is still playable",
             strcmp(plan.codec_type, "h264") == 0 && plan.has_codec_type == 1 &&
             plan.quality_type == 30 && plan.vwidth == 640 && plan.vheight == 360);
        }
      }
    }
  }
}

/* A NULL `count` must be legal.
 *
 * Every parse function both returns the count and writes it through a
 * `size_t *count`. That signature invites callers to pass NULL -- the return
 * value is right there -- and EVERY test in this suite passed a real pointer, so
 * the NULL path was never exercised by anything.
 *
 * It was exercised exactly once, on the console, where it wrote to address 0 and
 * aborted the process with 2168-0002, one line after the home page had parsed
 * correctly. These cases exist so that if the guard is ever removed, it crashes
 * here instead of on hardware. */
static void test_null_count(void) {
  static char b[FIX_MAX];
  static HgSeries items[HG_API_MAX_SERIES];
  static HgEpisode eps[HG_API_MAX_EPISODES];
  size_t n = 0;
  long got;

  printf("\nNULL out-parameter tolerance\n");

  /* The first two would abort the process without the guard. */
  got = load("app/src/tests/fixtures/home_router.json", b, sizeof b);
  ok("parse_home accepts a NULL count",
     got > 0 && hg_api_parse_home(b, (size_t)got, items, 8, NULL) > 0);

  got = load("app/src/tests/fixtures/detail_router.json", b, sizeof b);
  ok("parse_episode_ids accepts a NULL count",
     got > 0 && hg_api_parse_episode_ids(b, (size_t)got, eps, 4, NULL) > 0);

  /* An all-NULL call must be refused rather than dereferenced. */
  ok("parse_home with json=NULL is refused",
     hg_api_parse_home(NULL, 0, items, 8, &n) < 0);
  ok("parse_episode_ids with json=NULL is refused",
     hg_api_parse_episode_ids(NULL, 0, eps, 4, &n) < 0);

  /* A real pointer must still be written, including on the error path. */
  n = 12345;
  (void)hg_api_parse_home("{\"x\":1}", 7, items, 8, &n);
  ok("a real count pointer is still written on error", n == 0);

  n = 12345;
  (void)hg_api_parse_episode_ids("{\"x\":1}", 7, eps, 4, &n);
  ok("  ... and by parse_episode_ids too", n == 0);
}

/* The device's own blob, dumped from the console, as a regression fixture.
 *
 * This is the data that made the home page fail to parse BOTH on the console and
 * on the host, and every trimmed fixture passed on it. The cause was two
 * mBannerList cards whose series_intro is 553 and 570 bytes against a 512-byte
 * HgSeries.desc: the strict decoder returned -1, opt_field reported "present but
 * broken", series_from_card returned HG_API_ERR_JSON, and push_card propagated
 * that, so the WHOLE page was refused over two synopses.
 *
 * The trimmed fixtures could not catch it -- they carry short intros -- which is
 * the third time a fixture smaller than reality hid a bug in this project (an
 * 80-byte medusa buffer, a 1,976-byte router, now a 512-byte desc). The full
 * blob is kept deliberately rather than trimmed. */
static void test_device_blob(void) {
  static char b[1 << 20];
  static HgSeries items[HG_API_MAX_SERIES];
  size_t count = 0;
  long n;
  size_t i, longest = 0;

  printf("\nfull device blob (LIVE: fixtures/device_home_router.bin, 45609 bytes)\n");

  n = load("app/src/tests/fixtures/device_home_router.bin", b, sizeof b);
  ok("the device blob loaded", n > 40000);
  if (n <= 0) return;

  count = 0;
  ok("home page parses (was HG_API_ERR_JSON over two long intros)",
     hg_api_parse_home(b, (size_t)n, items, HG_API_MAX_SERIES, &count) > 0);
  ok("  ... and yields the full card set", count == 36);

  /* The cards that used to refuse the whole page must be present, not skipped. */
  {
    int found_long = 0;
    for (i = 0; i < count; i++) {
      size_t d = strlen(items[i].desc);
      if (d > longest) longest = d;
      if (d >= 500) found_long++;
    }
    ok("a description longer than the 512-byte buffer was truncated, not fatal",
       longest >= 500);
    ok("  ... and its card still landed", found_long >= 2);
    printf("        %u cards, longest desc=%u bytes (buffer is 512)\n",
           (unsigned)count, (unsigned)longest);
  }

  /* Every card must still be addressable: truncating text must not truncate ids. */
  {
    int all_valid = 1;
    for (i = 0; i < count; i++) if (!hg_api_valid_id(items[i].id)) all_valid = 0;
    ok("every id survived intact", all_valid);
  }

  ok("a capacity that cannot hold them reports SMALL, not JSON",
     hg_api_parse_home(b, (size_t)n, items, 8, &count) == HG_API_ERR_SMALL);
}

/* ---- home tiers ------------------------------------------------------------
 *
 * MEASURED, and the reason hg_api_parse_home_sections exists: the home page is
 * not a flat list. homeSections[] carries 4 named sections of 9 cards each
 * (4 x 9 = 36, which is exactly what hg_api_parse_home reports after merging the
 * banners in and de-duplicating), and more than one section OPENS WITH THE SAME
 * SERIES.
 *
 * Both facts are asserted below, for different reasons. The first because a
 * tiered UI has nothing to render without it. The second because it is the trap:
 * pushing every section through one shared de-duplicating counter would drop the
 * repeated cards, every section would still render, and the only symptom would be
 * a shorter list -- the kind of silent loss this project keeps having to design
 * against.
 *
 * The slugs (all / human / comic / ai) are asserted rather than the headings:
 * the headings are Chinese and this file must stay pure ASCII on a GBK(936)
 * machine, so only their PRESENCE is checked here. */
static void test_home_tiers(void) {
  static char b[FIX_MAX];
  static HgHomePage page;
  static HgSeries flat[HG_API_MAX_SERIES];
  int n, rc;
  size_t i, j, k, total = 0, headed = 0, nines = 0, tiled = 1, repeated = 0;

  printf("\nhome tiers (LIVE fixture: fixtures/home_router_full.json)\n");

  n = load("app/src/tests/fixtures/home_router_full.json", b, sizeof b);
  ok("the full home page loaded", n > 40000);
  if (n <= 0) return;

  rc = hg_api_parse_home_sections(b, (size_t)n, &page);
  ok("homeSections parses into tiers", rc > 0);
  if (rc <= 0) return;

  printf("        %d sections, %u cards in total\n", rc, (unsigned)page.item_count);

  ok("there are 4 sections", page.section_count == 4);
  ok("section 0 is tab_type=all", strcmp(page.sections[0].tab_type, "all") == 0);
  ok("section 1 is tab_type=human", strcmp(page.sections[1].tab_type, "human") == 0);
  ok("section 2 is tab_type=comic", strcmp(page.sections[2].tab_type, "comic") == 0);
  ok("section 3 is tab_type=ai", strcmp(page.sections[3].tab_type, "ai") == 0);

  for (i = 0; i < page.section_count; i++) {
    if (page.sections[i].tab_name[0] != '\0') headed++;
    if (page.sections[i].count == 9) nines++;
    if (page.sections[i].first != total) tiled = 0;
    total += page.sections[i].count;
  }
  ok("every section carries a heading", headed == page.section_count);
  ok("every section holds 9 cards", nines == page.section_count);
  ok("the section ranges tile the item array with no gap", tiled);
  ok("the ranges cover every parsed card", total == page.item_count);
  ok("4 x 9 = 36 cards, matching the flat parser's count", page.item_count == 36);

  /* Cross-section repeats. If the sections had been run through one shared
   * de-duplicating counter this count would be 0 and section 2 would be short. */
  for (i = 0; i < page.section_count; i++) {
    for (j = i + 1; j < page.section_count; j++) {
      for (k = 0; k < page.sections[i].count; k++) {
        size_t m;
        const HgSeries *a = &page.items[page.sections[i].first + k];
        for (m = 0; m < page.sections[j].count; m++) {
          const HgSeries *c = &page.items[page.sections[j].first + m];
          if (strcmp(a->id, c->id) == 0) repeated++;
        }
      }
    }
  }
  ok("a series really does appear in more than one section", repeated > 0);
  printf("        %u cross-section repeats preserved\n", (unsigned)repeated);

  /* The flat parser and the tiered one describe the same page from different
   * directions: it merges the banners and de-duplicates, this one keeps the
   * tiers and their repeats. Asserting that BOTH are 36 documents that the
   * tiers were not invented and that the flat count was not lost. */
  {
    size_t count = 0;
    int frc = hg_api_parse_home(b, (size_t)n, flat, HG_API_MAX_SERIES, &count);
    ok("hg_api_parse_home still reports 36 on the same bytes", frc > 0 && count == 36);
    ok("  ... and the tier view is a different composition, not a copy",
       page.item_count == 36 && page.section_count == 4);
  }

  /* Every card in every section must be usable on its own: an id to open and a
   * title to draw. A section of blanks would render as an empty tier. */
  {
    int usable = 1;
    for (i = 0; i < page.item_count; i++) {
      if (page.items[i].title[0] == '\0' || !hg_api_valid_id(page.items[i].id)) usable = 0;
    }
    ok("every tiered card has a valid id and a non-empty title", usable == 1);
  }

  /* TAGS, and why they are asserted at all.
   *
   * MEASURED: category_list is on 36/36 home cards while series_intro (`desc`) was
   * on 0/36 in this capture. A card UI that shows "something descriptive under the
   * title" therefore has to lean on the tags, so "the tags parse" is load-bearing
   * for the layout rather than cosmetic. Asserted: every card has tags, they are
   * joined with '/', and no tag is truncated mid-way (a truncated multi-byte tag
   * would show as mojibake, which reads as a parser bug).
   *
   * The tag TEXT is Chinese, so the file stays ASCII and only the STRUCTURE is
   * checked here -- the values themselves are verified by the dump in the
   * handover, taken straight from the fixture. */
  {
    size_t withTags = 0, joined = 0, emptyDesc = 0, longest = 0;
    for (i = 0; i < page.item_count; i++) {
      const HgSeries* s = &page.items[i];
      size_t n = strlen(s->tags);
      if (n > 0) withTags++;
      if (n > longest) longest = n;
      if (strchr(s->tags, '/') != NULL) joined++;
      /* A truncation would leave a dangling separator. */
      if (n > 0 && s->tags[n - 1] == '/') joined = 0;
      if (s->desc[0] == '\0') emptyDesc++;
    }
    ok("every card carries tags", withTags == page.item_count);
    ok("tags are '/' joined and not truncated at a separator", joined == page.item_count);
    ok("  ... and they fit the 128-byte buffer", longest > 0 && longest < sizeof page.items[0].tags);
    printf("        %u cards: tags on %u, empty desc on %u, longest tags=%u bytes\n",
           (unsigned)page.item_count, (unsigned)withTags, (unsigned)emptyDesc, (unsigned)longest);
    /* The reason the tags exist: the synopsis is NOT reliably there. If this
     * assertion ever flips, the card layout can start using desc first. */
    ok("the synopsis is absent upstream, which is why tags matter",
       emptyDesc == page.item_count);
  }
}

/* ---- home rows -------------------------------------------------------------
 *
 * The row model is what a tiered screen iterates: one header row per section,
 * then that section's cards. It lives in C so it can be tested here rather than
 * debugged on a console -- the last time this arithmetic was left to view code,
 * a recycler received a zero-row list and the process died with no message.
 *
 * The assertions below are about INDICES, because indices are what can be wrong
 * silently: an off-by-one would still render a full screen of plausible rows. */
static void test_home_rows(void) {
  static char b[FIX_MAX];
  static HgHomePage page;
  static HgHomeRow rows[HG_API_MAX_HOME_ITEMS + HG_API_MAX_SECTIONS];
  HgHomeRow small[4];
  size_t count = 0, i, headers = 0, series = 0;
  int n, rc;

  printf("\nhome rows (LIVE fixture: fixtures/home_router_full.json)\n");

  n = load("app/src/tests/fixtures/home_router_full.json", b, sizeof b);
  ok("the full home page loaded", n > 40000);
  if (n <= 0) return;

  {
    int prc = hg_api_parse_home_sections(b, (size_t)n, &page);
    ok("the tiered parse succeeds first", prc > 0);
    if (prc <= 0) return;
  }

  rc = hg_api_home_rows(&page, rows, sizeof rows / sizeof rows[0], &count);
  ok("rows are produced", rc > 0);
  if (rc <= 0) return;

  /* N cards + S headers. */
  ok("row count is cards + one header per section",
     count == page.item_count + page.section_count);
  printf("        %u rows = %u cards + %u headers\n",
         (unsigned)count, (unsigned)page.item_count, (unsigned)page.section_count);

  for (i = 0; i < count; i++) {
    if (rows[i].kind == HG_API_ROW_HEADER) headers++;
    if (rows[i].kind == HG_API_ROW_SERIES) series++;
  }
  ok("exactly one header row per section", headers == page.section_count);
  ok("every card got exactly one series row", series == page.item_count);

  /* The shape of the whole list, not just its totals: header first, then that
   * section's cards in order, then the next header. */
  {
    size_t at = 0, s;
    int shape_ok = 1;
    for (s = 0; s < page.section_count && shape_ok; s++) {
      if (rows[at].kind != HG_API_ROW_HEADER) shape_ok = 0;
      else if (rows[at].section != s) shape_ok = 0;
      at++;
      {
        size_t j;
        for (j = 0; j < page.sections[s].count; j++) {
          if (rows[at].kind != HG_API_ROW_SERIES) { shape_ok = 0; break; }
          if (rows[at].section != s) { shape_ok = 0; break; }
          if (rows[at].item != page.sections[s].first + j) { shape_ok = 0; break; }
          at++;
        }
      }
    }
    ok("the list is header, that section's cards in order, next header", shape_ok);
    ok("  ... and it consumed every row exactly once", at == count);
  }

  /* Every series row must point at a card that exists and carries a title. A
   * row pointing past item_count would draw a stale HgSeries. */
  {
    int items_ok = 1;
    for (i = 0; i < count; i++) {
      if (rows[i].kind != HG_API_ROW_SERIES) continue;
      if (rows[i].item >= page.item_count) { items_ok = 0; break; }
      if (page.items[rows[i].item].title[0] == '\0') { items_ok = 0; break; }
    }
    ok("every series row points at a real, titled card", items_ok);
    /* ... and every card is reachable, i.e. nothing was skipped. */
    {
      size_t seen[HG_API_MAX_HOME_ITEMS];
      size_t k, reachable = 0;
      for (k = 0; k < page.item_count; k++) seen[k] = 0;
      for (i = 0; i < count; i++)
        if (rows[i].kind == HG_API_ROW_SERIES && rows[i].item < page.item_count)
          seen[rows[i].item]++;
      for (k = 0; k < page.item_count; k++) reachable += (seen[k] == 1);
      ok("every card is reachable exactly once", reachable == page.item_count);
    }
  }

  /* Refusal, not truncation: a caller handed 4 rows of 40 could not tell that
   * from a page that has 4 rows. */
  {
    size_t small_count = 123;
    int src = hg_api_home_rows(&page, small, 4, &small_count);
    ok("too small a buffer is HG_API_ERR_SMALL, not a truncated list",
       src == HG_API_ERR_SMALL && small_count == 0);
  }

  /* A page with no sections is a description, not an error -- and the caller is
   * the one responsible for never handing zero rows to a RecyclerFrame. */
  {
    HgHomePage empty;
    size_t empty_count = 123;
    int erc;
    empty.item_count = 0;
    empty.section_count = 0;
    erc = hg_api_home_rows(&empty, rows, sizeof rows / sizeof rows[0], &empty_count);
    ok("an empty page yields 0 rows and no error", erc == 0 && empty_count == 0);
  }

  /* A section table that runs past the item array must be refused: these rows
   * would point at cards that do not exist. */
  {
    HgHomePage torn = page;
    size_t torn_count = 123;
    int trc;
    torn.sections[0].count = HG_API_MAX_HOME_ITEMS;   /* beyond item_count */
    trc = hg_api_home_rows(&torn, rows, sizeof rows / sizeof rows[0], &torn_count);
    ok("a section range past the end of the items is refused",
       trc == HG_API_ERR_JSON && torn_count == 0);
  }
}

int main(void) {
  printf("hg_fixture tests (i386 host build) -- fixtures captured from live upstream\n");

  test_home_fixture();
  test_dynamic_keys();
  test_episode_ids_fixture();
  test_play_fixture();
  test_play_fixture_v1();
  test_play_h264_fixture();
  test_null_count();
  test_device_blob();
  test_home_tiers();
  test_home_rows();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
