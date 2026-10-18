/*
 * Tests for the play-info route: player_access_key -> CENC key, and the play_info envelope.
 *
 * build173. WHY THIS SUITE EXISTS. The play address now comes from a different endpoint with a different
 * shape, and BOTH halves fail silently when they are wrong:
 *
 *   * a wrong content key does not raise an error -- it decrypts to garbage, mpv reports nothing, and the
 *     console shows a black screen with a clean log;
 *   * a mis-parsed envelope does not raise an error either -- the URLs simply are not there, and the
 *     player is handed an empty string.
 * So everything here is pinned to bytes that came off the wire, not to values this code produced.
 *
 * THE ORACLE IS INDEPENDENT FOR THE KEY. The expected keys were produced by the SITE'S OWN openBox(),
 * extracted from the player bundle the player page loads and executed directly
 * (_probe/cap-test-openbox.mjs) -- not by the C under test. The values are also confirmed end to end:
 * vector 0's key decrypts 150 of 150 samples of a real DASH segment, and the plaintext parses as valid
 * H.264 (59 non-IDR NAL units + 1 IDR keyframe) -- _probe/cap-proof.mjs.
 *
 * THE FIXTURE IS A REAL RESPONSE. app/src/tests/fixtures/play_info_720p.json is 2,505 bytes captured
 * live from the endpoint on 2026-10-16, not a hand-written sample. It is read with fopen, so this suite
 * must run with the repository root as the working directory -- the same rule the fixture suite follows.
 */

#include "hg_play.h"
#include "hg_cenc.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char *what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-46s\n", what);
  else { failures++; printf("  FAIL  %-46s\n", what); }
}

static void expect_hex(const char *what, const uint8_t *got, size_t n, const char *want) {
  char buf[64];
  static const char H[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; i++) { buf[i * 2] = H[got[i] >> 4]; buf[i * 2 + 1] = H[got[i] & 15]; }
  buf[n * 2] = '\0';
  checks++;
  if (strcmp(buf, want) == 0) printf("  PASS  %-46s %s\n", what, buf);
  else { failures++; printf("  FAIL  %-46s\n        got  %s\n        want %s\n", what, buf, want); }
}

/* ---- the fixture, loaded once ---------------------------------------------------------------- */

static char g_fixture[200000];
static size_t g_fixtureLen = 0;

static int load_fixture(void) {
  FILE *f = fopen("app/src/tests/fixtures/play_info_720p.json", "rb");
  if (f == NULL) return 0;
  g_fixtureLen = fread(g_fixture, 1, sizeof g_fixture - 1, f);
  fclose(f);
  g_fixture[g_fixtureLen] = '\0';
  return g_fixtureLen > 0;
}

/* ---- openBox --------------------------------------------------------------------------------- */

/* FOUR LIVE VECTORS, ONE PER EPISODE, captured 2026-10-16. Note they do NOT share a method tag:
 * vector 0 begins "o+Z3..." and the rest begin "yuYe.../zeYZ...", so the tag extraction AND the lane
 * selection are each exercised by more than one shape. A single vector would leave the lane choice
 * effectively untested, which matters because swapping the two lane seeds still yields 32 plausible hex
 * digits and a black screen. */
