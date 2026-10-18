/*
 * Playback plan assembly. See hg_play.h for the decision this encodes and for
 * where each upstream field was measured to live.
 */

#include "hg_play.h"
#include "hg_cenc.h"
#include "hg_router.h"
#include "hg_tables.h"

#include <string.h>

/* The reference's cleanUrl (ContentRepository.kt:15): undoes the escapes the
 * upstream leaves in URLs that have been through a JSON string. Applied before
 * decryption, because the encrypted payload is base64 and must not be touched. */
static void clean_url(char *s) {
  size_t r = 0, w = 0;
  while (s[r] != '\0') {
    if (s[r] == '\\' && s[r + 1] == '/') { s[w++] = '/'; r += 2; continue; }
    if (s[r] == '\\' && (s[r + 1] == 'u' || s[r + 1] == 'U')) {
      char a = s[r + 2], b = s[r + 3];
      if (a == '0' && b == '0') {
        char c = s[r + 4], d = s[r + 5];
        char up = (char)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
        char lo = (char)((d >= 'a' && d <= 'z') ? d - 'a' + 'A' : d);
        if (up == '2' && lo == 'F') { s[w++] = '/'; r += 6; continue; }
        if (up == '2' && lo == '6') { s[w++] = '&'; r += 6; continue; }
      }
    }
    if (s[r] == '&' && strncmp(s + r, "&amp;", 5) == 0) { s[w++] = '&'; r += 5; continue; }
    s[w++] = s[r++];
  }
  s[w] = '\0';
}

static int codec_is_decodable(const char *t) {
  if (t == NULL || t[0] == '\0') return 1;      /* unreported: do not filter */
  if (strcmp(t, "bytevc2") == 0) return 0;
  if (strcmp(t, "bvc2") == 0) return 0;
  return 1;
}

static int copy_bounded(char *dst, size_t cap, const char *src) {
  size_t n;
  if (src == NULL) { dst[0] = '\0'; return 1; }
  n = strlen(src);
  if (n + 1 > cap) return 0;
  memcpy(dst, src, n + 1);
  return 1;
}

static size_t skip_ws(const char *j, size_t len, size_t p) {
  while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
  return p;
}

/* A JSON string read into a fixed buffer. Three outcomes, kept distinct:
 *   0  read (possibly empty -- see *present)
 *  -1  present, but the decoded text does not fit `cap`
 * Truncating a main_url would hand mpv a URL that is silently wrong, so an
 * overlong string is refused rather than cut. */
static int json_str_field(const char *j, size_t len, size_t obj, const char *key,
                          char *dst, size_t cap, int *present_opt) {
  size_t v;
  int n, present = 0;
  dst[0] = '\0';
  if (hg_json_object_get(j, len, obj, key, &v)) {
    n = hg_json_string(j, len, v, dst, cap);
    if (n < 0) return -1;
    present = (n > 0);
  }
  if (present_opt != NULL) *present_opt = present;
  return 0;
}

/* A JSON number into an int. Returns 1 and writes when the key is present and
 * numeric; 0 when it is absent or of another type. Not an error either way: every
 * field read through this is a preference, not a requirement. */
static int json_int_field(const char *j, size_t len, size_t obj, const char *key,
                          int *out) {
  size_t v;
  long long n;
  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  if (!hg_json_int(j, len, v, &n)) return 0;
  *out = (int)n;
  return 1;
}

/* A JSON BOOLEAN into an int, for encrypt_info.encrypt. hg_json_int cannot do
 * this: `true` has no digits, so it returns 0 -- indistinguishable from "the key
 * was absent", and the two must not be confused (absent is -1 upstream of here).
 * A non-boolean literal (a number, a quoted "true") is read as its truthiness so
 * a shape drift degrades to the right decision instead of to "unknown". */
static int json_bool_field(const char *j, size_t len, size_t obj, const char *key,
                           int *out) {
  size_t v;
  long long n;
  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  v = skip_ws(j, len, v);
  if (v + 4 <= len && memcmp(j + v, "true", 4) == 0)  { *out = 1; return 1; }
  if (v + 5 <= len && memcmp(j + v, "false", 5) == 0) { *out = 0; return 1; }
  if (hg_json_int(j, len, v, &n)) { *out = (n != 0); return 1; }
  return 0;
}

/* ---- iterating an OBJECT -------------------------------------------------
 *
 * hg_router.h offers array iteration but not object iteration, and the envelope
 * needs it: the first member of `data` is keyed by the VIDEO ID, which the
 * caller cannot know. Rather than guess a key, walk the members.
 *
 * These mirror hg_json_object_get's own scanning rules (quoted key, `:`, value
 * skipped with hg_json_skip) so the two agree about what an object is.
 * ------------------------------------------------------------------------ */

/* Write the offset of the first value in the object at `obj`. 0 when empty or
 * not an object. `key_out`/`key_cap` optionally receive the member's NAME. */
