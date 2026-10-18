/*
 * Self-contained hash / cipher primitives for the 红果 request signer.
 * See hg_crypto.h for the portability contract and the RC4 warning.
 *
 * GPL-3.0-only. MD5 (RFC 1321), SHA-512 (FIPS 180-4) and SM3 (GB/T 32905-2016)
 * are implemented from their published specifications. The RC4 variant follows
 * the protocol's reference implementation (server/vendor/hongguo.cjs:360-364).
 */

#include "hg_crypto.h"

#include <string.h>

/* ======================================================================== */
/* shared helpers                                                            */
/* ======================================================================== */

uint32_t hg_rol32(uint32_t v, int n) { n &= 31; return n ? ((v << n) | (v >> (32 - n))) : v; }
uint32_t hg_ror32(uint32_t v, int n) { n &= 31; return n ? ((v >> n) | (v << (32 - n))) : v; }
uint64_t hg_ror64(uint64_t v, int n) { n &= 63; return n ? ((v >> n) | (v << (64 - n))) : v; }

static uint32_t rd32_be(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr32_be(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static uint32_t rd32_le(const unsigned char *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}
static void wr32_le(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint64_t rd64_be(const unsigned char *p) {
  return ((uint64_t)rd32_be(p) << 32) | (uint64_t)rd32_be(p + 4);
}
static void wr64_be(unsigned char *p, uint64_t v) {
  wr32_be(p, (uint32_t)(v >> 32)); wr32_be(p + 4, (uint32_t)v);
}

/* ======================================================================== */
/* MD5 (RFC 1321)                                                            */
/* ======================================================================== */

static const uint32_t MD5_K[64] = {
  0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
  0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
  0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
  0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
  0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
  0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
  0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
  0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
  0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
  0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
  0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};
static const unsigned char MD5_S[64] = {
  7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
  5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
  4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
  6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

void hg_md5_init(hg_md5_ctx *c) {
  c->state[0] = 0x67452301u; c->state[1] = 0xefcdab89u;
  c->state[2] = 0x98badcfeu; c->state[3] = 0x10325476u;
  c->bitlen = 0; c->buffer_len = 0;
}

static void md5_block(uint32_t st[4], const unsigned char b[64]) {
  uint32_t m[16], a = st[0], bb = st[1], cc = st[2], d = st[3];
  int i;
  for (i = 0; i < 16; i++) m[i] = rd32_le(b + i * 4);
  for (i = 0; i < 64; i++) {
    uint32_t f; int g;
    if (i < 16)      { f = (bb & cc) | (~bb & d);        g = i; }
    else if (i < 32) { f = (d & bb) | (~d & cc);         g = (5 * i + 1) & 15; }
    else if (i < 48) { f = bb ^ cc ^ d;                  g = (3 * i + 5) & 15; }
    else             { f = cc ^ (bb | ~d);               g = (7 * i) & 15; }
    {
      uint32_t tmp = d;
      d = cc;
      cc = bb;
      bb = bb + hg_rol32(a + f + MD5_K[i] + m[g], MD5_S[i]);
      a = tmp;
    }
  }
  st[0] += a; st[1] += bb; st[2] += cc; st[3] += d;
}

void hg_md5_update(hg_md5_ctx *c, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  c->bitlen += (uint64_t)len * 8u;
  if (c->buffer_len) {
    size_t need = 64 - c->buffer_len;
    if (len < need) { memcpy(c->buffer + c->buffer_len, p, len); c->buffer_len += len; return; }
    memcpy(c->buffer + c->buffer_len, p, need);
    md5_block(c->state, c->buffer);
    p += need; len -= need; c->buffer_len = 0;
  }
  while (len >= 64) { md5_block(c->state, p); p += 64; len -= 64; }
  if (len) { memcpy(c->buffer, p, len); c->buffer_len = len; }
}

void hg_md5_final(hg_md5_ctx *c, unsigned char out[16]) {
  unsigned char pad[72];
  size_t padlen;
  uint64_t bits = c->bitlen;
  int i;
  padlen = (c->buffer_len < 56) ? (56 - c->buffer_len) : (120 - c->buffer_len);
  memset(pad, 0, sizeof pad);
  pad[0] = 0x80;
  for (i = 0; i < 8; i++) pad[padlen + i] = (unsigned char)(bits >> (8 * i)); /* LE length */
  hg_md5_update(c, pad, padlen + 8);
  for (i = 0; i < 4; i++) wr32_le(out + i * 4, c->state[i]);
}

void hg_md5(const void *data, size_t len, unsigned char out[16]) {
  hg_md5_ctx c; hg_md5_init(&c); hg_md5_update(&c, data, len); hg_md5_final(&c, out);
}

/* ======================================================================== */
/* SHA-512 (FIPS 180-4)                                                      */
/* ======================================================================== */

static const uint64_t SHA512_K[80] = {
  0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
  0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
  0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
  0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
  0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
  0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
  0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
  0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
  0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
  0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
  0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
  0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
  0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
  0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
  0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
  0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
  0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
  0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
  0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
  0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

void hg_sha512_init(hg_sha512_ctx *c) {
  c->state[0] = 0x6a09e667f3bcc908ull; c->state[1] = 0xbb67ae8584caa73bull;
  c->state[2] = 0x3c6ef372fe94f82bull; c->state[3] = 0xa54ff53a5f1d36f1ull;
  c->state[4] = 0x510e527fade682d1ull; c->state[5] = 0x9b05688c2b3e6c1full;
  c->state[6] = 0x1f83d9abfb41bd6bull; c->state[7] = 0x5be0cd19137e2179ull;
  c->bitlen = 0; c->buffer_len = 0;
}

static void sha512_block(uint64_t st[8], const unsigned char b[128]) {
  uint64_t w[80], a, bb, cc, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) w[i] = rd64_be(b + i * 8);
  for (i = 16; i < 80; i++) {
    uint64_t s0 = hg_ror64(w[i - 15], 1) ^ hg_ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 = hg_ror64(w[i - 2], 19) ^ hg_ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = st[0]; bb = st[1]; cc = st[2]; d = st[3];
  e = st[4]; f = st[5]; g = st[6]; h = st[7];
  for (i = 0; i < 80; i++) {
    uint64_t S1 = hg_ror64(e, 14) ^ hg_ror64(e, 18) ^ hg_ror64(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = h + S1 + ch + SHA512_K[i] + w[i];
    uint64_t S0 = hg_ror64(a, 28) ^ hg_ror64(a, 34) ^ hg_ror64(a, 39);
    uint64_t maj = (a & bb) ^ (a & cc) ^ (bb & cc);
    uint64_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = bb; bb = a; a = t1 + t2;
  }
  st[0] += a; st[1] += bb; st[2] += cc; st[3] += d;
  st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void bitcount_add(uint64_t *bits, size_t len) {
  *bits += (uint64_t)len * 8u;
}

void hg_sha512_update(hg_sha512_ctx *c, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  bitcount_add(&c->bitlen, len);
  if (c->buffer_len) {
    size_t need = 128 - c->buffer_len;
    if (len < need) { memcpy(c->buffer + c->buffer_len, p, len); c->buffer_len += len; return; }
    memcpy(c->buffer + c->buffer_len, p, need);
    sha512_block(c->state, c->buffer);
    p += need; len -= need; c->buffer_len = 0;
  }
  while (len >= 128) { sha512_block(c->state, p); p += 128; len -= 128; }
  if (len) { memcpy(c->buffer, p, len); c->buffer_len = len; }
}

void hg_sha512_final(hg_sha512_ctx *c, unsigned char out[64]) {
  unsigned char pad[256];
  size_t padlen;
  /* The context carries a single 64-bit bit counter, so the 128-bit length
   * field is written with a zero high word. Correct for any message below
   * 2^61 bytes, which is far past anything this protocol can produce. */
  uint64_t bits = c->bitlen;
  int i;
  /* 128-bit length field; need buffer_len + 1 + pad + 16 == 0 mod 128 */
  padlen = (c->buffer_len < 112) ? (112 - c->buffer_len) : (240 - c->buffer_len);
  memset(pad, 0, padlen + 16);
  pad[0] = 0x80;
  wr64_be(pad + padlen, 0);
  wr64_be(pad + padlen + 8, bits);
  hg_sha512_update(c, pad, padlen + 16);
  for (i = 0; i < 8; i++) wr64_be(out + i * 8, c->state[i]);
}

void hg_sha512(const void *data, size_t len, unsigned char out[64]) {
  hg_sha512_ctx c; hg_sha512_init(&c); hg_sha512_update(&c, data, len); hg_sha512_final(&c, out);
}

/* ======================================================================== */
/* SM3 (GB/T 32905-2016)                                                     */
/* ======================================================================== */

static uint32_t sm3_p0(uint32_t x) { return x ^ hg_rol32(x, 9) ^ hg_rol32(x, 17); }
static uint32_t sm3_p1(uint32_t x) { return x ^ hg_rol32(x, 15) ^ hg_rol32(x, 23); }

void hg_sm3_init(hg_sm3_ctx *c) {
  c->state[0] = 0x7380166fu; c->state[1] = 0x4914b2b9u;
  c->state[2] = 0x172442d7u; c->state[3] = 0xda8a0600u;
  c->state[4] = 0xa96f30bcu; c->state[5] = 0x163138aau;
  c->state[6] = 0xe38dee4du; c->state[7] = 0xb0fb0e4eu;
  c->bitlen = 0; c->buffer_len = 0;
}

static void sm3_block(uint32_t st[8], const unsigned char b[64]) {
  uint32_t w[68], wp[64];
  uint32_t a, bb, cc, d, e, f, g, h;
  int j;
  for (j = 0; j < 16; j++) w[j] = rd32_be(b + j * 4);
  for (j = 16; j < 68; j++) {
    uint32_t x = w[j - 16] ^ w[j - 9] ^ hg_rol32(w[j - 3], 15);
    w[j] = sm3_p1(x) ^ hg_rol32(w[j - 13], 7) ^ w[j - 6];
  }
  for (j = 0; j < 64; j++) wp[j] = w[j] ^ w[j + 4];

  a = st[0]; bb = st[1]; cc = st[2]; d = st[3];
  e = st[4]; f = st[5]; g = st[6]; h = st[7];

  for (j = 0; j < 64; j++) {
    /* T_j rotates by j, which exceeds 32 for j >= 32: rotate j & 31. */
    uint32_t tj = (j < 16) ? 0x79cc4519u : 0x7a879d8au;
    uint32_t ss1 = hg_rol32(hg_rol32(a, 12) + e + hg_rol32(tj, j & 31), 7);
    uint32_t ss2 = ss1 ^ hg_rol32(a, 12);
    uint32_t ff, gg, tt1, tt2;
    if (j < 16) { ff = a ^ bb ^ cc; gg = e ^ f ^ g; }
    else        { ff = (a & bb) | (a & cc) | (bb & cc);
                  gg = (e & f) | (~e & g); }
    tt1 = ff + d + ss2 + wp[j];
    tt2 = gg + h + ss1 + w[j];
    d = cc;
    cc = hg_rol32(bb, 9);
    bb = a;
    a = tt1;
    h = g;
    g = hg_rol32(f, 19);
    f = e;
    e = sm3_p0(tt2);
  }
  st[0] ^= a; st[1] ^= bb; st[2] ^= cc; st[3] ^= d;
  st[4] ^= e; st[5] ^= f; st[6] ^= g; st[7] ^= h;
}

void hg_sm3_update(hg_sm3_ctx *c, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  bitcount_add(&c->bitlen, len);
  if (c->buffer_len) {
    size_t need = 64 - c->buffer_len;
    if (len < need) { memcpy(c->buffer + c->buffer_len, p, len); c->buffer_len += len; return; }
    memcpy(c->buffer + c->buffer_len, p, need);
    sm3_block(c->state, c->buffer);
    p += need; len -= need; c->buffer_len = 0;
  }
  while (len >= 64) { sm3_block(c->state, p); p += 64; len -= 64; }
  if (len) { memcpy(c->buffer, p, len); c->buffer_len = len; }
}

void hg_sm3_final(hg_sm3_ctx *c, unsigned char out[32]) {
  unsigned char pad[72];
  size_t padlen;
  uint64_t bits = c->bitlen;
  int i;
  padlen = (c->buffer_len < 56) ? (56 - c->buffer_len) : (120 - c->buffer_len);
  memset(pad, 0, sizeof pad);
  pad[0] = 0x80;
  /* SM3 hashes 64-byte blocks and appends an 8-byte big-endian bit count --
   * NOT the 16-byte length field SHA-512 uses. Getting this wrong both
   * produces the wrong digest and overruns pad[] (padlen can be 63, so a
   * 16-byte field would need 79 bytes). */
  wr64_be(pad + padlen, bits);
  hg_sm3_update(c, pad, padlen + 8);
  for (i = 0; i < 8; i++) wr32_be(out + i * 4, c->state[i]);
}

void hg_sm3(const void *data, size_t len, unsigned char out[32]) {
  hg_sm3_ctx c; hg_sm3_init(&c); hg_sm3_update(&c, data, len); hg_sm3_final(&c, out);
}

/* ======================================================================== */
/* the vendor's abridged "RC4"  -- see hg_crypto.h for why this is not RC4    */
/* ======================================================================== */

void hg_rc4_init(hg_rc4_ctx *ctx, const uint8_t *key, size_t key_len) {
  int i;
  uint32_t j = 0;
  for (i = 0; i < 256; i++) ctx->s[i] = (uint8_t)i;
  for (i = 0; i < 256; i++) {
    /* NOTE: reference does `s[i] = s[j]` with NO second half of the swap. */
    j = (j + ctx->s[i] + (uint32_t)key[i % (int)key_len]) & 255u;
    ctx->s[i] = ctx->s[j];
  }
  ctx->i = 0;
  ctx->j = 0;
}

uint8_t hg_rc4_next(hg_rc4_ctx *ctx, uint8_t in) {
  uint8_t x, y;
  ctx->i += 1;                       /* pre-increment: first index is 1 */
  x = ctx->s[ctx->i];
  ctx->j = (uint8_t)(ctx->j + x);
  y = ctx->s[ctx->j];
  ctx->s[ctx->i] = y;                /* again: no completed swap */
  return (uint8_t)(in ^ ctx->s[(uint8_t)(y + y)]);
}

void hg_rc4_apply(hg_rc4_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len) {
  size_t n;
  for (n = 0; n < len; n++) out[n] = hg_rc4_next(ctx, in[n]);
}

/* ======================================================================== */
/* hex + base64                                                              */
/* ======================================================================== */

static const char HEX_LO[] = "0123456789abcdef";
static const char HEX_UP[] = "0123456789ABCDEF";

static void hex_impl(const uint8_t *data, size_t len, char *out, const char *digits) {
  size_t i;
  for (i = 0; i < len; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 15];
  }
  out[len * 2] = '\0';
}

void hg_hex(const uint8_t *data, size_t len, char *out) { hex_impl(data, len, out, HEX_LO); }
void hg_hex_upper(const uint8_t *data, size_t len, char *out) { hex_impl(data, len, out, HEX_UP); }

static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

int hg_unhex(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len) {
  size_t n = 0, i = 0;
  size_t len = strlen(hex);
  if (len % 2) return 0;
  if (len / 2 > out_cap) return 0;
  while (i < len) {
    int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[n++] = (uint8_t)((hi << 4) | lo);
    i += 2;
  }
  if (out_len) *out_len = n;
  return 1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t hg_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap) {
  size_t need = ((len + 2) / 3) * 4;
  size_t i = 0, o = 0;
  if (need + 1 > out_cap) return need;
  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
    out[o++] = B64[(v >> 6) & 63];  out[o++] = B64[v & 63];
    i += 3;
  }
  if (len - i == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '=';
  } else if (len - i == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
    out[o++] = B64[(v >> 6) & 63];  out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

static int b64val(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

int hg_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
  size_t n = 0, i = 0, o = 0;
  while (in[n] && in[n] != '=') n++;
  /* recompute length including '=' run */
  n = strlen(in);
  while (i < n) {
    int v0, v1, v2, v3;
    int pad = 0;
    if (in[i] == '\n' || in[i] == '\r') { i++; continue; }
    v0 = b64val(in[i]); if (v0 < 0) return 0;
    if (i + 1 >= n) return 0;
    v1 = b64val(in[i + 1]); if (v1 < 0) return 0;
    v2 = (i + 2 < n && in[i + 2] != '=') ? b64val(in[i + 2]) : -2;
    v3 = (i + 3 < n && in[i + 3] != '=') ? b64val(in[i + 3]) : -2;
    if (v2 == -1 || v3 == -1) return 0;
    if (o >= out_cap) return 0;
    out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
    if (v2 == -2) { pad = 1; }
    else {
      if (o >= out_cap) return 0;
      out[o++] = (uint8_t)(((v1 & 15) << 4) | (v2 >> 2));
      if (v3 == -2) pad = 1;
      else {
        if (o >= out_cap) return 0;
        out[o++] = (uint8_t)(((v2 & 3) << 6) | v3);
      }
    }
    i += 4;
    if (pad) break;
  }
  if (out_len) *out_len = o;
  return 1;
}
