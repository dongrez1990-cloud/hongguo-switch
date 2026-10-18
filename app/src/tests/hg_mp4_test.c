/*
 * Tests for hg_mp4.c.
 *
 * The fixture is a synthetic ISO-BMFF header -- ftyp + moov{trak{...stbl{stsd
 * (encv > sinf > frma), stsz, stsc, stco, saiz, saio}} + free} -- built the same
 * way the Kotlin MediaCryptoTest builds it, including the two-pass trick that
 * resolves the saio auxiliary-data offset after moov's size is known.
 *
 * Three properties are checked, and they are the ones that matter for playback:
 *   1. the header handed to the player has the ORIGINAL codec back and no trace
 *      of encv/sinf/frma;
 *   2. the single sample is located exactly (offset 4096, length 16);
 *   3. malformed input is REFUSED rather than guessed past -- an auxiliary index
 *      that claims more bytes than exist, and a moov bigger than the file.
 */

#include "hg_mp4.h"
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

/* ---- tiny box builder ---------------------------------------------------- */
static uint8_t tmp[8192];
static size_t tmplen;

static void put(const void *p, size_t n) { memcpy(tmp + tmplen, p, n); tmplen += n; }
static void put_zeros(size_t n) { memset(tmp + tmplen, 0, n); tmplen += n; }
static void put_be32(uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
  put(b, 4);
}
static size_t box_begin(const char *type) {
  size_t at = tmplen;
  put_zeros(4);
  put(type, 4);
  return at;
}
static void box_end(size_t at) {
  uint32_t size = (uint32_t)(tmplen - at);
  tmp[at] = (uint8_t)(size >> 24); tmp[at + 1] = (uint8_t)(size >> 16);
  tmp[at + 2] = (uint8_t)(size >> 8); tmp[at + 3] = (uint8_t)size;
}

static const uint8_t IV8[8] = { 0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7 };

/* ftyp + moov{ trak{ mdia{ minf{ stbl{...} } } } + free{8 bytes of iv} } */
static size_t build_once(const char *codec, int aux_size, int64_t saio_offset) {
  size_t moov_at, trak, mdia, minf, stbl, stsd, encv, sinf, frma, freeb, at;
  tmplen = 0;

  put_be32(16); put("ftyp", 4); put("isom0000", 8);

  /* Keep moov's handle in its own variable: reusing one `at` for the nested
   * boxes would close the wrong box at the end. */
  moov_at = box_begin("moov");
  trak = box_begin("trak");
  mdia = box_begin("mdia");
  minf = box_begin("minf");
  stbl = box_begin("stbl");
  (void)trak; (void)mdia; (void)minf;

  stsd = box_begin("stsd");
  put_zeros(4); put_be32(1);
  encv = box_begin("encv");
  put_zeros(78);
  sinf = box_begin("sinf");
  frma = box_begin("frma");
  put(codec, 4);
  box_end(frma);
  box_end(sinf);
  box_end(encv);
  box_end(stsd);

  at = box_begin("stsz"); put_zeros(4); put_be32(16); put_be32(1); box_end(at);
  at = box_begin("stsc"); put_zeros(4); put_be32(1); put_be32(1); put_be32(1); put_be32(1); box_end(at);
  at = box_begin("stco"); put_zeros(4); put_be32(1); put_be32(4096); box_end(at);
  at = box_begin("saiz"); put_zeros(4); tmp[tmplen++] = (uint8_t)aux_size; put_be32(1); box_end(at);
  at = box_begin("saio"); put_zeros(4); put_be32(1); put_be32((uint32_t)saio_offset); box_end(at);

  box_end(stbl);

  freeb = box_begin("free");
  put(IV8, 8);
  box_end(freeb);

  box_end(moov_at);
  return tmplen;
}

/* Two passes: the saio offset must point at the free box's body, whose position
 * is only known once moov's size is known. Same trick as the Kotlin fixture. */
static size_t build_data(uint8_t *out, const char *codec, int aux_size) {
  size_t n1 = build_once(codec, aux_size, 0);
  size_t n2 = build_once(codec, aux_size, (int64_t)n1 - 8);
  memcpy(out, tmp, n2);
  return n2;
}

/* ---- tests --------------------------------------------------------------- */

