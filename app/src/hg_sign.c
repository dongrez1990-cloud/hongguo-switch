/*
 * 红果 request signing. See hg_sign.h for scope, provenance and branch coverage.
 * Every routine mirrors server/vendor/hongguo.cjs; comments cite its line numbers.
 */

#include "hg_sign.h"
#include "hg_tables.h"
#include "hg_branch2_tables.h"

#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t get_le64(const uint8_t *p) {
  uint64_t v = 0;
  int i;
  for (i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
  return v;
}
static void put_le64(uint8_t *p, uint64_t v) {
  int i;
  for (i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}
/* 8-bit reversal, from reverseBits() at hongguo.cjs:359. */
static uint8_t reverse_bits8(uint8_t v) {
  uint8_t r = 0;
  int i;
  for (i = 0; i < 8; i++) r = (uint8_t)((r << 1) | ((v >> i) & 1));
  return r;
}
/* Unsigned decimal, no stdio. Returns the number of digits written. */
static int u32_to_dec(uint32_t v, char *out, size_t cap) {
  char tmp[16];
  int n = 0, j;
  if (v == 0) tmp[n++] = '0';
  while (v > 0 && n < 15) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
  if ((size_t)n + 1 > cap) return 0;
  for (j = 0; j < n; j++) out[j] = tmp[n - 1 - j];
  out[n] = '\0';
  return n;
}

/* ---- getIv (hongguo.cjs:332) -------------------------------------------- */
uint32_t hg_get_iv(uint32_t iv, const uint8_t *data, size_t len) {
  uint32_t value = iv;
  size_t i;
  for (i = 0; i < len; i++) {
    if ((i & 1) == 0)
      value = (value >> 4) ^ value ^ (value << 6) ^ (uint32_t)data[i];
    else
      value = ~((value >> 7) ^ value ^ ((uint32_t)data[i] | (value << 12)));
  }
  return value;
}

/* ---- sumMd5 (hongguo.cjs:333) ------------------------------------------- */
uint32_t hg_sum_md5(const uint8_t *data) {
  uint32_t check = 0x20220420u;
  int i;
  for (i = 0; i < 12; i++) {
    uint32_t temp = ((i & 1) == 0) ? ((check >> 3) ^ check) : ((check >> 5) ^ check);
    check = ((i & 1) == 0) ? ((uint32_t)data[i] ^ (check << 7))
                           : ((uint32_t)data[i] | (check << 11));
    if ((i & 1) != 0) check = ~check;
    check = check ^ temp;
  }
  return (check | 4u) ^ 0x1000000u;
}

/* ---- branchOf (hongguo.cjs:403) ----------------------------------------- */
uint32_t hg_branch_of(const char *query, const uint8_t *body, size_t body_len,
                      uint32_t khronos) {
  uint8_t body_md5[16], q_sm3[32], ts_bytes[4];
  uint32_t iv, low;

  if (body != NULL && body_len > 0) hg_md5(body, body_len, body_md5);
  else memset(body_md5, 0, sizeof body_md5);

  hg_sm3(query, strlen(query), q_sm3);
  put_le32(ts_bytes, khronos);

  iv = hg_get_iv(0x20230928u, q_sm3, sizeof q_sm3);
  iv = hg_get_iv(iv, body_md5, sizeof body_md5);
  iv = hg_get_iv(iv, ts_bytes, sizeof ts_bytes);

  low = iv & 15u;
  /* low*171 <= 2565 so >>9 yields 0..5; the result is always 0, 1 or 2.
   * Distribution over the 16 values of `low`: six land on branch 0, five on
   * branch 1, five on branch 2. A 128-try ticket search therefore misses
   * branch 0 with probability about 0.625^128, i.e. never in practice. */
  return low - (((low * 171u) >> 9) * 3u);
}

/* ---- hashF13, branch 0 (hongguo.cjs:348-357) ---------------------------- */
static const uint32_t F13_IVV1[6] = {
  0xc4a78580u, 0xb3c0fd39u, 0xc58c5686u, 0xc9aa3ba7u, 0xf5a7adf2u, 0x963c2ed1u
};
/* `chosen` = [iterations, i1..i10]; every index is 0..7. */
static const int F13_CHOSEN[6][11] = {
  { 101, 5, 7, 6, 3, 2, 1, 0, 5, 4, 3 },
  {  96, 0, 6, 7, 5, 3, 2, 1, 5, 4, 4 },
  {  96, 7, 6, 2, 1, 4, 0, 5, 4, 3, 5 },
  {  99, 3, 6, 2, 4, 5, 1, 0, 0, 7, 6 },
  {  96, 0, 5, 6, 7, 3, 1, 2, 5, 4, 4 },
  { 100, 2, 0, 3, 5, 4, 6, 7, 2, 1, 5 }
};
static const uint32_t F13_INIT[8] = {
  0x7aba4fc8u, 0x67166507u, 0x6403fa00u, 0x340f512fu,
  984304912u, 3005047866u, 2874125293u, 2152413264u
};
static const uint8_t F13_TAIL[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xa0 };

/* ---- hashF13, branch 2 (hongguo.cjs:336-346) ---------------------------- */
/* A variant MD5 with a per-round message ordering, four phase-dependent
 * argument rotations, and a final fold. Written from the reference; there is no
 * published specification for any of it. */

static uint32_t md5_v3_step(int kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                            uint32_t m, int shift, uint32_t constant) {
  uint32_t f;
  switch (kind) {
    case 0:  f = (b & c) | (~b & d); break;
    case 1:  f = (b & d) | (c & ~d); break;
    case 2:  f = b ^ c ^ d;          break;
    default: f = c ^ (b | ~d);       break;
  }
  /* note the u32() around the sum happens BEFORE the rotate, and the rotate
   * result is added to b afterwards */
  return b + hg_rol32(a + f + m + constant, shift);
}

/* message must be at least 64 bytes; `orders` is one 64-entry slice. */
static void md5_sum_v3(const uint8_t *message, size_t message_len,
                       uint32_t count_v2, const uint8_t *orders, uint32_t count_v1,
                       uint8_t out[20]) {
  static const int SHIFTS[4][16] = {
    { 7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22 },
    { 5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20 },
    { 4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23 },
    { 6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21 }
  };
  uint32_t sv[64], start[4], end[4], m[16];
  uint32_t a, b, c, d, ret[4];
  uint32_t end_count;
  int i, kind;

  if (message_len < 64) return;

  for (i = 0; i < 64; i++) sv[i] = hg_ror32(HG_B2_SV[i], (int)count_v1);
  start[0] = hg_ror32(0x79e0f2fbu, (int)count_v2);
  start[1] = hg_ror32(0xc8b52570u, (int)count_v2);
  start[2] = hg_ror32(0xebc2f8cdu, (int)count_v2);
  start[3] = hg_ror32(0x7c104d93u, (int)count_v2);
  end_count = (count_v2 + 6u) & 255u;
  end[0] = hg_ror32(0x19be4866u, (int)end_count);
  end[1] = hg_ror32(0xe85986b4u, (int)end_count);
  end[2] = hg_ror32(0xe19b326eu, (int)end_count);
  end[3] = hg_ror32(0x71d1d7d4u, (int)end_count);

  for (i = 0; i < 16; i++)
    m[i] = (uint32_t)message[i * 4] | ((uint32_t)message[i * 4 + 1] << 8)
         | ((uint32_t)message[i * 4 + 2] << 16) | ((uint32_t)message[i * 4 + 3] << 24);

  a = start[0]; b = start[1]; c = start[2]; d = start[3];

  for (kind = 0; kind < 4; kind++) {
    int offset = kind * 16;
    for (i = 0; i < 16; i++) {
      int phase = i & 3;
      uint32_t next;
      uint32_t mv = m[orders[offset + i]];
      if (phase == 0)      next = md5_v3_step(kind, a, b, c, d, mv, SHIFTS[kind][i], sv[offset + i]);
      else if (phase == 1) next = md5_v3_step(kind, d, a, b, c, mv, SHIFTS[kind][i], sv[offset + i]);
      else if (phase == 2) next = md5_v3_step(kind, c, d, a, b, mv, SHIFTS[kind][i], sv[offset + i]);
      else                 next = md5_v3_step(kind, b, c, d, a, mv, SHIFTS[kind][i], sv[offset + i]);
      if (phase == 0) a = next;
      else if (phase == 1) d = next;
      else if (phase == 2) c = next;
      else b = next;
    }
  }

  ret[0] = (start[0] + a) ^ end[0];
  ret[1] = (start[1] + b) ^ end[1];
  ret[2] = (start[2] + c) ^ end[2];
  ret[3] = (start[3] + d) ^ end[3];
  for (i = 0; i < 4; i++) {
    out[i * 4 + 0] = (uint8_t)ret[i];
    out[i * 4 + 1] = (uint8_t)(ret[i] >> 8);
    out[i * 4 + 2] = (uint8_t)(ret[i] >> 16);
    out[i * 4 + 3] = (uint8_t)(ret[i] >> 24);
  }
  {
    uint8_t tail[4];
    uint32_t folded = hg_sum_md5(out);
    tail[0] = (uint8_t)folded; tail[1] = (uint8_t)(folded >> 8);
    tail[2] = (uint8_t)(folded >> 16); tail[3] = (uint8_t)(folded >> 24);
    memcpy(out + 16, tail, 4);
  }
}

/* ivV0 selects one of five 64-byte order slices -- see gen_branch2.mjs for why
 * it cannot exceed 4. */
static int branch2_f13(uint32_t iv, const uint8_t query_sm3[32],
                       const uint8_t body_md5[16], const uint8_t ts_bytes[4],
                       uint32_t khronos, uint8_t out[HG_HASH_F13_LEN]) {
  static const uint32_t N1[5] = {
    0x8980f29bu, 0xeb549c7fu, 0xb08726dbu, 0xd40cb5e6u, 0xe8f559e4u
  };
  static const uint8_t SEED[8] = { 0x84, 0x96, 0x77, 0x9d, 0xd4, 0x15, 0x0b, 0xf8 };
  uint32_t v = (iv & 13u) * 86u;
  uint32_t iv_v0 = ((v >> 15) & 255u) + ((v >> 8) & 255u);
  uint32_t n1, count_v1, count_v2, shift;
  uint8_t pad[8], input[64];
  int i;

  if (iv_v0 > 4u) return 0;
  n1 = N1[iv_v0];
  count_v1 = (n1 + khronos + 1u) & 255u;
  count_v2 = n1 + khronos;
  shift = (count_v2 + 5u) & 7u;

  for (i = 0; i < 8; i++) {
    uint32_t value = SEED[i];
    pad[i] = (uint8_t)(((value | (value << 8)) >> shift) & 255u);
  }

  memcpy(input, query_sm3, 32);
  memcpy(input + 32, body_md5, 16);
  memcpy(input + 48, ts_bytes, 4);
  memcpy(input + 52, pad, 8);
  memcpy(input + 60, "\xa0\x01\x00\x00", 4);

  md5_sum_v3(input, sizeof input, count_v2, HG_B2_ORDERS + iv_v0 * 64, count_v1, out);
  return 1;
}

int hg_hash_f13_bytes(const uint8_t query_sm3[32], const uint8_t body_md5[16],
                      uint32_t khronos, uint8_t out[HG_HASH_F13_LEN]) {
  uint8_t ts_bytes[4], pad[4], data[64];
  uint8_t ret[32], folded[16], tail[4];
  uint32_t iv, low, iv_v0, iv_v1, count_v1, count_v2, tt[64], init[8], d[8];
  uint32_t di[128];
  const int *chosen;
  uint32_t n0;
  int i, branch, k;

  put_le32(ts_bytes, khronos);

  iv = hg_get_iv(0x20230928u, query_sm3, 32);
  iv = hg_get_iv(iv, body_md5, 16);
  iv = hg_get_iv(iv, ts_bytes, 4);

  low = iv & 15u;
  iv_v0 = (low * 171u) >> 9;
  branch = (int)(low - iv_v0 * 3u);

  /* Branch 2 is implemented: the reference deliberately accepts branch 0 OR 2
   * and only retries away from branch 1
   *     if (branchOf(url, body, khronos) !== 1) break;      hongguo.cjs:428
   * -- so refusing branch 2 would fail roughly five requests in sixteen on the
   * search endpoint, which has no retry loop at all.
   *
   * Branch 1 stays refused, on purpose: the reference itself treats it as
   * unusable ("服务器的签名变体不认" / the server does not accept it) and jitters
   * the ticket to escape it. Its output is also 36 bytes rather than 20, so
   * returning it through a 20-byte buffer would overrun. */
  if (branch == 2) {
    return branch2_f13(iv, query_sm3, body_md5, ts_bytes, khronos, out);
  }
  if (branch != 0) return 0;

  iv_v1 = F13_IVV1[iv_v0];
  count_v1 = (iv_v1 + khronos + 1u) & 255u;
  count_v2 = iv_v1 + khronos;

  for (i = 0; i < 64; i++) tt[i] = hg_ror32(HG_HASH_TABLE[i], (int)count_v1);

  n0 = (count_v2 + 2u) & 7u;
  {
    static const uint8_t seed[4] = { 0xfa, 0x45, 0x61, 0xd7 };
    for (i = 0; i < 4; i++) {
      uint32_t v = seed[i];
      pad[i] = (uint8_t)(((v | (v << 8)) >> n0) & 255u);
    }
  }

  memcpy(data, query_sm3, 32);
  memcpy(data + 32, body_md5, 16);
  memcpy(data + 48, ts_bytes, 4);
  memcpy(data + 52, pad, 4);
  memcpy(data + 56, F13_TAIL, 8);

  for (i = 0; i < 16; i++) di[i] = get_be32(data + i * 4);
  {
    uint32_t di0 = di[0];
    for (i = 0; i < 112; i++) {
      uint32_t di1 = di[i + 1];
      uint32_t di14 = di[i + 14];
      uint32_t r1 = hg_rol32(di1, 14) ^ hg_rol32(di1, 25) ^ (di1 >> 3);
      uint32_t r2 = hg_rol32(di14, 13) ^ hg_rol32(di14, 15) ^ (di14 >> 10);
      di[16 + i] = di0 + di[i + 9] + r1 + r2;
      di0 = di1;
    }
  }

  for (i = 0; i < 8; i++) init[i] = hg_ror32(F13_INIT[i], (int)(count_v2 & 31u));
  memcpy(d, init, sizeof d);
  chosen = F13_CHOSEN[iv_v0];

  for (i = 0; i < chosen[0]; i++) {
    /* `base` is a uint32 add; the reference masks its low bits, which keep
     * exactly the bits the uint32 arithmetic keeps. */
    uint32_t base = iv_v1 + (uint32_t)i;
    uint32_t n0v = di[base & 127u];
    uint32_t n1 = ((d[chosen[3]] ^ d[chosen[4]]) & d[chosen[1]]) ^ d[chosen[3]];
    uint32_t n2 = hg_rol32(d[chosen[1]], 26) ^ hg_rol32(d[chosen[1]], 21) ^ hg_rol32(d[chosen[1]], 7);
    uint32_t n4 = n0v + n1 + n2 + tt[base & 63u] + d[chosen[5]];
    uint32_t n5 = hg_rol32(d[chosen[2]], 30) ^ hg_rol32(d[chosen[2]], 19) ^ hg_rol32(d[chosen[2]], 10);
    uint32_t n6 = (d[chosen[2]] & d[chosen[6]]) | ((d[chosen[2]] | d[chosen[6]]) & d[chosen[7]]);
    uint32_t n7 = n5 + n6;
    uint32_t old = d[chosen[9]];
    uint32_t last = d[7];
    /* unshift(pop()) == rotate right by one */
    for (k = 7; k > 0; k--) d[k] = d[k - 1];
    d[0] = last;
    d[chosen[10]] = n7 + n4;
    d[chosen[8]] = old + n4;
  }

  for (i = 0; i < 8; i++) put_be32(ret + i * 4, d[i] + init[i]);
  for (i = 0; i < 16; i++) folded[i] = (uint8_t)(ret[i] ^ ret[16 + i]);
  memcpy(ret, folded, 16);
  put_le32(tail, hg_sum_md5(ret));   /* reads ret[0..11]: the folded half */
  memcpy(ret + 16, tail, 4);

  memcpy(out, ret, HG_HASH_F13_LEN);
  return 1;
}

int hg_hash_f13(const uint8_t query_sm3[32], const uint8_t body_md5[16],
                uint32_t khronos, char out_hex[HG_HASH_F13_LEN * 2 + 1]) {
  uint8_t raw[HG_HASH_F13_LEN];
  if (!hg_hash_f13_bytes(query_sm3, body_md5, khronos, raw)) return 0;
  hg_hex(raw, HG_HASH_F13_LEN, out_hex);
  return 1;
}

/* ---- xGorgon (hongguo.cjs:366-372) -------------------------------------- */
void hg_xgorgon(const char *query, const uint8_t *body, size_t body_len,
                uint32_t timestamp, uint32_t random,
                char out_hex[HG_GORGON_LEN * 2 + 1]) {
  uint8_t qmd5[16], bmd5[16], input[20], key[8], out[20], final[26];
  hg_rc4_ctx rc;
  int i;

  hg_md5(query, strlen(query), qmd5);
  if (body != NULL && body_len > 0) hg_md5(body, body_len, bmd5);
  else memset(bmd5, 0, sizeof bmd5);

  memcpy(input, qmd5, 4);            /* md5(query)[0..4] */
  memcpy(input + 4, bmd5, 4);        /* md5(body)[0..4], or four zero bytes */
  memset(input + 8, 0, 4);           /* Buffer.alloc(4) */
  put_le32(input + 12, 67503104u);   /* SDK build, little-endian */
  put_be32(input + 16, timestamp);

  key[0] = 0x4a; key[1] = 0x40; key[2] = 0x16;
  key[3] = (uint8_t)((random >> 8) & 255u);
  key[4] = 0x47; key[5] = 0x6c; key[6] = 0x01;
  key[7] = (uint8_t)(random & 255u);

  hg_rc4_init(&rc, key, sizeof key);
  hg_rc4_apply(&rc, input, out, sizeof out);

  /* In-place post-mix. The reference reads out[i+1] BEFORE it is rewritten, so
   * it is still the pre-mix byte -- except on the last iteration, where it
   * wraps to out[0] and that byte HAS already been rewritten. Preserving that
   * asymmetry is required; a "clean" version that buffers the whole array
   * first produces a different header. */
  for (i = 0; i < (int)sizeof out; i++) {
    uint8_t value = out[i];
    uint8_t next;
    value = (uint8_t)(((value >> 4) | (value << 4)) & 255);
    next = (i + 1 < (int)sizeof out) ? out[i + 1] : out[0];
    out[i] = (uint8_t)(~(reverse_bits8((uint8_t)((next ^ value) & 255)) ^ 20) & 255);
  }

  final[0] = 0x84; final[1] = 0x04;
  final[2] = (uint8_t)(random & 255u);
  final[3] = (uint8_t)((random >> 8) & 255u);
  final[4] = 0x40; final[5] = 0x01;
  memcpy(final + 6, out, sizeof out);

  hg_hex(final, sizeof final, out_hex);
}

/* ---- helios (hongguo.cjs:394-401) --------------------------------------- */
int hg_helios(uint32_t khronos, hg_rand32_fn rnd, void *user,
              char *out_b64, size_t cap) {
  uint8_t data[8];                   /* le32(random) + "8662" */
  uint8_t digest[16];
  uint8_t ascii[33];                 /* hex of the digest + NUL */
  uint8_t pad[64], block[16], out[68];
  uint64_t words[4], table[35];
  uint64_t b0, b8, q0, q1;
  char text[64];
  int text_len, pad_len, at, i, nblocks, n;
  size_t produced;
  uint32_t random;

  if (rnd == NULL) return 0;
  random = rnd(user);                /* helios consumes exactly one draw, and in
                                      * the oracle's field order it is the FIRST
                                      * draw of the row */

  put_le32(data, random);
  memcpy(data + 4, "8662", 4);
  hg_md5(data, sizeof data, digest);
  hg_hex(digest, 16, (char *)ascii);

  for (i = 0; i < 4; i++) words[i] = get_le64(ascii + i * 8);

  /* rolling window: the reference's words[] holds [w2,w3] and push+shift keeps
   * it at length two throughout */
  table[0] = words[0];
  b0 = words[0];
  b8 = words[1];
  q0 = words[2];
  q1 = words[3];
  for (i = 0; i < 34; i++) {
    uint64_t x8 = hg_ror64(b8, 8) + b0;   /* uint64 wraps == BigInt.asUintN(64) */
    uint64_t shifted;
    x8 = x8 ^ (uint64_t)i;
    shifted = q0;                          /* words.shift() */
    q0 = q1;
    q1 = x8;
    x8 = x8 ^ hg_ror64(b0, 61);
    table[i + 1] = x8;
    b0 = x8;
    b8 = shifted;
  }

  /* `${khronos}-1588093228-8662` */
  n = u32_to_dec(khronos, text, sizeof text);
  if (n <= 0) return 0;
  text_len = n;
  {
    static const char suffix[] = "-1588093228-8662";
    size_t sl = strlen(suffix);
    if ((size_t)text_len + sl + 1 > sizeof text) return 0;
    memcpy(text + text_len, suffix, sl + 1);
    text_len += (int)sl;
  }

  /* Buffer.alloc(ceil((len+1)/16)*16, 16 - len%16) then text.copy(pad) */
  pad_len = ((text_len + 1 + 15) / 16) * 16;
  if (pad_len > (int)sizeof pad) return 0;
  {
    uint8_t fill = (uint8_t)(16 - (text_len % 16));
    memset(pad, fill, (size_t)pad_len);
    memcpy(pad, text, (size_t)text_len);
  }

  nblocks = pad_len / 16;
  for (at = 0; at < pad_len; at += 16) {
    uint64_t a = get_le64(pad + at);
    uint64_t b = get_le64(pad + at + 8);
    for (i = 0; i < 34; i++) {
      b = table[i] ^ (a + hg_ror64(b, 8));
      a = b ^ hg_ror64(a, 61);
    }
    put_le64(block, a);
    put_le64(block + 8, b);
    memcpy(out + 4 + (at / 16) * 16, block, 16);
  }
  put_le32(out, random);

  produced = (size_t)(4 + nblocks * 16);
  if (hg_base64_encode(out, produced, out_b64, cap) > cap) return 0;
  return 1;
}
