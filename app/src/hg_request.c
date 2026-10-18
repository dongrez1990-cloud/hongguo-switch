/*
 * Signed-request assembly. See hg_request.h for the encoder rules and why they
 * are pinned to vectors. Line references are to hongguo.cjs.
 */

#include "hg_request.h"
#include "hg_sign.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* URL encoding (hongguo.cjs:79)                                             */
/* ------------------------------------------------------------------------ */

/* Safe: A-Z a-z 0-9 - _ . ~ *   -- note `*`, which Python's quote() escapes. */
static int enc_is_safe(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= 'a' && c <= 'z') return 1;
  if (c >= '0' && c <= '9') return 1;
  if (c == '-' || c == '_' || c == '.' || c == '~') return 1;
  if (c == '*') return 1;
  return 0;
}

int hg_url_encode(const char *in, char *out, size_t cap) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t w = 0;
  const unsigned char *p = (const unsigned char *)in;
  if (in == NULL || out == NULL || cap == 0) return -1;
  for (; *p != 0; p++) {
    if (enc_is_safe(*p)) {
      if (w + 1 >= cap) return -1;
      out[w++] = (char)*p;
    } else {
      if (w + 3 >= cap) return -1;
      out[w++] = '%';
      /* Byte-wise, so UTF-8 comes out as one %XX per byte, uppercase hex. */
      out[w++] = HEX[(*p) >> 4];
      out[w++] = HEX[(*p) & 15];
    }
  }
  out[w] = '\0';
  return (int)w;
}

static int append(char *out, size_t cap, size_t *w, const char *s) {
  size_t n = strlen(s);
  if (*w + n + 1 > cap) return 0;
  memcpy(out + *w, s, n);
  *w += n;
  out[*w] = '\0';
  return 1;
}

int hg_url_encode_map(const HgKv *extra, size_t extra_count, char *out, size_t cap) {
  return hg_url_encode_map_ex(NULL, 0, extra, extra_count, out, cap);
}

static const char *find_override(const HgKv *overrides, size_t n, const char *key) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (strcmp(overrides[i].key, key) == 0) return overrides[i].value;
  }
  return NULL;
}

int hg_url_encode_map_ex(const HgKv *overrides, size_t override_count,
                         const HgKv *extra, size_t extra_count,
                         char *out, size_t cap) {
  size_t i, w = 0;
  char kbuf[256], vbuf[1024];

  if (out == NULL || cap == 0) return -1;
  out[0] = '\0';

  /* The device fingerprint first, in its declared order, then the extras. The
   * signature covers this string, so the order is load-bearing. */
  for (i = 0; i < HG_DEVICE_COUNT + extra_count; i++) {
    const char *k, *v;
    const char *ov;
    if (i < HG_DEVICE_COUNT) {
      k = HG_DEVICE[i].key;
      /* An override replaces the value but keeps the key's position, matching
       * object-spread semantics in the reference. */
      ov = (overrides != NULL) ? find_override(overrides, override_count, k) : NULL;
      v = (ov != NULL) ? ov : HG_DEVICE[i].value;
    } else {
      k = extra[i - HG_DEVICE_COUNT].key;
      v = extra[i - HG_DEVICE_COUNT].value;
      /* A key that already exists in the device table must not be appended a
       * second time. */
      if (find_override(HG_DEVICE, HG_DEVICE_COUNT, k) != NULL) continue;
    }

    if (hg_url_encode(k, kbuf, sizeof kbuf) < 0) return -1;
    if (hg_url_encode(v, vbuf, sizeof vbuf) < 0) return -1;
    if (w > 0 && !append(out, cap, &w, "&")) return -1;
    if (!append(out, cap, &w, kbuf)) return -1;
    if (!append(out, cap, &w, "=")) return -1;
    if (!append(out, cap, &w, vbuf)) return -1;
  }
  return (int)w;
}

/* ------------------------------------------------------------------------ */
/* header assembly                                                           */
/* ------------------------------------------------------------------------ */

