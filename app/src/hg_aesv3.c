/*
 * "AesV3" -- the protocol's NOT-standard-AES block transform.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV AesV3.kt (GPL-3.0), cross-checked
 * line by line against the reference it derives from,
 * server/vendor/hongguo.cjs:378-388, following woshishiq1/drpys @22261ad
 * (GPL-3.0). The combined work stays GPL-3.0.
 *
 * This is deliberately not real AES and must not be "cleaned up" into AES:
 *   * the S-box is a 256-byte quarter of a 1024-byte table, picked by
 *     (timestamp & 3);
 *   * there are only two rounds, and only the first one runs a MixColumns;
 *   * MixColumns is applied to con-permuted columns, and the round-key
 *     "add" permutes the key bytes with con rather than using them in order.
 *
 * Layout note (verified against the reference, see hg_crypto.h): a round key
 * is FOUR byte-values.  `expanded` holds byte values 0..255, and the reference
 * builds `keys` as 12 slices of 4.
 */

#include "hg_crypto.h"
#include "hg_tables.h"

#include <string.h>

/* The permutation the reference actually reads in addCon(), sub() and
 * shiftCon().  The reference ALSO declares a table named `con` with a
 * different entry [1] ([1,3,0,2]) that is never referenced anywhere; using it
 * would break every signature.  The Kotlin port kept only the live table. */
static const int CON2[4][4] = {
  { 1, 0, 2, 3 },
  { 2, 0, 3, 1 },
  { 0, 1, 3, 2 },
  { 1, 0, 2, 3 }
};

static const int ORDER[4][16] = {
  { 0, 9,14,11, 4,13, 2, 7, 8, 1, 6,15,12, 5,10, 3 },
  { 0, 9,14,15, 4,13, 2, 7, 8, 1, 6, 3,12, 5,10,11 },
  { 0, 9,14, 7, 4,13, 2,11, 8, 1, 6, 3,12, 5,10,15 },
  { 0, 9,14,11, 4,13, 2, 7, 8, 1, 6,15,12, 5,10, 3 }
};

static const uint32_t INITIAL[4] = {
  0xca025ddcu, 0x823dc546u, 0xc9420583u, 0xc298225fu
};

/* GF(2^8) doubling with the 0x1b reduction polynomial, as in aesMix(). */
static uint8_t aesv3_xt(uint8_t x) {
  return (uint8_t)(((x << 1) ^ ((x & 128) ? 0x1b : 0)) & 255);
}

void hg_aesv3_init(hg_aesv3_ctx *ctx, const uint8_t key16[HG_AESV3_BLOCK_LEN],
                   uint32_t timestamp) {
  uint8_t expanded[HG_AESV3_KEY_LEN];
  uint32_t word = timestamp & 3u;
  uint32_t initial = INITIAL[word];
  const uint8_t *box = HG_SBOX + word * 256;
  int i, n, rounds;

  ctx->word = word;

  /* initial16 = the 32-bit constant repeated four times, little-endian.
   * expanded[0..15] = initial16 XOR key16 (the key is exactly 16 bytes, so
   * the reference's `b[it % b.size]` cycling is a plain 1:1 XOR). */
  for (n = 0; n < 16; n++) {
    uint8_t ib = (uint8_t)(initial >> (8 * (n & 3)));
    expanded[n] = (uint8_t)(ib ^ key16[n]);
  }
  for (n = 16; n < HG_AESV3_KEY_LEN; n++) expanded[n] = 0;

  rounds = 8;
  for (i = 4; i < 12; i++) {
    int at = 4 * (i - 1);
    uint8_t a = expanded[at], b = expanded[at + 1];
    uint8_t c = expanded[at + 2], d = expanded[at + 3];
    if ((i & 3) == 0) {
      /* `rounds & 24` yields 8 and then 16 over this loop; every read is
       * then masked to a byte, so an arithmetic or logical shift agree. */
      uint8_t t = (uint8_t)(((initial >> (rounds & 24)) ^ box[b]) & 255);
      b = box[c];
      c = box[d];
      d = box[a];
      a = t;
    }
    rounds += 2;
    expanded[at + 4] = (uint8_t)(a ^ expanded[at - 12]);
    expanded[at + 5] = (uint8_t)(b ^ expanded[at - 11]);
    expanded[at + 6] = (uint8_t)(c ^ expanded[at - 10]);
    expanded[at + 7] = (uint8_t)(d ^ expanded[at - 9]);
  }

  memcpy(ctx->keys, expanded, HG_AESV3_KEY_LEN);
}

