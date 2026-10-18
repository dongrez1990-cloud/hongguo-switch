/*
 * Vector tests for hg_crypto.c.
 *
 * Two kinds of check, deliberately kept separate:
 *   * MD5 / SHA-512 / SM3 are checked against their PUBLISHED test vectors
 *     (RFC 1321, FIPS 180-4, GB/T 32905-2016). Independent ground truth.
 *   * The vendor's abridged "RC4" has no published vector, so it is checked
 *     against output captured from the reference implementation
 *     (server/vendor/hongguo.cjs rc4Gorgon) by tools/gen_crypto_vectors.mjs.
 *     That pins the port to observed behaviour but cannot prove the reference
 *     itself is right -- it only proves we agree with it.
 *
 * Prints one line per check and exits non-zero on any failure.
 * Build with TinyCC (i386 Windows) -- see run.ps1.
 */

#include "hg_crypto.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void hex_of(const unsigned char *b, size_t n, char *out) { hg_hex(b, n, out); }

static void expect_hex(const char *what, const unsigned char *got, size_t gotlen,
                       const char *want) {
  char buf[257];
  checks++;
  hex_of(got, gotlen, buf);
  if (strcmp(buf, want) == 0) {
    printf("  PASS  %-34s %s\n", what, buf);
  } else {
    failures++;
    printf("  FAIL  %-34s\n        got  %s\n        want %s\n", what, buf, want);
  }
}

/* ---- vectors captured from the reference implementation ---------------- */
/* Regenerate with: node tools/gen_crypto_vectors.mjs <hongguo.cjs> > tests/vectors_gen.h
 * RC4 has no published vector (the vendor's variant is not RC4), and AesV3 is
 * a protocol-specific construction, so both are pinned to observed reference
 * output. That proves we agree with the reference; it cannot prove the
 * reference itself is correct. */
#include "vectors_gen.h"

static void test_md5(void) {
  unsigned char d[16];
  printf("MD5 (RFC 1321 vectors)\n");
  hg_md5("", 0, d);
  expect_hex("md5(\"\")", d, 16, "d41d8cd98f00b204e9800998ecf8427e");
  hg_md5("abc", 3, d);
  expect_hex("md5(\"abc\")", d, 16, "900150983cd24fb0d6963f7d28e17f72");
  hg_md5("The quick brown fox jumps over the lazy dog", 43, d);
  expect_hex("md5(quick brown fox)", d, 16, "9e107d9d372bb6826bd81d3542a419d6");
  /* multi-block + streaming split must agree with the one-shot call */
  {
    static const char *longmsg = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned char a[16], b[16];
    hg_md5_ctx ctx;
    hg_md5(longmsg, 62, a);
    hg_md5_init(&ctx);
    hg_md5_update(&ctx, longmsg, 7);
    hg_md5_update(&ctx, longmsg + 7, 1);
    hg_md5_update(&ctx, longmsg + 8, 54);
    hg_md5_final(&ctx, b);
    checks++;
    if (memcmp(a, b, 16) == 0) printf("  PASS  %-34s %s\n", "md5 streaming split", "matches one-shot");
    else { failures++; printf("  FAIL  %-34s\n", "md5 streaming split"); }
  }
}

