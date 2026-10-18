/*
 * Tests for the _ROUTER_DATA extractor.
 *
 * The case that matters is the one ContentRepositoryTest pins: a brace INSIDE a
 * JSON string, next to an escaped quote. A naive brace counter stops at that
 * brace and hands back a truncated object, which then parses "successfully" into
 * a document missing most of its content -- a failure that looks like empty data
 * rather than an error.
 */

#include "hg_router.h"
/* Needed for hg_base64_encode, used to build the playback-plan fixture. Without
 * this TinyCC only warns and the link still resolves; devkitA64 would make it a
 * hard error. Third occurrence of this class in the project -- see the hg_ror64
 * and hg_cenc.h notes in docs/. */
#include "hg_crypto.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-52s\n", what);
  else { failures++; printf("  FAIL  %-52s\n", what); }
}

/* Exactly the input from ContentRepositoryTest.routerParserHandlesEscapedQuotes... */
static const char *SAMPLE =
  "<script>window._ROUTER_DATA = "
  "{\"loaderData\":{\"x\":\"a}\\\"b\",\"list\":[1,2]}};</script>";

/* The signed API and the scraped blob are both JSON, and the shapes that matter
 * are arrays of objects. These fixtures mirror the real response shapes from the
 * reference (search hits under video_data[], video model entries with main_url /
 * key_seed / spade_a) so the reader is exercised on what it will actually see,
 * not on a toy object. */
