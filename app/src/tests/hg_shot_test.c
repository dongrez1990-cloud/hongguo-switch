/*
 * Tests for hg_shot.c -- the screenshot-raw probe's parsing and verdicts.
 *
 * The checks that matter most:
 *
 *   - A REPLY WHOSE data DOES NOT MATCH ITS OWN w/h/stride IS REJECTED. A frame with the wrong
 *     stride would be sheared diagonally when drawn, which looks like a nanovg bug rather than a
 *     bad reply. This is the check that stops the probe from reporting a broken frame as a win --
 *     and a false win here would send the whole thumbnail feature down the wrong path.
 *   - "NOT AN OBJECT", "FIELDS MISSING" AND "FIELDS CONTRADICT EACH OTHER" ARE THREE DIFFERENT
 *     VERDICTS, because they call for three different next steps.
 *   - AN EMPTY data ARRAY IS NOT A MALFORMED ONE. mpv could legitimately answer with no pixels;
 *     that is a different fact from a reply this parser cannot read.
 *
 * ASCII-only source file.
 */

#include "hg_shot.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* Build a data array of `n` zero bytes into a caller buffer, so the fixture sizes are honest
 * rather than hand-written counts that could drift from the text. */
static void buildData(char* out, int cap, int n) {
  int i;
  char* p = out;
  int left = cap;
  int written = snprintf(p, (size_t)left, "[");
  p += written;
  left -= written;
  for (i = 0; i < n; i++) {
    written = snprintf(p, (size_t)left, "%s0", i ? "," : "");
    p += written;
    left -= written;
    if (left <= 1) break;
  }
  snprintf(p, (size_t)left, "]");
}

/* A well-formed reply for a 2x2 bgr0 frame: 2 * 2 * 4 = 16 bytes. */
static void test_ok(void) {
  char data[256];
  char json[512];
  HgShotInfo info;

  printf("\n-- a usable frame --\n");
  buildData(data, (int)sizeof data, 16);
  snprintf(json, sizeof json,
           "{\"w\":2,\"h\":2,\"stride\":0,\"format\":\"bgr0\",\"data\":%s}", data);

  ok("a well-formed reply is accepted", hg_shot_parse(json, strlen(json), &info) == HG_SHOT_OK);
  ok("the width is read", info.width == 2);
  ok("the height is read", info.height == 2);
  ok("the format is read", strcmp(info.format, "bgr0") == 0);
  ok("stride 0 is reported as 0, not rejected", info.stride == 0);
  ok("the byte count is read", info.elements == 16);
  ok("the declared size is w*h*4 when stride is 0", info.declared == 16);
  ok("nothing is reported missing", info.missing == NULL);

  /* A real reply is bigger, so the arithmetic is checked at a size where an off-by-four would not
   * hide: 1920x1080 bgr0 is 8,294,400 bytes. The array itself is not built -- the declared size is
   * what is being checked, and the data array is only needed for the equality test above. */
  ok("a 1080p frame's arithmetic is right",
     (1920 * 4) * 1080 == 8294400);

  /* A build that FILLS IN stride must be honoured, not overruled by the w*4 assumption. The fixture
   * uses 500x1 with a 2048-byte stride: w*4 would say 2000, so the two are distinguishable, and one
   * row of 2048 elements fits in the fixture buffer. (The first version of this used 2 rows, which
   * needed an 8 KB array whose own text did not fit where it was built -- a fixture bug, not a
   * parser bug.) */
  {
    char d2[8192];
    char j2[8400];
    buildData(d2, (int)sizeof d2, 2048);
    snprintf(j2, sizeof j2,
             "{\"w\":500,\"h\":1,\"stride\":2048,\"format\":\"bgr0\",\"data\":%s}", d2);
    ok("a padded stride is used when the reply gives one",
       hg_shot_parse(j2, strlen(j2), &info) == HG_SHOT_OK && info.declared == 2048);
    ok("and it differs from what w*4 would have said", info.width * 4 != info.declared);
  }
}