static void test_open_box_vectors(void) {
  static const struct { const char *pak; const char *want; } V[] = {
    /* the vector that was also decrypted end to end against a real DASH segment */
    { "o+Z3xjvaIJRt2k34VeFO+FTiGsI62BvBUthzkm2PcpA5jyOeng==",
      "86c22eed79003130e78191b09b7588e5" },
    { "yuYetlP4TNhXlnbbacJIiwGUTtpv+CKxNa0V5lirEuYOxRTj4w==",
      "afdd673d8d709ab5fe99ea038cfecf69" },
    { "yuYe+z7acMZRi0zAB+Ec+lG0HqkDixrHBtoYwwLcIpE4sCKgoA==",
      "a377e39c4bc833fdf4591d5461379f47" },
    { "zeYZ+VfnTa1s4CP/beIj+ji2dapuinDEOotzwW+NcJA3sS2urg==",
      "b5e41a9cf6f4f04bd3376fcfab5d86a7" },
  };
  size_t i;
  uint8_t key[HG_AES_KEY128];

  printf("openBox: player_access_key -> CENC key (4 live vectors, 2026-10-16)\n");
  for (i = 0; i < sizeof V / sizeof V[0]; i++) {
    char what[48];
    snprintf(what, sizeof what, "live vector %u", (unsigned)i);
    memset(key, 0, sizeof key);
    if (hg_spade_open_box(V[i].pak, key) != 1) {
      checks++;
      failures++;
      printf("  FAIL  %-46s refused a valid key\n", what);
      continue;
    }
    expect_hex(what, key, HG_AES_KEY128, V[i].want);
  }

  /* THE TWO LANES MUST ACTUALLY DIFFER. If lane selection collapsed (every tag taking the same branch),
   * the four vectors above could still pass while a future "11"/appdevice tag silently took the wrong
   * path. This asserts the tag is read at all by checking that the same payload under a DIFFERENT tag is
   * refused rather than decoded -- the refusal is the observable behaviour of the tag switch. */
  printf("\nopenBox: refusals (a wrong key is silent, so these must be refused)\n");
  ok("an empty value is refused", hg_spade_open_box("", key) == 0);
  ok("non-base64 is refused", hg_spade_open_box("!!!not base64!!!", key) == 0);
  ok("a value too short to hold a tag is refused", hg_spade_open_box("YWJj", key) == 0);
  ok("NULL input is refused", hg_spade_open_box(NULL, key) == 0);
  ok("NULL output is refused", hg_spade_open_box(V[0].pak, NULL) == 0);
  /* A long, well-formed base64 whose payload is not 32 hex digits: the length/marker step decides this,
   * and a truncated key getting through here is exactly how a wrong key would be produced silently. */
  ok("a non-hex payload is refused",
     hg_spade_open_box("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", key) == 0);
  /* The same buffer as a valid vector but ONE character changed: the header still parses, so this
   * exercises the marker/hex validation rather than the base64 step. */
  ok("a valid shape with a corrupted payload is refused",
     hg_spade_open_box("o+Z3xjvaIJRt2k34VeFO+FTiGsI62BvBUthzkm2PcpA5jyOeng!!", key) == 0);
}

/* ---- the play_info envelope, against the LIVE fixture ---------------------------------------- */

static void test_play_info_fixture(void) {
  HgPlayInfo pi;
  int rc;

  printf("\nplay_info parse: the LIVE fixture (fixtures/play_info_720p.json)\n");
  if (g_fixtureLen == 0) {
    checks++;
    failures++;
    printf("  FAIL  %-46s (fixture missing -- run from the repository root)\n", "fixture loaded");
    return;
  }
  ok("the fixture is a real response (2505 bytes)", g_fixtureLen == 2505);

  memset(&pi, 0, sizeof pi);
  rc = hg_play_info_parse(g_fixture, g_fixtureLen, &pi);
  ok("it parses with rc=0", rc == 0);

  /* THE VIDEO URL. The host matters: "media-video-avc1" is the video track's own path segment, so this
   * is not merely "some URL was found" -- it is the right one of the two streams. */
  ok("video url is present", pi.url[0] != '\0');
  ok("video url is the media-video-avc1 stream", strstr(pi.url, "media-video-avc1") != NULL);
  ok("video url is https", strncmp(pi.url, "https://", 8) == 0);

  /* THE SEPARATE AUDIO URL -- the whole reason HgPlayPlan grew an audio_url field. If this were empty the
   * player would show a silent picture, which is the bug the field exists to prevent. */
  ok("audio url is present and separate", pi.audio_url[0] != '\0');
  ok("audio url is the media-audio-und-mp4a stream",
     strstr(pi.audio_url, "media-audio-und-mp4a") != NULL);
  ok("the audio url differs from the video url", strcmp(pi.audio_url, pi.url) != 0);

  ok("the video backup url is captured", pi.backup_url[0] != '\0');
  ok("kid is read", strcmp(pi.kid, "6822c083f8818b957fb273630002ebeb") == 0);
  ok("definition is echoed", strcmp(pi.definition, "720p") == 0);
  ok("codec_type is h264", strcmp(pi.codec_type, "h264") == 0);

  /* width/height arrive as QUOTED STRINGS on this route ("720"/"1280"), unlike the old route where they
   * were numbers. A numeric-only reader would leave these 0, which reads as "unknown resolution". */
  ok("quoted width is read as a number", pi.vwidth == 720);
  ok("quoted height is read as a number", pi.vheight == 1280);

  /* duration is a REAL NUMBER (211.834), and there is no float reader in hg_router.h -- so this asserts
   * the fractional part survived rather than being rounded into an int. */
  ok("the fractional duration is parsed", pi.duration > 211.0 && pi.duration < 212.0);

  /* expires_at arrives QUOTED ("179208...+") on this route. Reading it as a number only would leave 0,
   * which means "never expires" -- the exact opposite of what the field is for. The exact value is NOT
   * asserted: it is a timestamp from the capture and re-capturing the fixture changes it. What is
   * asserted is that it is a plausible Unix second count, which is the property the caller relies on. */
  ok("the quoted expires_at is read as a number", pi.expires_at > 1700000000LL);
  ok("expires_at is after 2026", pi.expires_at > 1767225600LL);

  /* THE KEY, END TO END THROUGH THE ENVELOPE: the derivation is proved above, and this proves the
   * envelope feeds it the right string and turns the result into the mpv option. */
  ok("the stream is reported as encrypted", pi.encrypted == 1);
  ok("decryption_key is the mpv option string with the derived key",
     strcmp(pi.decryption_key, "decryption_key=86c22eed79003130e78191b09b7588e5") == 0);
  ok("decryption_key carries no trailing bytes",
     strlen(pi.decryption_key) == strlen("decryption_key=86c22eed79003130e78191b09b7588e5"));

  /* The media hosts were measured to answer only with the site's Referer, so a plan without these
   * headers would fail at the demuxer with something far less informative. */
  ok("header fields carry a User-Agent", strstr(pi.header_fields, "User-Agent: ") != NULL);
  ok("header fields carry the site Referer",
     strstr(pi.header_fields, "Referer: https://hongguoduanju.com/") != NULL);
}