static int json_object_first(const char *j, size_t len, size_t obj,
                             size_t *val, char *key_out, size_t key_cap) {
  size_t p = skip_ws(j, len, obj), ks, ke, after;
  if (p >= len || j[p] != '{') return 0;
  p = skip_ws(j, len, p + 1);
  if (p >= len || j[p] != '"') return 0;

  ks = p + 1;
  ke = ks;
  while (ke < len && j[ke] != '"') { if (j[ke] == '\\') ke++; ke++; }
  if (ke >= len) return 0;

  if (key_out != NULL && key_cap > 0) {
    size_t n = ke - ks;
    if (n + 1 > key_cap) return 0;
    memcpy(key_out, j + ks, n);
    key_out[n] = '\0';
  }

  p = skip_ws(j, len, ke + 1);
  if (p >= len || j[p] != ':') return 0;
  p = skip_ws(j, len, p + 1);
  if (!hg_json_skip(j, len, p, &after)) return 0;
  *val = p;
  return 1;
}

/* Advance to the member after `val` in the object at `obj`. 0 at the end. */
static int json_object_next(const char *j, size_t len, size_t obj, size_t val,
                            size_t *next, char *key_out, size_t key_cap) {
  size_t p, after;
  (void)obj;
  if (!hg_json_skip(j, len, val, &after)) return 0;
  p = skip_ws(j, len, after);
  if (p >= len || j[p] != ',') return 0;      /* end of object */
  p = skip_ws(j, len, p + 1);
  if (p >= len || j[p] != '"') return 0;      /* trailing junk / '}' */

  {
    size_t ks = p + 1, ke = ks;
    while (ke < len && j[ke] != '"') { if (j[ke] == '\\') ke++; ke++; }
    if (ke >= len) return 0;
    if (key_out != NULL && key_cap > 0) {
      size_t n = ke - ks;
      if (n + 1 > key_cap) return 0;
      memcpy(key_out, j + ks, n);
      key_out[n] = '\0';
    }
    p = skip_ws(j, len, ke + 1);
  }
  if (p >= len || j[p] != ':') return 0;
  p = skip_ws(j, len, p + 1);
  if (!hg_json_skip(j, len, p, &after)) return 0;
  *next = p;
  return 1;
}

int hg_play_plan(const char *main_url, const char *key_seed, const char *spade_a,
                 const char *codec_type, HgPlayPlan *out) {
  if (out == NULL) return HG_PLAY_ERR_ARGS;
  memset(out, 0, sizeof *out);
  out->encrypt_flag = -1;      /* -1 = encrypt_info.encrypt was absent */
  out->has_codec_type = (codec_type != NULL && codec_type[0] != '\0');

  if (main_url == NULL || main_url[0] == '\0') return HG_PLAY_ERR_URL;
  if (!copy_bounded(out->url, sizeof out->url, main_url)) return HG_PLAY_ERR_URL;
  clean_url(out->url);
  if (!copy_bounded(out->codec_type, sizeof out->codec_type, codec_type))
    return HG_PLAY_ERR_SMALL;

  /* key_seed arrives as BASE64 TEXT and must be decoded first. Passing the text
   * straight through as seed bytes -- which this file did at first -- means the
   * seed is never actually decoded and every decrypt fails. The reference makes
   * the decode explicit: Buffer.from(str(info.key_seed), 'base64').
   *
   * ON THE LIVE RESPONSE key_seed IS ABSENT ENTIRELY: main_url is already a
   * 692-character plaintext URL. This branch is kept because the reference still
   * runs it and because some resources do carry a seed; it must not be removed on
   * the strength of one sample. */
  if (key_seed != NULL && key_seed[0] != '\0') {
    uint8_t seed[128];
    char decoded[HG_PLAY_URL_MAX];
    size_t seed_len = 0;
    if (!hg_base64_decode(key_seed, seed, sizeof seed, &seed_len) || seed_len == 0)
      return HG_PLAY_ERR_DECODE;
    if (!hg_spade_decrypt_url(out->url, seed, seed_len, decoded, sizeof decoded)) {
      /* Do not fall back to the raw URL: handing mpv an encrypted URL fails at
       * the demuxer with a much less useful message than failing here. */
      return HG_PLAY_ERR_DECODE;
    }
    copy_bounded(out->url, sizeof out->url, decoded);
  }

  /* mpv gets one header-fields string. Both headers come from the reference's
   * media fetch (hongguo.cjs:688); Range is left to ffmpeg. Assembled by
   * explicit lengths rather than snprintf arithmetic, which is easier to get
   * wrong than to read. */
  {
    static const char UA_PREFIX[] = "User-Agent: ";
    static const char REFERER[] = ",Referer: https://novel.snssdk.com/";
    size_t ua_len = strlen(HG_VIDEO_UA);
    size_t need = (sizeof UA_PREFIX - 1) + ua_len + sizeof REFERER;
    if (need > sizeof out->header_fields) return HG_PLAY_ERR_ARGS;
    memcpy(out->header_fields, UA_PREFIX, sizeof UA_PREFIX - 1);
    memcpy(out->header_fields + sizeof UA_PREFIX - 1, HG_VIDEO_UA, ua_len);
    memcpy(out->header_fields + sizeof UA_PREFIX - 1 + ua_len, REFERER, sizeof REFERER);
  }

  if (spade_a != NULL && spade_a[0] != '\0') {
    char hex[33];
    if (!hg_spade_content_key(spade_a, out->key)) return HG_PLAY_ERR_KEY;
    out->encrypted = 1;
    hg_hex(out->key, 16, hex);
    /* "decryption_key=" is 15 chars + 32 hex + NUL = 48, comfortably inside
     * HG_PLAY_DK_MAX. */
    memcpy(out->decryption_key, "decryption_key=", 15);
    memcpy(out->decryption_key + 15, hex, 32);
    out->decryption_key[47] = '\0';
  }
  return 0;
}

