/*
 * Tests for the response parsers.
 *
 * The fixtures are shaped like the responses the app actually receives, not like
 * hand-made objects: the search hit nests its fields under video_data[], the
 * detail blob carries bare episode-id strings, and the video model mixes codecs.
 * A parser tested only against a flat { "id": ... } proves nothing about the
 * response that will break it.
 *
 * This file is ASCII only. Chinese text in the fixtures is written as \uXXXX
 * escapes INSIDE the JSON, which is also how the upstream sends it, and the
 * expected UTF-8 is asserted byte for byte. A length check alone passes for
 * mojibake.
 */

#include "hg_api.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-56s\n", what);
  else { failures++; printf("  FAIL  %-56s\n", what); }
}

/* ---- fixtures ------------------------------------------------------------ */

/* A search response with two hits. The first uses series_id / cover / video_desc
 * / episode_cnt; the second has NO series_id and NO video.title -- its id comes
 * from book_id and its title only from search_high_light.title.text. That second
 * row is the case the reference's fallback chain exists for. */
static const char *SEARCH_JSON =
  "{\"code\":0,\"data\":{\"video_data\":["
  "{"
    "\"video_data\":{"
      "\"series_id\":\"7300149283017403913\","
      "\"title\":\"\\u77ed\\u5267\\u4e4b\\u738b\","
      "\"cover\":\"https:\\/\\/img.example.com\\/cover\\/1.jpg\","
      "\"video_desc\":\"\\u4e00\\u4e2a\\u6d4b\\u8bd5\\u63cf\\u8ff0\","
      "\"episode_cnt\":\"86\","
      "\"rec_text\":\"\\u70ed\\u64ad\","
      "\"sub_title\":\"\\u90fd\\u5e02\""
    "},"
    "\"cell_name\":\"ignored when a title exists\""
  "},"
  "{"
    "\"book_id\":\"7100223344556677889\","
    "\"search_result_id\":\"9999\","
    "\"search_high_light\":{\"title\":{\"text\":\"<em>\\u91cd\\u751f</em>\\u4e4b\\u8def\"}},"
    "\"video_data\":{"
      "\"title\":\"\","
      "\"cover_url\":\"https://img.example.com/cover/2.jpg\","
      "\"episode_cnt\":71,"
      "\"rec_text\":\"\\u5b8c\\u7ed3\""
    "}"
  "}"
  "],\"has_more\":true}}";

/* The detail blob as hg_router_extract() returns it: loaderData first. */
static const char *DETAIL_JSON =
  "{\"loaderData\":{\"detail_page\":{\"name\":\"page level name\"},"
  "\"seriesDetail\":{"
    "\"series_id\":\"7300149283017403913\","
    "\"series_name\":\"\\u77ed\\u5267\\u4e4b\\u738b\","
    "\"series_cover\":\"https:\\/\\/img.example.com\\/cover\\/1.jpg\","
    "\"series_intro\":\"\\u4e00\\u4e2a\\u6d4b\\u8bd5\\u63cf\\u8ff0\","
    "\"vid_list\":[\"1001\",\"1002\",\"1003\"]"
  "}}}";

static const char *DETAIL_BAD_JSON =
  "{\"loaderData\":{\"seriesDetail\":{"
    "\"series_name\":\"x\",\"vid_list\":[\"1001\",\"10a3\",\"1003\"]}}}";

static const char *DETAIL_DUP_JSON =
  "{\"loaderData\":{\"seriesDetail\":{"
    "\"series_name\":\"x\",\"vid_list\":[\"1001\",\"1002\",\"1001\"]}}}";

static const char *DETAIL_EMPTY_JSON =
  "{\"loaderData\":{\"seriesDetail\":{\"series_name\":\"x\",\"vid_list\":[]}}}";

static const char *DETAIL_OBJECT_ENTRY_JSON =
  "{\"loaderData\":{\"seriesDetail\":{"
    "\"series_name\":\"x\",\"vid_list\":[{\"vid\":\"1001\"}]}}}";

/* A video model response, in the MEASURED shape: codec_type lives at
 * video_meta.codec_type and spade_a at encrypt_info.spade_a -- neither is on the
 * entry. The second entry is the incompatible codec; the third has no codec_type
 * at all and must survive, as legacy resources do.
 *
 * THE QUALITY FIELDS ARE READ FROM THE SAME TWO PLACES the real response puts them in, and
 * they are here because they were being discarded: `quality_type` is a NUMBER on the ENTRY
 * (measured against app/src/tests/fixtures/video_model.json: 40/30/20/10/1 for
 * 360p/480p/540p/720p/1080p, 0 never sent), while definition/vwidth/vheight/bitrate/size are
 * inside video_meta. Note the ordering trap this fixture also encodes: the FIRST entry is the
 * best (quality_type 1) and the LAST is the worst (quality_type 40), which is the reverse of
 * what a numeric sort would guess.
 *
 * hg_api_parse_video_model takes the INNER document (the parsed result of
 * data[vid].video_model); the doubly-encoded envelope is tested in
 * hg_fixture_test.c against the real response. */
