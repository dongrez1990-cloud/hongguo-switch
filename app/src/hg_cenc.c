/*
 * CENC sample decryption. See hg_cenc.h for the counter layout and provenance.
 */

#include "hg_cenc.h"

#include <string.h>

void hg_cenc_decrypt(const uint8_t *data, size_t len, int64_t start,
                     const uint8_t key[HG_AES_KEY128],
                     const HgCencPrepared *prepared, uint8_t *out) {
  int64_t end = start + (int64_t)len;
  size_t i, low, high;

  if (len == 0) return;
  memcpy(out, data, len);

  /* Header overlay: the rewritten header replaces the on-disk bytes for the
   * part of this range that lies inside it. */
  if (prepared->header != NULL && start < (int64_t)prepared->header_len) {
    int64_t stop = (end < (int64_t)prepared->header_len) ? end : (int64_t)prepared->header_len;
    if (stop > start) memcpy(out, prepared->header + start, (size_t)(stop - start));
  }

  if (key == NULL) return;

  /* First sample whose end is strictly after `start`. */
  low = 0;
  high = prepared->count;
  while (low < high) {
    size_t mid = (low + high) / 2;
    if (prepared->samples[mid].end <= start) low = mid + 1;
    else high = mid;
  }

  for (i = low; i < prepared->count; i++) {
    const HgCencSample *s = &prepared->samples[i];
    int64_t begin, stop, relative;
    size_t skip;
    uint8_t counter[HG_AES_BLOCK];

    if (s->start >= end) break;
    begin = (start > s->start) ? start : s->start;
    stop = (end < s->end) ? end : s->end;
    if (begin >= stop) continue;

    relative = begin - s->start + s->encrypted_offset;
    if (relative < 0) continue;                 /* cannot happen for a valid table */
    skip = (size_t)(relative % 16);

    memcpy(counter, s->iv, HG_AES_BLOCK);
    hg_aes_counter_add(counter, (uint64_t)(relative / 16));

    /* hg_aes_ctr reads in[i] and writes out[i] in the same iteration, so an
     * in-place call is safe. */
    {
      uint8_t *dst = out + (size_t)(begin - start);
      hg_aes_ctr(key, counter, skip, dst, (size_t)(stop - begin), dst);
    }
  }
}

/* ------------------------------------------------------------------------ */
/* spade crypto                                                              */
/* ------------------------------------------------------------------------ */

/* The reference's decode64() maps the URL-safe alphabet onto the standard one
 * before decoding; a plain base64 decode would fail on any '=' free URL-safe
 * string containing '-' or '_'. */
static int decode64_urlsafe(const char *in, uint8_t *out, size_t cap, size_t *out_len) {
  static char normalised[4096];
  size_t i, n = strlen(in);
  if (n + 1 > sizeof normalised) return 0;
  for (i = 0; i < n; i++) {
    char c = in[i];
    normalised[i] = (c == '-') ? '+' : (c == '_') ? '/' : c;
  }
  normalised[n] = '\0';
  return hg_base64_decode(normalised, out, cap, out_len);
}

static int popcount32(uint32_t v) {
  int n = 0;
  while (v) { n += (int)(v & 1u); v >>= 1; }
  return n;
}

int hg_spade_content_key(const char *value, uint8_t out[16]) {
  uint8_t raw[512], work[256];
  char hexbuf[33];
  size_t rawlen = 0, v8, i, got = 0;
  uint32_t a = 85, b = 246;

  if (!decode64_urlsafe(value, raw, sizeof raw, &rawlen)) return 0;
  if (rawlen < 3 || rawlen > sizeof raw) return 0;

  {
    int64_t len = (int64_t)rawlen
                - (int64_t)((uint32_t)raw[0] ^ (uint32_t)raw[1] ^ (uint32_t)raw[2])
                + 47;
    if (len > (int64_t)rawlen - 1) len = (int64_t)rawlen - 1;
    if (len < 33) return 0;
    v8 = (size_t)len;
  }
  if (v8 > sizeof work || 1 + v8 > rawlen) return 0;

  memcpy(work, raw + 1, v8);
  for (i = 0; i < v8; i++) {
    uint8_t old = work[i];
    uint32_t previous = (i & 1) ? a : b;
    if (i & 1) a = old; else b = old;
    work[i] = (uint8_t)((-21 - popcount32((uint32_t)i) + (int)(previous ^ (uint32_t)old)) & 255);
  }

  /* work[1..33] must be 32 ASCII hex digits; hg_unhex rejects anything else, which
   * is how an invalid spade_a value is detected. */
  memcpy(hexbuf, work + 1, 32);
  hexbuf[32] = '\0';
  if (!hg_unhex(hexbuf, out, 16, &got) || got != 16) return 0;
  return 1;
}