/* Fill the descriptive fields of `plan` from one video_list entry. Separate from
 * hg_play_plan() because that one's job is "build a playable plan from three
 * strings" and this one's is "read the entry's shape"; keeping them apart is what
 * lets the entry layout change here without touching the crypto glue.
 * Returns 0, or HG_PLAY_ERR_SMALL on an overlong inner string. */
static int fill_entry_meta(const char *j, size_t len, size_t elem,
                           HgPlayPlan *plan) {
  size_t meta = 0, enc = 0;

  /* video_meta carries codec_type, definition and the pixel/bitrate facts.
   * codec_type is read HERE, not from the entry: measured, the live entry has no
   * top-level codec_type at all. */
  if (hg_json_object_get(j, len, elem, "video_meta", &meta)) {
    if (json_str_field(j, len, meta, "codec_type", plan->codec_type,
                       sizeof plan->codec_type, &plan->has_codec_type) < 0)
      return HG_PLAY_ERR_SMALL;
    if (json_str_field(j, len, meta, "definition", plan->definition,
                       sizeof plan->definition, NULL) < 0)
      return HG_PLAY_ERR_SMALL;
    (void)json_int_field(j, len, meta, "vwidth", &plan->vwidth);
    (void)json_int_field(j, len, meta, "vheight", &plan->vheight);
    (void)json_int_field(j, len, meta, "bitrate", &plan->bitrate);
    {
      int sz;
      if (json_int_field(j, len, meta, "size", &sz)) plan->size_bytes = sz;
    }
  }

  /* quality_type is a NUMBER on the entry itself -- not a string, and not inside
   * video_meta. Measured 40 / 30 / 20 / 10 / 1 for 360p / 480p / 540p / 720p /
   * 1080p, so it is the stable ladder and `definition` is only the label. */
  plan->has_quality_type = json_int_field(j, len, elem, "quality_type",
                                          &plan->quality_type);

  /* encrypt_info.encrypt / .kid / .encryption_method. `encrypt` is a BOOLEAN and
   * needs its own reader: hg_json_int reads `true` as "no number" and would leave
   * encrypt_flag at -1, making "upstream says plaintext" look like "upstream said
   * nothing". */
  if (hg_json_object_get(j, len, elem, "encrypt_info", &enc)) {
    int flag;
    if (json_bool_field(j, len, enc, "encrypt", &flag)) plan->encrypt_flag = flag;
    if (json_str_field(j, len, enc, "kid", plan->kid, sizeof plan->kid, NULL) < 0)
      return HG_PLAY_ERR_SMALL;
    if (json_str_field(j, len, enc, "encryption_method", plan->encryption_method,
                       sizeof plan->encryption_method, NULL) < 0)
      return HG_PLAY_ERR_SMALL;
  }
  return 0;
}

/* Try one video_list entry. Returns 0 when a plan was built, 1 when this entry is
 * simply not usable (incompatible codec, no URL), or a negative HG_PLAY_ERR_* for
 * a genuine fault worth reporting rather than skipping. */