/* Formats "Name: value" into the next slice of the store and records it. */
static int add_header(HgRequest *r, size_t *cursor, const char *name, const char *value) {
  size_t used = 0, i;
  char *dst;
  if (*cursor >= HG_REQ_MAX_HEADERS) return 0;
  /* find the end of what previous headers consumed */
  for (i = 0; i < *cursor; i++) used += strlen(r->headers[i]) + 1;
  if (used + strlen(name) + 2 + strlen(value) + 1 > HG_REQ_STORE) return 0;
  dst = r->store + used;
  sprintf(dst, "%s: %s", name, value);
  r->headers[*cursor] = dst;
  (*cursor)++;
  r->headers[*cursor] = NULL;
  return 1;
}

static void put_dec(char *dst, size_t cap, uint64_t v) {
  char tmp[24];
  int n = 0, j;
  if (v == 0) tmp[n++] = '0';
  while (v > 0 && n < 22) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (j = 0; j < n && (size_t)j + 1 < cap; j++) dst[j] = tmp[n - 1 - j];
  dst[j] = '\0';
}

/* base64 of a 4-byte integer, big- or little-endian, as x-ladon / x-argus want. */
static void khronos_b64(uint32_t khronos, int big_endian, char *out, size_t cap) {
  uint8_t raw[4];
  if (big_endian) {
    raw[0] = (uint8_t)(khronos >> 24); raw[1] = (uint8_t)(khronos >> 16);
    raw[2] = (uint8_t)(khronos >> 8);  raw[3] = (uint8_t)khronos;
  } else {
    raw[0] = (uint8_t)khronos; raw[1] = (uint8_t)(khronos >> 8);
    raw[2] = (uint8_t)(khronos >> 16); raw[3] = (uint8_t)(khronos >> 24);
  }
  hg_base64_encode(raw, 4, out, cap);
}

/* A deterministic nonce source so the self-test is reproducible; the caller may
 * replace it. Mirrors the reference's Math.random()*65536 for `random`. */
static uint32_t g_nonce_state = 1;
static uint32_t default_rng(void *user) {
  (void)user;
  g_nonce_state = g_nonce_state * 1664525u + 1013904223u;
  return g_nonce_state;
}

