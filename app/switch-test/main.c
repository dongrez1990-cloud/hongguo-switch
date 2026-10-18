/*
 * On-device verification for the 红果 crypto/media stack.
 *
 * The host suites (i386 TinyCC) prove the algorithms against published vectors and
 * against the reference implementation. This program proves something the host
 * cannot: that the same sources LINK and RUN as a Nintendo Switch homebrew binary,
 * with libnx, and produce the same answers on a real Cortex-A57.
 *
 * Coverage deliberately mirrors the host suites. Anything NOT here is a gap in the
 * hardware evidence, so the gap is kept as small as is practical: the only host
 * check with no on-device counterpart is nothing algorithmic -- the counts differ
 * slightly because the host splits some checks more finely.
 *
 * Build with scripts/build-switch-test.ps1, deploy to sdmc:/switch/, run from hbmenu.
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>

#include "hg_crypto.h"
#include "hg_aes.h"
#include "hg_cenc.h"
#include "hg_mp4.h"
#include "hg_sign.h"
#include "hg_router.h"
#include "sign_vectors_gen.h"
#include "hg_media_vectors.h"
#include "vectors_gen.h"

static int g_checks = 0;
static int g_failures = 0;

static void check(const char *what, int cond) {
  g_checks++;
  if (!cond) {
    g_failures++;
    printf("  FAIL %s\n", what);
    consoleUpdate(NULL);
  }
}

static void check_hex(const char *what, const unsigned char *got, size_t n, const char *want) {
  char buf[256];
  hg_hex(got, n, buf);
  g_checks++;
  if (strcmp(buf, want) != 0) {
    g_failures++;
    printf("  FAIL %s\n        got  %s\n        want %s\n", what, buf, want);
    consoleUpdate(NULL);
  }
}

/* The oracle's seeded LCG; draw order is part of the contract. */
static uint32_t g_seed;
static uint32_t lcg(void *user) {
  (void)user;
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed;
}