static int try_entry(const char *j, size_t len, size_t elem, HgPlayPlan *out) {
  char main_url[HG_PLAY_URL_MAX];
  char codec[32];
  size_t meta = 0, enc = 0;
  int present = 0;
  char key_seed[256], spade_a[1024];
  int rc;

  /* The codec is read BEFORE the URL, because filtering a bytevc2 row is the
   * common case and a long URL on such a row could trip the buffer check, which
   * would report SMALL for a row that should simply be skipped. Read it from
   * video_meta.codec_type, and treat "absent" as "not filtered": legacy entries
   * carry no codec_type and are perfectly playable. */
  codec[0] = '\0';
  if (hg_json_object_get(j, len, elem, "video_meta", &meta)) {
    if (json_str_field(j, len, meta, "codec_type", codec, sizeof codec, NULL) < 0)
      return HG_PLAY_ERR_SMALL;
  }
  if (!codec_is_decodable(codec)) return 1;

  /* main_url is on the ENTRY, at the top level -- this one the old code had
   * right. Absent or empty means the row is unusable; skip it rather than failing
   * the whole response, since it is a per-row property. */
  if (json_str_field(j, len, elem, "main_url", main_url, sizeof main_url,
                     &present) < 0)
    return HG_PLAY_ERR_SMALL;
  if (!present) return 1;

  /* No key_seed: measured ABSENT on the live response, because main_url is
   * already plaintext. Reading it anyway costs one lookup and preserves the
   * behaviour for resources that do carry one. spade_a comes from
   * encrypt_info.spade_a and NOWHERE else. */
  if (json_str_field(j, len, elem, "key_seed", key_seed, sizeof key_seed, NULL) < 0)
    return HG_PLAY_ERR_SMALL;
  spade_a[0] = '\0';
  if (hg_json_object_get(j, len, elem, "encrypt_info", &enc)) {
    if (json_str_field(j, len, enc, "spade_a", spade_a, sizeof spade_a, NULL) < 0)
      return HG_PLAY_ERR_SMALL;
  }

  /* A spade_a is what makes a stream encrypted, so it wins over encrypt == false,
   * which is also what the reference does by testing spade_a and never the flag. */
  rc = hg_play_plan(main_url, key_seed, spade_a, codec, out);
  if (rc != 0) return rc;

  return fill_entry_meta(j, len, elem, out);
}

/* The inner document, and what it took to get it. Shared by the two public entry points below:
 * hg_play_plan_from_model wants one source out of it, and hg_play_model_inner hands the whole
 * thing back so the quality ladder can be built from every source. */
static int unwrapModel(const char *json, size_t len, const char **out, size_t *out_len,
                       char *inner_buf, size_t inner_cap) {
  size_t data = 0, vid = 0, direct = 0;
  int found = 0, slen = 0;

  if (json == NULL) return HG_PLAY_ERR_ARGS;
  if (out == NULL || out_len == NULL) return HG_PLAY_ERR_ARGS;

  /* THE DOUBLE ENCODING. video_model is a JSON STRING, so the reader must run a second time over
   * the decoded text. Looking for video_list in the outer document -- which this file used to do --
   * can never find it, so a perfectly good response was reported as "no model".
   *
   * The inner text is extracted into a NUL-terminated buffer rather than searched in place:
   * hg_json_string resolves \" and \uXXXX, and without that second resolution the inner braces
   * are still escaped and every nested lookup fails. */
  if (hg_json_object_get(json, len, 0, "data", &data)) {
    /* ---- THE ENVELOPE IS NO LONGER KEYED BY THE VIDEO ID (build156) ----------------------------
     *
     * The batched route this port used to call answered `data` as an OBJECT OF ONE MEMBER PER VIDEO,
     * keyed by that video's long numeric id, each holding its own video_model. The route that
     * answers now -- /novel/player/video_model/v1/, one video per request -- returns `data` as THE
     * VIDEO'S OWN OBJECT, with video_model sitting directly in it:
     *
     *   OLD  {"data":{"7692096729776131096":{"disable_vsr":true,...,"video_model":"{...}"}}}
     *   NEW  {"data":{"disable_vsr":true,"expire_time":...,"video_model":"{...}"}}
     *
     * Both are parsed, in this order, because the two differ only in one nesting level and a build
     * that accepts only one of them turns a five-minute server change into the same silent
     * "rc=-5" this port just spent a round diagnosing. The captured evidence for the new shape is
     * app/src/tests/fixtures/video_model_v1.json, and the old shape is still covered by
     * video_model.json. */
    if (hg_json_object_get(json, len, data, "video_model", &direct)) {
      slen = hg_json_string(json, len, direct, inner_buf, (int)inner_cap);
      /* An inner model that does not fit is REFUSED, never truncated: a truncated document still
       * parses as JSON and would yield a smaller video_list with no error at all. */
      if (slen < 0) return HG_PLAY_ERR_SMALL;
      if (slen > 0) found = 1;
    }

    /* The keyed envelope, for as long as the batched route is something anyone can reach.
     *
     * The FIRST data member carrying a non-empty video_model string wins. Upstream sent exactly
     * one for a one-vid request, so there is no ordering question in practice; first-wins is a
     * documented choice rather than an accident. */
    if (!found && json_object_first(json, len, data, &vid, NULL, 0)) {
      for (;;) {
        size_t probe = vid;
        probe = skip_ws(json, len, probe);
        if (probe < len && json[probe] == '{') {
          size_t vm;
          if (hg_json_object_get(json, len, vid, "video_model", &vm)) {
            slen = hg_json_string(json, len, vm, inner_buf, (int)inner_cap);
            if (slen < 0) return HG_PLAY_ERR_SMALL;
            if (slen > 0) { found = 1; break; }
          }
        }
        {
          size_t next;
          if (!json_object_next(json, len, data, vid, &next, NULL, 0)) break;
          vid = next;
        }
      }
    }
  }

  if (found) {
    *out = inner_buf;
    *out_len = strlen(inner_buf);
  } else {
    /* Not the envelope. Accept a bare inner object ({"video_list":[...]}) so this still works
     * for a caller that already unwrapped it, for the tests, and for any future endpoint that
     * inlines the model. */
    *out = json;
    *out_len = len;
  }
  return 0;
}