static int build_common(HgRequest *r, const char *api, const char *query,
                        const char *body, size_t body_len,
                        uint32_t khronos, uint64_t ticket,
                        const char *ua, int is_post, uint32_t *random_out) {
  size_t c = 0;
  char buf[64];
  /* floor(Math.random() * 65536) takes the HIGH 16 bits of the 32-bit draw, not
   * the low ones. This is the same trap as envLaunch/envPid in hg_medusa.c:
   * Math.random() returns seed/2^32, so multiplying scales it, and `% 65536`
   * would give a completely different number. The randomness only has to be
   * plausible, but getting it wrong here would silently diverge from the
   * reference in a way no vector covers. */
  uint32_t random = default_rng(NULL) >> 16;

  {
    /* Assembled explicitly rather than with snprintf: the length check is the
     * guarantee, and writing it out makes that guarantee local to the compiler
     * instead of leaving it to infer from strlen() calls. */
    size_t alen = strlen(api), qlen = strlen(query);
    if (alen + qlen + 2 > sizeof r->url) return -1;
    memcpy(r->url, api, alen);
    r->url[alen] = '?';
    memcpy(r->url + alen + 1, query, qlen);
    r->url[alen + 1 + qlen] = '\0';
  }
  /* Do NOT strcpy(r->query, query) unconditionally: the search path passes
   * r->query itself, and a self-copy through strcpy is undefined behaviour.
   * Only copy when the caller used a different buffer. */
  if (query != r->query) {
    if (strlen(query) + 1 > sizeof r->query) return -1;
    memcpy(r->query, query, strlen(query) + 1);
  }
  if (body != NULL && body_len < HG_REQ_BODY_MAX) {
    memcpy(r->body, body, body_len);
    r->body[body_len] = '\0';
    r->body_len = body_len;
  } else {
    r->body[0] = '\0';
    r->body_len = 0;
  }
  r->headers[0] = NULL;

  if (!add_header(r, &c, "User-Agent", ua)) return -1;
  if (!add_header(r, &c, "Accept", "application/json; charset=utf-8,application/x-protobuf")) return -1;
  if (is_post && !add_header(r, &c, "Content-Type", "application/json; charset=UTF-8")) return -1;
  if (!add_header(r, &c, "x-xs-from-web", "0")) return -1;

  put_dec(buf, sizeof buf, ticket);
  if (!add_header(r, &c, "x-ss-req-ticket", buf)) return -1;
  if (!add_header(r, &c, "x-tt-request-tag", "t=0;n=0")) return -1;
  if (!add_header(r, &c, "sdk-version", "2")) return -1;
  if (!add_header(r, &c, "passport-sdk-version", "50561")) return -1;
  if (is_post && !add_header(r, &c, "x-vc-bdturing-sdk-version", "3.7.2.cn")) return -1;

  put_dec(buf, sizeof buf, khronos);
  if (!add_header(r, &c, "x-khronos", buf)) return -1;

  {
    char b64[16];
    khronos_b64(khronos, 1, b64, sizeof b64);
    if (!add_header(r, &c, "x-ladon", b64)) return -1;
    khronos_b64(khronos, 0, b64, sizeof b64);
    if (!add_header(r, &c, "x-argus", b64)) return -1;
  }

  if (is_post) {
    uint8_t md5[16];
    char upper[33];
    hg_md5(body, body_len, md5);
    hg_hex_upper(md5, 16, upper);
    if (!add_header(r, &c, "x-ss-stub", upper)) return -1;
  }

  {
    /* x-gorgon signs the QUERY, not the whole URL. The body is passed as bytes;
     * the cast is only because this layer carries it as a NUL-terminated char
     * buffer, not because the two types differ in meaning. */
    char gorgon[HG_GORGON_LEN * 2 + 1];
    hg_xgorgon(query, (const uint8_t *)body, body_len, khronos, random, gorgon);
    if (!add_header(r, &c, "x-gorgon", gorgon)) return -1;
  }

  {
    char helios[HG_HELIOS_MAX];
    if (!hg_helios(khronos, default_rng, NULL, helios, sizeof helios)) return -1;
    if (!add_header(r, &c, "x-helios", helios)) return -1;
  }

  {
    /* x-medusa takes the FULL URL including the query, and the body. */
    static char medusa[1600];
    if (!hg_build_medusa(r->url, (const uint8_t *)body, body_len, khronos, default_rng, NULL,
                         medusa, sizeof medusa)) return -1;
    if (!add_header(r, &c, "x-medusa", medusa)) return -1;
  }

  if (!add_header(r, &c, "x-tt-dt", "")) return -1;
  if (random_out != NULL) *random_out = random;
  return 0;
}