int hg_spade_decrypt_url(const char *value, const uint8_t *seed, size_t seed_len,
                         char *out, size_t cap) {
  static const uint8_t CONSTANTS[64] = {
    0x4d,0xd4,0xc2,0xe6,0xb8,0x31,0x62,0x09,0x0e,0x52,0xb3,0xc7,0xa6,0x73,0x3b,0xa4,
    0x1c,0xb2,0x46,0x2b,0x82,0x9a,0xb5,0x8a,0x19,0x6b,0x39,0xdb,0x57,0x17,0x75,0x24,
    0xf4,0x9b,0xaf,0x7f,0x08,0xe8,0xd6,0x8d,0x26,0xa7,0x2e,0x37,0xc1,0xa9,0x5a,0x2f,
    0x1f,0x05,0xa5,0x18,0x92,0xae,0xf2,0x94,0x97,0x32,0xb6,0x2a,0x38,0xaa,0xdd,0x58
  };
  uint8_t raw[4096], h1[64], h2[64], buf[4096];
  size_t rawlen = 0, cipher_len, end;
  uint32_t padding;

  if (!decode64_urlsafe(value, raw, sizeof raw, &rawlen)) return 0;
  if (rawlen < 20) return 0;                       /* the reference requires 20 */
  if (raw[0] != 0xa8 || raw[2] != 0x01 || raw[3] != 0x00) return 0;

  cipher_len = ((rawlen - 4) / 16) * 16;
  if (cipher_len == 0 || 4 + cipher_len > rawlen) return 0;

  /* h = SHA512(SHA512(seed) || constants) */
  hg_sha512(seed, seed_len, h1);
  {
    uint8_t join[128];
    memcpy(join, h1, 64);
    memcpy(join + 64, CONSTANTS, 64);
    hg_sha512(join, 128, h2);
  }

  if (hg_aes128_cbc_decrypt(h2, h2 + 16, raw + 4, cipher_len, buf) != cipher_len) return 0;

  end = cipher_len;
  padding = buf[end - 1];
  if (padding >= 1 && padding <= 16 && padding <= end) end -= padding;
  while (end > 0 && buf[end - 1] == 0) end--;
  if (end + 1 > cap) return 0;

  memcpy(out, buf, end);
  out[end] = '\0';
  return 1;
}

/* ---- openBox: player_access_key -> the 16-byte CENC content key (build173) ------------------------
 *
 * WHY THIS EXISTS. The play-info route (the one that answers now, see hg_request.h) does not return a
 * `spade_a`; it returns a single opaque `player_access_key` plus a `kid`. The neighbouring
 * hg_spade_content_key() expects base64 that decodes to 32 ASCII hex characters and CANNOT read this
 * value: measured, the live key base64-decodes to 37 bytes whose hex is
 * a3e677c63bda20946dda4df855e14ef854e21ac23ad81bc152d873926d8f7290398f239e9e -- 37, not 32, and
 * random-looking rather than ASCII hex. So a second derivation is needed, not a parameter change.
 *
 * It lives in THIS file, directly beside hg_spade_content_key(), because both answer the same question --
 * "what is the content key" -- and differ only in which upstream scheme produced the value. Keeping them
 * together is what makes it obvious that a third scheme would belong here too.
 *
 * WHERE THE ALGORITHM COMES FROM, AND WHY IT IS NOT A GUESS. It is the live web client's own
 * `openBox()` (chunk t.78fb905b21.js sets `useEME: false` for this route, so the Widevine path in that
 * bundle is dead code and the key travels through pure JS). The steps below are that function, and every
 * constant is copied from it rather than fitted to a sample: popcount, a two-lane rolling XOR seeded
 * 85/250, and a position-dependent +/-21 applied mod 256.
 *
 * IT IS PINNED TO A MEASURED VECTOR, and that is the whole point of writing it down:
 *     "o+Z3xjvaIJRt2k34VeFO+FTiGsI62BvBUthzkm2PcpA5jyOeng=="  ->  86c22eed79003130e78191b09b7588e5
 * Verified three ways on 2026-10-16 (_probe/cap-test-openbox.mjs loads the REAL bundle and calls its own
 * openBox; cap-reimplement-openbox.mjs is this algorithm; cap-proof.mjs decrypts a real DASH segment):
 *   1. this derivation and the bundle's own function agree byte for byte on that vector;
 *   2. the key decrypts 150 of 150 real samples of the video track (invalid = 0);
 *   3. the decrypted bytes parse as valid H.264 -- 59 non-IDR NAL units + 1 IDR keyframe.
 * A wrong key here does NOT report an error: it produces a black screen, which is why the vector is
 * asserted in the host tests instead of being discovered on the console.
 *
 * WHAT THIS FUNCTION REFUSES. The method tag selects the lane and the arithmetic, so an unrecognised
 * tag is REFUSED (return 0) rather than run through the "22" branch: computing a plausible-looking wrong
 * key is the exact failure mode that shows up as a black screen with clean logs. The same goes for a
 * value too short to hold a tag, and for any marker digit that runs past the buffer.
 *
 * `value` is the base64 TEXT as it arrives in play_info (player_access_key). Returns 1 on success. */