static void test_prepare_ok(void) {
  uint8_t data[4096], header[4096];
  HgCencPrepared p;
  size_t n;
  uint8_t key[16];
  int rc;

  memset(key, 0x11, sizeof key);
  n = build_data(data, "avc1", 8);
  printf("prepare() on a well-formed header (%u bytes)\n", (unsigned)n);

  rc = hg_mp4_prepare(data, n, key, header, &p);
  ok("returns 0", rc == 0);
  if (rc != 0) { printf("        rc=%d\n", rc); return; }

  ok("one sample found", p.count == 1);
  ok("sample is [4096, 4112)", p.samples[0].start == 4096 && p.samples[0].end == 4112);
  ok("sample IV is the auxiliary data", memcmp(p.samples[0].iv, IV8, 8) == 0);

  {
    /* String(header) over the whole buffer must see avc1 but no encv/sinf. */
    int saw_avc1 = 0, saw_encv = 0, saw_sinf = 0;
    size_t i;
    for (i = 0; i + 4 <= n; i++) {
      if (memcmp(header + i, "avc1", 4) == 0) saw_avc1 = 1;
      if (memcmp(header + i, "encv", 4) == 0) saw_encv = 1;
      if (memcmp(header + i, "sinf", 4) == 0) saw_sinf = 1;
    }
    ok("header carries the original codec (avc1)", saw_avc1);
    ok("header has no encv left", !saw_encv);
    ok("header has no sinf left", !saw_sinf);
  }

  ok("headerEnd == data length", hg_mp4_header_end(data, n, 5000) == (int)n);

  /* a different original codec must be restored verbatim, not hardcoded */
  n = build_data(data, "hvc1", 8);
  rc = hg_mp4_prepare(data, n, key, header, &p);
  {
    int saw = 0; size_t i;
    for (i = 0; i + 4 <= n; i++) if (memcmp(header + i, "hvc1", 4) == 0) saw = 1;
    ok("hvc1 (HEVC) is restored too", rc == 0 && saw);
  }
}

static void test_fails_closed(void) {
  uint8_t data[4096], header[4096];
  HgCencPrepared p;
  size_t n;
  uint8_t key[16];
  int rc;

  memset(key, 0x11, sizeof key);
  printf("malformed input is refused, not guessed past\n");

  /* aux_size 16 against only 8 bytes of auxiliary data: the index claims more
   * than exists, so the whole header must be rejected. */
  n = build_data(data, "avc1", 16);
  rc = hg_mp4_prepare(data, n, key, header, &p);
  ok("truncated auxiliary data is rejected", rc < 0);

  /* moov claiming 9,000,000 bytes exceeds the 8 MiB cap, so it is refused even
   * though the file supposedly has 10,000,000 bytes. */
  {
    uint8_t bad[24];
    bad[0] = 0; bad[1] = 0; bad[2] = 0; bad[3] = 16;      /* ftyp size */
    memcpy(bad + 4, "ftyp", 4);
    memset(bad + 8, 0, 8);
    bad[16] = (uint8_t)(9000000 >> 24); bad[17] = (uint8_t)(9000000 >> 16);
    bad[18] = (uint8_t)(9000000 >> 8);  bad[19] = (uint8_t)9000000;
    memcpy(bad + 20, "moov", 4);
    ok("oversized moov is rejected",
       hg_mp4_header_end(bad, sizeof bad, 10000000) < 0);
  }

  /* a container that does not start with ftyp is not ours */
  {
    uint8_t junk[32];
    memset(junk, 0x42, sizeof junk);
    ok("non-MP4 container is rejected", hg_mp4_header_end(junk, sizeof junk, 1000) < 0);
  }

  /* NULL key means "unencrypted": header copied, no samples, no error */
  n = build_data(data, "avc1", 8);
  rc = hg_mp4_prepare(data, n, NULL, header, &p);
  ok("NULL key yields header-only success", rc == 0 && p.count == 0);
}

int main(void) {
  printf("hg_mp4 tests (i386 host build)\n");
  printf("sizeof(long)=%d  sizeof(void*)=%d\n\n", (int)sizeof(long), (int)sizeof(void *));

  test_prepare_ok();
  printf("\n");
  test_fails_closed();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