static void test_json(void) {
  static const char *SEARCH =
    "{\"code\":0,\"data\":{\"video_data\":["
    "{\"series_id\":\"7001\",\"title\":\"\\u77ed\\u5267 A\",\"cover\":\"https://c/1.jpg\",\"episode_cnt\":86},"
    "{\"series_id\":\"7002\",\"title\":\"B}\\\"quote\",\"episode_cnt\":\"71\"}"
    "]}}";
  static const char *MODEL =
    "{\"video_list\":["
    "{\"main_url\":\"https://cdn/enc\",\"key_seed\":\"c2VlZA==\",\"spade_a\":\"AAA\",\"codec_type\":\"h264\"},"
    "{\"main_url\":\"https://cdn/other\",\"codec_type\":\"bytevc2\"}"
    "],\"fallback_api\":\"https://fb\"}";
  char out[256];
  size_t data, arr, elem, v;
  long long num;
  int count = 0;

  printf("\nJSON reader\n");

  /* nested walk: data -> video_data[] */
  ok("find nested object 'data'", hg_json_object_get(SEARCH, strlen(SEARCH), 0, "data", &data));
  ok("find array 'video_data'", hg_json_object_get(SEARCH, strlen(SEARCH), data, "video_data", &arr));

  if (hg_json_array_first(SEARCH, strlen(SEARCH), arr, &elem)) {
    count = 1;
    if (hg_json_object_get(SEARCH, strlen(SEARCH), elem, "series_id", &v)) {
      ok("element 0 series_id", hg_json_string(SEARCH, strlen(SEARCH), v, out, sizeof out) == 4 &&
                                strcmp(out, "7001") == 0);
    } else ok("element 0 series_id", 0);
    if (hg_json_object_get(SEARCH, strlen(SEARCH), elem, "title", &v)) {
      /* \u IS decoded to UTF-8: U+77ED U+5267 then " A" -> 8 bytes. Asserting the
       * exact bytes rather than only a length, because a length can be right
       * while the bytes are wrong. */
      int n = hg_json_string(SEARCH, strlen(SEARCH), v, out, sizeof out);
      ok("element 0 title decodes \\u to UTF-8",
         n == 8 && memcmp(out, "\xE7\x9F\xAD\xE5\x89\xA7 A", 8) == 0);
    } else ok("element 0 title", 0);
    if (hg_json_object_get(SEARCH, strlen(SEARCH), elem, "episode_cnt", &v)) {
      ok("element 0 episode_cnt", hg_json_int(SEARCH, strlen(SEARCH), v, &num) && num == 86);
    } else ok("element 0 episode_cnt", 0);
    if (hg_json_object_get(SEARCH, strlen(SEARCH), elem, "absent", &v)) {
      ok("absent key not found", 0);
    } else ok("absent key not found", 1);

    {
      size_t e2;
      if (hg_json_array_next(SEARCH, strlen(SEARCH), arr, elem, &e2)) {
        count = 2;
        /* a brace and a quote inside a string must not end the element */
        if (hg_json_object_get(SEARCH, strlen(SEARCH), e2, "title", &v)) {
          /* B } \" q u o t e  ->  B } " q u o t e  = 8 characters */
          ok("element 1 title survives } and \\\"",
             hg_json_string(SEARCH, strlen(SEARCH), v, out, sizeof out) == 8 &&
             strcmp(out, "B}\"quote") == 0);
        } else ok("element 1 title survives } and \\\"", 0);
        /* a quoted integer is still an integer */
        if (hg_json_object_get(SEARCH, strlen(SEARCH), e2, "episode_cnt", &v)) {
          ok("quoted integer parsed", hg_json_int(SEARCH, strlen(SEARCH), v, &num) && num == 71);
        } else ok("quoted integer parsed", 0);
      } else ok("second array element found", 0);
    }
  } else {
    ok("video_data array found", 0);
  }
  ok("array had both elements", count == 2);

  /* video model: the fields the player actually needs */
  {
    size_t vl, e0, vv;
    ok("find 'video_list'", hg_json_object_get(MODEL, strlen(MODEL), 0, "video_list", &vl));
    if (hg_json_array_first(MODEL, strlen(MODEL), vl, &e0)) {
      if (hg_json_object_get(MODEL, strlen(MODEL), e0, "main_url", &vv))
        ok("main_url", hg_json_string(MODEL, strlen(MODEL), vv, out, sizeof out) == 15 &&
                       strcmp(out, "https://cdn/enc") == 0);
      else ok("main_url", 0);
      if (hg_json_object_get(MODEL, strlen(MODEL), e0, "key_seed", &vv))
        ok("key_seed", hg_json_string(MODEL, strlen(MODEL), vv, out, sizeof out) == 8);
      else ok("key_seed", 0);
      if (hg_json_object_get(MODEL, strlen(MODEL), e0, "spade_a", &vv))
        ok("spade_a", hg_json_string(MODEL, strlen(MODEL), vv, out, sizeof out) == 3);
      else ok("spade_a", 0);
    } else ok("video_list first element", 0);
    /* a sibling key AFTER the array must still be reachable */
    /* "https://fb" is 10 characters (counted, not guessed this time) */
    ok("fallback_api after the array",
       hg_json_object_get(MODEL, strlen(MODEL), 0, "fallback_api", &vv) &&
       hg_json_string(MODEL, strlen(MODEL), vv, out, sizeof out) == 10 &&
       strcmp(out, "https://fb") == 0);
  }

  /* a surrogate pair (emoji in a title) must become 4 UTF-8 bytes */
  {
    static const char *EMOJI = "{\"t\":\"\\uD83C\\uDFAC\"}";
    size_t tv;
    ok("surrogate pair decodes to UTF-8",
       hg_json_object_get(EMOJI, strlen(EMOJI), 0, "t", &tv) &&
       hg_json_string(EMOJI, strlen(EMOJI), tv, out, sizeof out) == 4 &&
       memcmp(out, "\xF0\x9F\x8E\xAC", 4) == 0);
  }

  /* malformed / edge */
  ok("object_get on a non-object returns 0",
     hg_json_object_get("[1,2]", 5, 0, "x", &v) == 0);
  ok("array_first on a non-array returns 0",
     hg_json_array_first("{}", 2, 0, &elem) == 0);
  ok("empty array has no first element",
     hg_json_array_first("[]", 2, 0, &elem) == 0);
  ok("json_string on a number returns -1",
     hg_json_string("123", 3, 0, out, sizeof out) == -1);
  ok("skip handles nesting",
     hg_json_skip("{\"a\":[1,{\"b\":2}]}", 17, 0, &v) && v == 17);
}

#include "hg_play.h"
#include "hg_media_vectors.h"

/* Playback plan. Uses the spade vectors already pinned to the reference, so this
 * exercises the real decrypt path rather than a stub.
 *
 * The fixtures use the MEASURED upstream layout: codec_type at video_meta.codec_type
 * and spade_a at encrypt_info.spade_a. The entry level has neither. The real
 * doubly-encoded envelope (data[vid].video_model as a JSON string) is tested in
 * hg_fixture_test.c against bytes captured from the live API.
 *
 * The bytevc2 row is placed FIRST on purpose: the api parser had a bug where a
 * probe decoded into out[0] unconditionally, so once a filtered row was skipped
 * the result silently held the NEXT row's URL. Testing only with a clean list
 * would not have caught it, and neither would this. */