int hg_spade_open_box(const char *value, uint8_t out[HG_AES_KEY128]) {
  uint8_t raw[128];
  size_t raw_len = 0;
  size_t method_len, convert_len, i;
  char method[16];
  char conv[128];
  int lane;
  unsigned prev_a, prev_r;

  if (value == NULL || out == NULL) return 0;
  if (!hg_base64_decode(value, raw, sizeof raw, &raw_len)) return 0;
  /* A tag needs at least three header bytes plus one method byte plus one marker byte. */
  if (raw_len < 5 || raw_len > sizeof conv) return 0;

  /* Step 1 -- header. methodLen is the XOR of the first three bytes, minus '0'. */
  method_len = (size_t)((raw[0] ^ raw[1] ^ raw[2]) - (uint8_t)'0');
  if (method_len == 0 || method_len >= sizeof method) return 0;
  if (raw_len < method_len + 2) return 0;
  convert_len = raw_len - method_len - 1;

  /* The tag is the XOR of one tail byte with the two bytes before the tail, per tag byte. */
  for (i = 0; i < method_len; i++) {
    method[i] = (char)(raw[raw_len - method_len - 2] ^ raw[raw_len - method_len - 1] ^
                       raw[raw_len - method_len + i]);
  }
  method[method_len] = '\0';

  /* Step 2 -- lane. Only these three tags use lane 0; everything else (including the "22" measured on
   * the live response) uses lane 1. Kept as the bundle writes it so a future tag cannot silently
   * change lane. */
  lane = (strcmp(method, "11") == 0 || strcmp(method, "appdevice") == 0 ||
          strcmp(method, "app_v2") == 0) ? 0 : 1;

  if (strcmp(method, "11") != 0 && strcmp(method, "22") != 0 &&
      strcmp(method, "appdevice") != 0 && strcmp(method, "webdevice") != 0) {
    return 0;   /* REFUSED -- see the note above. */
  }

  /* The payload starts at byte 1 and runs convert_len bytes. */
  for (i = 0; i < convert_len; i++) conv[i] = (char)raw[1 + i];

  /* Step 3 -- two lanes, seeded 250 (even positions) and 85 (odd positions), each then carrying the
   * PREVIOUS input byte. The position enters the arithmetic as a popcount plus a fixed 21, so this is
   * not a plain XOR stream.
   *
   * TWO DETAILS THAT ARE EASY TO GET BACKWARDS AND ARE SILENT WHEN THEY ARE, because a wrong key
   * produces a black screen and no error at all:
   *   - the seeds belong to the PARITY, not to the variable names: the bundle is
   *     `if (o % 2 === 1) { v = rLane ^ l; rLane = l } else { v = aLane ^ l; aLane = l }` with
   *     `rLane = 85, aLane = 250` (cap-reimplement-openbox.mjs:40-45), so the EVEN positions use 250.
   *     Swapping them still yields 32 plausible hex digits.
   *   - the popcount is of the INDEX, not of the XOR result: the bundle calls ph(v, o, 21) with o the
   *     loop counter, i.e. `(v + popcount(o) + 21) & 255`.
   * Both were wrong in the first draft of this function; the measured vector in the header comment is
   * what caught them, which is why that vector is asserted in the host tests. */
  prev_a = 250;   /* even positions */
  prev_r = 85;    /* odd positions  */
  for (i = 0; i < convert_len; i++) {
    unsigned l = (unsigned char)conv[i];
    unsigned v, pc = 0, t;
    if (i % 2 == 1) { v = prev_r ^ l; prev_r = l; }
    else            { v = prev_a ^ l; prev_a = l; }
    t = (unsigned)i;
    while (t != 0) { t &= t - 1; pc++; }
    if (lane == 0) {
      conv[i] = (char)((uint8_t)(v - pc - 21));   /* fh(v, o, 21) */
    } else {
      conv[i] = (char)((uint8_t)(v + pc + 21));   /* ph(v, o, 21) */
    }
  }

  /* Step 4 -- the first character is a length marker; the key is what follows it. */
  {
    char m = conv[0];
    int marker;
    size_t key_len, k;
    if (m >= '0' && m <= '9') marker = m - '0';
    else if (m >= 'a' && m <= 'z') marker = m - 'a' + 10;
    else return 0;                       /* REFUSED: not a marker this scheme defines. */
    if ((size_t)marker + 1 > convert_len) return 0;

    key_len = convert_len - (size_t)marker - 1;
    /* The result must be exactly the 32 ASCII hex digits a 16-byte key needs. Measured: it is. A value
     * that is not is refused rather than truncated into a key. */
    if (key_len != 32) return 0;

    /* The 32 digits go through a NUL-terminated copy rather than being handed to hg_unhex as a pointer
     * into `conv`. hg_unhex measures with strlen(), and on the measured vector the byte AFTER the key
     * is another '1' -- itself a valid hex digit -- so a pointer into conv would be read as 33 digits,
     * fail the even-length check and return 0 while `out` kept whatever it held. Copying first makes
     * the length the caller's, not the terminator's. */
    {
      char hex32[33];
      int bad = 0;
      for (k = 0; k < 32; k++) {
        char c = conv[k + 1];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) bad = 1;
        hex32[k] = c;
      }
      if (bad) return 0;
      hex32[32] = '\0';
      if (hg_unhex(hex32, out, HG_AES_KEY128, NULL) != 1) return 0;
    }
  }
  return 1;
}
