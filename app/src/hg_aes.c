/*
 * Standard AES-128 (FIPS 197) + CTR / CBC. See hg_aes.h for why the S-box is
 * computed rather than transcribed.
 */

#include "hg_aes.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* GF(2^8) arithmetic and the S-box                                          */
/* ------------------------------------------------------------------------ */

static uint8_t gf_mul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  int i;
  for (i = 0; i < 8; i++) {
    if (b & 1u) p ^= a;
    b = (uint8_t)(b >> 1);
    {
      uint8_t hi = (uint8_t)(a & 0x80u);
      a = (uint8_t)(a << 1);
      if (hi) a ^= 0x1bu;
    }
  }
  return p;
}

/* a^254 == a^-1 in GF(2^8); 0 maps to 0 by definition. */
static uint8_t gf_inv(uint8_t a) {
  uint8_t result = 1, base = a;
  unsigned int exp = 254;
  if (a == 0) return 0;
  while (exp) {
    if (exp & 1u) result = gf_mul(result, base);
    base = gf_mul(base, base);
    exp >>= 1;
  }
  return result;
}

static uint8_t rotl8(uint8_t v, int n) { return (uint8_t)((v << n) | (v >> (8 - n))); }

static uint8_t* aes_sbox(void) {
  static uint8_t table[256];
  static int ready = 0;
  int x;
  if (ready) return table;
  for (x = 0; x < 256; x++) {
    uint8_t b = gf_inv((uint8_t)x);
    table[x] = (uint8_t)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63u);
  }
  ready = 1;
  return table;
}

static const uint8_t* aes_inv_sbox(void) {
  static uint8_t inv[256];
  static int ready = 0;
  int x;
  if (ready) return inv;
  {
    const uint8_t *sbox = aes_sbox();
    for (x = 0; x < 256; x++) inv[sbox[x]] = (uint8_t)x;
  }
  ready = 1;
  return inv;
}

/* ------------------------------------------------------------------------ */
/* key expansion (FIPS 197 5.2)                                              */
/* ------------------------------------------------------------------------ */

void hg_aes128_init(uint8_t round_keys[176], const uint8_t key[16]) {
  const uint8_t *sbox = aes_sbox();
  static const uint8_t RCON[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
  int i;
  memcpy(round_keys, key, 16);
  for (i = 4; i < 44; i++) {
    uint8_t t[4];
    memcpy(t, round_keys + (i - 1) * 4, 4);
    if (i % 4 == 0) {
      uint8_t tmp = t[0];
      t[0] = (uint8_t)(sbox[t[1]] ^ RCON[i / 4 - 1]);
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[tmp];
    }
    round_keys[i * 4 + 0] = (uint8_t)(round_keys[(i - 4) * 4 + 0] ^ t[0]);
    round_keys[i * 4 + 1] = (uint8_t)(round_keys[(i - 4) * 4 + 1] ^ t[1]);
    round_keys[i * 4 + 2] = (uint8_t)(round_keys[(i - 4) * 4 + 2] ^ t[2]);
    round_keys[i * 4 + 3] = (uint8_t)(round_keys[(i - 4) * 4 + 3] ^ t[3]);
  }
}

/* ------------------------------------------------------------------------ */
/* block encryption / decryption (FIPS 197 5.1 / 5.3)                        */
/* ------------------------------------------------------------------------ */

/* State is column-major: s[r + 4*c]. */
static void add_round_key(uint8_t s[16], const uint8_t *rk) {
  int i;
  for (i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void sub_bytes(uint8_t s[16]) {
  const uint8_t *sbox = aes_sbox();
  int i;
  for (i = 0; i < 16; i++) s[i] = sbox[s[i]];
}

static void inv_sub_bytes(uint8_t s[16]) {
  const uint8_t *inv = aes_inv_sbox();
  int i;
  for (i = 0; i < 16; i++) s[i] = inv[s[i]];
}

static void shift_rows(uint8_t s[16]) {
  uint8_t t;
  t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;   /* row 1 left 1 */
  t = s[2];  s[2]  = s[10]; s[10] = t;                                  /* row 2 left 2 */
  t = s[6];  s[6]  = s[14]; s[14] = t;
  t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;   /* row 3 left 3 */
}

static void inv_shift_rows(uint8_t s[16]) {
  uint8_t t;
  t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1]  = t;   /* row 1 right 1 */
  t = s[2];  s[2]  = s[10]; s[10] = t;                                  /* row 2 right 2 */
  t = s[6];  s[6]  = s[14]; s[14] = t;
  t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;   /* row 3 right 3 */
}

static void mix_columns(uint8_t s[16]) {
  int c;
  for (c = 0; c < 4; c++) {
    uint8_t *col = s + c * 4;
    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
    uint8_t t = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
    col[0] = (uint8_t)(a0 ^ t ^ gf_mul((uint8_t)(a0 ^ a1), 2));
    col[1] = (uint8_t)(a1 ^ t ^ gf_mul((uint8_t)(a1 ^ a2), 2));
    col[2] = (uint8_t)(a2 ^ t ^ gf_mul((uint8_t)(a2 ^ a3), 2));
    col[3] = (uint8_t)(a3 ^ t ^ gf_mul((uint8_t)(a3 ^ a0), 2));
  }
}

/* InvMixColumns uses the [0e 0b 0d 09] matrix -- it is NOT MixColumns run
 * backwards, and it is NOT MixColumns itself. Getting this wrong produces
 * plausible-looking garbage. */
static void inv_mix_columns(uint8_t s[16]) {
  int c;
  for (c = 0; c < 4; c++) {
    uint8_t *col = s + c * 4;
    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
    col[0] = (uint8_t)(gf_mul(a0,14) ^ gf_mul(a1,11) ^ gf_mul(a2,13) ^ gf_mul(a3, 9));
    col[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1,14) ^ gf_mul(a2,11) ^ gf_mul(a3,13));
    col[2] = (uint8_t)(gf_mul(a0,13) ^ gf_mul(a1, 9) ^ gf_mul(a2,14) ^ gf_mul(a3,11));
    col[3] = (uint8_t)(gf_mul(a0,11) ^ gf_mul(a1,13) ^ gf_mul(a2, 9) ^ gf_mul(a3,14));
  }
}

void hg_aes128_encrypt_block(const uint8_t round_keys[176],
                             const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  int round;
  memcpy(s, in, 16);
  add_round_key(s, round_keys);
  for (round = 1; round < 10; round++) {
    sub_bytes(s);
    shift_rows(s);
    mix_columns(s);
    add_round_key(s, round_keys + round * 16);
  }
  sub_bytes(s);
  shift_rows(s);
  add_round_key(s, round_keys + 10 * 16);
  memcpy(out, s, 16);
}

static void aes128_decrypt_block(const uint8_t rk[176],
                                 const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  int round;
  memcpy(s, in, 16);
  add_round_key(s, rk + 10 * 16);
  for (round = 9; round >= 1; round--) {
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, rk + round * 16);
    inv_mix_columns(s);
  }
  inv_shift_rows(s);
  inv_sub_bytes(s);
  add_round_key(s, rk);
  memcpy(out, s, 16);
}