/* NIST SP 800-38A F.5.1 */
static const uint8_t NIST_KEY[16] = {
  0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
static const uint8_t NIST_IV[16] = {
  0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };
static const uint8_t NIST_PLAIN[64] = {
  0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
  0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
  0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
  0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
static const uint8_t NIST_CIPHER[64] = {
  0x87,0x4d,0x61,0x91,0xb6,0x20,0xe3,0x26,0x1b,0xef,0x68,0x64,0x99,0x0d,0xb6,0xce,
  0x98,0x06,0xf6,0x6b,0x79,0x70,0xfd,0xff,0x86,0x17,0x18,0x7b,0xb9,0xff,0xfd,0xff,
  0x5a,0xe4,0xdf,0x3e,0xdb,0xd5,0xd3,0x5e,0x5b,0x4f,0x09,0x02,0x0d,0xb0,0x3e,0xab,
  0x1e,0x03,0x1d,0xda,0x2f,0xbe,0x03,0xd1,0x79,0x21,0x70,0xa0,0xf3,0x00,0x9c,0xee };

static void run_primitives(void) {
  unsigned char d[64];
  hg_md5_ctx mc;
  hg_sha512_ctx sc;
  unsigned char a[64], b[64];
  static const char *longmsg = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char msg[65];
  int i;

  printf("[1/8] hash primitives\n");
  hg_md5("", 0, d);
  check_hex("md5('')", d, 16, "d41d8cd98f00b204e9800998ecf8427e");
  hg_md5("abc", 3, d);
  check_hex("md5(abc)", d, 16, "900150983cd24fb0d6963f7d28e17f72");
  hg_md5("The quick brown fox jumps over the lazy dog", 43, d);
  check_hex("md5(quick brown fox)", d, 16, "9e107d9d372bb6826bd81d3542a419d6");

  /* streaming split must agree with the one-shot call */
  hg_md5(longmsg, 62, a);
  hg_md5_init(&mc);
  hg_md5_update(&mc, longmsg, 7);
  hg_md5_update(&mc, longmsg + 7, 1);
  hg_md5_update(&mc, longmsg + 8, 54);
  hg_md5_final(&mc, b);
  check("md5 streaming split", memcmp(a, b, 16) == 0);

  hg_sha512("", 0, d);
  check_hex("sha512('')", d, 64,
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
  hg_sha512("abc", 3, d);
  check_hex("sha512(abc)", d, 64,
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

  /* straddle the 112-byte padding boundary */
  {
    unsigned char buf[200];
    int okall = 1;
    size_t n;
    for (n = 0; n < sizeof buf; n++) buf[n] = (unsigned char)(n * 31 + 7);
    for (n = 110; n <= 114; n++) {
      hg_sha512(buf, n, a);
      hg_sha512_init(&sc);
      hg_sha512_update(&sc, buf, n / 2);
      hg_sha512_update(&sc, buf + n / 2, n - n / 2);
      hg_sha512_final(&sc, b);
      if (memcmp(a, b, 64) != 0) okall = 0;
    }
    check("sha512 split across 112-byte edge", okall);
  }

  hg_sm3("abc", 3, d);
  check_hex("sm3(abc)", d, 32, "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
  for (i = 0; i < 16; i++) memcpy(msg + i * 4, "abcd", 4);
  msg[64] = '\0';
  hg_sm3(msg, 64, d);
  check_hex("sm3(abcd x16)", d, 32,
    "debe9ff92275b8a138604889c18e5a4d6fdb70e5387e5765293dcba39c0c5732");

  /* the vendor's abridged RC4 -- captured from the reference, no published vector */
  {
    hg_rc4_ctx rc;
    unsigned char out[20];
    hg_rc4_init(&rc, RC4V_KEY, sizeof RC4V_KEY);
    hg_rc4_apply(&rc, RC4V_DATA, out, sizeof RC4V_DATA);
    check("vendor abridged RC4", memcmp(out, RC4V_OUT, 20) == 0);
  }
  consoleUpdate(NULL);
}

static void run_aes_ctr(void) {
  uint8_t out[64];
  printf("[2/8] AES-128 / CTR\n");
  hg_aes_ctr(NIST_KEY, NIST_IV, 0, NIST_PLAIN, 64, out);
  check_hex("nist ctr", out, 64,
    "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
    "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
  consoleUpdate(NULL);
}

static void run_signer(void) {
  int i;
  printf("[3/8] signer vs signing.json (%d rows x 5 fields)\n", SV_COUNT);
  for (i = 0; i < SV_COUNT; i++) {
    const SignVector *v = &SIGN_VECTORS[i];
    const char *qmark = strchr(v->url, '?');
    const char *query = qmark ? qmark + 1 : v->url;
    const uint8_t *body = (const uint8_t *)v->body;
    uint8_t q_sm3[32], b_md5[16];
    char buf[160], b64[HG_HELIOS_MAX], medusa[1600];
    char label[48];

    if (v->body != NULL) hg_md5(v->body, v->body_len, b_md5);
    else memset(b_md5, 0, sizeof b_md5);
    hg_sm3(query, strlen(query), q_sm3);

    sprintf(label, "row %d branchOf", i);
    check(label, hg_branch_of(query, body, v->body_len, v->ts) == 0);

    sprintf(label, "row %d hashF13", i);
    if (!hg_hash_f13(q_sm3, b_md5, v->ts, buf)) { check(label, 0); }
    else { check(label, strcmp(buf, v->hash) == 0); }

    sprintf(label, "row %d xGorgon", i);
    hg_xgorgon(query, body, v->body_len, v->ts, 42131u, buf);
    check(label, strcmp(buf, v->gorgon) == 0);

    /* helios is the row's FIRST draw; medusa continues the same stream */
    g_seed = SV_RNG_SEED;
    sprintf(label, "row %d helios", i);
    if (!hg_helios(v->ts, lcg, NULL, b64, sizeof b64)) { check(label, 0); }
    else { check(label, strcmp(b64, v->helios) == 0); }

    sprintf(label, "row %d medusa", i);
    if (!hg_build_medusa(v->url, body, v->body_len, v->ts, lcg, NULL, medusa, sizeof medusa)) {
      check(label, 0);
    } else {
      check(label, strcmp(medusa, v->medusa) == 0);
    }
  }
  consoleUpdate(NULL);
}

static void run_spade(void) {
  uint8_t key[16];
  char url[256];
  printf("[4/8] spade crypto\n");
  check("spade_a -> content key",
        hg_spade_content_key(SPADE_A_INPUT, key) && memcmp(key, SPADE_A_KEY, 16) == 0);
  url[0] = '\0';
  check("spade URL decrypt",
        hg_spade_decrypt_url(SPADE_INPUT, SPADE_SEED, sizeof SPADE_SEED, url, sizeof url) &&
        strcmp(url, SPADE_EXPECTED) == 0);
  check("malformed inputs rejected",
        !hg_spade_content_key("AAAA", key) &&
        !hg_spade_decrypt_url("AAAA", SPADE_SEED, sizeof SPADE_SEED, url, sizeof url));
  consoleUpdate(NULL);
}

static void run_router(void) {
  static const char *SAMPLE =
    "<script>window._ROUTER_DATA = "
    "{\"loaderData\":{\"x\":\"a}\\\"b\",\"list\":[1,2]}};</script>";
  static const char *EXPECTED =
    "{\"loaderData\":{\"x\":\"a}\\\"b\",\"list\":[1,2]}}";
  static const char *UNBALANCED = "var _ROUTER_DATA = {\"a\":1";
  char buf[512];
  printf("[5/8] _ROUTER_DATA scraping\n");
  check("brace in a string does not truncate",
        hg_router_extract(SAMPLE, strlen(SAMPLE), buf, sizeof buf) > 0 &&
        strcmp(buf, EXPECTED) == 0);
  check("escaped quote unescapes",
        hg_json_find_string(buf, "x", buf + 256, sizeof buf - 256) == 4 &&
        strcmp(buf + 256, "a}\"b") == 0);
  check("unbalanced braces refused",
        hg_router_extract(UNBALANCED, strlen(UNBALANCED), buf, sizeof buf) == HG_ROUTER_ERR_INCOMPLETE);
  check("missing marker refused",
        hg_router_extract("<html>nothing</html>", 19, buf, sizeof buf) == HG_ROUTER_ERR_NO_DATA);
  consoleUpdate(NULL);
}

/* The check that caught a real bug on the host: decrypting ANY sub-range of a
 * buffer at its true file offset must yield exactly the matching plaintext. A
 * whole-buffer test passes even when the CTR keystream skip is off by a block. */
static void run_cenc(void) {
  uint8_t source[23 + 64 + 12], expected[23 + 64 + 12], got[64], tail[25];
  uint8_t whole[23 + 64 + 12];
  HgCencPrepared p;
  int start, length, bad = 0;

  printf("[6/8] CENC sample walk\n");

  memset(source, 0x66, 23);
  memcpy(source + 23, NIST_CIPHER, 64);
  memset(source + 87, 0x77, 12);
  memset(expected, 0x66, 23);
  memcpy(expected + 23, NIST_PLAIN, 64);
  memset(expected + 87, 0x77, 12);

  memset(&p, 0, sizeof p);
  p.count = 1;
  p.samples[0].start = 23;
  p.samples[0].end = 87;
  p.samples[0].encrypted_offset = 0;
  memcpy(p.samples[0].iv, NIST_IV, 16);

  hg_cenc_decrypt(source, sizeof source, 0, NIST_KEY, &p, whole);
  check("whole buffer: prefix+plaintext+suffix", memcmp(whole, expected, sizeof expected) == 0);

  for (start = 0; start < (int)sizeof source && !bad; start++) {
    int maxlen = (int)sizeof source - start;
    if (maxlen > 29) maxlen = 29;
    for (length = 1; length <= maxlen; length++) {
      hg_cenc_decrypt(source + start, (size_t)length, start, NIST_KEY, &p, got);
      if (memcmp(got, expected + start, (size_t)length) != 0) { bad = 1; break; }
    }
  }
  check("every start x length combination", !bad);

  /* two samples, 2 clear bytes between them, CTR offset continuing across the gap */
  {
    uint8_t s2[3 + 21 + 2 + 43], e2[3 + 21 + 2 + 43], g2[3 + 21 + 2 + 43];
    HgCencPrepared q;
    memset(s2, 0, sizeof s2);
    memcpy(s2, (const uint8_t[]){1,2,3}, 3);
    memcpy(s2 + 3, NIST_CIPHER, 21);
    memcpy(s2 + 24, (const uint8_t[]){4,5}, 2);
    memcpy(s2 + 26, NIST_CIPHER + 21, 43);
    memcpy(e2, (const uint8_t[]){1,2,3}, 3);
    memcpy(e2 + 3, NIST_PLAIN, 21);
    memcpy(e2 + 24, (const uint8_t[]){4,5}, 2);
    memcpy(e2 + 26, NIST_PLAIN + 21, 43);

    memset(&q, 0, sizeof q);
    q.count = 2;
    q.samples[0].start = 3;  q.samples[0].end = 24;
    q.samples[0].encrypted_offset = 0;
    memcpy(q.samples[0].iv, NIST_IV, 16);
    q.samples[1].start = 26; q.samples[1].end = 69;
    q.samples[1].encrypted_offset = 21;
    memcpy(q.samples[1].iv, NIST_IV, 16);

    hg_cenc_decrypt(s2, sizeof s2, 0, NIST_KEY, &q, g2);
    check("two samples with a clear gap", memcmp(g2, e2, sizeof e2) == 0);

    hg_cenc_decrypt(s2 + 22, 25, 22, NIST_KEY, &q, tail);
    check("range straddling both samples", memcmp(tail, e2 + 22, 25) == 0);
  }
  consoleUpdate(NULL);
}

/* --- the synthetic ISO-BMFF fixture, same construction as the host test --- */
static uint8_t tmp[8192];
static size_t tmplen;
static const uint8_t IV8[8] = { 0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7 };

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

static size_t build_once(const char *codec, int aux_size, int64_t saio_offset) {
  size_t moov_at, stbl, stsd, encv, sinf, frma, freeb, at;
  tmplen = 0;
  put_be32(16); put("ftyp", 4); put("isom0000", 8);
  moov_at = box_begin("moov");
  box_begin("trak"); box_begin("mdia"); box_begin("minf");
  stbl = box_begin("stbl");
  stsd = box_begin("stsd");
  put_zeros(4); put_be32(1);
  encv = box_begin("encv");
  put_zeros(78);
  sinf = box_begin("sinf");
  frma = box_begin("frma");
  put(codec, 4);
  box_end(frma); box_end(sinf); box_end(encv); box_end(stsd);
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

/* Two passes: saio must point at the free box body, whose offset is only known
 * once moov's size is known. */
static size_t build_data(uint8_t *out, const char *codec, int aux_size) {
  size_t n1 = build_once(codec, aux_size, 0);
  size_t n2 = build_once(codec, aux_size, (int64_t)n1 - 8);
  memcpy(out, tmp, n2);
  return n2;
}

static int contains(const uint8_t *buf, size_t n, const char *needle) {
  size_t i;
  for (i = 0; i + 4 <= n; i++) if (memcmp(buf + i, needle, 4) == 0) return 1;
  return 0;
}

static void run_mp4(void) {
  uint8_t data[4096], header[4096];
  HgCencPrepared p;
  size_t n;
  uint8_t key[16];
  int rc;
  static const char *EXPECTED = "{\"loaderData\":{\"x\":\"a}\\\"b\",\"list\":[1,2]}}";
  (void)EXPECTED;

  memset(key, 0x11, sizeof key);
  printf("[7/8] MP4 header parsing\n");

  n = build_data(data, "avc1", 8);
  rc = hg_mp4_prepare(data, n, key, header, &p);
  check("prepare returns 0", rc == 0);
  if (rc == 0) {
    check("one sample found", p.count == 1);
    check("sample is [4096, 4112)", p.samples[0].start == 4096 && p.samples[0].end == 4112);
    check("sample IV is the auxiliary data", memcmp(p.samples[0].iv, IV8, 8) == 0);
    check("header restores avc1", contains(header, n, "avc1"));
    check("header has no encv", !contains(header, n, "encv"));
    check("header has no sinf", !contains(header, n, "sinf"));
    check("headerEnd == data length", hg_mp4_header_end(data, n, 5000) == (int)n);
  } else {
    printf("        prepare rc=%d\n", rc);
  }

  /* a different original codec must be restored verbatim, not hardcoded */
  n = build_data(data, "hvc1", 8);
  rc = hg_mp4_prepare(data, n, key, header, &p);
  check("hvc1 (HEVC) restored too", rc == 0 && contains(header, n, "hvc1"));

  /* fails closed */
  n = build_data(data, "avc1", 16);
  check("truncated auxiliary data rejected", hg_mp4_prepare(data, n, key, header, &p) < 0);

  {
    uint8_t bad[24];
    bad[0] = 0; bad[1] = 0; bad[2] = 0; bad[3] = 16;
    memcpy(bad + 4, "ftyp", 4);
    memset(bad + 8, 0, 8);
    bad[16] = (uint8_t)(9000000 >> 24); bad[17] = (uint8_t)(9000000 >> 16);
    bad[18] = (uint8_t)(9000000 >> 8);  bad[19] = (uint8_t)9000000;
    memcpy(bad + 20, "moov", 4);
    check("oversized moov rejected", hg_mp4_header_end(bad, sizeof bad, 10000000) < 0);
  }
  {
    uint8_t junk[32];
    memset(junk, 0x42, sizeof junk);
    check("non-MP4 container rejected", hg_mp4_header_end(junk, sizeof junk, 1000) < 0);
  }

  n = build_data(data, "avc1", 8);
  rc = hg_mp4_prepare(data, n, NULL, header, &p);
  check("NULL key yields header-only success", rc == 0 && p.count == 0);
  consoleUpdate(NULL);
}

static void run_misc(void) {
  uint8_t c[16];
  printf("[8/8] AesV3 / counter / helpers\n");

  /* AesV3: all four (timestamp & 3) branches -- the S-box quarter, both
   * permutation tables and the initial constant all change with it */
  {
    static const uint32_t khronos[4] = { 1800500000u, 1800500001u, 1800500002u, 1800500003u };
    const unsigned char *want[4];
    unsigned char got[257];
    int w;
    want[0] = AV3_OUT0; want[1] = AV3_OUT1; want[2] = AV3_OUT2; want[3] = AV3_OUT3;
    for (w = 0; w < 4; w++) {
      hg_aesv3_ctx ctx;
      char label[40];
      hg_aesv3_init(&ctx, AV3_KEY, khronos[w]);
      hg_aesv3_encrypt(&ctx, AV3_IV, AV3_SRC, 256, got);
      sprintf(label, "AesV3 word=%d", w);
      check(label, memcmp(got, want[w], 257) == 0);
    }
  }

  /* counter arithmetic, including carry across the 64-bit halves */
  memcpy(c, NIST_IV, 16);
  hg_aes_counter_add(c, 1);
  check_hex("iv + 1", c, 16, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff00");
  memset(c, 0, 16); memset(c + 8, 0xff, 8);
  hg_aes_counter_add(c, 1);
  check_hex("carry crosses into the high half", c, 16, "00000000000000010000000000000000");
  memset(c, 0xff, 16);
  hg_aes_counter_add(c, 1);
  check_hex("overflow past 2^128 wraps to zero", c, 16, "00000000000000000000000000000000");
  /* helpers. Deliberately NOT checking the 1024-byte vendor S-box base64 here:
   * that is a generator concern already covered on the host, and transcribing a
   * 1024-character literal into this file would add a copy error for no runtime
   * value. */
  {
    unsigned char dec[16];
    char hexbuf[32];
    size_t dn = 0;
    check("base64 decode", hg_base64_decode("YWJj", dec, sizeof dec, &dn) && dn == 3 && memcmp(dec, "abc", 3) == 0);
    hg_hex((const unsigned char *)"\x00\x01\xab\xff", 4, hexbuf);
    check("hg_hex", memcmp(hexbuf, "0001abff", 8) == 0);
    hg_hex_upper((const unsigned char *)"\xde\xad", 2, hexbuf);
    check("hg_hex_upper", memcmp(hexbuf, "DEAD", 4) == 0);
    {
      uint8_t raw[8];
      size_t rn = 0;
      check("hg_unhex", hg_unhex("00ff10", raw, sizeof raw, &rn) && rn == 3 && raw[1] == 0xff);
      check("hg_unhex rejects bad input", !hg_unhex("0g", raw, sizeof raw, &rn));
    }
  }
  consoleUpdate(NULL);
}

int main(int argc, char **argv) {
  PadState pad;
  (void)argc; (void)argv;
  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  printf("hongguo crypto/media stack -- on-device verification\n");
  printf("target: aarch64 Cortex-A57 (real hardware)\n\n");

  run_primitives();
  run_aes_ctr();
  run_signer();
  run_spade();
  run_router();
  run_cenc();
  run_mp4();
  run_misc();

  printf("\n=== %d checks, %d failure(s) ===\n", g_checks, g_failures);
  printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
  printf("\nPress + to exit.\n");
  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }

  consoleExit(NULL);
  return g_failures == 0 ? 0 : 1;
}
