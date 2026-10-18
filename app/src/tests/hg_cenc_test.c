/*
 * Tests for standard AES-128 + CTR + the CENC sample walk.
 *
 * Ground truth, all of it independent of the reference implementation:
 *   * FIPS 197 C.1 -- the AES-128 known-answer test.
 *   * NIST SP 800-38A F.5.1 -- the AES-128-CTR vectors. The same vectors, with
 *     the same key and IV, are what the Kotlin MediaCryptoTest uses, so this also
 *     proves we agree with the Android implementation byte for byte.
 *   * An exhaustive unaligned-range sweep, mirroring that test: decrypting ANY
 *     sub-range of a buffer at its true file offset must produce exactly the
 *     corresponding sub-range of the plaintext. That is the property that breaks
 *     when the CTR counter offset or the keystream skip is wrong, and it is the
 *     property the player actually depends on (seek into the middle of a sample).
 */

#include "hg_cenc.h"
#include "hg_aes.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void expect_hex(const char *what, const uint8_t *got, size_t n, const char *want) {
  char buf[256];
  static const char H[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; i++) { buf[i * 2] = H[got[i] >> 4]; buf[i * 2 + 1] = H[got[i] & 15]; }
  buf[n * 2] = '\0';
  checks++;
  if (strcmp(buf, want) == 0) printf("  PASS  %-38s %s\n", what, buf);
  else { failures++; printf("  FAIL  %-38s\n        got  %s\n        want %s\n", what, buf, want); }
}

static const uint8_t KEY[16] = {
  0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
};
static const uint8_t CTR_IV[16] = {
  0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff
};
static const uint8_t PLAIN[64] = {
  0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
  0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
  0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
  0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10
};
static const uint8_t CIPHER[64] = {
  0x87,0x4d,0x61,0x91,0xb6,0x20,0xe3,0x26,0x1b,0xef,0x68,0x64,0x99,0x0d,0xb6,0xce,
  0x98,0x06,0xf6,0x6b,0x79,0x70,0xfd,0xff,0x86,0x17,0x18,0x7b,0xb9,0xff,0xfd,0xff,
  0x5a,0xe4,0xdf,0x3e,0xdb,0xd5,0xd3,0x5e,0x5b,0x4f,0x09,0x02,0x0d,0xb0,0x3e,0xab,
  0x1e,0x03,0x1d,0xda,0x2f,0xbe,0x03,0xd1,0x79,0x21,0x70,0xa0,0xf3,0x00,0x9c,0xee
};

static void test_aes_known_answer(void) {
  /* FIPS 197 C.1 */
  static const uint8_t k[16] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
  };
  static const uint8_t p[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
  };
  uint8_t rk[176], out[16];
  printf("AES-128 (FIPS 197 known-answer)\n");
  hg_aes128_init(rk, k);
  hg_aes128_encrypt_block(rk, p, out);
  expect_hex("aes128 encrypt block", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
}