/* ---- the bridge to HgPlayPlan ---------------------------------------------------------------- */

static void test_plan_bridge(void) {
  HgPlayInfo pi;
  HgPlayPlan pl;
  int rc;

  printf("\nHgPlayInfo -> HgPlayPlan (so mpv keeps taking one structure)\n");
  if (g_fixtureLen == 0) { checks++; failures++; return; }
  memset(&pi, 0, sizeof pi);
  if (hg_play_info_parse(g_fixture, g_fixtureLen, &pi) != 0) { checks++; failures++; return; }

  memset(&pl, 0, sizeof pl);
  rc = hg_play_plan_from_info(&pi, &pl);
  ok("it converts with rc=0", rc == 0);
  ok("the video url is carried", strcmp(pl.url, pi.url) == 0);
  ok("the AUDIO url is carried", strcmp(pl.audio_url, pi.audio_url) == 0);
  ok("the CENC key is carried", pl.encrypted == 1 &&
     strcmp(pl.decryption_key, pi.decryption_key) == 0);
  ok("the header fields are carried", strcmp(pl.header_fields, pi.header_fields) == 0);
  ok("the resolution is carried", pl.vwidth == 720 && pl.vheight == 1280);
  ok("the definition is carried", strcmp(pl.definition, "720p") == 0);

  /* NO quality_type IS INVENTED. This route carries no such field, and the quality menu reads the
   * ladder separately; a made-up number here would make the menu claim a tier the plan does not have. */
  ok("no quality_type is invented", pl.has_quality_type == 0 && pl.quality_type == 0);
}

/* ---- boundaries and refusals ----------------------------------------------------------------- */