void hg_aesv3_block(const hg_aesv3_ctx *ctx,
                    const uint8_t in[HG_AESV3_BLOCK_LEN],
                    uint8_t out[HG_AESV3_BLOCK_LEN]) {
  const uint8_t *box = HG_SBOX + ctx->word * 256;
  const int *con = CON2[ctx->word];
  const int *order = ORDER[ctx->word];
  uint8_t rows[4][4];
  int i, j, round;

  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      rows[i][j] = in[i * 4 + j];

  /* addCon(keys[0..3]) */
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      rows[i][j] = (uint8_t)(rows[i][j] ^ ctx->keys[i * 4 + con[j]]);

  for (round = 1; round <= 2; round++) {
    uint8_t before[4][4], flat[16];

    /* sub(): substitute every byte, then permute the ROWS with con. */
    for (i = 0; i < 4; i++)
      for (j = 0; j < 4; j++)
        rows[i][j] = box[rows[i][j]];
    memcpy(before, rows, sizeof rows);
    for (i = 0; i < 4; i++)
      for (j = 0; j < 4; j++)
        rows[i][j] = before[con[i]][j];

    /* shift(): byte permutation over the row-major flattening. */
    for (i = 0; i < 4; i++)
      for (j = 0; j < 4; j++)
        flat[i * 4 + j] = rows[i][j];
    for (i = 0; i < 16; i++)
      rows[i / 4][i % 4] = flat[order[i]];

    if (round == 1) {
      uint8_t old[4][4];
      /* shiftCon(): permute the columns of every row with con. */
      memcpy(old, rows, sizeof rows);
      for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
          rows[i][j] = old[i][con[j]];
      /* aesMix(): one MixColumns pass.  t and u must be read from the
       * pre-mix values; each row only reads itself and the next, so the
       * in-place assignment order below is safe once t/u are captured. */
      for (i = 0; i < 4; i++) {
        uint8_t t = (uint8_t)(rows[0][i] ^ rows[1][i] ^ rows[2][i] ^ rows[3][i]);
        uint8_t u = rows[0][i];
        rows[0][i] = (uint8_t)(rows[0][i] ^ t ^ aesv3_xt((uint8_t)(rows[0][i] ^ rows[1][i])));
        rows[1][i] = (uint8_t)(rows[1][i] ^ t ^ aesv3_xt((uint8_t)(rows[1][i] ^ rows[2][i])));
        rows[2][i] = (uint8_t)(rows[2][i] ^ t ^ aesv3_xt((uint8_t)(rows[2][i] ^ rows[3][i])));
        rows[3][i] = (uint8_t)(rows[3][i] ^ t ^ aesv3_xt((uint8_t)(rows[3][i] ^ u)));
      }
    }

    /* addCon(keys[round*4 ..]) */
    for (i = 0; i < 4; i++)
      for (j = 0; j < 4; j++)
        rows[i][j] = (uint8_t)(rows[i][j] ^ ctx->keys[(round * 4 + i) * 4 + con[j]]);
  }

  /* final add, reusing keys[4..7] exactly as the reference does. */
  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      rows[i][j] = (uint8_t)(rows[i][j] ^ ctx->keys[(4 + i) * 4 + j]);

  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      out[i * 4 + j] = rows[i][j];
}

void hg_aesv3_encrypt(const hg_aesv3_ctx *ctx,
                      const uint8_t iv[HG_AESV3_BLOCK_LEN],
                      const uint8_t *source, size_t source_len,
                      uint8_t *out) {
  uint8_t plaintext[32], key[32], first[16], tmp[16];
  size_t at;
  int i;

  /* The reference asserts source.size >= 248 and only ever reads the first
   * 31*8 = 248 bytes.  Refuse rather than read past the buffer. */
  if (source_len < 248) return;

  for (i = 0; i < 32; i++) plaintext[i] = 0;

  /* Gather one bit from each of eight consecutive source bytes. */
  for (i = 0; i < 31; i++) {
    at = (size_t)i * 8;
    plaintext[i] = (uint8_t)(
        ((source[at]     >> 4) & 2)   |
        ( source[at + 1]       & 64)  |
        ((source[at + 2] >> 2) & 1)   |
        ((source[at + 3] << 3) & 128) |
        ((source[at + 4] >> 1) & 4)   |
        ((source[at + 5] << 3) & 16)  |
        ((source[at + 6] << 5) & 32)  |
        ((source[at + 7] >> 4) & 8));
  }
  plaintext[31] = 1;

  /* CBC-like chaining over the two 16-byte halves. */
  for (i = 0; i < 16; i++) tmp[i] = (uint8_t)(plaintext[i] ^ iv[i]);
  hg_aesv3_block(ctx, tmp, first);
  for (i = 0; i < 16; i++) tmp[i] = (uint8_t)(plaintext[16 + i] ^ first[i]);
  hg_aesv3_block(ctx, tmp, key + 16);
  memcpy(key, first, 16);

  /* out[0] is the last key byte; the rest is the source with 31 bytes patched. */
  memcpy(out + 1, source, source_len);
  for (i = 0; i < 31; i++) {
    uint8_t k = key[i];
    at = (size_t)i * 8 + 1;
    out[at]     = (uint8_t)((out[at]     & 0xdf) | ((k << 4) & 32));
    out[at + 1] = (uint8_t)((out[at + 1] & 0xbf) | ( k       & 64));
    out[at + 2] = (uint8_t)((out[at + 2] & 0xfb) | ((k << 2) & 4));
    out[at + 3] = (uint8_t)((out[at + 3] & 0xef) | ((k >> 3) & 16));
    out[at + 4] = (uint8_t)((out[at + 4] & 0xf7) | ((k + k)  & 8));
    out[at + 5] = (uint8_t)((out[at + 5] & 0xfd) | ((k >> 3) & 2));
    out[at + 6] = (uint8_t)((out[at + 6] & 0xfe) | ((k >> 5) & 1));
    out[at + 7] = (uint8_t)((out[at + 7] & 0x7f) | ((k << 4) & 128));
  }
  out[0] = key[31];
}