static void test_play(void) {
  static HgPlayPlan plan;
  char seed_b64[64];
  char json[4096];
  int n;

  printf("\nplayback plan\n");

  hg_base64_encode(SPADE_SEED, sizeof SPADE_SEED, seed_b64, sizeof seed_b64);

  /* 1. a decodable, encrypted source */
  n = sprintf(json,
    "{\"video_list\":[{\"main_url\":\"%s\",\"key_seed\":\"%s\","
    "\"video_meta\":{\"codec_type\":\"h264\"},"
    "\"encrypt_info\":{\"encrypt\":true,\"spade_a\":\"%s\","
    "\"kid\":\"6ac13d1b476d5a03cc1186750002ebeb\",\"encryption_method\":\"cenc-aes-ctr\"},"
    "\"quality_type\":40}]}",
    SPADE_INPUT, seed_b64, SPADE_A_INPUT);
  ok("plan from an encrypted source built",
     hg_play_plan_from_model(json, (size_t)n, &plan) == 0);
  ok("  key_seed base64 was decoded and the URL decrypted",
     strcmp(plan.url, SPADE_EXPECTED) == 0);
  if (strcmp(plan.url, SPADE_EXPECTED) != 0)
    printf("        got  %s\n        want %s\n", plan.url, SPADE_EXPECTED);
  ok("  a CENC key was derived", plan.encrypted == 1 && memcmp(plan.key, SPADE_A_KEY, 16) == 0);
  ok("  mpv decryption_key string is well formed",
     strncmp(plan.decryption_key, "decryption_key=", 15) == 0 &&
     strlen(plan.decryption_key) == 47);
  ok("  mpv header fields carry UA and Referer",
     strstr(plan.header_fields, "User-Agent: ") == plan.header_fields &&
     strstr(plan.header_fields, "Referer: https://novel.snssdk.com/") != NULL);
  /* 1b. the nested meta reached the plan */
  ok("  codec_type came from video_meta.codec_type",
     strcmp(plan.codec_type, "h264") == 0 && plan.has_codec_type == 1);
  ok("  spade_a came from encrypt_info.spade_a", plan.encrypted == 1);
  ok("  kid and encryption_method came from encrypt_info",
     strcmp(plan.kid, "6ac13d1b476d5a03cc1186750002ebeb") == 0 &&
     strcmp(plan.encryption_method, "cenc-aes-ctr") == 0);
  ok("  quality_type was read as a number from the entry",
     plan.has_quality_type == 1 && plan.quality_type == 40);

  /* 2. bytevc2 first, decodable second -- must not end up with the wrong URL */
  n = sprintf(json,
    "{\"video_list\":["
    "{\"main_url\":\"https://cdn/should-be-skipped.mp4\","
    "\"video_meta\":{\"codec_type\":\"bytevc2\"}},"
    "{\"main_url\":\"%s\",\"key_seed\":\"%s\","
    "\"video_meta\":{\"codec_type\":\"h264\"},"
    "\"encrypt_info\":{\"spade_a\":\"%s\"}}"
    "]}", SPADE_INPUT, seed_b64, SPADE_A_INPUT);
  memset(&plan, 0, sizeof plan);
  ok("bytevc2 row skipped in favour of h264",
     hg_play_plan_from_model(json, (size_t)n, &plan) == 0 &&
     strcmp(plan.url, SPADE_EXPECTED) == 0 &&
     strcmp(plan.codec_type, "h264") == 0);
  if (strcmp(plan.url, SPADE_EXPECTED) != 0)
    printf("        got  %s (the skipped row's URL would be the bug)\n", plan.url);

  /* 3. an unencrypted source: no seed, no spade_a */
  memset(&plan, 0, sizeof plan);
  n = sprintf(json,
    "{\"video_list\":[{\"main_url\":\"https://cdn/plain.mp4\","
    "\"encrypt_info\":{\"encrypt\":false},"
    "\"video_meta\":{\"codec_type\":\"h264\"}}]}");
  ok("plaintext source stays plaintext",
     hg_play_plan_from_model(json, (size_t)n, &plan) == 0 &&
     strcmp(plan.url, "https://cdn/plain.mp4") == 0 &&
     plan.encrypted == 0 && plan.decryption_key[0] == '\0');
  /* encrypt:false must be read as 0, not left as the "absent" sentinel -1: an
   * upstream that says "not encrypted" and an upstream that says nothing are
   * different facts, and hg_json_int alone cannot tell them apart. */
  ok("encrypt:false is read as 0, not as absent",
     plan.encrypt_flag == 0);

  /* 4. only bytevc2 rows -> no usable source, and it must SAY SO with the code
   * that means "the list was there, every entry was undecodable" rather than the
   * one that means "there was no list". */
  n = sprintf(json,
    "{\"video_list\":[{\"main_url\":\"https://cdn/a.mp4\",\"video_meta\":{\"codec_type\":\"bytevc2\"}},"
    "{\"main_url\":\"https://cdn/b.mp4\",\"video_meta\":{\"codec_type\":\"bvc2\"}}]}");
  ok("a list with no decodable source is refused",
     hg_play_plan_from_model(json, (size_t)n, &plan) < 0);

  /* 5. no video_list at all */
  ok("a response without video_list is refused",
     hg_play_plan_from_model("{\"code\":0}", 10, &plan) < 0);

  /* 6. a malformed seed is refused rather than half-applied */
  memset(&plan, 0, sizeof plan);
  n = sprintf(json,
    "{\"video_list\":[{\"main_url\":\"%s\",\"key_seed\":\"!!!not base64!!!\","
    "\"video_meta\":{\"codec_type\":\"h264\"}}]}",
    SPADE_INPUT);
  ok("a malformed key_seed is refused",
     hg_play_plan_from_model(json, (size_t)n, &plan) < 0);
}