int hg_request_search(HgRequest *r, const char *keyword,
                      uint32_t khronos, uint64_t ticket_ms, uint32_t nonce) {
  static const char API[] =
    "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v";
  /* APP_DEVICE = { ...DEVICE, <these five> } (hongguo.cjs:36). The search API
   * wants the app build stamped at 72232; the video model API keeps the base
   * DEVICE. Sending the wrong one is not rejected -- the server returns HTTP 200
   * with an EMPTY body, which looks like a network problem and is not one. */
  static const HgKv APP_OVERRIDES[] = {
    { "version_code",            "72232" },
    { "version_name",            "7.2.2.32" },
    { "manifest_version_code",   "72232" },
    { "update_version_code",     "72232" },
    { "pv_player",               "72232" }
  };
  char ts[16];
  int offset;

  (void)nonce;
  put_dec(ts, sizeof ts, khronos);

  for (offset = 0; offset < 32; offset++) {
    char rt[24];
    HgKv extra[10];
    int nn = 0;

    put_dec(rt, sizeof rt, ticket_ms + (uint64_t)offset);
    /* The search endpoint needs all of these; appSearchParams (hongguo.cjs:249)
     * sends them and the server returns an empty body without them. count=24 is
     * PAGE_SIZE (hongguo.cjs:66) and tab_type=11 is what the reference passes. */
    if (keyword != NULL && keyword[0] != '\0') {
      extra[nn].key = "query"; extra[nn].value = keyword; nn++;
    }
    extra[nn].key = "count";                  extra[nn].value = "24";             nn++;
    extra[nn].key = "offset";                 extra[nn].value = "0";              nn++;
    extra[nn].key = "tab_type";               extra[nn].value = "11";             nn++;
    extra[nn].key = "bookshelf_search_plan";  extra[nn].value = "4";              nn++;
    extra[nn].key = "use_correct";            extra[nn].value = "true";           nn++;
    extra[nn].key = "user_is_login";          extra[nn].value = "0";              nn++;
    extra[nn].key = "ts";                     extra[nn].value = ts;               nn++;
    extra[nn].key = "_rticket";               extra[nn].value = rt;               nn++;

    if (hg_url_encode_map_ex(APP_OVERRIDES,
                             sizeof APP_OVERRIDES / sizeof APP_OVERRIDES[0],
                             extra, (size_t)nn, r->query, sizeof r->query) < 0) return -1;

    /* Branch 1 is the only one that fails; anything else can proceed. */
    if (hg_branch_of(r->query, NULL, 0, khronos) != 1) break;
  }

  return build_common(r, API, r->query, NULL, 0, khronos,
                      ticket_ms + (uint64_t)offset, HG_APP_UA, 0, NULL);
}

/* ---- THE ENDPOINT MOVED, AND THE SHAPE CHANGED WITH IT (build156) -------------------------------
 *
 * MEASURED, 2026-10-13, from this machine and from the console, and it is why every plan request had
 * started answering "play_plan rc=-5":
 *
 *   OLD  POST /novel/player/multi_video_model/v1/   body {"biz_param":{...},"mixed_video_id_map":{"1004":["<vid>"]}}
 *        -> HTTP 200, 125 bytes, {"Code":110001,"Message":"未知异常",...}   for EVERY request, including
 *           one with no signing headers at all and one whose x-gorgon was deliberately corrupted.
 *        A code that comes back identically for a good signature, a mangled signature and no signature
 *        is not a signature complaint: that route now refuses everything, so no body shape can fix it
 *        (six variants were tried, all 110001).
 *
 *   NEW  POST /novel/player/video_model/v1/         body {"video_id":"<vid>","biz_param":{...}}
 *        -> HTTP 200, 24,124 bytes, {"code":0,...,"data":{"...","video_model":"<JSON string>"}}
 *           with video_list carrying all five tiers, CENC encrypt_info and spade_a -- a real model,
 *           from which a real encrypted MP4 was fetched (206 video/mp4, 13,080,187 bytes = the 1080p size).
 *
 * Note the path: "video_model", SINGULAR. The plural, batched route is the dead one. `video_id` must be
 * a JSON STRING -- sending the id as a number is rejected with HTTP 400
 * "(1)req.(1)VideoId: invalid request value, expected string", so the quotes below are load-bearing.
 *
 * SIGNING IS STILL REQUIRED: the same call without the signing headers answers 110001, so everything
 * around this function (the branch jitter, the gorgon/ladon/argus/helios/medusa headers, the stub) is
 * unchanged and still doing its job. Only the route and the parameter name moved. */