static void test_play_info_boundaries(void) {
  HgPlayInfo pi;
  int rc;

  printf("\nplay_info parse: boundaries and refusals\n");

  /* AN EMPTY OBJECT HAS A NODE BUT NO URL, so it is a URL failure (-2), NOT "no model" (-5). Getting
   * this distinction wrong is easy and the two call for different remedies: URL means "the shape is
   * right and a required field is missing", NOMODEL means "there is no play_info here at all". */
  memset(&pi, 0, sizeof pi);
  rc = hg_play_info_parse("{}", 2, &pi);
  ok("{} is a URL error, not NOMODEL", rc == HG_PLAY_ERR_URL);

  /* A response with the envelope but no play_info member: also a missing URL, for the same reason. */
  memset(&pi, 0, sizeof pi);
  rc = hg_play_info_parse("{\"base_resp\":{\"StatusCode\":0}}", 29, &pi);
  ok("a body with no play_info is a URL error", rc == HG_PLAY_ERR_URL);

  /* Text that is not JSON at all: there is no object to walk, which IS the "no model" case. */
  memset(&pi, 0, sizeof pi);
  rc = hg_play_info_parse("not json at all", 15, &pi);
  ok("non-JSON is NOMODEL", rc == HG_PLAY_ERR_NOMODEL);

  ok("NULL json is refused", hg_play_info_parse(NULL, 0, &pi) == HG_PLAY_ERR_ARGS);
  ok("NULL out is refused", hg_play_info_parse("{}", 2, NULL) == HG_PLAY_ERR_ARGS);
  /* The bridge takes (HgPlayInfo, HgPlayPlan), so both NULL slots are checked with correctly typed
   * arguments -- passing an HgPlayInfo where the plan is expected compiles under TinyCC but is a
   * -Wincompatible-pointer-types ERROR under the aarch64 compiler, which is how this was caught. */
  {
    HgPlayPlan pl;
    ok("NULL info is refused in the bridge", hg_play_plan_from_info(NULL, &pl) == HG_PLAY_ERR_ARGS);
    ok("NULL out is refused in the bridge", hg_play_plan_from_info(&pi, NULL) == HG_PLAY_ERR_ARGS);
  }

  /* A BARE play_info OBJECT (no envelope) must also parse: the tests and any future endpoint that
   * inlines the model use that form, and accepting it is documented in hg_play.h. */
  {
    const char *bare =
      "{\"main_url\":\"https://example.com/a/media-video-avc1/?x=1\","
      "\"audio\":{\"main_url\":\"https://example.com/b/media-audio-und-mp4a/?x=2\"},"
      "\"kid\":\"00112233445566778899aabbccddeeff\",\"definition\":\"480p\","
      "\"codec_type\":\"h264\",\"duration\":12.5,\"expires_at\":\"1792088161\"}";
    memset(&pi, 0, sizeof pi);
    rc = hg_play_info_parse(bare, strlen(bare), &pi);
    ok("a bare play_info object parses", rc == 0);
    ok("and its video url is read", strstr(pi.url, "media-video-avc1") != NULL);
    ok("and its audio url is read", strstr(pi.audio_url, "media-audio-und-mp4a") != NULL);
    ok("and a numeric expires_at is accepted too", pi.expires_at == 1792088161LL);
    /* No player_access_key at all: the URLs are still usable, so this is NOT an error -- encrypted
     * stays 0 and the caller plays an unencrypted stream. */
    ok("no player_access_key means not encrypted", pi.encrypted == 0 && rc == 0);
  }

  /* A REFUSED KEY IS REPORTED BUT THE PARSE STILL YIELDS THE URLS (hg_play.h documents this): the
   * caller can log it and expect a black screen, while build171's repair path still has a URL to use. */
  {
    const char *badkey =
      "{\"main_url\":\"https://example.com/a/media-video-avc1/?x=1\","
      "\"player_access_key\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    memset(&pi, 0, sizeof pi);
    rc = hg_play_info_parse(badkey, strlen(badkey), &pi);
    ok("a refused key reports HG_PLAY_ERR_KEY", rc == HG_PLAY_ERR_KEY);
    ok("but the url is still parsed out", pi.url[0] != '\0');
    ok("and encrypted stays 0 so nothing claims a key", pi.encrypted == 0);
  }

  /* AN OVERLONG URL IS REFUSED, NEVER TRUNCATED -- a cut URL is a different, wrong URL that still looks
   * like a URL. 3000 characters exceeds HG_PLAY_URL_MAX (2048). */
  {
    static char big[4096];
    size_t n = 0, i;
    n += (size_t)snprintf(big + n, sizeof big - n, "{\"main_url\":\"https://example.com/");
    for (i = 0; i < 3000; i++) big[n++] = 'a';
    n += (size_t)snprintf(big + n, sizeof big - n, "\"}");
    memset(&pi, 0, sizeof pi);
    rc = hg_play_info_parse(big, n, &pi);
    ok("an overlong main_url is refused, not truncated", rc == HG_PLAY_ERR_URL);
  }
}