int hg_play_model_inner(const char *json, size_t len, const char **out, size_t *out_len) {
  /* static: the decoded inner document is up to ~64 KB and this is called from a thread with a
   * small stack. Single-threaded use, like the rest of this port -- and the caller is told the
   * buffer is borrowed, not given. */
  static char inner_buf[HG_PLAY_RESPONSE_MAX];
  return unwrapModel(json, len, out, out_len, inner_buf, sizeof inner_buf);
}

int hg_play_plan_from_model(const char *json, size_t len, HgPlayPlan *out) {
  const char *j;
  size_t jlen, list = 0, elem;
  int saw_entry = 0;

  if (json == NULL || out == NULL) return HG_PLAY_ERR_ARGS;

  /* ONE unwrapping, in one place. This function used to carry its own copy of the
   * doubly-encoded-envelope walk; hg_play_model_inner is that walk, exposed so the quality
   * ladder can run hg_api_parse_video_model over the whole source list. Two copies of a piece
   * of parsing logic is two places for the next fix to be applied to only one of them, and this
   * particular piece has already been wrong once (it looked for video_list at the top level,
   * where it can never be).
   *
   * The buffer hg_play_model_inner hands back is borrowed and is invalidated by the next call,
   * which is fine here: this function finishes with it before returning. */
  {
    int rc = hg_play_model_inner(json, len, &j, &jlen);
    if (rc != 0) return rc;
  }

  if (!hg_json_object_get(j, jlen, 0, "video_list", &list)) return HG_PLAY_ERR_NOMODEL;
  {
    size_t probe = skip_ws(j, jlen, list);
    if (probe >= jlen || j[probe] != '[') return HG_PLAY_ERR_NOMODEL;
  }
  if (!hg_json_array_first(j, jlen, list, &elem)) return HG_PLAY_ERR_NOMODEL;

  /* ---- THE BEST SOURCE, NOT THE FIRST ONE (build158) ---------------------------------------------
   *
   * WHAT CHANGED AND WHY IT IS ONE LINE OF POLICY RATHER THAN AN EXTRA REQUEST.
   *
   * Playback used to start at 360p, because upstream lists its sources worst-first (measured order:
   * 360p, 480p, 540p, 720p, 1080p) and this loop returned the first entry it could use. The user asked
   * for 1080p by default.
   *
   * build143 answered that by asking for the QUALITY LADDER before the loadfile and then fetching the
   * plan of the chosen tier -- two requests to one endpoint. That is what produced "play_plan rc=-5"
   * for a whole round: this service answers one good request per short window, so the ladder consumed
   * it and the plan got the empty answer (see docs/新播放接口-第21轮.md and §5 of the handover).
   *
   * IT IS NOT NEEDED ANY MORE. The single-video route this app calls now (build156) returns the WHOLE
   * video_list -- all five tiers, 24 KB of it -- in the one response the plan already comes from. So
   * choosing the best entry costs ZERO extra requests: the list is already in hand, and the only
   * question was which entry to take from it. That is this change.
   *
   * THE RULE: highest pixel count wins, and equal pixel counts are broken by bitrate. Ties beyond that
   * keep the earlier entry, so the choice is deterministic rather than dependent on list order beyond
   * the tie-break. Nothing is capped: on this service the ladder tops out at 1920x1080, and a cap would
   * be a second policy to keep true for no measured benefit. The measurement behind that is in
   * docs/播放链路自检-第21轮.md -- 1080p HEVC plays at the same frame rate as 360p on this console.
   *
   * The entry is remembered and parsed ONCE MORE at the end rather than kept as a parsed copy: a
   * HgPlayPlan is several kilobytes and re-running try_entry on the winning index keeps this loop's
   * stack the same size it was. */
  {
    size_t bestElem = 0;
    int haveBest = 0;
    int firstFault = 0;
    long bestPixels = 0, bestBitrate = 0;

    for (;;) {
      int rc;

      /* Entries are objects. A non-object where the list should hold entries is a
       * shape change, not a skippable row. */
      {
        size_t probe = skip_ws(j, jlen, elem);
        if (probe >= jlen || j[probe] != '{') return HG_PLAY_ERR_SHAPE;
      }

      memset(out, 0, sizeof *out);
      rc = try_entry(j, jlen, elem, out);
      if (rc < 0) {
        /* ---- A FAULT ON ONE ROW IS REMEMBERED, NOT RAISED IMMEDIATELY (build158) ---------------------
         *
         * The old loop returned at the first usable entry, so a malformed row AFTER it was never looked
         * at. Choosing the best entry means looking at every row, which would turn "tier 4 of 5 has a URL
         * too long for the buffer" into "this episode will not play" -- a loss of playback bought by
         * nothing. So the fault is remembered and the search continues.
         *
         * IT IS STILL REPORTED when no row turns out to be usable: the error the caller gets is then the
         * real one (a field that did not fit, a URL that was refused) instead of the generic "no source",
         * which is the property that makes it worth remembering rather than dropping. */
        if (firstFault == 0) firstFault = rc;
      } else if (rc == 0) {
        const long pixels = (long)out->vwidth * (long)out->vheight;
        if (!haveBest || pixels > bestPixels ||
            (pixels == bestPixels && (long)out->bitrate > bestBitrate)) {
          bestElem = elem;
          bestPixels = pixels;
          bestBitrate = (long)out->bitrate;
          haveBest = 1;
        }
      } else {
        saw_entry = 1;                /* examined and unusable */
      }

      if (!hg_json_array_next(j, jlen, list, elem, &elem)) break;
    }

    if (haveBest) {
      memset(out, 0, sizeof *out);
      if (try_entry(j, jlen, bestElem, out) == 0) return 0;
      /* Unreachable in practice: this exact entry parsed a moment ago, and the document has not
       * changed. Falling through is the honest answer if it ever does. */
      saw_entry = 1;
    }
    if (firstFault != 0) return firstFault;
  }

  /* Nothing playable. CODEC says "the response was fine and every quality was
   * bytevc2"; NOMODEL says "there was never a list". A caller that re-picks a
   * quality on CODEC and re-fetches on NOMODEL needs them to differ. */
  return saw_entry ? HG_PLAY_ERR_CODEC : HG_PLAY_ERR_NOMODEL;
}