int main(void) {
  char buf[512];
  int n;
  static const char *EXPECTED =
    "{\"loaderData\":{\"x\":\"a}\\\"b\",\"list\":[1,2]}}";

  printf("hg_router tests (i386 host build)\n");
  printf("sizeof(long)=%d  sizeof(void*)=%d\n\n", (int)sizeof(long), (int)sizeof(void *));
  printf("_ROUTER_DATA extraction\n");

  n = hg_router_extract(SAMPLE, strlen(SAMPLE), buf, sizeof buf);
  ok("extracts the object", n > 0);
  ok("brace inside a string does not truncate it", strcmp(buf, EXPECTED) == 0);
  if (strcmp(buf, EXPECTED) != 0) printf("        got  %s\n        want %s\n", buf, EXPECTED);

  /* the escaped-quote value resolves to the literal the Kotlin test asserts */
  n = hg_json_find_string(buf, "x", buf + 256, sizeof buf - 256);
  ok("x unescapes to a}\"b", n == 4 && strcmp(buf + 256, "a}\"b") == 0);
  if (!(n == 4 && strcmp(buf + 256, "a}\"b") == 0)) printf("        got  %s\n", buf + 256);

  /* A non-string value must be REFUSED, not silently mangled: loaderData holds an
   * object, and hg_json_find_string is documented as a string lookup only. An
   * earlier version of this test expected it to succeed, which was the test being
   * wrong rather than the helper. */
  n = hg_json_find_string(buf, "loaderData", buf + 256, sizeof buf - 256);
  ok("a non-string value is refused, not mangled", n == HG_ROUTER_ERR_NOT_FOUND);

  printf("\nmalformed input\n");
 {
    const char *no_marker = "<html><body>nothing here</body></html>";
    const char *no_brace = "var _ROUTER_DATA = null;";
    const char *unbalanced = "var _ROUTER_DATA = {\"a\":1";
    ok("missing marker is refused",
       hg_router_extract(no_marker, strlen(no_marker), buf, sizeof buf) == HG_ROUTER_ERR_NO_DATA);
    ok("marker without an object is refused",
       hg_router_extract(no_brace, strlen(no_brace), buf, sizeof buf) == HG_ROUTER_ERR_FORMAT);
    ok("unbalanced braces are refused",
       hg_router_extract(unbalanced, strlen(unbalanced), buf, sizeof buf) == HG_ROUTER_ERR_INCOMPLETE);
    ok("a too-small buffer is refused",
       hg_router_extract(SAMPLE, strlen(SAMPLE), buf, 8) == HG_ROUTER_ERR_SMALL);
    ok("a missing key is refused",
       hg_json_find_string("{\"a\":1}", "zzz", buf, sizeof buf) == HG_ROUTER_ERR_NOT_FOUND);
  }

  /* a string value that itself looks like the marker must not confuse a later
   * extraction, and only the first marker is used */
  printf("\nresilience\n");
  {
    const char *twice =
      "x _ROUTER_DATA {\"first\":\"]\"} y _ROUTER_DATA {\"second\":2}";
    n = hg_router_extract(twice, strlen(twice), buf, sizeof buf);
    ok("a bracket inside a string is not counted", n > 0 && strcmp(buf, "{\"first\":\"]\"}") == 0);
  }

  test_json();
  test_play();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