static const char *MODEL_JSON =
  "{\"video_list\":["
  "{"
    "\"main_url\":\"https:\\/\\/v.example.com\\/enc\\/1080.mp4\","
    "\"key_seed\":\"c2VlZC1ieXRlcw==\","
    "\"quality_type\":1,"
    "\"video_meta\":{\"codec_type\":\"h264\",\"definition\":\"1080p\","
                    "\"vwidth\":1920,\"vheight\":1080,\"bitrate\":2500000,\"size\":4096000},"
    "\"encrypt_info\":{\"encrypt\":true,\"spade_a\":\"AAAABBBBCCCC\"},"
    "\"quality_desc\":\"1080P\""
  "},"
  "{"
    "\"main_url\":\"https://v.example.com/enc/1080.bvc2.mp4\","
    "\"key_seed\":\"c2VlZC1ieXRlcw==\","
    "\"quality_type\":2,"
    "\"video_meta\":{\"codec_type\":\"bytevc2\"},"
    "\"encrypt_info\":{\"encrypt\":true,\"spade_a\":\"AAAABBBBCCCC\"},"
    "\"quality_desc\":\"1080P\""
  "},"
  "{"
    "\"main_url\":\"https://v.example.com/enc/720.mp4\","
    "\"key_seed\":\"\","
    "\"quality_type\":10,"
    "\"video_meta\":{\"definition\":\"720p\",\"vwidth\":1280,\"vheight\":720,"
                    "\"bitrate\":1200000},"
    "\"encrypt_info\":{\"encrypt\":false,\"spade_a\":\"\"},"
    "\"quality_desc\":\"720P\""
  "},"
  "{"
    "\"main_url\":\"https://v.example.com/enc/360.mp4\","
    "\"key_seed\":\"\","
    "\"quality_type\":40,"
    "\"video_meta\":{\"codec_type\":\"h264\",\"definition\":\"360p\","
                    "\"vwidth\":640,\"vheight\":360},"
    "\"encrypt_info\":{\"encrypt\":false,\"spade_a\":\"\"},"
    "\"quality_desc\":\"360P\""
  "}"
  "]}";

/* A model whose entries carry NO quality metadata at all: the pre-ladder shape. Every new
 * field must come back zero/empty and the sources must still parse, because "upstream stopped
 * sending the ladder" must degrade to "no quality menu", not to "no playback". */
static const char *MODEL_NO_QUALITY_JSON =
  "{\"video_list\":["
  "{\"main_url\":\"https://a/1\",\"video_meta\":{\"codec_type\":\"h264\"}},"
  "{\"main_url\":\"https://a/2\",\"video_meta\":{\"codec_type\":\"h264\"}}"
  "]}";

/* An entry whose codec_type is at the ENTRY level, as the port originally read
 * it. This must NOT be filtered: reading the codec from the correct nested place
 * means an entry-level codec_type is simply absent, and absent is not
 * incompatible. Pinning that here is what stops a future "helpful" change from
 * reintroducing a top-level read. */
static const char *MODEL_TOPLEVEL_CODEC_JSON =
  "{\"video_list\":[{\"main_url\":\"https://a/1\",\"codec_type\":\"bytevc2\"},"
  "{\"main_url\":\"https://a/2\",\"video_meta\":{\"codec_type\":\"h264\"}}]}";

static const char *MODEL_BVC2_UPPER_JSON =
  "{\"video_list\":[{\"main_url\":\"https://a/1\",\"video_meta\":{\"codec_type\":\"BVC2\"}},"
  "{\"main_url\":\"https://a/2\",\"video_meta\":{\"codec_type\":\"H265\"}}]}";

static const char *MODEL_ALL_BAD_JSON =
  "{\"video_list\":[{\"main_url\":\"https://a/1\",\"video_meta\":{\"codec_type\":\"bytevc2\"}},"
  "{\"main_url\":\"https://a/2\",\"video_meta\":{\"codec_type\":\"bvc2\"}}]}";

/* ---- search -------------------------------------------------------------- */