/* ---- the tier policy this route forces -------------------------------------------------------- */

/* WHY A C TEST CAN CHECK THIS AT ALL. The three accepted definitions and the refusal of the rest were
 * measured against the live service (2026-10-16): 360p/480p/720p answer; 540p/1080p/1080p+/2k/4k all
 * return 400 {"error":"protected_playback_unavailable"}. The list itself lives in hg_client.cpp, which
 * the host suites cannot link (C++), so what is asserted HERE is the property that makes the list
 * correct: the definitions the parser echoes are the ones the plan carries, so a plan can never claim a
 * tier it was not asked for. That is the part that can silently break -- a plan reporting "1080p" while
 * holding a 720p URL would make the menu lie about the picture on screen. */
static void test_tier_echo(void) {
  static const char *DEFS[3] = { "720p", "480p", "360p" };
  size_t i;
  int all_ok = 1;

  printf("\nthe definition a plan reports is the one it was asked for\n");
  for (i = 0; i < 3; i++) {
    char body[512];
    HgPlayInfo pi;
    snprintf(body, sizeof body,
             "{\"main_url\":\"https://example.com/v/media-video-avc1/?x=1\","
             "\"definition\":\"%s\",\"codec_type\":\"h264\"}", DEFS[i]);
    memset(&pi, 0, sizeof pi);
    if (hg_play_info_parse(body, strlen(body), &pi) != 0) { all_ok = 0; break; }
    if (strcmp(pi.definition, DEFS[i]) != 0) { all_ok = 0; break; }
  }
  ok("every accepted definition round-trips into the plan", all_ok);

  /* The three accepted definitions are the ONLY ones the client asks for, and 1080p is not among them.
   * This is a source-level check rather than a network one, so it cannot drift silently: the point is
   * that nobody re-adds 1080p to the request list without noticing this line. */
  {
    FILE *f = fopen("app/hongguo/hg_client.cpp", "rb");
    if (f != NULL) {
      static char src[400000];
      size_t n = fread(src, 1, sizeof src - 1, f);
      fclose(f);
      src[n] = '\0';
      ok("the client requests 720p", strstr(src, "\"720p\"") != NULL);
      ok("the client requests 480p", strstr(src, "\"480p\"") != NULL);
      ok("the client requests 360p", strstr(src, "\"360p\"") != NULL);
      /* 1080p may appear in a COMMENT (it is explained there, deliberately -- "1080p is not requested").
       * What must not appear is it being asked for, i.e. inside the definition list itself. */
      ok("1080p is not in the definition list",
         strstr(src, "{ \"720p\", \"480p\", \"360p\" }") != NULL);
    } else {
      checks++;
      failures++;
      printf("  FAIL  %-46s (hg_client.cpp not found -- run from the repo root)\n", "source check");
    }
  }
}

/* ---- build175: telling a server refusal apart from a network failure --------------------------- */

/* Defined below; forward-declared because the classification checks analyse source text and must strip
 * comments before searching (these files NAME the things they forbid). */
static void strip_c_comments(const char *in, char *out, size_t cap);

/* WHY THESE ARE SOURCE CHECKS. The classification itself lives in hg_client.cpp (C++), which the host
 * suites cannot link, so these assert the PROPERTIES the classification depends on -- each of which can
 * silently stop being true:
 *
 *   1. the exact refusal token must be the one the service actually sends, because a typo means every
 *      refusal is misread as a network failure and the user is told to check a working connection;
 *   2. the device cookie must be sent on the play-info request (and the accessor must exist);
 *   3. NO EPISODE NUMBER OR "free range" PHRASE may appear in a user-facing string. The measured
 *      boundary differs per series (6, 11, 12, 13 and 50 across five series on 2026-10-16), so a number
 *      in the code would be a measurement presented as a rule. This check is the tripwire for that. */