int hg_request_video_model(HgRequest *r, const char *vid,
                           uint32_t khronos, uint64_t ticket_ms, uint32_t nonce) {
  static const char API[] =
    "https://api5-normal-sinfonlineb.fqnovel.com/novel/player/video_model/v1/";
  char payload[HG_REQ_BODY_MAX];
  char ts[16], rt[24];
  HgKv extra[3];
  uint8_t q_sm3[32], b_md5[16];
  size_t body_len;
  int offset;

  (void)nonce;
  put_dec(ts, sizeof ts, khronos);
  /* Field order matches the request that was measured to answer code 0. The signature covers these
   * bytes, so any order would sign correctly -- this one is kept because it is the one with evidence. */
  snprintf(payload, sizeof payload,
    "{\"video_id\":\"%s\",\"biz_param\":{\"detail_page_version\":0,\"device_level\":3,"
    "\"disable_digg_stat\":false,\"disable_video_relate_book\":false,"
    "\"need_all_video_definition\":true,"
    "\"need_mp4_align\":false,\"use_os_player\":false,"
    "\"use_server_dns\":false,\"video_platform\":1024}}", vid);
  body_len = strlen(payload);
  if (body_len >= HG_REQ_BODY_MAX) return -1;

  /* Avoid branch 1: the server does not accept that variant, so jitter the
   * ticket until branchOf() says 0 or 2 (hongguo.cjs:428). */
  hg_md5(payload, body_len, b_md5);
  for (offset = 0; offset < 32; offset++) {
    int n = 0;
    put_dec(rt, sizeof rt, ticket_ms + (uint64_t)offset);
    extra[n].key = "ts";       extra[n].value = ts; n++;
    extra[n].key = "_rticket"; extra[n].value = rt; n++;
    if (hg_url_encode_map(extra, (size_t)n, r->query, sizeof r->query) < 0) return -1;
    hg_sm3(r->query, strlen(r->query), q_sm3);
    if (hg_branch_of(r->query, (const uint8_t *)payload, body_len, khronos) != 1) break;
  }

  return build_common(r, API, r->query, payload, body_len, khronos,
                      ticket_ms + (uint64_t)offset, HG_VIDEO_UA, 1, NULL);
}

/* ---- the WEB client's play-info route (build173) --------------------------------------------------
 *
 * MEASURED 2026-10-16, and it is a different world from everything above: this endpoint needs NO
 * SIGNATURE. There is no x-gorgon, x-argus, x-ladon, x-medusa, x-helios, x-ss-stub, no device
 * fingerprint query and no _rticket branch jitter. The entire request is:
 *
 *     POST https://hongguoduanju.com/api/video/protected/play-info?aid=8662
 *     Content-Type: application/json
 *     X-Xs-From-Web: 1
 *     {"series_id":"<seriesId>","item_id":"<vid>","definition":"720p"}
 *
 * → HTTP 200, 2,505 bytes, {"play_info":{...}} (see hg_play.c for the full shape).
 *
 * THAT IS WHY THIS FUNCTION IS SO MUCH SHORTER THAN hg_request_video_model. It is not a simplification
 * of the signed path -- it is an unsigned endpoint, and pretending otherwise (by reusing the signer)
 * would send headers the server does not expect and make the request harder to reason about when it
 * changes again.
 *
 * `item_id` IS THE `vid`, not the episode id: measured, it is exactly what the site's own player URL
 * carries (/player/<series_id>/<vid>) and what video_list[].vid holds.
 *
 * `definition` must be 360p / 480p / 720p. 540p, 1080p, 1080p+, 2k and 4k all answer
 * 400 {"error":"protected_playback_unavailable"} -- measured across every tier -- so the caller must
 * not ask for 1080p here even though the OLD route offered it. (See hg_client.cpp for the ladder.)
 *
 * `khronos`/`ticket_ms`/`nonce` are accepted only so this matches the shape of its siblings and can be
 * swapped in behind one helper; the endpoint uses none of them. */