static void test_search(void) {
  static HgSeriesPage page;
  size_t count = 123;
  int more = -1;
  int rc;

  printf("\nsearch\n");

  rc = hg_api_parse_search(SEARCH_JSON, strlen(SEARCH_JSON), page.items,
                           HG_API_MAX_SERIES, &count, &more);
  ok("two hits parsed", rc == 2 && count == 2);
  if (rc != 2) { printf("        rc=%d count=%u\n", rc, (unsigned)count); return; }
  ok("has_more reported", more == 1);

  /* Hit 0: every field present, cover unescaped from \/ to /. */
  ok("hit0 id", strcmp(page.items[0].id, "7300149283017403913") == 0);
  /* U+77ED U+5267 U+4E4B U+738B = 12 UTF-8 bytes */
  ok("hit0 title exact UTF-8",
     memcmp(page.items[0].title,
            "\xE7\x9F\xAD\xE5\x89\xA7\xE4\xB9\x8B\xE7\x8E\x8B", 12) == 0 &&
     page.items[0].title[12] == '\0');
  if (memcmp(page.items[0].title,
             "\xE7\x9F\xAD\xE5\x89\xA7\xE4\xB9\x8B\xE7\x8E\x8B", 12) != 0)
    printf("        got title bytes: %s\n", page.items[0].title);
  ok("hit0 cover \\/ collapsed to /",
     strcmp(page.items[0].cover, "https://img.example.com/cover/1.jpg") == 0);
  ok("hit0 cover is not escaped",
     strstr(page.items[0].cover, "\\") == NULL);
  /* U+4E00 U+4E2A U+6D4B U+8BD5 U+63CF U+8FF0 = 18 bytes */
  ok("hit0 desc exact UTF-8",
     memcmp(page.items[0].desc,
            "\xE4\xB8\x80\xE4\xB8\xAA\xE6\xB5\x8B\xE8\xAF\x95\xE6\x8F\x8F\xE8\xBF\xB0",
            18) == 0 && page.items[0].desc[18] == '\0');
  /* badge comes from episode_cnt, and the quoted "86" must still parse as an
   * integer. The expected bytes are the Chinese badge "%d"-in-U+5168/U+96C6:
   * 3 + 1 + 2 + 1 + 3 = 10 bytes. */
  ok("hit0 badge is count-derived",
     memcmp(page.items[0].badge,
            "\xE5\x85\xA8 86 \xE9\x9B\x86", 10) == 0 &&
     page.items[0].badge[10] == '\0');
  if (memcmp(page.items[0].badge,
             "\xE5\x85\xA8 86 \xE9\x9B\x86", 10) != 0)
    printf("        got badge bytes: %s\n", page.items[0].badge);

  /* Hit 1: the fallback row. */
  ok("hit1 id from book_id", strcmp(page.items[1].id, "7100223344556677889") == 0);
  /* search_high_light.title.text wins over the empty video.title. The highlight
   * carries markup; the parser must NOT strip it (the reference does not either),
   * but it must take the text node verbatim and decode its escapes. */
  ok("hit1 title via highlight fallback",
     strncmp(page.items[1].title, "<em>", 4) == 0 &&
     memcmp(page.items[1].title + 4,
            "\xE9\x87\x8D\xE7\x94\x9F", 6) == 0 &&
     strcmp(page.items[1].title + 10, "</em>\xE4\xB9\x8B\xE8\xB7\xAF") == 0);
  if (strncmp(page.items[1].title, "<em>", 4) != 0)
    printf("        got hit1 title: %.40s\n", page.items[1].title);
  ok("hit1 cover from cover_url",
     strcmp(page.items[1].cover, "https://img.example.com/cover/2.jpg") == 0);
  /* rec_text passes through when episode_cnt is 0/absent... here cnt IS 71, so the
   * badge is count-derived even though rec_text is present. */
  ok("hit1 badge prefers count over rec_text",
     memcmp(page.items[1].badge, "\xE5\x85\xA8 71 \xE9\x9B\x86", 10) == 0);

  /* search_result_id is the LAST fallback and must not shadow book_id */
  {
    static const char *ONLY_SRID =
      "{\"data\":{\"video_data\":[{\"search_result_id\":\"4242\","
      "\"video_data\":{\"title\":\"t\"}}]}}";
    ok("id falls through to search_result_id",
       hg_api_parse_search(ONLY_SRID, strlen(ONLY_SRID), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == 1 &&
       strcmp(page.items[0].id, "4242") == 0);
  }

  /* rec_text is the badge when there is no episode count */
  {
    static const char *NO_CNT =
      "{\"data\":{\"video_data\":[{\"video_data\":{\"series_id\":\"1\","
      "\"title\":\"t\",\"rec_text\":\"\\u70ed\\u64ad\"}}]}}";
    ok("rec_text used when episode_cnt is absent",
       hg_api_parse_search(NO_CNT, strlen(NO_CNT), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == 1 &&
       memcmp(page.items[0].badge, "\xE7\x83\xAD\xE6\x92\xAD", 6) == 0);
  }

  /* an id that is not [0-9]{1,30} is refused outright, not skipped */
  {
    static const char *BAD_ID =
      "{\"data\":{\"video_data\":[{\"video_data\":{\"series_id\":\"7x01\","
      "\"title\":\"t\"}}]}}";
    ok("a non-numeric series id is refused",
       hg_api_parse_search(BAD_ID, strlen(BAD_ID), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == HG_API_ERR_EPISODE &&
       count == 0);
  }

  /* a row with no usable title is refused */
  {
    static const char *NO_TITLE =
      "{\"data\":{\"video_data\":[{\"video_data\":{\"series_id\":\"1\"}}]}}";
    ok("a title-less hit is refused",
       hg_api_parse_search(NO_TITLE, strlen(NO_TITLE), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == HG_API_ERR_MISSING);
  }

  /* a JSON null cover must NOT become the literal string "null" */
  {
    static const char *NULL_COVER =
      "{\"data\":{\"video_data\":[{\"video_data\":{\"series_id\":\"1\","
      "\"title\":\"t\",\"cover\":null}}]}}";
    ok("null cover stays empty",
       hg_api_parse_search(NULL_COVER, strlen(NULL_COVER), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == 1 &&
       page.items[0].cover[0] == '\0');
  }
}

/* ---- detail -------------------------------------------------------------- */

static void test_detail(void) {
  static HgEpisode eps[HG_API_MAX_EPISODES];
  HgSeries series;
  size_t count = 123;
  int rc;

  printf("\ndetail\n");

  rc = hg_api_parse_detail(DETAIL_JSON, strlen(DETAIL_JSON),
                           "7300149283017403913", &series, eps,
                           HG_API_MAX_EPISODES, &count);
  ok("three episodes parsed", rc == 3 && count == 3);
  if (rc == 3) {
    ok("ep0", strcmp(eps[0].id, "1001") == 0);
    ok("ep1", strcmp(eps[1].id, "1002") == 0);
    ok("ep2", strcmp(eps[2].id, "1003") == 0);
  }
  ok("series id is the caller's", strcmp(series.id, "7300149283017403913") == 0);
  ok("series_name exact UTF-8",
     memcmp(series.title,
            "\xE7\x9F\xAD\xE5\x89\xA7\xE4\xB9\x8B\xE7\x8E\x8B", 12) == 0);
  ok("series_cover unescaped",
     strcmp(series.cover, "https://img.example.com/cover/1.jpg") == 0);
  ok("series_intro exact UTF-8",
     memcmp(series.desc,
            "\xE4\xB8\x80\xE4\xB8\xAA\xE6\xB5\x8B\xE8\xAF\x95\xE6\x8F\x8F\xE8\xBF\xB0",
            18) == 0);
  ok("badge reflects the real episode count",
     memcmp(series.badge, "\xE5\x85\xA8 3 \xE9\x9B\x86", 9) == 0);

  /* ONE bad entry invalidates the WHOLE list -- it must not be skipped. */
  rc = hg_api_parse_detail(DETAIL_BAD_JSON, strlen(DETAIL_BAD_JSON), "1", NULL,
                           eps, HG_API_MAX_EPISODES, &count);
  ok("an invalid episode entry refuses the list",
     rc == HG_API_ERR_EPISODE && count == 0);

  rc = hg_api_parse_detail(DETAIL_DUP_JSON, strlen(DETAIL_DUP_JSON), "1", NULL,
                           eps, HG_API_MAX_EPISODES, &count);
  ok("a duplicate episode id refuses the list",
     rc == HG_API_ERR_DUPLICATE && count == 0);

  rc = hg_api_parse_detail(DETAIL_EMPTY_JSON, strlen(DETAIL_EMPTY_JSON), "1", NULL,
                           eps, HG_API_MAX_EPISODES, &count);
  ok("an empty vid_list is refused",
     rc == HG_API_ERR_EMPTY && count == 0);

  rc = hg_api_parse_detail(DETAIL_OBJECT_ENTRY_JSON,
                           strlen(DETAIL_OBJECT_ENTRY_JSON), "1", NULL,
                           eps, HG_API_MAX_EPISODES, &count);
  ok("an object entry (wrong shape) is refused",
     rc == HG_API_ERR_EPISODE);

  /* a missing loaderData is a format fault, not an empty series */
  {
    static const char *NO_LOADER = "{\"other\":1}";
    ok("missing loaderData is refused",
       hg_api_parse_detail(NO_LOADER, strlen(NO_LOADER), "1", NULL, eps,
                           HG_API_MAX_EPISODES, &count) == HG_API_ERR_JSON);
  }
  /* seriesDetail present but no vid_list */
  {
    static const char *NO_VID =
      "{\"loaderData\":{\"seriesDetail\":{\"series_name\":\"x\"}}}";
    ok("missing vid_list is refused",
       hg_api_parse_detail(NO_VID, strlen(NO_VID), "1", NULL, eps,
                           HG_API_MAX_EPISODES, &count) == HG_API_ERR_MISSING);
  }
  /* a 31-digit episode id is outside [0-9]{1,30} */
  {
    static const char *LONG_ID =
      "{\"loaderData\":{\"seriesDetail\":{\"vid_list\":"
      "[\"1234567890123456789012345678901\"]}}}";
    ok("a 31-digit episode id is refused",
       hg_api_parse_detail(LONG_ID, strlen(LONG_ID), "1", NULL, eps,
                           HG_API_MAX_EPISODES, &count) == HG_API_ERR_EPISODE);
  }
  /* a 30-digit id is inside the rule and must be accepted */
  {
    static const char *MAX_ID =
      "{\"loaderData\":{\"seriesDetail\":{\"vid_list\":"
      "[\"123456789012345678901234567890\"]}}}";
    ok("a 30-digit episode id is accepted",
       hg_api_parse_detail(MAX_ID, strlen(MAX_ID), "1", NULL, eps,
                           HG_API_MAX_EPISODES, &count) == 1);
  }
}

/* ---- video model --------------------------------------------------------- */

static void test_model(void) {
  static HgVideoList list;
  size_t count = 123;
  int rc;

  printf("\nvideo model\n");

  rc = hg_api_parse_video_model(MODEL_JSON, strlen(MODEL_JSON), list.items,
                                HG_API_MAX_SOURCES, &count);
  /* FOUR rows are offered and ONE (bytevc2) is filtered, so three remain. The count moved
   * from 2 to 3 when the 360p row was added to give the quality ladder a worst tier; the
   * check is stated as an absolute because "did the filter drop exactly the right rows" is
   * the question, and a relative count would not answer it. */
  ok("bytevc2 filtered, three remain", rc == 3 && count == 3);
  if (rc != 3) { printf("        rc=%d\n", rc); return; }
  ok("no bytevc2 entry survived",
     strstr(list.items[0].url, "bvc2") == NULL &&
     strstr(list.items[1].url, "bvc2") == NULL &&
     strstr(list.items[2].url, "bvc2") == NULL);
  ok("source0 url unescaped",
     strcmp(list.items[0].url, "https://v.example.com/enc/1080.mp4") == 0);
  ok("source0 key_seed", strcmp(list.items[0].key_seed, "c2VlZC1ieXRlcw==") == 0);
  ok("source0 spade_a", strcmp(list.items[0].spade_a, "AAAABBBBCCCC") == 0);
  ok("source0 codec_type", strcmp(list.items[0].codec_type, "h264") == 0);
  /* the entry with no codec_type is kept: absence is not incompatibility */
  ok("entry without codec_type is kept",
     strcmp(list.items[1].url, "https://v.example.com/enc/720.mp4") == 0 &&
     list.items[1].codec_type[0] == '\0');

  /* ---- the quality ladder's half ------------------------------------------
   *
   * Four entries were offered and one (bytevc2) was filtered out, so three survive: 1080p,
   * 720p and 360p -- in the ORDER UPSTREAM SENT THEM, which is best first here because the
   * fixture puts quality_type 1 first. The parser must NOT reorder anything; ordering is
   * hg_quality_build's job, and it sorts by pixel height precisely so it does not have to
   * trust this order. */
  ok("three sources survive the bytevc2 filter", count == 3);
  ok("source0 carries its quality_type from the ENTRY", list.items[0].has_quality_type == 1 &&
                                                             list.items[0].quality_type == 1);
  ok("source0 definition comes from video_meta",
     strcmp(list.items[0].definition, "1080p") == 0);
  ok("source0 pixel size", list.items[0].vwidth == 1920 && list.items[0].vheight == 1080);
  ok("source0 bitrate", list.items[0].bitrate == 2500000);
  ok("source0 size in bytes", list.items[0].size_bytes == 4096000);

  ok("source1 is the 720p row (the 1080p bytevc2 row is gone)",
     list.items[1].quality_type == 10 && strcmp(list.items[1].definition, "720p") == 0 &&
         list.items[1].vheight == 720);
  /* A row with NO codec_type still has quality metadata; the two are independent. */
  ok("a row without a codec still reports its quality",
     list.items[1].codec_type[0] == '\0' && list.items[1].has_quality_type == 1);

  /* The last row omits bitrate and size: absent must stay 0, not become garbage. */
  ok("source2 is the 360p row",
     list.items[2].quality_type == 40 && list.items[2].vheight == 360);
  ok("an absent bitrate is 0", list.items[2].bitrate == 0);
  ok("an absent size is 0", list.items[2].size_bytes == 0);

  /* THE DEGRADATION PATH. A response with no quality metadata at all must still parse into
   * usable sources -- "upstream stopped sending the ladder" means "no quality menu", not "no
   * playback". */
  rc = hg_api_parse_video_model(MODEL_NO_QUALITY_JSON, strlen(MODEL_NO_QUALITY_JSON),
                                list.items, HG_API_MAX_SOURCES, &count);
  ok("a model with no quality metadata still parses", rc == 2 && count == 2);
  ok("and every new field is zero or empty",
     list.items[0].has_quality_type == 0 && list.items[0].quality_type == 0 &&
         list.items[0].definition[0] == '\0' && list.items[0].vwidth == 0 &&
         list.items[0].vheight == 0 && list.items[0].bitrate == 0 &&
         list.items[0].size_bytes == 0);

  /* the match is case-insensitive, and a merely similar label is not filtered */
  rc = hg_api_parse_video_model(MODEL_BVC2_UPPER_JSON,
                                strlen(MODEL_BVC2_UPPER_JSON), list.items,
                                HG_API_MAX_SOURCES, &count);
  ok("BVC2 uppercase filtered, H265 kept",
     rc == 1 && strcmp(list.items[0].codec_type, "H265") == 0);

  /* THE REGRESSION GUARD. codec_type is read from video_meta, so a bytevc2 label
   * sitting at the ENTRY level is invisible -- and invisible means "absent",
   * which is not incompatible. The row is therefore kept, which is the correct
   * consequence of reading the right path; this test exists so that anyone who
   * "fixes" it by also reading the entry level has to delete a passing test
   * rather than silently undo the fix. */
  rc = hg_api_parse_video_model(MODEL_TOPLEVEL_CODEC_JSON,
                                strlen(MODEL_TOPLEVEL_CODEC_JSON), list.items,
                                HG_API_MAX_SOURCES, &count);
  ok("an entry-level codec_type is NOT read (both rows kept)",
     rc == 2 && list.items[0].codec_type[0] == '\0' &&
     strcmp(list.items[1].codec_type, "h264") == 0);

  /* everything filtered out is an error, not an empty success */
  rc = hg_api_parse_video_model(MODEL_ALL_BAD_JSON, strlen(MODEL_ALL_BAD_JSON),
                                list.items, HG_API_MAX_SOURCES, &count);
  ok("all-incompatible is refused as empty",
     rc == HG_API_ERR_EMPTY && count == 0);

  /* a missing top-level key */
  {
    static const char *NO_KEY = "{\"other\":1}";
    ok("missing video_list is refused",
       hg_api_parse_video_model(NO_KEY, strlen(NO_KEY), list.items,
                                HG_API_MAX_SOURCES, &count) == HG_API_ERR_MISSING &&
       count == 0);
  }
  {
    static const char *EMPTY = "{\"video_list\":[]}";
    ok("an empty video_list is refused",
       hg_api_parse_video_model(EMPTY, strlen(EMPTY), list.items,
                                HG_API_MAX_SOURCES, &count) == HG_API_ERR_EMPTY);
  }
  {
    static const char *NOT_ARRAY = "{\"video_list\":{}}";
    ok("video_list that is not an array is refused",
       hg_api_parse_video_model(NOT_ARRAY, strlen(NOT_ARRAY), list.items,
                                HG_API_MAX_SOURCES, &count) == HG_API_ERR_JSON);
  }
  /* a row with an empty main_url is skipped, not fatal */
  {
    static const char *EMPTY_URL =
      "{\"video_list\":[{\"main_url\":\"\",\"codec_type\":\"h264\"},"
      "{\"main_url\":\"https://a/ok\",\"codec_type\":\"h264\"}]}";
    ok("a row with an empty main_url is skipped",
       hg_api_parse_video_model(EMPTY_URL, strlen(EMPTY_URL), list.items,
                                HG_API_MAX_SOURCES, &count) == 1 &&
       strcmp(list.items[0].url, "https://a/ok") == 0);
  }
  /* a url carrying \\u002F is unescaped before it reaches the player */
  {
    static const char *ESC_URL =
      "{\"video_list\":[{\"main_url\":\"https:\\u002F\\u002Fa\\u002Fb?x=1\\u0026y=2\","
      "\"codec_type\":\"h264\"}]}";
    ok("\\u002F and \\u0026 in a url are decoded",
       hg_api_parse_video_model(ESC_URL, strlen(ESC_URL), list.items,
                                HG_API_MAX_SOURCES, &count) == 1 &&
       strcmp(list.items[0].url, "https://a/b?x=1&y=2") == 0);
  }
}

/* ---- capacity ------------------------------------------------------------ */

static void test_capacity(void) {
  static HgSeriesPage page;
  static HgEpisode eps[HG_API_MAX_EPISODES];
  static HgVideoSource src[HG_API_MAX_SOURCES];
  size_t count;

  printf("\ncapacity\n");

  count = 999;
  ok("too small for search hits",
     hg_api_parse_search(SEARCH_JSON, strlen(SEARCH_JSON), page.items, 1,
                         &count, NULL) == HG_API_ERR_SMALL);
  ok("count is 0 after a capacity refusal", count == 0);
  /* the caller cannot be told "1 of 2": that is indistinguishable from a page
   * that genuinely holds one hit */
  ok("no partial list is reported",
     hg_api_parse_search(SEARCH_JSON, strlen(SEARCH_JSON), page.items, 1,
                         &count, NULL) < 0 && count == 0);

  count = 999;
  ok("exactly-enough capacity succeeds",
     hg_api_parse_search(SEARCH_JSON, strlen(SEARCH_JSON), page.items, 2,
                         &count, NULL) == 2 && count == 2);

  count = 999;
  ok("too small for episodes",
     hg_api_parse_detail(DETAIL_JSON, strlen(DETAIL_JSON), "1", NULL, eps, 2,
                         &count) == HG_API_ERR_SMALL && count == 0);
  count = 999;
  ok("exactly-enough episode capacity succeeds",
     hg_api_parse_detail(DETAIL_JSON, strlen(DETAIL_JSON), "1", NULL, eps, 3,
                         &count) == 3);

  count = 999;
  ok("too small for sources",
     hg_api_parse_video_model(MODEL_JSON, strlen(MODEL_JSON), src, 1, &count)
       == HG_API_ERR_SMALL && count == 0);
  count = 999;
  ok("exactly-enough source capacity succeeds",
     hg_api_parse_video_model(MODEL_JSON, strlen(MODEL_JSON), src, 3, &count) == 3);

  /* a zero-capacity array is a refusal, never a silent success */
  count = 999;
  ok("zero capacity is refused",
     hg_api_parse_search(SEARCH_JSON, strlen(SEARCH_JSON), page.items, 0,
                         &count, NULL) == HG_API_ERR_SMALL && count == 0);
}

/* ---- structural refusals ------------------------------------------------- */

static void test_shapes(void) {
  static HgSeriesPage page;
  size_t count = 999;

  printf("\nrefuse, do not guess\n");

  /* valid JSON, wrong shape: this must be an error, because reporting "no
   * results" for a changed upstream shape is the failure that hides a break. */
  {
    static const char *NO_DATA = "{\"code\":0,\"message\":\"ok\"}";
    ok("search without data is refused",
       hg_api_parse_search(NO_DATA, strlen(NO_DATA), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == HG_API_ERR_JSON);
    ok("count is 0 after a shape refusal", count == 0);
  }
  {
    static const char *NO_ARR = "{\"data\":{\"search_tabs\":[]}}";
    ok("data without video_data is refused",
       hg_api_parse_search(NO_ARR, strlen(NO_ARR), page.items,
                           HG_API_MAX_SERIES, &count, NULL) == HG_API_ERR_MISSING);
  }
  {
    static const char *EMPTY_ARR = "{\"data\":{\"video_data\":[]}}";
    int more = 1;
    ok("an empty video_data is zero hits, not an error",
       hg_api_parse_search(EMPTY_ARR, strlen(EMPTY_ARR), page.items,
                           HG_API_MAX_SERIES, &count, &more) == 0 &&
       count == 0 && more == 0);
  }
  {
    static const char *TRUNC = "{\"data\":{\"video_data\":[{\"video_data\":";
    ok("a truncated response is refused",
       hg_api_parse_search(TRUNC, strlen(TRUNC), page.items,
                           HG_API_MAX_SERIES, &count, NULL) < 0);
  }
  {
    static const char *ARR_NOT_OBJ = "{\"data\":{\"video_data\":[1,2]}}";
    ok("numeric hits are refused",
       hg_api_parse_search(ARR_NOT_OBJ, strlen(ARR_NOT_OBJ), page.items,
                           HG_API_MAX_SERIES, &count, NULL) != 0);
  }
  ok("an empty document is refused",
     hg_api_parse_search("", 0, page.items, HG_API_MAX_SERIES, &count, NULL) < 0);
}

/* ---- cleanUrl ------------------------------------------------------------ */

static void test_clean_url(void) {
  char a[] = "https:\\/\\/a.example.com\\/x.jpg";
  /* A URL needs TWO escaped slashes to produce "//" -- "https:\u002F..." would
   * correctly yield "https:/...". Getting this wrong the other way round is how
   * a cleaner test passes while the real one silently loses a slash. */
  char b[] = "https:\\u002F\\u002Fa\\u002Fb?x=1\\u0026y=2";
  char c[] = "https:\\u002f\\u002fLOWER\\u002Fcase";
  char d[] = "a&amp;b&amp;c";
  char e[] = "no escapes here";
  char f[] = "";

  printf("\ncleanUrl\n");

  hg_api_clean_url(a);
  ok("\\/ becomes /", strcmp(a, "https://a.example.com/x.jpg") == 0);
  hg_api_clean_url(b);
  ok("\\u002F and \\u0026 decoded", strcmp(b, "https://a/b?x=1&y=2") == 0);
  hg_api_clean_url(c);
  ok("\\u002f lowercase decoded", strcmp(c, "https://LOWER/case") == 0);
  hg_api_clean_url(d);
  ok("&amp; becomes &", strcmp(d, "a&b&c") == 0);
  hg_api_clean_url(e);
  ok("a string with no escapes is unchanged", strcmp(e, "no escapes here") == 0);
  hg_api_clean_url(f);
  ok("an empty string stays empty", f[0] == '\0');
  /* a lone backslash or a malformed escape must not eat the following bytes */
  {
    char g[] = "a\\uZZb\\";
    hg_api_clean_url(g);
    ok("a malformed escape is left alone", strcmp(g, "a\\uZZb\\") == 0);
  }
  hg_api_clean_url(NULL);
  ok("NULL is tolerated", 1);
}

/* ---- validators ---------------------------------------------------------- */

static void test_validators(void) {
  printf("\nvalidators\n");
  ok("digits accepted", hg_api_valid_id("7300149283017403913") == 1);
  ok("single digit accepted", hg_api_valid_id("7") == 1);
  ok("30 digits accepted", hg_api_valid_id("123456789012345678901234567890") == 1);
  ok("31 digits rejected", hg_api_valid_id("1234567890123456789012345678901") == 0);
  ok("empty rejected", hg_api_valid_id("") == 0);
  ok("letters rejected", hg_api_valid_id("12a") == 0);
  ok("leading zero accepted", hg_api_valid_id("007") == 1);
  ok("negative rejected", hg_api_valid_id("-1") == 0);
  ok("space rejected", hg_api_valid_id("1 2") == 0);
  ok("NULL rejected", hg_api_valid_id(NULL) == 0);
  ok("episode validator agrees", hg_api_valid_episode_id("1001") == 1);
}

/* ---- the search PAGE (server-rendered HTML, not the dead signed API) -------
 *
 * The fixture below is the shape MEASURED against the live page this round (see the probe in
 * app/src/tools/probe_search_page.mjs), trimmed to two items and shortened where the value does not
 * matter. What matters, and what each check is about:
 *
 *   - the card fields sit ONE LEVEL DOWN, inside `video_data`. The id is `keyword` on the item and
 *     `series_id` inside the card; the title is `name` on the item and `series_title` inside. A
 *     parser that looks at the item level finds neither -- which is exactly the "the search page has
 *     no content" report, when the page carried 108 results.
 *   - `totalCount` is a STRING ("201" for "ai"), so it has to be read as text and converted.
 *   - an item with no usable id is skipped, not fatal: an ad or a placeholder row must not empty the
 *     page.
 */
static const char *SEARCH_PAGE_JSON =
    "{\"loaderData\":{\"search_layout\":null,\"search_(keyword)/page\":{"
    "\"req\":{\"isSSRPage\":true},"
    "\"query\":\"ai\","
    "\"isSuccess\":true,"
    "\"totalCount\":\"201\","
    "\"accessibleEpisodeCnt\":3,"
    "\"searchList\":["
    "{\"doc_type\":23,\"name\":\"\\u8bf1\\u60c5\\u67d3\\u6307\",\"keyword\":\"7688736152076962840\","
    "\"video_data\":{\"series_id\":\"7688736152076962840\",\"episode_cnt\":66,"
    "\"series_title\":\"\\u8bf1\\u60c5\\u67d3\\u6307\","
    "\"series_cover\":\"https:\\/\\/p6-novel.byteimg.com\\/novel-pic\\/8644e220.jpg\","
    "\"episode_right_text\":\"\\u5168 66 \\u96c6\","
    "\"category_list\":[{\"name\":\"\\u603b\\u699c\"},{\"name\":\"\\u70ed\\u64ad\"}]},"
    "\"user_info\":{},\"video_list\":[]},"
    "{\"doc_type\":23,\"name\":\"\\u9519\\u628a\\u95ea\\u5a5a\",\"keyword\":\"7665195424583519256\","
    "\"video_data\":{\"series_id\":\"7665195424583519256\",\"episode_cnt\":88,"
    "\"series_title\":\"\\u9519\\u628a\\u95ea\\u5a5a\","
    "\"series_cover\":\"https:\\/\\/p3-novel.byteimg.com\\/novel-pic\\/ca789e.jpg\"},"
    "\"user_info\":{},\"video_list\":[]},"
    "{\"doc_type\":99,\"name\":\"an ad slot with no video_data\"}"
    "]}}}";

static void test_search_page(void) {
  static HgSeries out[16];
  size_t count = 999;
  long long total = -1;
  int rc;

  printf("\nsearch page (server-rendered)\n");

  rc = hg_api_parse_search_page(SEARCH_PAGE_JSON, strlen(SEARCH_PAGE_JSON), out, 16, &count, &total);
  ok("two usable hits parsed from searchList", rc == 2 && count == 2);
  if (rc != 2) {
    printf("        rc=%d count=%u\n", rc, (unsigned)count);
    return;
  }

  /* THE NESTED unwrap: the id comes from video_data.series_id even though the item's own id field is
   * spelled `keyword`, and the title from video_data.series_title even though the item spells it
   * `name`. */
  ok("hit0 id from the nested card", strcmp(out[0].id, "7688736152076962840") == 0);
  ok("hit0 title from the nested card", strcmp(out[0].title, "\xE8\xAF\xB1\xE6\x83\x85\xE6\x9F\x93\xE6\x8C\x87") == 0);
  ok("hit0 cover is the nested series_cover",
     strcmp(out[0].cover, "https://p6-novel.byteimg.com/novel-pic/8644e220.jpg") == 0);
  ok("hit0 cover is unescaped", strstr(out[0].cover, "\\") == NULL);
  ok("hit0 badge comes from the nested episode_right_text",
     strstr(out[0].badge, "66") != NULL);
  ok("hit0 tags come from the nested category_list",
     strstr(out[0].tags, "\xE6\x80\xBB\xE6\xA6\x9C") != NULL);

  ok("hit1 id", strcmp(out[1].id, "7665195424583519256") == 0);
  ok("hit1 title", strcmp(out[1].title, "\xE9\x94\x99\xE6\x8A\x8A\xE9\x97\xAA\xE5\xA9\x9A") == 0);

  ok("totalCount was read as a string and converted", total == 201);

  /* An empty list is a search that matched NOTHING, which is not an error the user should see as a
   * failure -- but it is not "success with 0 items" either, so it reports EMPTY. */
  {
    static const char *EMPTY_LIST =
        "{\"loaderData\":{\"search_(keyword)/page\":{\"totalCount\":\"0\",\"searchList\":[]}}}";
    static HgSeries none[4];
    size_t c = 77;
    long long t = -1;
    int r = hg_api_parse_search_page(EMPTY_LIST, strlen(EMPTY_LIST), none, 4, &c, &t);
    ok("an empty searchList reports EMPTY, not a crash", r == HG_API_ERR_EMPTY && c == 0);
    ok("and the total is still reported (0)", t == 0);
  }

  /* A page with no searchList AND no video_data array: MISSING, so the caller can say "the page
   * changed" rather than "no results". */
  {
    static const char *NO_LIST = "{\"loaderData\":{\"search_layout\":{\"a\":1}}}";
    static HgSeries none[4];
    size_t c = 5;
    int r = hg_api_parse_search_page(NO_LIST, strlen(NO_LIST), none, 4, &c, NULL);
    ok("a page with no result list is refused", r < 0 && c == 0);
  }

  /* NULL out-parameters are tolerated, because the NULL path is the one the console found. */
  ok("count and total may be NULL",
     hg_api_parse_search_page(SEARCH_PAGE_JSON, strlen(SEARCH_PAGE_JSON), out, 16, NULL, NULL) == 2);
  ok("a NULL json is refused",
     hg_api_parse_search_page(NULL, 0, out, 16, &count, &total) == HG_API_ERR_JSON);
  ok("a zero cap is refused",
     hg_api_parse_search_page(SEARCH_PAGE_JSON, strlen(SEARCH_PAGE_JSON), out, 0, &count, &total) ==
         HG_API_ERR_SMALL);
}

/* ---- pagination: how a category page says how far it goes ------------------
 *
 * The fixture is the MEASURED block (probe_category_paging.mjs):
 *   loaderData.category_$.pagination = { total: 800, pageNum: 1, pageSize: 24, totalPages: 34 }
 *
 * The checks that matter, and why:
 *   - a page with no `pagination` returns 0 and leaves the out-struct ZEROED, so a caller can treat
 *     "no block" as "no paging" instead of reading garbage;
 *   - the numbers are plain JSON numbers HERE while the search page's totalCount is a STRING, so both
 *     spellings are tried (a quoted number read as absent would silently drop the end-of-list guard);
 *   - each field stands alone: a page that states only `totalPages` still tells the caller when to stop.
 */
static const char *PAGED_JSON =
    "{\"loaderData\":{\"category_layout\":null,\"category_$\":{"
    "\"pagination\":{\"total\":800,\"pageNum\":1,\"pageSize\":24,\"totalPages\":34},"
    "\"recommendList\":[{\"series_id\":\"7686713195246930968\",\"series_name\":\"x\"}]}}}";

static void test_pagination(void) {
  HgApiPagination p;
  int rc;

  printf("\npagination\n");

  /* A deliberately dirty out-struct: every field must be written (or zeroed), never left as-is. */
  p.page = -7;
  p.page_size = -7;
  p.total = -7;
  p.total_pages = -7;
  rc = hg_api_parse_pagination(PAGED_JSON, strlen(PAGED_JSON), &p);
  ok("the pagination block is found", rc == 1);
  ok("total is read as a number", p.total == 800);
  ok("pageNum", p.page == 1);
  ok("pageSize", p.page_size == 24);
  ok("totalPages -- the number the screen stops at", p.total_pages == 34);

  /* No block: 0, and the caller's struct is zeroed rather than half-written. */
  {
    static const char *NO_PAG =
        "{\"loaderData\":{\"category_$\":{\"recommendList\":[{\"series_id\":\"1\",\"series_name\":\"x\"}]}}}";
    HgApiPagination q;
    q.total = 99;
    q.total_pages = 99;
    ok("a page with no pagination block reports 0", hg_api_parse_pagination(NO_PAG, strlen(NO_PAG), &q) == 0);
    ok("and the struct is zeroed, not left dirty", q.total == 0 && q.total_pages == 0);
  }

  /* A QUOTED number is accepted: the search page uses that spelling for its own total, so a page that
   * started doing the same must not silently lose its end-of-list guard. */
  {
    static const char *QUOTED =
        "{\"loaderData\":{\"category_$\":{\"pagination\":{\"totalPages\":\"34\"},"
        "\"recommendList\":[{\"series_id\":\"1\",\"series_name\":\"x\"}]}}}";
    HgApiPagination q;
    ok("a quoted totalPages is read too", hg_api_parse_pagination(QUOTED, strlen(QUOTED), &q) == 1 &&
                                             q.total_pages == 34);
    ok("and the fields that were absent stay 0", q.total == 0 && q.page == 0);
  }

  ok("NULL json is refused", hg_api_parse_pagination(NULL, 0, &p) == 0);
  ok("NULL out is refused", hg_api_parse_pagination(PAGED_JSON, strlen(PAGED_JSON), NULL) == 0);
}

/* ---- matching a title against a query --------------------------------------
 *
 * The three decisions this function makes are the three a naive `strstr` gets wrong, and each has a case
 * here: case, spaces from the keyboard, and the empty query.
 */
static void test_title_match(void) {
  printf("\ntitle matching\n");

  /* U+77ED U+5267 = 短剧 */
  ok("an exact Chinese title matches",
     hg_title_matches("\xE7\x9F\xAD\xE5\x89\xA7\xE4\xB9\x8B\xE7\x8E\x8B", "\xE7\x9F\xAD\xE5\x89\xA7") == 1);
  ok("a Chinese title that does not contain it does not match",
     hg_title_matches("\xE5\xBC\x82\xE4\xB8\x96", "\xE7\x9F\xAD\xE5\x89\xA7") == 0);
  ok("a match in the middle of the title counts",
     hg_title_matches("abc\xE7\x9F\xAD\xE5\x89\xA7xyz", "\xE7\x9F\xAD\xE5\x89\xA7") == 1);

  /* CASE, which matters because the catalogue has "AI", "BOSS", "CEO" inside otherwise Chinese titles. */
  ok("ASCII case is folded (query lowercase)", hg_title_matches("wrong AI romance", "ai") == 1);
  ok("ASCII case is folded (query uppercase)", hg_title_matches("ai romance", "AI") == 1);

  /* SPACES FROM THE KEYBOARD. */
  ok("a leading space is trimmed", hg_title_matches("short drama", " short") == 1);
  ok("a trailing space is trimmed", hg_title_matches("short drama", "short ") == 1);
  ok("a query of spaces alone matches nothing", hg_title_matches("short drama", "   ") == 0);

  /* THE EMPTY QUERY IS NOT "MATCH EVERYTHING": the caller filters with this. */
  ok("an empty query matches nothing", hg_title_matches("short drama", "") == 0);
  ok("NULL query matches nothing", hg_title_matches("short drama", NULL) == 0);
  ok("NULL title matches nothing", hg_title_matches(NULL, "a") == 0);
  ok("an empty title does not match a real query", hg_title_matches("", "a") == 0);

  /* A QUERY LONGER THAN THE TITLE must not read past the end. */
  ok("a query longer than the title is safe", hg_title_matches("ab", "abcdefghij") == 0);
  ok("a one-character title matches exactly", hg_title_matches("a", "a") == 1);
}

int main(void) {
  printf("hg_api tests (i386 host build)\n");
  printf("sizeof(size_t)=%d  HgSeries=%d  HgVideoSource=%d\n\n",
         (int)sizeof(size_t), (int)sizeof(HgSeries), (int)sizeof(HgVideoSource));

  test_search();
  test_detail();
  test_model();
  test_capacity();
  test_shapes();
  test_clean_url();
  test_validators();
  test_search_page();
  test_pagination();
  test_title_match();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