static void test_sha512(void) {
  unsigned char d[64];
  printf("SHA-512 (FIPS 180-4 vectors)\n");
  hg_sha512("", 0, d);
  expect_hex("sha512(\"\")", d, 64,
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
  hg_sha512("abc", 3, d);
  expect_hex("sha512(\"abc\")", d, 64,
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  hg_sha512("The quick brown fox jumps over the lazy dog", 43, d);
  expect_hex("sha512(quick brown fox)", d, 64,
    "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb64"
    "2e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6");
  /* cross-block boundary: 111/112/113 bytes straddle the 112-byte limit */
  {
    unsigned char buf[200];
    unsigned char a[64], b[64];
    size_t n;
    int okall = 1;
    for (n = 0; n < sizeof buf; n++) buf[n] = (unsigned char)(n * 31 + 7);
    for (n = 110; n <= 114; n++) {
      hg_sha512_ctx ctx;
      hg_sha512(buf, n, a);
      hg_sha512_init(&ctx);
      hg_sha512_update(&ctx, buf, n / 2);
      hg_sha512_update(&ctx, buf + n / 2, n - n / 2);
      hg_sha512_final(&ctx, b);
      if (memcmp(a, b, 64) != 0) okall = 0;
    }
    checks++;
    if (okall) printf("  PASS  %-34s %s\n", "sha512 split across 112-byte edge", "matches");
    else { failures++; printf("  FAIL  %-34s\n", "sha512 split across 112-byte edge"); }
  }
}

static void test_sm3(void) {
  unsigned char d[32];
  printf("SM3 (GB/T 32905-2016 vectors)\n");
  hg_sm3("abc", 3, d);
  expect_hex("sm3(\"abc\")", d, 32,
    "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
  /* the standard's second vector: "abcd" repeated 16 times (64 bytes) */
  {
    char msg[65];
    int i;
    for (i = 0; i < 16; i++) memcpy(msg + i * 4, "abcd", 4);
    msg[64] = '\0';
    hg_sm3(msg, 64, d);
    expect_hex("sm3(\"abcd\" x16)", d, 32,
      "debe9ff92275b8a138604889c18e5a4d6fdb70e5387e5765293dcba39c0c5732");
  }
}

static void test_rc4_bytes(void) {
  unsigned char out[20];
  hg_rc4_ctx ctx;
  size_t i;
  int ok = 1;
  hg_rc4_init(&ctx, RC4V_KEY, sizeof RC4V_KEY);
  for (i = 0; i < 20; i++) out[i] = hg_rc4_next(&ctx, RC4V_DATA[i]);
  for (i = 0; i < 20; i++) if (out[i] != RC4V_OUT[i]) ok = 0;
  checks++;
  if (ok) {
    char buf[64];
    hex_of(out, 20, buf);
    printf("  PASS  %-34s %s\n", "rc4Gorgon byte-at-a-time", buf);
  } else {
    char got[64], want[64];
    failures++;
    hex_of(out, 20, got);
    hex_of(RC4V_OUT, 20, want);
    printf("  FAIL  %-34s\n        got  %s\n        want %s\n", "rc4Gorgon", got, want);
  }
}

static void test_aesv3(void) {
  static const uint32_t khronos[4] = { 1800500000u, 1800500001u, 1800500002u, 1800500003u };
  static const unsigned char *const want[4] = { AV3_OUT0, AV3_OUT1, AV3_OUT2, AV3_OUT3 };
  unsigned char got[257];
  int w;

  printf("AesV3 (captured from the reference class, all 4 word branches)\n");
  for (w = 0; w < 4; w++) {
    hg_aesv3_ctx ctx;
    hg_aesv3_init(&ctx, AV3_KEY, khronos[w]);
    hg_aesv3_encrypt(&ctx, AV3_IV, AV3_SRC, 256, got);
    checks++;
    if (memcmp(got, want[w], 257) == 0) {
      printf("  PASS  %-34s word=%d  (%d bytes)\n", "AesV3 encrypt", w, 257);
    } else {
      size_t bad = 0;
      while (bad < 257 && got[bad] == want[w][bad]) bad++;
      failures++;
      printf("  FAIL  %-34s word=%d first differs at byte %d: got %02x want %02x\n",
             "AesV3 encrypt", w, (int)bad, got[bad], want[w][bad]);
    }
  }
}

static void test_helpers(void) {
  unsigned char raw[8], back[16];
  char b64[64], hexbuf[64];
  size_t n = 0;
  printf("helpers\n");

  hg_hex((const unsigned char *)"\x00\x01\xab\xff", 4, hexbuf);
  checks++;
  if (strcmp(hexbuf, "0001abff") == 0) printf("  PASS  %-34s %s\n", "hg_hex", hexbuf);
  else { failures++; printf("  FAIL  %-34s got %s\n", "hg_hex", hexbuf); }

  hg_hex_upper((const unsigned char *)"\xde\xad", 2, hexbuf);
  checks++;
  if (strcmp(hexbuf, "DEAD") == 0) printf("  PASS  %-34s %s\n", "hg_hex_upper", hexbuf);
  else { failures++; printf("  FAIL  %-34s got %s\n", "hg_hex_upper", hexbuf); }

  checks++;
  if (hg_unhex("00ff10", raw, sizeof raw, &n) && n == 3 && raw[0] == 0 && raw[1] == 0xff && raw[2] == 0x10)
    printf("  PASS  %-34s %s\n", "hg_unhex", "ok");
  else { failures++; printf("  FAIL  %-34s\n", "hg_unhex"); }

  checks++;
  if (!hg_unhex("0g", raw, sizeof raw, &n)) printf("  PASS  %-34s %s\n", "hg_unhex rejects bad input", "ok");
  else { failures++; printf("  FAIL  %-34s\n", "hg_unhex rejects bad input"); }

  /* base64 round trip, including the 1- and 2-byte padding cases */
  {
    static const char *cases[] = { "a", "ab", "abc", "abcd", "hello world" };
    int i, okall = 1;
    for (i = 0; i < 5; i++) {
      size_t len = strlen(cases[i]), dn = 0;
      size_t elen = hg_base64_encode((const unsigned char *)cases[i], len, b64, sizeof b64);
      unsigned char dec[32];
      if (elen == 0) { okall = 0; continue; }
      if (!hg_base64_decode(b64, dec, sizeof dec, &dn)) { okall = 0; continue; }
      if (dn != len || memcmp(dec, cases[i], len) != 0) okall = 0;
    }
    checks++;
    if (okall) printf("  PASS  %-34s %s\n", "base64 encode/decode round trip", "ok");
    else { failures++; printf("  FAIL  %-34s\n", "base64 encode/decode round trip"); }
  }

  /* the exact base64 the vendor sbox arrives in: decode length must be 1024 */
  {
    unsigned char dec[2048];
    size_t dn = 0;
    static const char *b64sbox = "+n0Ia5xZs0sEXznQOEqRmQBnpiCf9U2Ccybu3xhmgzOAAxn72f6uqqmwUsYL83klTni0NqxdGieeiNu9PGPsSRXBMB/cuFbUbM3KCUPINaPvHvSW0vwOcnuUhNHqRVpiAj/TEoE0K91+5ijypUYTATsh9mE3KSoN7Yyvv51cuyR2D3XkU4nhmI2xmmVwT1RMWKtub4sjxAcRDLrPoKSO2AU9FLLadMPX577Wf95IFj6FkKFVt3dCIsmGUC4X+WQxLJvxbRxEaOPpqJOXyzJX6+Vxaq3AzMfF/WAdoi1Hp+JRaV56zgpBtpWP97mH4DoGEIq1+FvV8LyS/3wvwugbQOwb2r26mJEMsiuDQTRn+wrYdrVGBVlhI3WQhyrjUBVMrLF5667llUcEaPCGPVGLD8qO5LlO8hKCvA7V9+8oJc9bXelqVQLhM76T5/WtnT45JKji+hdX0HoNCDDWuKON/QeaxB5uImSX0h2wv0VmP2zd2yeApxHcpsVS+MC2yFwAc2B7oBkTqsk1SEvTpM2fmfMQREBUfin0Bh+iq6EvPPavhWI2IX9e3yAas7Tm/3KEj2UmlFp36kN4x0rMLBRrxuh0U/zUHM4xcAMYjJY4MonxOl/X+alpt2M3WMI7w3HLnpIBigtNiJu7T21v4P6lSd5WFgntnMEt7oF92XzRLkJbTcGmXepE/UVOG6E/0YnhfS+q26utWcuxzpooyeD2cDlK1/8w9d28VzsRjbLuALbmGlp8+d7EzS6Au7lMpZ+ECMZvQmzwJ+eLOpxR+2chdUExp8ogQyq3v9l68rX4jCwjg0+PYKAEEzcU4wHFY2ZcdIHfWL1okD3SszT0GZMyKdZJrg1L2AeerB4tC0C4crp2EHGo5FYdSP7lwkeR2ocmnR+Ia8CYviUJlzOjhRZef9xuVOn3qcjow3fQgivsAmKKkg4+sA8F8/GWeDiGNhg8JM8KtFPMYWWkx5TVFX5t73kiNRJqjlIGVXtGZFCV4gzt0xcDopuZ6xz8r9Rzafpf9ywev8jh8592gHFIqpStZPuJxmDDMrNN0uBE3V+oscdoIzTJbRJ/t+sVvqnReJOgDJKk10fjisJwqyZBmnmn2BSFj8BvVtCMEbkuPOKdzw7eA11GPs04Qw8zWtkaZWwiO/wwpojqN6K0jY5RnNZA7vn4hPSul+nKCkVnVwQvg1zVxcSCtqORmB9KrJaBbssbCQivGJVJfVTt+hYxOtq4ZvWl8f4QAQZ0zGPffCgl9s6yT4vlvIdpu4YhBwA25wtQWZsc6GJYGWHyvSdeuh3mmUI9DSq13Fsp8C1MU3tqc04/df9LoTUXVXI5INOw/e8C7Hd+5CvbkMEFnnrUUmskEw==";
    checks++;
    if (hg_base64_decode(b64sbox, dec, sizeof dec, &dn) && dn == 1024)
      printf("  PASS  %-34s %s\n", "vendor sbox base64 decodes to 1024", "ok");
    else { failures++; printf("  FAIL  %-34s decoded %d bytes\n", "vendor sbox base64", (int)dn); }
  }
}

int main(void) {
  printf("hg_crypto vector tests (i386 host build)\n");
  printf("sizeof(long)=%d  sizeof(void*)=%d\n\n", (int)sizeof(long), (int)sizeof(void *));

  test_md5();
  printf("\n");
  test_sha512();
  printf("\n");
  test_sm3();
  printf("\n");
  test_rc4_bytes();
  printf("\n");
  test_aesv3();
  printf("\n");
  test_helpers();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
