/*
 * 红果 "x-medusa" header.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV and cross-checked line for line
 * against the reference it derives from, server/vendor/hongguo.cjs:319-393
 * (varint/zigzag/protoField/proto, deviceProto, xmxor, keyHash, buildMedusa),
 * following woshishiq1/drpys @22261ad (GPL-3.0). The combined work stays GPL-3.0.
 *
 * This is the largest single piece of the signer: a protobuf-encoded message
 * containing a 38-field device fingerprint and an environment block, run through
 * a byte transform and then through the protocol's non-standard AesV3.
 *
 * THREE THINGS THAT SILENTLY CHANGE THE OUTPUT IF YOU GET THEM WRONG
 *   1. Draw order. buildMedusa consumes five draws -- messageRand, envLaunch,
 *      envPid, random, xmRand -- and helios has already consumed one before it
 *      in the reference's field order. See hg_sign.h.
 *   2. envLaunch/envPid are floor(random() * N), NOT random() % N. A modulo
 *      gives a different value for almost every draw.
 *   3. `transformed` is reversed AFTER an 8-byte header is prepended to it, so
 *      the header ends up at the END of the reversed buffer, and only then is
 *      the 4-byte seed XOR applied -- on the reversed bytes, not the original.
 *
 * Every buffer here is on the stack (~6 KB total). That is fine for a single
 * call on the app's main thread, but this function is NOT reentrant and must not
 * be called from a smaller stack (e.g. an audio callback).
 */

#include "hg_sign.h"
#include "hg_tables.h"

#include <string.h>

/* ---- bounded output buffer ---------------------------------------------- */
typedef struct { uint8_t *p; size_t len; size_t cap; int overflow; } HgBuf;

static void pb_raw(HgBuf *b, const void *data, size_t n) {
  if (b->overflow) return;
  if (b->len + n > b->cap) { b->overflow = 1; return; }
  memcpy(b->p + b->len, data, n);
  b->len += n;
}
static void pb_byte(HgBuf *b, uint8_t v) { pb_raw(b, &v, 1); }

/* varint (hongguo.cjs:321). Every value reaching this is non-negative: signed
 * fields go through pb_zigzag() first, which maps into 0..2^65. */
static void pb_varint(HgBuf *b, uint64_t v) {
  while (v > 127u) { pb_byte(b, (uint8_t)((v & 127u) | 128u)); v >>= 7; }
  pb_byte(b, (uint8_t)v);
}
/* zigzag (hongguo.cjs:320): n < 0 ? (-n)*2 - 1 : n*2 */
static uint64_t pb_zigzag(int64_t v) {
  return (v < 0) ? (((uint64_t)(-v) * 2u) - 1u) : ((uint64_t)v * 2u);
}
static void pb_key(HgBuf *b, int tag, int wire) {
  pb_varint(b, ((uint64_t)tag << 3) | (uint64_t)wire);
}
/* wire type 2, string. Empty strings are omitted, matching protoField(). */
static void pb_string(HgBuf *b, int tag, const char *s) {
  size_t n;
  if (s == NULL || s[0] == '\0') return;
  n = strlen(s);
  pb_key(b, tag, 2);
  pb_varint(b, (uint64_t)n);
  pb_raw(b, s, n);
}
/* wire type 2, already-encoded bytes or a nested message */
static void pb_bytes(HgBuf *b, int tag, const uint8_t *data, size_t n) {
  if (data == NULL) return;
  pb_key(b, tag, 2);
  pb_varint(b, (uint64_t)n);
  pb_raw(b, data, n);
}
static void pb_message(HgBuf *b, int tag, const HgBuf *sub) {
  if (sub->overflow) { b->overflow = 1; return; }
  pb_key(b, tag, 2);
  pb_varint(b, (uint64_t)sub->len);
  pb_raw(b, sub->p, sub->len);
}
/* wire type 0 */
static void pb_sint(HgBuf *b, int tag, int64_t v) {
  pb_key(b, tag, 0);
  pb_varint(b, pb_zigzag(v));
}
/* wire type 5. Written byte-by-byte so the encoding does not depend on the
 * host's float endianness. */