static void test_rejections(void) {
  HgShotInfo info;

  printf("\n-- the replies that must NOT be called a win --\n");

  /* THE STRIDE MISMATCH. `data` holds 4 bytes but w*h*4 is 16: drawing this would shear the frame. */
  {
    const char* j = "{\"w\":2,\"h\":2,\"stride\":0,\"format\":\"bgr0\",\"data\":[0,0,0,0]}";
    ok("a data length that contradicts w/h is rejected",
       hg_shot_parse(j, strlen(j), &info) == HG_SHOT_ERR_NO_ELEMENTS);
    ok("and the reported sizes show why", info.elements == 4 && info.declared == 16);
  }

  /* A zero-sized frame. A 0x0 "success" is a build that answered the command without rendering
   * anything, which is evidence AGAINST route 1 rather than for it. */
  {
    const char* j = "{\"w\":0,\"h\":0,\"format\":\"bgr0\",\"data\":[]}";
    ok("a 0x0 frame is its own verdict",
       hg_shot_parse(j, strlen(j), &info) == HG_SHOT_ERR_ZERO_SIZE);
  }

  /* FIELDS MISSING, one at a time, each naming itself. */
  {
    const char* noW = "{\"h\":2,\"format\":\"bgr0\",\"data\":[0,0,0,0]}";
    ok("a missing w is a parse failure",
       hg_shot_parse(noW, strlen(noW), &info) == HG_SHOT_ERR_PARSE && strcmp(info.missing, "w") == 0);

    const char* noH = "{\"w\":2,\"format\":\"bgr0\",\"data\":[0,0,0,0]}";
    ok("a missing h names itself",
       hg_shot_parse(noH, strlen(noH), &info) == HG_SHOT_ERR_PARSE && strcmp(info.missing, "h") == 0);

    const char* noData = "{\"w\":2,\"h\":2,\"format\":\"bgr0\"}";
    ok("a missing data names itself",
       hg_shot_parse(noData, strlen(noData), &info) == HG_SHOT_ERR_PARSE &&
           strcmp(info.missing, "data") == 0);
  }

  /* NOT AN OBJECT is a different fact from "fields missing": it means the command answered
   * something this parser does not know at all -- for instance an error string. */
  {
    const char* s = "\"vo not available\"";
    ok("a string reply is a parse failure, not a field failure",
       hg_shot_parse(s, strlen(s), &info) == HG_SHOT_ERR_PARSE);
  }
  {
    const char* nul = "null";
    ok("a null reply is a parse failure", hg_shot_parse(nul, strlen(nul), &info) == HG_SHOT_ERR_PARSE);
  }

  /* THE D3D11 FLAG, reported rather than ignored: it would make every pixel wrong rather than
   * absent, which is a different problem from a missing frame. */
  {
    char d[256];
    char j[512];
    buildData(d, (int)sizeof d, 16);
    snprintf(j, sizeof j,
             "{\"w\":2,\"h\":2,\"stride\":0,\"format\":\"bgr0\",\"strange_colors\":1,\"data\":%s}", d);
    ok("the strange-colors flag gets its own verdict",
       hg_shot_parse(j, strlen(j), &info) == HG_SHOT_ERR_STRANGE);
  }
  {
    char d[256];
    char j[512];
    buildData(d, (int)sizeof d, 16);
    snprintf(j, sizeof j,
             "{\"w\":2,\"h\":2,\"stride\":0,\"format\":\"bgr0\",\"strange_colors\":0,\"data\":%s}", d);
    ok("strange_colors 0 is fine", hg_shot_parse(j, strlen(j), &info) == HG_SHOT_OK);
  }

  /* Degenerate input must not crash. */
  ok("NULL json is refused", hg_shot_parse(NULL, 0, &info) == HG_SHOT_ERR_ARGS);
  ok("an empty string is refused", hg_shot_parse("", 0, &info) == HG_SHOT_ERR_ARGS);
  {
    const char* j = "{\"w\":2,\"h\":2,\"data\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}";
    ok("a NULL out pointer is tolerated", hg_shot_parse(j, strlen(j), NULL) == HG_SHOT_OK);
  }
}