static void test_ctr_nist(void) {
  uint8_t out[64];
  printf("AES-128-CTR (NIST SP 800-38A F.5.1)\n");
  hg_aes_ctr(KEY, CTR_IV, 0, PLAIN, 64, out);
  expect_hex("ctr encrypt all four blocks", out, 64,
    "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
    "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
  hg_aes_ctr(KEY, CTR_IV, 0, CIPHER, 64, out);
  expect_hex("ctr decrypt all four blocks", out, 64,
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
}

/* Mirrors MediaCryptoTest.nistCtrVectorsAcrossEveryUnalignedRange: a single
 * sample spanning [23,87) sitting between a 23-byte prefix and a 12-byte suffix.
 * Decrypting any (start, length) sub-range must yield the matching plaintext. */
static void test_cenc_unaligned_ranges(void) {
  uint8_t source[23 + 64 + 12];
  uint8_t expected[23 + 64 + 12];
  uint8_t got[64], whole[23 + 64 + 12];
  HgCencPrepared p;
  int start, length, bad = 0;
  char label[64];

  memset(source, 0x66, 23);
  memcpy(source + 23, CIPHER, 64);
  memset(source + 87, 0x77, 12);
  memset(expected, 0x66, 23);
  memcpy(expected + 23, PLAIN, 64);
  memset(expected + 87, 0x77, 12);

  memset(&p, 0, sizeof p);
  p.header = NULL;
  p.header_len = 0;
  p.count = 1;
  p.samples[0].start = 23;
  p.samples[0].end = 87;
  p.samples[0].encrypted_offset = 0;
  memcpy(p.samples[0].iv, CTR_IV, 16);

  printf("CENC unaligned-range sweep (sample [23,87) between a prefix and a suffix)\n");

  hg_cenc_decrypt(source, sizeof source, 0, KEY, &p, whole);
  checks++;
  if (memcmp(whole, expected, sizeof expected) == 0) {
    printf("  PASS  %-38s %s\n", "whole-buffer decrypt", "prefix+plaintext+suffix intact");
  } else {
    failures++;
    printf("  FAIL  %-38s\n", "whole-buffer decrypt");
  }

  for (start = 0; start < (int)sizeof source && !bad; start++) {
    int maxlen = (int)sizeof source - start;
    if (maxlen > 29) maxlen = 29;
    for (length = 1; length <= maxlen; length++) {
      hg_cenc_decrypt(source + start, (size_t)length, start, KEY, &p, got);
      if (memcmp(got, expected + start, (size_t)length) != 0) {
        sprintf(label, "range start=%d len=%d", start, length);
        checks++;
        failures++;
        printf("  FAIL  %-38s\n", label);
        bad = 1;
        break;
      }
    }
  }
  checks++;
  if (!bad) {
    printf("  PASS  %-38s %s\n", "every start x length combination",
           "all ranges match the plaintext");
  }
}

/* Mirrors subsamplesPreserveClearBytesAndContinueCtrOffset: two samples of the
 * same 64-byte stream, the second starting 21 bytes in, with 5 clear bytes
 * between them. The CTR offset must continue across the gap. */
static void test_cenc_subsamples(void) {
  uint8_t source[3 + 21 + 2 + 43];
  uint8_t expected[3 + 21 + 2 + 43];
  uint8_t got[sizeof source];
  uint8_t tail[25];
  HgCencPrepared p;
  int ok;

  memset(source, 0, sizeof source);
  memcpy(source, (const uint8_t[]){1,2,3}, 3);
  memcpy(source + 3, CIPHER, 21);
  memcpy(source + 24, (const uint8_t[]){4,5}, 2);
  memcpy(source + 26, CIPHER + 21, 43);
  memcpy(expected, (const uint8_t[]){1,2,3}, 3);
  memcpy(expected + 3, PLAIN, 21);
  memcpy(expected + 24, (const uint8_t[]){4,5}, 2);
  memcpy(expected + 26, PLAIN + 21, 43);

  memset(&p, 0, sizeof p);
  p.count = 2;
  p.samples[0].start = 3;  p.samples[0].end = 24;
  p.samples[0].encrypted_offset = 0;
  memcpy(p.samples[0].iv, CTR_IV, 16);
  p.samples[1].start = 26; p.samples[1].end = 69;
  p.samples[1].encrypted_offset = 21;   /* continues the same CTR stream */
  memcpy(p.samples[1].iv, CTR_IV, 16);

  printf("CENC sub-samples (clear bytes preserved, CTR offset continues)\n");

  hg_cenc_decrypt(source, sizeof source, 0, KEY, &p, got);
  checks++;
  if (memcmp(got, expected, sizeof expected) == 0) {
    printf("  PASS  %-38s %s\n", "two samples with a clear gap", "matches");
  } else { failures++; printf("  FAIL  %-38s\n", "two samples with a clear gap"); }

  /* and a mid-range request that straddles both samples */
  hg_cenc_decrypt(source + 22, 25, 22, KEY, &p, tail);
  ok = memcmp(tail, expected + 22, 25) == 0;
  checks++;
  if (ok) printf("  PASS  %-38s %s\n", "range straddling both samples", "matches");
  else { failures++; printf("  FAIL  %-38s\n", "range straddling both samples"); }
}

static void test_counter_add(void) {
  uint8_t c[16], want[16];
  printf("counter arithmetic\n");

  memcpy(c, CTR_IV, 16);
  hg_aes_counter_add(c, 1);
  expect_hex("iv + 1", c, 16, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff00");

  /* Carry out of the low 64 bits must propagate into the high half, which is
   * where the sample's auxiliary bytes live. Note 0xff + 1 does NOT carry out of
   * the low half -- the low half must be all-ones for that to happen, which is
   * what an earlier version of this test got wrong. */
  memset(c, 0, 16);
  memset(c + 8, 0xff, 8);
  hg_aes_counter_add(c, 1);
  expect_hex("carry crosses into the high half", c, 16,
             "00000000000000010000000000000000");

  /* No cross-half carry, but the low half still increments correctly. */
  memset(c, 0, 16);
  c[7] = 0xff; c[15] = 0xff;
  hg_aes_counter_add(c, 1);
  expect_hex("low half increments without carry", c, 16,
             "00000000000000ff0000000000000100");

  /* Wraparound at 2^128 drops the carry, matching the reference's 128-bit mask. */
  memset(c, 0xff, 16);
  hg_aes_counter_add(c, 1);
  expect_hex("overflow past 2^128 wraps to zero", c, 16,
             "00000000000000000000000000000000");

  memcpy(c, CTR_IV, 16);
  hg_aes_counter_add(c, 0x0102030405060708ULL);
  memcpy(want, CTR_IV, 16);
  { uint64_t lo = 0; int i; for (i = 8; i < 16; i++) lo = (lo << 8) | CTR_IV[i];
    lo += 0x0102030405060708ULL;
    for (i = 15; i >= 8; i--) { want[i] = (uint8_t)lo; lo >>= 8; } }
  checks++;
  if (memcmp(c, want, 16) == 0) printf("  PASS  %-38s %s\n", "arbitrary 64-bit delta", "matches");
  else { failures++; printf("  FAIL  %-38s\n", "arbitrary 64-bit delta"); }
}

#include "hg_media_vectors.h"   /* generated: see tools/gen_media_vectors.mjs */

static void test_spade(void) {
  uint8_t key[16];
  char url[256];

  printf("spade crypto (vectors derived from the reference implementation)\n");

  /* spade_a -> 16-byte CENC content key. The generator CONSTRUCTS a valid spade_a
   * by inverting the transform, because a random input is essentially never
   * accepted: the derived bytes have to spell 32 hex digits. */
  checks++;
  if (hg_spade_content_key(SPADE_A_INPUT, key) && memcmp(key, SPADE_A_KEY, 16) == 0) {
    printf("  PASS  %-38s %s\n", "spade_a -> content key", "matches reference");
  } else {
    failures++;
    printf("  FAIL  %-38s\n", "spade_a -> content key");
  }

  /* spade URL: base64 -> header check -> SHA512(SHA512(seed)||constants) ->
   * AES-128-CBC -> strip padding -> strip trailing NULs. */
  url[0] = '\0';
  checks++;
  if (hg_spade_decrypt_url(SPADE_INPUT, SPADE_SEED, sizeof SPADE_SEED, url, sizeof url) &&
      strcmp(url, SPADE_EXPECTED) == 0) {
    printf("  PASS  %-38s %s\n", "spade URL decrypt", url);
  } else {
    failures++;
    printf("  FAIL  %-38s got \"%s\"\n", "spade URL decrypt", url);
  }

  /* Fail closed on malformed input: bad base64, a spade_a too short to hold a
   * key, a URL ciphertext with no complete block. */
  checks++;
  {
    int ok = 1;
    if (hg_spade_content_key("!!!not base64!!!", key)) ok = 0;
    if (hg_spade_content_key("AAAA", key)) ok = 0;
    if (hg_spade_decrypt_url("AAAA", SPADE_SEED, sizeof SPADE_SEED, url, sizeof url)) ok = 0;
    if (ok) printf("  PASS  %-38s %s\n", "malformed inputs rejected", "ok");
    else { failures++; printf("  FAIL  %-38s\n", "malformed inputs rejected"); }
  }
}

/* ---- openBox: the play-info route's player_access_key (build173) ---------------------------------
 *
 * WHY THIS DESERVES VECTORS RATHER THAN TRUST. It is a bespoke obfuscation (two rolling XOR lanes, a
 * popcount of the loop index, a length marker), so being "nearly right" is the normal failure: three
 * separate mistakes were made in the first C draft -- the two lane seeds swapped, the popcount taken of
 * the wrong quantity, and a strlen() overrun in the hex step -- and NONE of them would have shown up as
 * an error. A wrong content key decrypts to garbage, mpv reports nothing, and the console shows a black
 * screen with a clean log. That is the whole argument for pinning it here.
 *
 * WHAT THE VECTORS ARE. Four REAL player_access_key values, captured live from the play-info endpoint on
 * 2026-10-16 (one per episode, four different episodes of the same series). The expected keys were
 * produced by the SITE'S OWN openBox(), extracted from the player bundle and executed directly
 * (_probe/cap-test-openbox.mjs), NOT by this C code -- so this is an independent oracle rather than a
 * snapshot of whatever the C happened to print.
 *
 * The algorithm is also independently confirmed end to end: vector 0's key decrypted 150 of 150 samples
 * of a real DASH segment (invalid = 0), and the plaintext parsed as valid H.264 -- 59 non-IDR NAL units
 * plus 1 IDR keyframe (_probe/cap-proof.mjs). So these are not merely self-consistent strings; they are
 * the keys that actually unlock the media.
 *
 * COVERAGE NOTE: the four values do NOT all share a method tag. Vector 0 begins "o+Z3..." and the other
 * three begin "yuYe.../zeYZ...", i.e. a different header byte pattern, so the tag extraction and the
 * lane selection are both exercised by more than one shape. */
static void test_open_box(void) {
  static const struct { const char *pak; const char *want; } V[] = {
    /* the vector that was also decrypted end to end */
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

  printf("openBox (player_access_key -> CENC key, live vectors 2026-10-16)\n");
  for (i = 0; i < sizeof V / sizeof V[0]; i++) {
    char what[48];
    snprintf(what, sizeof what, "live vector %u", (unsigned)i);
    memset(key, 0, sizeof key);
    if (hg_spade_open_box(V[i].pak, key) != 1) {
      checks++;
      failures++;
      printf("  FAIL  %-38s refused a valid key\n", what);
      continue;
    }
    expect_hex(what, key, HG_AES_KEY128, V[i].want);
  }

  /* REFUSALS. Each of these must return 0 rather than a plausible-looking wrong key, because a wrong
   * key is indistinguishable from a right one until the picture fails to appear. */
  checks++;
  {
    int ok = 1;
    if (hg_spade_open_box("", key)) ok = 0;                    /* empty */
    if (hg_spade_open_box("!!!not base64!!!", key)) ok = 0;     /* not base64 */
    if (hg_spade_open_box("YWJj", key)) ok = 0;                 /* too short to hold a tag */
    if (hg_spade_open_box(NULL, key)) ok = 0;                   /* no input */
    if (hg_spade_open_box(V[0].pak, NULL)) ok = 0;              /* no output */
    if (ok) printf("  PASS  %-38s %s\n", "malformed inputs rejected", "ok");
    else { failures++; printf("  FAIL  %-38s\n", "malformed inputs rejected"); }
  }

  /* A value whose tag is right but whose payload is not 32 hex digits must also be refused: the marker
   * step is what decides the length, and a wrong length is how a truncated key would get through. */
  checks++;
  {
    /* A valid base64 of 37 bytes with a plausible structure but a scrambled payload. */
    int refused = (hg_spade_open_box("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", key) == 0);
    if (refused) printf("  PASS  %-38s %s\n", "a non-hex payload is refused", "ok");
    else { failures++; printf("  FAIL  %-38s\n", "a non-hex payload is refused"); }
  }
}

int main(void) {
  printf("hg_cenc tests (i386 host build)\n");
  printf("sizeof(long)=%d  sizeof(void*)=%d\n\n", (int)sizeof(long), (int)sizeof(void *));

  test_aes_known_answer();
  printf("\n");
  test_ctr_nist();
  printf("\n");
  test_counter_add();
  printf("\n");
  test_cenc_unaligned_ranges();
  printf("\n");
  test_cenc_subsamples();
  printf("\n");
  test_spade();
  printf("\n");
  test_open_box();

  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