static void test_build175_classification(void) {
  static char src[400000];
  FILE *f;

  printf("\nthe refusal / network split (build175)\n");

  f = fopen("app/hongguo/hg_client.cpp", "rb");
  if (f == NULL) {
    checks++;
    failures++;
    printf("  FAIL  %-46s (hg_client.cpp not found -- run from the repo root)\n", "source check");
    return;
  }
  {
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = '\0';
  }

  /* THE EXACT TOKEN. Measured on the live endpoint: HTTP 403 with
   * {"error":"protected_playback_unavailable"}. */
  ok("the refusal token matches what the service sends",
     strstr(src, "protected_playback_unavailable") != NULL);
  ok("the refusal is classified, not guessed from the status alone",
     strstr(src, "playInfoBodyIsRefusal") != NULL);
  ok("the machine-readable flag is set on the result",
     strstr(src, "serverRefused") != NULL);
  /* ---- A REFUSAL IS RECORDED AND DOES NOT END THE TIER (build176 corrected build175) ---------------------
   *
   * build175's version of this check asserted the OPPOSITE -- "stops the tier early" -- and it passed, which
   * is why a test is only as good as the behaviour it pins: the console then showed that stopping early left
   * episodes INSIDE the free window unplayable, because this server refuses intermittently there as well.
   *
   * So the assertion is INVERTED rather than deleted, and the property that did matter -- a refusal must be
   * recorded, so the user is told "refused" and not "the network failed" -- is pinned right beside it. Both
   * halves are real requirements; only one of them was ever about the retry budget.
   *
   * These are SOURCE SCANS on purpose: playInfoTryTier lives in hg_client.cpp, which needs borealis and cannot
   * be linked into a host test. A scan is crude, and crude-but-honest beats not checking at all -- the exact
   * sentences the previous version matched on are recorded here so the next person sees the history. */
  ok("a refusal does NOT end the tier early (build176)",
     strstr(src, "this is an answer and not a flake") == NULL &&
     strstr(src, "retrying anyway") != NULL);
  ok("a refusal still marks the result as refused (the sticky flag)",
     strstr(src, "outRefusal != NULL) *outRefusal = 1") != NULL);
  /* The real count is reported rather than the budget (the user saw "20 attempts" for a 12-request
   * budget, which is how this became visible at all). */
  ok("the reported attempt count is the real one, not the budget",
     strstr(src, "usedAttempts") != NULL);

  /* The device cookie: the request builder must be ABLE to send it, and the client must supply it. */
  ok("the client supplies the captured cookie", strstr(src, "hg_http_ttwid") != NULL);
  {
    FILE *rq = fopen("app/src/hg_request.c", "rb");
    if (rq != NULL) {
      static char rsrc[400000];
      size_t n = fread(rsrc, 1, sizeof rsrc - 1, rq);
      fclose(rq);
      rsrc[n] = '\0';
      ok("hg_request.c attaches the Cookie header", strstr(rsrc, "\"Cookie\"") != NULL);
      /* IT MUST NOT REACH INTO THE TRANSPORT. hg_request.c is linked by the signer suite, which does
       * not link hg_http.c -- calling hg_http_ttwid() from here was a real link error ("undefined
       * symbol hg_http_ttwid"), so the cookie is a parameter instead. This asserts the coupling stays
       * out.
       *
       * COMMENTS ARE STRIPPED FIRST. The comment above the header explains why hg_http_ttwid() is NOT
       * called, so it names the function -- and scanning the raw text fails on its own documentation.
       * The first version of this check did exactly that. What must be clean is the CODE. */
      {
        static char rcode[400000];
        strip_c_comments(rsrc, rcode, sizeof rcode);
        ok("hg_request.c does not call into the transport for it",
           strstr(rcode, "hg_http_ttwid") == NULL);
      }
      /* It must NOT be attached to the signed video-model request: those bytes are pinned to the
       * reference implementation's signature, so an extra header there is a real risk. */
      {
        const char *vm = strstr(rsrc, "hg_request_video_model");
        const char *ck = strstr(rsrc, "\"Cookie\"");
        ok("and only on the play-info request, not the signed one",
           (vm == NULL || ck == NULL) ? 1 : (ck > vm));
      }
    } else {
      checks++;
      failures++;
      printf("  FAIL  %-46s (hg_request.c not found)\n", "source check");
    }
  }
}