int hg_request_play_info(HgRequest *r, const char *series_id, const char *vid,
                        const char *definition, const char *cookie, uint32_t khronos,
                        uint64_t ticket_ms, uint32_t nonce) {
  static const char API[] = "https://hongguoduanju.com/api/video/protected/play-info?aid=8662";
  size_t url_len, body_len;

  (void)khronos;
  (void)ticket_ms;
  (void)nonce;

  if (r == NULL || series_id == NULL || vid == NULL) return -1;

  /* The ids go into a JSON body, so they must be digits only -- the same policy hg_api_valid_episode_id
   * enforces on the episode id. Refusing here rather than escaping means a malformed id is a clean
   * failure instead of a body that happens to parse into a different request. `definition` is a fixed
   * token from the caller's ladder and never user text. */
  {
    const char *p;
    if (series_id[0] == '\0' || strlen(series_id) > 30) return -1;
    for (p = series_id; *p != '\0'; p++) if (*p < '0' || *p > '9') return -1;
    if (vid[0] == '\0' || strlen(vid) > 30) return -1;
    for (p = vid; *p != '\0'; p++) if (*p < '0' || *p > '9') return -1;
  }
  if (definition == NULL || definition[0] == '\0') return -1;

  url_len = strlen(API);
  if (url_len + 1 > sizeof r->url) return -1;
  memcpy(r->url, API, url_len + 1);

  /* Field order as measured; nothing signs these bytes, so the order is for readability only. */
  {
    int n = snprintf(r->body, sizeof r->body,
                     "{\"series_id\":\"%s\",\"item_id\":\"%s\",\"definition\":\"%s\"}",
                     series_id, vid, definition);
    if (n < 0 || (size_t)n >= sizeof r->body) return -1;
    body_len = (size_t)n;
  }
  r->body_len = body_len;
  r->query[0] = '\0';

  /* The unsigned header set, in full. `headers` is a NULL-terminated array that backs onto r->store. */
  {
    size_t c = 0;
    if (!add_header(r, &c, "Content-Type", "application/json")) return -1;
    if (!add_header(r, &c, "X-Xs-From-Web", "1")) return -1;
    /* The site's own client sends a browser UA on this route; the media hosts were measured to accept
     * the request with it (206 on a Range fetch). */
    if (!add_header(r, &c, "User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")) return -1;
    if (!add_header(r, &c, "Accept", "application/json, text/plain, */*")) return -1;

    /* ---- THE DEVICE COOKIE, ON THIS REQUEST ONLY (build175) --------------------------------------
     *
     * MEASURED 2026-10-16: the site issues `set-cookie: ttwid=...` on GET / and on the player page, and
     * its own web client sends it back. It is attached HERE -- on the play-info request alone -- so no
     * other request in this file gains a header it was not measured with. The signed fqnovel requests in
     * particular must stay byte-identical to the reference implementation that pins their signature, so
     * adding a cookie to those would be a real risk for no measured benefit.
     *
     * IT ARRIVES AS A PARAMETER RATHER THAN BEING READ FROM THE TRANSPORT. Calling hg_http_ttwid() here
     * would make this translation unit depend on hg_http.c, and the signer test suite links THIS file
     * without the transport (it cannot link it: hg_http.c needs curl and POSIX headers that the host
     * build does not have). That was a real link failure, not a hypothetical -- `undefined symbol
     * hg_http_ttwid` -- so the dependency is avoided by construction: this function builds a request and
     * says nothing about where a header's value came from.
     *
     * IT IS OMITTED WHEN NULL OR EMPTY rather than sent blank: "Cookie: " with nothing after it is a
     * different request from no Cookie header, and the honest thing is to send nothing until there is
     * something. The caller fetches the cookie when it has none (hg_client.cpp).
     *
     * WHAT IT DOES NOT DO: it does not unlock the episodes that answer 403 -- measured with and without
     * it, interleaved and repeated, those answer 403 either way. See the note on hg_http_ttwid. */
    if (cookie != NULL && cookie[0] != '\0') {
      if (!add_header(r, &c, "Cookie", cookie)) return -1;
    }
    r->headers[c] = NULL;
  }

  return 0;
}