static void pb_float(HgBuf *b, int tag, float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  pb_key(b, tag, 5);
  pb_byte(b, (uint8_t)bits);
  pb_byte(b, (uint8_t)(bits >> 8));
  pb_byte(b, (uint8_t)(bits >> 16));
  pb_byte(b, (uint8_t)(bits >> 24));
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- constants ---------------------------------------------------------- */
static const char SIGN_KEY_HEX[] =
  "8ebdfa3806ecc5cee79423e6029ed82540bc2218bb7eaef71cb691f7aa8aa2f5";
static const char AESV3_KEY_HEX[] = "f1593376766ea98d34f31b057a9d5be4";
static const char AESV3_IV_HEX[]  = "1fe109a4125283f418de9e051a969e12";
static const uint8_t VERSION_20[20] = {
  0x03, 0x00, 0x00, 0x00, 0xf7, 0xe8, 0x5f, 0xfa, 0xd7, 0xd7,
  0xdc, 0x3b, 0xd6, 0x2a, 0xc8, 0x70, 0x57, 0xcf, 0x61, 0x18
};
static const uint8_t MSG1_BYTES[16] = {
  0xf7, 0xe8, 0x5f, 0xfa, 0xd7, 0xd7, 0xdc, 0x3b,
  0xd6, 0x2a, 0xc8, 0x70, 0x57, 0xcf, 0x61, 0x18
};
static const uint8_t MSG10_BYTES[8] = { 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t MEDUSA_PRE8[8] = { 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const char MSG24_JSON[] =
  "{\"cmr\":16777216,\"cmr2\":16777216,\"un_h\":1879194040,\"vpn\":0,\"kd\":0,"
  "\"fkd\":3672518972,\"pd\":-1872573247,\"dyn\":\"\",\"do\":0,\"tk\":true}";

/* Device fingerprint values come from the generated table so they stay in sync
 * with VendorConstants.kt rather than being duplicated here. */
static const char *device_value(const char *key) {
  int i;
  for (i = 0; i < HG_DEVICE_COUNT; i++)
    if (strcmp(HG_DEVICE[i].key, key) == 0) return HG_DEVICE[i].value;
  return NULL;
}

/* floor(random() * scale), where random() is uniform in [0,1) and `draw` is the
 * underlying 32-bit value. Deliberately not `draw % scale`: the reference scales
 * the fraction, so the two disagree on almost every draw. */
static uint32_t floor_frac(uint32_t draw, uint32_t scale) {
  return (uint32_t)(((uint64_t)draw * (uint64_t)scale) >> 32);
}

/* ---- deviceProto (hongguo.cjs:373) -------------------------------------- */
/* The reference's deviceProto(deviceId, version) never reads `version`; the
 * parameter is kept here only so the two signatures stay recognisably the same. */
static void build_device_proto(HgBuf *b, const char *device_id, const char *version) {
  (void)version;
  pb_sint(b, 1, 1);
  pb_sint(b, 2, 2);
  pb_string(b, 3, "8662");
  pb_string(b, 4, device_id);
  pb_string(b, 5, "Ai6svO3PyrwDOUSmO6ZcResxu");
  pb_string(b, 6, "!noperm!");
  pb_sint(b, 7, -888888);
  pb_sint(b, 8, -888888);
  pb_sint(b, 9, 3);
  pb_sint(b, 10, -888888);
  pb_string(b, 11, "!notset!");
  pb_string(b, 12, "Asia/Shanghai,8");
  pb_string(b, 13, "zh_CN");
  pb_sint(b, 14, 4);
  pb_float(b, 16, 255.24993896484375f);
  pb_float(b, 17, 35.58599090576172f);
  pb_float(b, 18, 3.467449188232422f);
  pb_float(b, 19, 3.467449188232422f);
  pb_float(b, 20, 255.1754913330078f);
  pb_float(b, 21, 42.17544174194336f);
  pb_string(b, 22, "16");
  pb_sint(b, 23, 41);
  pb_sint(b, 24, 36);
  /* these four exceed 2^32, so the varint must be computed in 64 bits */
  pb_sint(b, 25, 1728388016635LL);
  pb_sint(b, 26, 1728388016635LL);
  pb_sint(b, 27, 1728388016635LL);
  pb_sint(b, 28, 1728388016637LL);
  pb_sint(b, 29, -1);
  pb_string(b, 30, "25053RT47C");
  pb_string(b, 31, "Redmi");
  pb_string(b, 32, "25053RT47C");
  pb_string(b, 33, "25053RT47C");
  pb_string(b, 34, "Xiaomi");
  pb_string(b, 35, "Redmi");
  pb_string(b, 36, "Redmi");
  pb_sint(b, 38, 31);
}

/* ---- mm4 => xmxor (hongguo.cjs:389) ------------------------------------- */
static void xmxor(const uint8_t *data, size_t len, const uint8_t *key,
                  size_t keylen, uint8_t *encoded) {
  size_t i;
  uint32_t last, first, sum;

  if (key == NULL) return;
  if (keylen < 32) return;     /* the key is always the 32-byte sm3 */
  if (len < 3) return;

  for (i = 0; i < len; i++) {
    size_t at = (i * 4u) & 28u;
    uint8_t d0, d1;
    uint32_t d2;
    d0 = key[at];
    d1 = key[at + 1];
    d2 = ((uint32_t)((uint8_t)((data[i] << 4) | (data[i] >> 4)))) + (uint32_t)d0;
    d2 = (~d2) ^ (uint32_t)d1;
    {
      uint32_t low = d2 & 255u;
      d2 = (uint32_t)((((low << 3) | (low >> 5))) & 255u);
    }
    d2 = (d2 + (uint32_t)d1) & 255u;
    d2 = (d2 ^ (uint32_t)d0) & 255u;
    encoded[len - i - 1] = (uint8_t)((~d2) & 255u);
  }

  last = (uint32_t)encoded[len - 1] ^ (uint32_t)encoded[len - 2];
  first = encoded[0];
  encoded[0] = (uint8_t)(((~last) + first) & 255u);
  encoded[1] = (uint8_t)((((uint32_t)encoded[0] ^ (uint32_t)encoded[len - 1] ^ 254u) + (uint32_t)encoded[1]) & 255u);
  encoded[2] = (uint8_t)(((uint32_t)encoded[2] +
      (((last - first) ^ (((uint32_t)encoded[1] << 3) | ((uint32_t)encoded[1] >> 5)) ^ 2u))) & 255u);

  for (i = 0; i + 4 <= len - 1; i++) {
    uint32_t temp = ((((uint32_t)encoded[i + 2] << 3) | ((uint32_t)encoded[i + 2] >> 5)) & 255u)
                    ^ (uint32_t)encoded[i + 1] ^ (uint32_t)(i + 3);
    encoded[i + 3] = (uint8_t)(((~temp) + (uint32_t)encoded[i + 3]) & 255u);
  }

  encoded[len - 1] ^= encoded[len - 2];

  sum = 0;
  for (i = 0; i + 1 < len; i++) sum += encoded[i + 1];
  encoded[0] = (uint8_t)((((uint32_t)encoded[0] ^ (uint32_t)encoded[1]) + sum) & 255u);
}

/* ---- keyHash (hongguo.cjs:390) ------------------------------------------ */
static void key_hash(const uint8_t *sign_key, size_t sign_key_len, uint32_t random,
                     uint8_t hash_out[32], uint8_t seed_out[4]) {
  uint8_t input[80];
  uint8_t tmp[4];
  uint32_t d1, d2;

  memcpy(input, sign_key, sign_key_len);
  put_le32(input + sign_key_len, random);
  memcpy(input + sign_key_len + 4, sign_key, sign_key_len);
  hg_sm3(input, sign_key_len * 2 + 4, hash_out);

  d1 = (random >> 16) & 255u;
  d2 = ((d1 << 11) | (random >> 24)) ^ (d1 >> 5) ^ d1;
  d2 = ~d2;
  put_le32(tmp, d2);
  memcpy(seed_out, tmp, 4);
}

/* ---- buildMedusa (hongguo.cjs:391-392) ---------------------------------- */
int hg_build_medusa(const char *url, const uint8_t *body, size_t body_len,
                    uint32_t khronos, hg_rand32_fn rnd, void *user,
                    char *out_b64, size_t cap) {
  uint8_t body_md5[16], query_sm3[32], query_body_ts[20], query_hash[32];
  uint8_t key_hash_out[32];
  uint8_t sign_key[32], aes_key[16], aes_iv[16];
  uint8_t nested_buf[64], dev_buf[512], env_buf[640], msg13_buf[64], msg_buf[1024];
  uint8_t xmx_buf[1024], transformed[1040], packed[1120], encrypted[1140];
  uint8_t final[1200], prefix[20];
  HgBuf nested, dev, env, msg, msg13;
  const char *qmark, *query;
  const char *device_id, *version_name;
  uint32_t message_rand, env_launch, env_pid, random, xm_rand, check;
  uint8_t seed[4];
  hg_aesv3_ctx aes;
  size_t nested_len, transformed_len, packed_len, final_len, sign_key_len, klen;
  size_t i, b64_need;

  if (rnd == NULL) return 0;
  qmark = strchr(url, '?');
  if (qmark == NULL) return 0;
  query = qmark + 1;

  device_id = device_value("device_id");
  version_name = device_value("version_name");
  if (device_id == NULL || version_name == NULL) return 0;

  klen = 0;
  if (!hg_unhex(SIGN_KEY_HEX, sign_key, sizeof sign_key, &sign_key_len)) return 0;
  if (!hg_unhex(AESV3_KEY_HEX, aes_key, sizeof aes_key, &klen) || klen != 16) return 0;
  if (!hg_unhex(AESV3_IV_HEX, aes_iv, sizeof aes_iv, &klen) || klen != 16) return 0;

  if (body != NULL && body_len > 0) hg_md5(body, body_len, body_md5);
  else memset(body_md5, 0, sizeof body_md5);
  hg_sm3(query, strlen(query), query_sm3);

  if (!hg_hash_f13_bytes(query_sm3, body_md5, khronos, query_body_ts)) return 0;

  /* nested (field 15) */
  nested.p = nested_buf; nested.cap = sizeof nested_buf; nested.len = 0; nested.overflow = 0;
  pb_sint(&nested, 1, 111);
  pb_sint(&nested, 2, 10);
  pb_sint(&nested, 3, 694367);
  pb_sint(&nested, 5, 586952199);
  if (nested.overflow) return 0;
  nested_len = nested.len;

  /* draws, in the reference's evaluation order */
  message_rand = rnd(user);
  env_launch = floor_frac(rnd(user), 21u) + 100u;
  env_pid = floor_frac(rnd(user), 2000u) + 10001u;

  /* env (field 23) */
  dev.p = dev_buf; dev.cap = sizeof dev_buf; dev.len = 0; dev.overflow = 0;
  build_device_proto(&dev, device_id, version_name);
  if (dev.overflow) return 0;

  msg13.p = msg13_buf; msg13.cap = sizeof msg13_buf; msg13.len = 0; msg13.overflow = 0;
  /* Date.now() is mocked to khronos*1000 in the oracle, so this is khronos. */
  pb_sint(&msg13, 1, (int64_t)khronos);
  pb_sint(&msg13, 2, -2);
  pb_sint(&msg13, 4, 200);
  if (msg13.overflow) return 0;

  env.p = env_buf; env.cap = sizeof env_buf; env.len = 0; env.overflow = 0;
  pb_sint(&env, 1, (int64_t)env_launch);
  pb_sint(&env, 2, 146331399);
  pb_sint(&env, 3, 146331396);
  pb_sint(&env, 5, 7);
  pb_string(&env, 6, "v04.06.04.03-bugfix");
  pb_sint(&env, 7, (int64_t)env_pid);
  pb_message(&env, 12, &dev);
  pb_message(&env, 13, &msg13);
  pb_string(&env, 14, version_name);
  if (env.overflow) return 0;

  {
    /* Sized for the LARGEST query a request can carry, not for the fixtures.
     *
     * This buffer was 80 bytes, chosen by looking at the test vectors -- whose
     * URLs carry a query of about 30 characters. A real device fingerprint query
     * is 555, so `qlen + 20 > 80` refused EVERY genuine request while all 12
     * vectors kept passing, because the bound was never reached by a short
     * fixture. On the console this surfaced as "FAILED to build request: -1"
     * from hg_request_search, with nothing pointing at medusa.
     *
     * The limit must come from the caller's maximum URL (HG_REQ_URL_MAX), never
     * from the size of the data that happens to be in the tests. */
    uint8_t concat[2048 + 20];
    size_t qlen = strlen(query);
    if (qlen + 16 + 4 > sizeof concat) return 0;
    memcpy(concat, query, qlen);
    memcpy(concat + qlen, body_md5, 16);
    memcpy(concat + qlen + 16, "none", 4);
    hg_sm3(concat, qlen + 20, query_hash);
  }

  /* message */
  msg.p = msg_buf; msg.cap = sizeof msg_buf; msg.len = 0; msg.overflow = 0;
  pb_bytes(&msg, 1, MSG1_BYTES, sizeof MSG1_BYTES);
  pb_sint(&msg, 2, 3);
  pb_sint(&msg, 3, (int64_t)message_rand);
  pb_string(&msg, 4, "8662");
  pb_string(&msg, 5, device_id);
  pb_string(&msg, 6, "1588093228");
  pb_string(&msg, 7, version_name);
  pb_string(&msg, 8, "v04.06.04-ml-android");
  pb_sint(&msg, 9, 67503104);
  pb_bytes(&msg, 10, MSG10_BYTES, sizeof MSG10_BYTES);
  pb_sint(&msg, 12, (int64_t)khronos);
  pb_bytes(&msg, 13, query_body_ts, sizeof query_body_ts);
  pb_bytes(&msg, 14, query_sm3, 6);
  pb_bytes(&msg, 15, nested_buf, nested_len);
  pb_string(&msg, 16, "AXYQOS6n2m60x1fVZHIrH3iol");
  pb_sint(&msg, 17, (int64_t)khronos);
  pb_bytes(&msg, 19, query_hash, sizeof query_hash);
  pb_string(&msg, 20, "none");
  pb_sint(&msg, 21, 312);
  pb_bytes(&msg, 23, env.p, env.len);
  pb_string(&msg, 24, MSG24_JSON);
  if (msg.overflow) return 0;

  /* the fifth draw happens here, after `random` was used above */
  random = rnd(user);
  key_hash(sign_key, sign_key_len, random, key_hash_out, seed);

  xmxor(msg_buf, msg.len, key_hash_out, sizeof key_hash_out, xmx_buf);

  /* prepend 8 bytes, THEN reverse the whole thing */
  memcpy(transformed + 8, xmx_buf, msg.len);
  memcpy(transformed, MEDUSA_PRE8, 8);
  transformed_len = msg.len + 8;
  for (i = 0; i < transformed_len / 2; i++) {
    uint8_t t = transformed[i];
    transformed[i] = transformed[transformed_len - 1 - i];
    transformed[transformed_len - 1 - i] = t;
  }
  for (i = 0; i < transformed_len; i++)
    transformed[i] ^= seed[(uint32_t)(~(uint32_t)i) & 3u];

  check = ((uint32_t)(query_sm3[0] & 63u) << 14) | 0x18000001u
        | ((uint32_t)(query_body_ts[0] & 63u) << 8);

  xm_rand = rnd(user);

  /* packed */
  {
    size_t p = 0;
    packed[p++] = 0x35;
    put_le32(packed + p, xm_rand); p += 4;
    put_le32(packed + p, check);   p += 4;
    memcpy(packed + p, transformed, transformed_len); p += transformed_len;
    packed[p++] = (uint8_t)(random >> 16);
    packed[p++] = (uint8_t)(random >> 24);
    packed_len = p;
  }
  if (packed_len < 248) return 0;   /* AesV3.encrypt requires >= 248 bytes */

  hg_aesv3_init(&aes, aes_key, khronos);
  hg_aesv3_encrypt(&aes, aes_iv, packed, packed_len, encrypted);

  for (i = 0; i < 20; i += 4)
    put_le32(prefix + i, get_le32(VERSION_20 + i) ^ khronos);

  final_len = 0;
  memcpy(final + final_len, prefix, 20); final_len += 20;
  final[final_len++] = (uint8_t)(random & 255u);
  final[final_len++] = (uint8_t)((random >> 8) & 255u);
  final[final_len++] = 0;
  final[final_len++] = 1;
  memcpy(final + final_len, encrypted, packed_len + 1); final_len += packed_len + 1;

  b64_need = ((final_len + 2) / 3) * 4;
  if (b64_need + 1 > cap) return 0;          /* would not fit the caller's buffer */
  hg_base64_encode(final, final_len, out_b64, cap);
  return 1;
}