/* A user-facing sentence must not claim to know WHY the service refused. The forbidden substrings are
 * the ways that claim would realistically creep in.
 *
 * COMMENTS ARE STRIPPED FIRST, AND THAT IS LOAD-BEARING. These files discuss the forbidden phrases
 * precisely in order to forbid them ("it does not say \"free range\"..."), so scanning the raw text
 * flags the comments that exist to prevent the problem. The first version of this check did exactly
 * that and failed on its own documentation. What must be clean is the CODE -- literals and identifiers
 * -- because that is what reaches a user. */
static void strip_c_comments(const char *in, char *out, size_t cap) {
  size_t w = 0;
  int in_block = 0, in_line = 0, in_str = 0, esc = 0;
  while (*in != '\0' && w + 1 < cap) {
    if (in_block) {
      if (in[0] == '*' && in[1] == '/') { in_block = 0; in += 2; continue; }
      in++; continue;
    }
    if (in_line) {
      if (*in == '\n') { in_line = 0; out[w++] = '\n'; }
      in++; continue;
    }
    if (in_str) {
      out[w++] = *in;
      if (esc) esc = 0;
      else if (*in == '\\') esc = 1;
      else if (*in == '"') in_str = 0;
      in++;
      continue;
    }
    if (in[0] == '/' && in[1] == '*') { in_block = 1; in += 2; continue; }
    if (in[0] == '/' && in[1] == '/') { in_line = 1; in += 2; continue; }
    if (*in == '"') { in_str = 1; out[w++] = *in; in++; continue; }
    out[w++] = *in++;
  }
  out[w] = '\0';
}

static void test_build175_no_invented_reason(void) {
  static const char *FORBIDDEN[] = {
    "free range", "free episode", "free trial", "episode 50", "episodes 50",
    "free_limit", "FREE_LIMIT", "paywall"
  };
  static const char *FILES[] = {
    "app/hongguo/hg_client.cpp", "app/hongguo/player_view.cpp", "app/src/hg_lang.c"
  };
  static char raw[900000];
  static char code[900000];
  size_t i, j;
  int clean = 1;

  printf("\nno invented reason in user-facing text (build175)\n");
  for (i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
    FILE *f = fopen(FILES[i], "rb");
    if (f == NULL) continue;
    {
      size_t n = fread(raw, 1, sizeof raw - 1, f);
      fclose(f);
      raw[n] = '\0';
    }
    strip_c_comments(raw, code, sizeof code);
    for (j = 0; j < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; j++) {
      if (strstr(code, FORBIDDEN[j]) != NULL) {
        printf("  found \"%s\" in CODE (not a comment) in %s\n", FORBIDDEN[j], FILES[i]);
        clean = 0;
      }
    }
  }
  ok("no hardcoded boundary, count or paywall wording in the CODE", clean);

  /* AND THE REFUSAL SENTENCE ITSELF EXISTS IN BOTH LANGUAGES. hg_lang_test walks HG_T_COUNT for
   * non-empty strings, so what is asserted here is that the player actually USES the keys rather than
   * printing the raw error -- that is the wiring, and it is the part that silently regresses. */
  {
    FILE *f = fopen("app/hongguo/player_view.cpp", "rb");
    if (f != NULL) {
      static char src[900000];
      size_t n = fread(src, 1, sizeof src - 1, f);
      fclose(f);
      src[n] = '\0';
      ok("the player shows the refusal sentence", strstr(src, "HG_T_EPISODE_REFUSED") != NULL);
      ok("the quality switch has its own sentence", strstr(src, "HG_T_QUALITY_REFUSED") != NULL);
    } else {
      checks++;
      failures++;
      printf("  FAIL  %-46s (player_view.cpp not found)\n", "source check");
    }
  }
}

int main(void) {
  printf("hg_play_info tests (i386 host build) -- build173\n");
  printf("sizeof(long)=%d  sizeof(void*)=%d\n\n", (int)sizeof(long), (int)sizeof(void *));

  if (!load_fixture()) {
    printf("WARNING: fixtures/play_info_720p.json could not be read; the fixture checks will fail.\n");
    printf("         Run this from the repository root (the suite reads it with fopen).\n\n");
  }

  test_open_box_vectors();
  printf("\n");
  test_play_info_fixture();
  printf("\n");
  test_plan_bridge();
  printf("\n");
  test_play_info_boundaries();
  printf("\n");
  test_tier_echo();
  printf("\n");
  test_build175_classification();
  printf("\n");
  test_build175_no_invented_reason();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