/* ---- the play-info envelope (build173) ------------------------------------------------------------
 *
 * MEASURED SHAPE, one live response on 2026-10-16 (2,505 bytes, HTTP 200):
 *
 *   {"play_info":{
 *      "vid":"v02ebeg10000d0hc10vog65pavtiedhg",
 *      "kid":"6822c083f8818b957fb273630002ebeb",
 *      "main_url":"https://v26-hgweb.qznovelvod.com/.../media-video-avc1/?...",   <- VIDEO
 *      "backup_url":"https://v3-hgweb.qznovelvod.com/...",
 *      "player_access_key":"o+Z3xjvaIJRt2k34VeFO+FTiGsI62BvBUthzkm2PcpA5jyOeng==",
 *      "definition":"720p","format":"dash","codec_type":"h264",
 *      "width":"720","height":"1280","duration":211.834,
 *      "size":"31731824","bitrate":"1552519",
 *      "init_range":"0-1276","index_range":"1277-1824","first_moof_range":"1825-6927",
 *      "expires_at":"1792088311",
 *      "audio":{
 *         "main_url":"https://.../media-audio-und-mp4a/?...",                    <- AUDIO, SEPARATE
 *         "player_access_key":"<same value as video>","kid":"<same kid>",
 *         "codec_type":"h264","format":"dash","duration":211.834,
 *         "init_range":"0-1026","index_range":"1027-1574","expires_at":"1792088313"}},
 *    "base_resp":{"StatusCode":0,"StatusMessage":""}}
 *
 * FOUR THINGS THIS SHAPE FORCES, each of which was verified rather than assumed:
 *
 *  1. THE AUDIO IS A SEPARATE URL. There is no muxed progressive file: measured across every
 *     `definition` (360p/480p/720p all answer, 540p/1080p/2k/4k answer 400), the video and audio
 *     arrive as two different URLs. Both are COMPLETE fMP4 files -- each carries ftyp, moov, mvhd,
 *     trak, mvex, trex and its own sidx, and both serve `accept-ranges: bytes` -- so each opens on
 *     its own and the audio is added as a second track. That measurement is why the struct carries
 *     audio_url at all.
 *
 *  2. THE KEY IS `player_access_key`, NOT `spade_a`. There is no spade_a and no key_seed anywhere in
 *     this response (searched the raw 2,505 bytes: neither string occurs). So hg_spade_open_box()
 *     above is the only thing that can produce the key. ONE key covers BOTH tracks: measured, the
 *     audio object repeats the video's player_access_key and kid byte for byte.
 *
 *  3. `expires_at` IS A STRING, NOT A NUMBER. It arrives quoted ("1792088311"), so a numeric reader
 *     would see no digits and silently leave 0 -- which would read as "never expires" and defeat the
 *     one thing the field exists for. Both forms are therefore accepted.
 *
 *  4. `duration` IS A REAL NUMBER (211.834). There is no float reader in hg_router.h, so one is
 *     written below rather than rounding the value into an int; the caller shows it to the user.
 *
 * A MISSING/REFUSED KEY IS REPORTED BUT NOT FATAL TO THE PARSE. The URLs are still correct and worth
 * having -- build171's "source failed, re-fetch" path and the log both need them -- so the plan is
 * filled in and HG_PLAY_ERR_KEY is returned so the caller can say so. Callers that need to PLAY must
 * treat a non-zero return as fatal; `hg_play_info_parse` returning 0 with encrypted==0 is the
 * "genuinely not encrypted" case.
 */