/* ------------------------------------------------------------------------ */
/* counter arithmetic                                                        */
/* ------------------------------------------------------------------------ */

/* counter += delta, as a 128-bit big-endian add with carry. */
void hg_aes_counter_add(uint8_t counter[16], uint64_t delta) {
  int i;
  uint64_t carry = delta;
  for (i = 15; i >= 0 && carry; i--) {
    uint64_t sum = (uint64_t)counter[i] + (carry & 0xffu);
    counter[i] = (uint8_t)sum;
    carry = (carry >> 8) + (sum >> 8);
  }
}

/* ------------------------------------------------------------------------ */
/* CTR                                                                       */
/* ------------------------------------------------------------------------ */

size_t hg_aes_ctr(const uint8_t key[16], const uint8_t counter_block[16],
                  size_t skip, const uint8_t *in, size_t len, uint8_t *out) {
  uint8_t round_keys[176], stream[16], counter[16];
  size_t written = 0;
  size_t stream_pos;
  int i;

  hg_aes128_init(round_keys, key);
  memcpy(counter, counter_block, 16);
  hg_aes_counter_add(counter, (uint64_t)(skip / 16));

  /* Generate the block that contains the requested start and enter it at
   * `skip % 16`. The counter must NOT be advanced past this block: doing so
   * discards a whole 16 bytes instead of a few, which is exactly the bug the
   * unaligned-range sweep in hg_cenc_test.c exists to catch. */
  stream_pos = skip % 16;
  hg_aes128_encrypt_block(round_keys, counter, stream);

  while (written < len) {
    size_t take;
    if (stream_pos == 16) {
      hg_aes_counter_add(counter, 1);
      hg_aes128_encrypt_block(round_keys, counter, stream);
      stream_pos = 0;
    }
    take = len - written;
    if (take > 16 - stream_pos) take = 16 - stream_pos;
    for (i = 0; i < (int)take; i++)
      out[written + (size_t)i] = (uint8_t)(in[written + (size_t)i] ^ stream[stream_pos + (size_t)i]);
    written += take;
    stream_pos += take;
  }
  return written;
}

/* ------------------------------------------------------------------------ */
/* CBC decrypt (no padding handling; the caller strips it)                   */
/* ------------------------------------------------------------------------ */

size_t hg_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                             const uint8_t *in, size_t len, uint8_t *out) {
  uint8_t round_keys[176], prev[16], block[16], plain[16];
  size_t off = 0;
  int i;
  hg_aes128_init(round_keys, key);
  memcpy(prev, iv, 16);
  while (off + 16 <= len) {
    memcpy(block, in + off, 16);
    aes128_decrypt_block(round_keys, block, plain);
    for (i = 0; i < 16; i++) {
      out[off + (size_t)i] = (uint8_t)(plain[i] ^ prev[i]);
      prev[i] = block[i];
    }
    off += 16;
  }
  return off;
}