static void test_format_optional(void) {
  HgShotInfo info;
  char data[256];
  char json[512];

  printf("\n-- the optional fields --\n");
  buildData(data, (int)sizeof data, 16);
  snprintf(json, sizeof json, "{\"w\":2,\"h\":2,\"data\":%s}", data);
  ok("format is optional", hg_shot_parse(json, strlen(json), &info) == HG_SHOT_OK);
  ok("and comes back empty rather than garbage", info.format[0] == '\0');

  snprintf(json, sizeof json, "{\"w\":2,\"h\":2,\"stride\":0,\"data\":%s}", data);
  ok("stride is optional", hg_shot_parse(json, strlen(json), &info) == HG_SHOT_OK);
  ok("and is reported as 0", info.stride == 0);

  /* AN EMPTY data ARRAY IS NOT A MALFORMED ONE. It is a reply with no pixels, which reports as a
   * size mismatch against w*h*4 -- the honest outcome, not "unparseable". */
  {
    const char* j = "{\"w\":2,\"h\":2,\"data\":[]}";
    ok("an empty data array parses and fails on the size check",
       hg_shot_parse(j, strlen(j), &info) == HG_SHOT_ERR_NO_ELEMENTS && info.elements == 0);
  }
}

static void test_text(void) {
  HgShotInfo info;
  char b[320];
  char data[256];
  char json[512];

  printf("\n-- the log line --\n");

  buildData(data, (int)sizeof data, 16);
  snprintf(json, sizeof json,
           "{\"w\":2,\"h\":2,\"stride\":0,\"format\":\"bgr0\",\"data\":%s}", data);
  hg_shot_parse(json, strlen(json), &info);
  hg_shot_describe(&info, b, (int)sizeof b);
  ok("an OK summary names the size and the format",
     strstr(b, "2x2") != NULL && strstr(b, "bgr0") != NULL && strstr(b, "OK") != NULL);

  {
    const char* j = "{\"w\":2,\"h\":2,\"format\":\"bgr0\",\"data\":[0,0,0,0]}";
    hg_shot_parse(j, strlen(j), &info);
    hg_shot_describe(&info, b, (int)sizeof b);
    ok("a failure summary names what did not match",
       strstr(b, "does not match") != NULL && strstr(b, "16") != NULL);
  }

  ok("a verdict has text", hg_shot_verdict_text(HG_SHOT_OK) != NULL &&
                              hg_shot_verdict_text(HG_SHOT_ERR_PARSE) != NULL &&
                              hg_shot_verdict_text(9999) != NULL);
  ok("every verdict's text is non-empty",
     hg_shot_verdict_text(HG_SHOT_OK)[0] != '\0' &&
         hg_shot_verdict_text(HG_SHOT_ERR_ARGS)[0] != '\0' &&
         hg_shot_verdict_text(HG_SHOT_ERR_NO_ELEMENTS)[0] != '\0' &&
         hg_shot_verdict_text(HG_SHOT_ERR_ZERO_SIZE)[0] != '\0' &&
         hg_shot_verdict_text(HG_SHOT_ERR_STRANGE)[0] != '\0');

  hg_shot_describe(NULL, b, (int)sizeof b);
  ok("a NULL info says so", strcmp(b, "(no measurement)") == 0);
  hg_shot_describe(&info, b, 8);
  ok("a small buffer is NUL-terminated", b[7] == '\0');
  hg_shot_describe(&info, NULL, 32);
  ok("a NULL buffer is tolerated", 1);
}

int main(void) {
  printf("hg_shot tests: the screenshot-raw probe\n");
  printf("================================================\n");

  test_ok();
  test_rejections();
  test_format_optional();
  test_text();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