/* A JSON number into a double. Written here because hg_router.h has only the integer reader, and
 * `duration` is fractional on the live response. Accepts an optional sign, digits, a fraction and an
 * exponent; refuses anything that starts with a non-digit so a string value is not silently read. */
static int json_double_field(const char *j, size_t len, size_t obj, const char *key,
                             double *out) {
  size_t v, p;
  int neg = 0;
  double val = 0.0, scale = 0.1;
  int any = 0;

  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  p = skip_ws(j, len, v);
  if (p < len && (j[p] == '-' || j[p] == '+')) { neg = (j[p] == '-'); p++; }
  while (p < len && j[p] >= '0' && j[p] <= '9') { val = val * 10.0 + (double)(j[p] - '0'); p++; any = 1; }
  if (p < len && j[p] == '.') {
    p++;
    while (p < len && j[p] >= '0' && j[p] <= '9') {
      val += (double)(j[p] - '0') * scale;
      scale *= 0.1;
      p++;
      any = 1;
    }
  }
  if (!any) return 0;
  if (p < len && (j[p] == 'e' || j[p] == 'E')) {
    int esign = 0, exp = 0, eany = 0;
    p++;
    if (p < len && (j[p] == '-' || j[p] == '+')) { esign = (j[p] == '-'); p++; }
    while (p < len && j[p] >= '0' && j[p] <= '9') { exp = exp * 10 + (j[p] - '0'); p++; eany = 1; }
    if (eany) {
      int i;
      for (i = 0; i < exp && i < 64; i++) val = esign ? val / 10.0 : val * 10.0;
    }
  }
  *out = neg ? -val : val;
  return 1;
}

/* A string-or-number field into a long long, for `expires_at`. MEASURED: it arrives QUOTED
 * ("1792088311"), so the string form is the one that actually happens; the numeric form is accepted
 * because the same field is unquoted elsewhere in this API and a shape flip should not turn a real
 * expiry into "no expiry". */
static int json_ll_field_either(const char *j, size_t len, size_t obj, const char *key,
                                long long *out) {
  size_t v, p;
  long long n = 0;
  int any = 0;

  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  p = skip_ws(j, len, v);
  if (p < len && j[p] == '"') {
    p = skip_ws(j, len, p + 1);
  }
  while (p < len && j[p] >= '0' && j[p] <= '9') { n = n * 10 + (j[p] - '0'); p++; any = 1; }
  if (!any) return 0;
  *out = n;
  return 1;
}

/* Fill the header-field string mpv needs. The play-info URLs are fetched by the same player as the
 * old ones, so the same two headers are sent; the Referer is the site the URLs belong to (measured:
 * the media host answers 206 with it, and the web client itself sends it). */
static int play_info_headers(HgPlayInfo *out) {
  static const char UA_PREFIX[] = "User-Agent: ";
  static const char REFERER[] = ",Referer: https://hongguoduanju.com/";
  size_t ua_len = strlen(HG_VIDEO_UA);
  size_t need = (sizeof UA_PREFIX - 1) + ua_len + sizeof REFERER;
  if (need > sizeof out->header_fields) return 0;
  memcpy(out->header_fields, UA_PREFIX, sizeof UA_PREFIX - 1);
  memcpy(out->header_fields + sizeof UA_PREFIX - 1, HG_VIDEO_UA, ua_len);
  memcpy(out->header_fields + sizeof UA_PREFIX - 1 + ua_len, REFERER, sizeof REFERER);
  return 1;
}

int hg_play_info_parse(const char *json, size_t len, HgPlayInfo *out) {
  size_t node = 0;
  int have_node = 0;
  int key_refused = 0;

  if (json == NULL || out == NULL) return HG_PLAY_ERR_ARGS;
  memset(out, 0, sizeof *out);

  /* The real envelope is {"play_info":{...}}. A bare object is also accepted so the tests and any
   * caller that already unwrapped it share this one entry point. */
  if (hg_json_object_get(json, len, 0, "play_info", &node)) {
    have_node = 1;
  } else {
    size_t p = skip_ws(json, len, 0);
    if (p < len && json[p] == '{') { node = 0; have_node = 1; }
  }
  if (!have_node) return HG_PLAY_ERR_NOMODEL;

  /* main_url is the one field with no substitute: without it there is nothing to play. Too long is
   * REFUSED rather than truncated -- a truncated URL is silently a different, wrong URL. */
  {
    size_t url = 0;
    if (!hg_json_object_get(json, len, node, "main_url", &url)) return HG_PLAY_ERR_URL;
    if (hg_json_string(json, len, url, out->url, sizeof out->url) <= 0) return HG_PLAY_ERR_URL;
    clean_url(out->url);
  }

  /* The video backup URL. Optional: measured present, but a plan without it is still playable, so an
   * absent or oversized value is left empty instead of failing the parse. */
  {
    size_t b = 0;
    if (hg_json_object_get(json, len, node, "backup_url", &b)) {
      if (hg_json_string(json, len, b, out->backup_url, sizeof out->backup_url) < 0)
        out->backup_url[0] = '\0';
      else
        clean_url(out->backup_url);
    }
  }

  (void)json_str_field(json, len, node, "kid", out->kid, sizeof out->kid, NULL);
  (void)json_str_field(json, len, node, "definition", out->definition, sizeof out->definition, NULL);
  (void)json_str_field(json, len, node, "codec_type", out->codec_type, sizeof out->codec_type, NULL);
  (void)json_double_field(json, len, node, "duration", &out->duration);
  (void)json_ll_field_either(json, len, node, "expires_at", &out->expires_at);
  /* width/height arrive as QUOTED STRINGS here ("720" / "1280"), unlike the old route where they were
   * numbers. The int reader would see no digits and leave 0, which would silently read as "unknown
   * resolution"; json_ll_field_either accepts both forms, which is why it is used. */
  {
    long long w = 0, h = 0;
    if (json_ll_field_either(json, len, node, "width", &w)) out->vwidth = (int)w;
    if (json_ll_field_either(json, len, node, "height", &h)) out->vheight = (int)h;
  }

  /* ---- the CENC key ----------------------------------------------------------------------------
   * ONE player_access_key for both tracks (measured: the audio object repeats it verbatim), so the
   * video's is read and the audio's is NOT required to be present or identical. */
  {
    char pak[512];
    /* json_str_field returns 0 on success and -1 when the value did not fit -- it does NOT return a
     * length. Testing it for "greater than zero" would skip every key that parsed fine, which is
     * exactly the kind of silent miss this whole parse exists to avoid. */
    int present = 0;
    if (json_str_field(json, len, node, "player_access_key", pak, sizeof pak, &present) == 0 &&
        present && pak[0] != '\0') {
      char hex[33];
      if (hg_spade_open_box(pak, out->key)) {
        out->encrypted = 1;
        hg_hex(out->key, 16, hex);
        /* "decryption_key=" is 15 chars + 32 hex + NUL = 48, inside HG_PLAY_DK_MAX. Same option
         * string the old path already feeds mpv through --demuxer-lavf-o, so the player side needs
         * no new mechanism. */
        memcpy(out->decryption_key, "decryption_key=", 15);
        memcpy(out->decryption_key + 15, hex, 32);
        out->decryption_key[47] = '\0';
      } else {
        key_refused = 1;
      }
    }
  }

  /* ---- the separate audio track ---------------------------------------------------------------- */
  {
    size_t audio = 0;
    if (hg_json_object_get(json, len, node, "audio", &audio)) {
      size_t au = 0;
      if (hg_json_object_get(json, len, audio, "main_url", &au)) {
        if (hg_json_string(json, len, au, out->audio_url, sizeof out->audio_url) < 0)
          out->audio_url[0] = '\0';   /* refused, never truncated */
        else
          clean_url(out->audio_url);
      }
    }
  }

  if (!play_info_headers(out)) return HG_PLAY_ERR_ARGS;

  /* The URLs are usable, so the parse succeeded; the refused key is reported separately because the
   * caller can act on it (log it, and expect a black screen) while still holding a valid plan. */
  return key_refused ? HG_PLAY_ERR_KEY : 0;
}

int hg_play_plan_from_info(const HgPlayInfo *info, HgPlayPlan *out) {
  if (info == NULL || out == NULL) return HG_PLAY_ERR_ARGS;
  memset(out, 0, sizeof *out);

  if (!copy_bounded(out->url, sizeof out->url, info->url)) return HG_PLAY_ERR_URL;
  if (!copy_bounded(out->audio_url, sizeof out->audio_url, info->audio_url)) {
    /* The audio is optional; an overlong one is dropped rather than failing the whole plan, because a
     * picture with no sound beats no picture at all. */
    out->audio_url[0] = '\0';
  }
  if (!copy_bounded(out->codec_type, sizeof out->codec_type, info->codec_type))
    return HG_PLAY_ERR_SMALL;
  out->has_codec_type = (info->codec_type[0] != '\0');
  (void)copy_bounded(out->definition, sizeof out->definition, info->definition);
  (void)copy_bounded(out->kid, sizeof out->kid, info->kid);
  (void)copy_bounded(out->header_fields, sizeof out->header_fields, info->header_fields);
  (void)copy_bounded(out->decryption_key, sizeof out->decryption_key, info->decryption_key);

  out->vwidth = info->vwidth;
  out->vheight = info->vheight;
  out->encrypted = info->encrypted;
  if (info->encrypted) memcpy(out->key, info->key, sizeof out->key);

  /* quality_type stays 0 with has_quality_type 0: this route has no such field. The quality menu reads
   * the ladder from the OLD route separately and will simply find nothing here, which is the honest
   * outcome -- there is no ladder to switch between on this endpoint (see the tier note in hg_client.cpp:
   * only 360p/480p/720p exist and they are chosen by re-requesting, not by index). */
  out->quality_type = 0;
  out->has_quality_type = 0;
  return 0;
}
