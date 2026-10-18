/*
 * Response parsing. See hg_api.h for provenance, the three shapes, and the
 * fail-closed contract.
 */

#include "hg_api.h"
#include "hg_router.h"

#include <stdlib.h>   /* strtoll, for the search page's totalCount (a STRING) */
#include <string.h>

/* ---- validation ---------------------------------------------------------- */

/* [0-9]{1,30}: the reference's id rule, applied wherever an id crosses this
 * layer. Leading zeros are allowed (the regex allows them) but an empty string is
 * not, and neither is a 31st digit -- a 31-digit id would not fit HG_API_ID_MAX
 * and would be truncated on copy, which is exactly the kind of silent damage the
 * error codes exist to prevent. */
/* Assign an out-parameter only when the caller asked for it.
 *
 * Every parse function in this file both RETURNS the count and writes it through
 * a `size_t *count` out-parameter. That signature invites callers to pass NULL --
 * the return value is right there -- and every test in the suite passes a real
 * pointer, so the NULL path was never exercised by anything.
 *
 * It was exercised once, on the console: hg_api_parse_home(json, len, out, 8, NULL)
 * wrote to address 0 and the process aborted with 2168-0002, one line after the
 * home page had parsed correctly. A whole evening of that crash traced back to an
 * out-parameter that was never validated. */
static void set_count(size_t *count, size_t value) {
  if (count != NULL) *count = value;
}

static int valid_num_id(const char *s) {
  size_t n = 0;
  if (s == NULL) return 0;
  while (s[n] != '\0') {
    if (s[n] < '0' || s[n] > '9') return 0;
    n++;
    if (n > 30) return 0;
  }
  return n > 0;
}

int hg_api_valid_id(const char *s) { return valid_num_id(s); }
int hg_api_valid_episode_id(const char *s) { return valid_num_id(s); }

/* ---- small helpers ------------------------------------------------------- */

static int ascii_isspace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Copy a decoded string into a fixed field, failing rather than truncating.
 * `n` is the length hg_json_string reported. An empty value is not an error:
 * upstream omits fields freely and an absent description is ordinary. */
static int copy_field(char *dst, size_t cap, const char *src, int n) {
  if (n < 0) return 0;
  if ((size_t)n + 1 > cap) return 0;
  if (n > 0) memcpy(dst, src, (size_t)n);
  dst[n] = '\0';
  return 1;
}

/* Decode the string at `pos` straight into a fixed field. */
static int str_field(const char *j, size_t len, size_t pos, char *dst, size_t cap) {
  return copy_field(dst, cap, dst, hg_json_string(j, len, pos, dst, cap));
}

static int is_null_at(const char *j, size_t len, size_t pos);

/* Decode the string at `pos` into `dst` ONLY when the key exists. A missing key
 * leaves `dst` alone, which is what lets the fallback chains decide precedence:
 * the first candidate that is present and non-empty wins.
 *
 * A JSON null counts as ABSENT, not as a broken value: the reference's str()
 * checks isNull() first for exactly this reason, and treating null as a parse
 * failure turned an ordinary "no cover for this row" into a refused page. */
static int opt_field_ex(const char *j, size_t len, size_t obj, const char *key,
                        char *dst, size_t cap, int *present, int truncate) {
  size_t v;
  int n;
  *present = 0;
  if (!hg_json_object_get(j, len, obj, key, &v)) return 1;   /* absent is fine */
  if (is_null_at(j, len, v)) return 1;                       /* null is absent */
  n = truncate ? hg_json_string_fit(j, len, v, dst, cap)
               : hg_json_string(j, len, v, dst, cap);
  if (n < 0) return 0;                                       /* present but broken */
  /* A truncated truncate=1 result is still a usable value, so it counts as
   * present; only the strict path can fail here. */
  *present = (n > 0);
  return 1;
}

/* DISPLAY text is truncated, identity is not.
 *
 * Real series_intro values run to 570 bytes while HgSeries.desc is 512, and the
 * strict decode turned that into HG_API_ERR_JSON, which made series_from_card
 * refuse its card, which made the WHOLE home page fail to load over two
 * mBannerList rows. A synopsis losing its last line is not a reason to show the
 * user nothing.
 *
 * series_id is deliberately NOT truncated: an id that got cut short still looks
 * like a valid id, and the caller would then request a different series. */
static int opt_field(const char *j, size_t len, size_t obj, const char *key,
                     char *dst, size_t cap, int *present) {
  return opt_field_ex(j, len, obj, key, dst, cap, present, 0);
}

static int opt_field_fit(const char *j, size_t len, size_t obj, const char *key,
                         char *dst, size_t cap, int *present) {
  return opt_field_ex(j, len, obj, key, dst, cap, present, 1);
}

/* The reference's str(): a JSON null is an absent field, not the text "null".
 * org.json's optString would render null as "null"; the authors' helper checks
 * isNull first and returns "". Without this a null cover becomes a URL of the
 * literal string "null" that is then fetched. */
static int is_null_at(const char *j, size_t len, size_t pos) {
  size_t p = pos;
  while (p < len && ascii_isspace(j[p])) p++;
  return (p + 4 <= len && memcmp(j + p, "null", 4) == 0);
}

/* An OPTIONAL integer field. Returns 1 when the key was present and held a number, 0
 * otherwise, leaving *out untouched on failure -- so a caller can tell "absent" from "zero",
 * which matters for every one of the quality fields (0 is a legal bitrate? no, but 0 is a
 * legal `size` on an empty entry, and `quality_type` never sends 0 at all).
 *
 * hg_play.c has a static twin of this for the plan path. They are separate because each is
 * file-local and the two files deliberately do not share a header for one three-line
 * function; duplication of THIS size is cheaper than a shared header. */
static int opt_int_field(const char *j, size_t len, size_t obj, const char *key,
                         int *out) {
  size_t v;
  long long n;
  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  if (!hg_json_int(j, len, v, &n)) return 0;
  *out = (int)n;
  return 1;
}

/* ---- in-place unescaping -------------------------------------------------- */

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void hg_api_clean_url(char *s) {
  size_t r = 0, w = 0;
  if (s == NULL) return;
  while (s[r] != '\0') {
    /* &amp; -> & */
    if (s[r] == '&' && strncmp(s + r, "&amp;", 5) == 0) {
      s[w++] = '&';
      r += 5;
      continue;
    }
    if (s[r] == '\\') {
      /* \/ -> / */
      if (s[r + 1] == '/') {
        s[w++] = '/';
        r += 2;
        continue;
      }
      /* \u002F -> / and \u0026 -> &, hex digits case-insensitive */
      if (s[r + 1] == 'u' || s[r + 1] == 'U') {
        int h0 = hexval(s[r + 2]), h1 = hexval(s[r + 3]);
        int h2 = hexval(s[r + 4]), h3 = hexval(s[r + 5]);
        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
          unsigned int cp = (unsigned int)(h0 << 12 | h1 << 8 | h2 << 4 | h3);
          if (cp == 0x2Fu) { s[w++] = '/'; r += 6; continue; }
          if (cp == 0x26u) { s[w++] = '&'; r += 6; continue; }
        }
      }
    }
    s[w++] = s[r++];
  }
  s[w] = '\0';
}

/* ---- search -------------------------------------------------------------- */

/* video_data[] elements. The reference's card() accepts either the cell itself or
 * a nested video_data object, because the two callers differ; the search path
 * always wraps one level. The wrapper is what carries book_id and
 * search_result_id, so it must be kept alongside the inner object. */
static int parse_search_row(const char *j, size_t len, size_t cell,
                            HgSeries *out) {
  size_t video = 0, v, hl = 0;
  int have_video, present, cnt_present;
  char id[HG_API_ID_MAX], title[192], cover[512], badge[64], desc[512];
  long long cnt = 0;

  badge[0] = '\0';
  cover[0] = '\0';
  desc[0] = '\0';

  have_video = hg_json_object_get(j, len, cell, "video_data", &video);
  if (have_video) {
    /* A video_data that is present but not an object (null, or an empty array)
     * means this row has no hit in it. The reference's objects() drops it. */
    size_t probe = video;
    while (probe < len && ascii_isspace(j[probe])) probe++;
    if (probe >= len || j[probe] != '{') have_video = 0;
  }

  /* id: video.series_id, else cell.book_id, else cell.search_result_id. */
  id[0] = '\0';
  if (have_video && opt_field(j, len, video, "series_id", id, sizeof id, &present)) {
    if (!present) id[0] = '\0';
  }
  if (id[0] == '\0' &&
      !opt_field(j, len, cell, "book_id", id, sizeof id, &present)) return HG_API_ERR_JSON;
  if (id[0] == '\0' &&
      !opt_field(j, len, cell, "search_result_id", id, sizeof id, &present)) return HG_API_ERR_JSON;
  /* No id at all, or an id outside [0-9]{1,30}: the row is not addressable and
   * the reference's valid() would drop it. */
  if (!valid_num_id(id)) return HG_API_ERR_EPISODE;
  memcpy(out->id, id, strlen(id) + 1);

  /* title: cell.search_high_light.title.text, else series_name, else title, else
   * cell.cell_name. The highlight comes FIRST -- it is the matched, unabridged
   * spelling, and for rows where the title field is absent it is the only one.
   *
   * series_name THE MEASURED SPELLING (the home/category/rank pages use it
   * everywhere) and comes before the Kotlin port's inferred `title`. Both are
   * kept because the signed search API, if it ever returns, is a different
   * endpoint from the HTML pages this fallback was written against. */
  title[0] = '\0';
  if (hg_json_object_get(j, len, cell, "search_high_light", &hl)) {
    size_t probe = hl;
    while (probe < len && ascii_isspace(j[probe])) probe++;
    if (probe < len && j[probe] == '{') {
      size_t tt;
      if (hg_json_object_get(j, len, hl, "title", &tt)) {
        size_t probe2 = tt;
        while (probe2 < len && ascii_isspace(j[probe2])) probe2++;
        if (probe2 < len && j[probe2] == '{') {
          if (!opt_field(j, len, tt, "text", title, sizeof title, &present)) return HG_API_ERR_JSON;
        }
      }
    }
  }
  if (title[0] == '\0' && have_video) {
    if (!opt_field(j, len, video, "series_name", title, sizeof title, &present))
      return HG_API_ERR_JSON;
  }
  if (title[0] == '\0' && have_video) {
    if (!opt_field(j, len, video, "title", title, sizeof title, &present)) return HG_API_ERR_JSON;
  }
  if (title[0] == '\0') {
    if (!opt_field(j, len, cell, "cell_name", title, sizeof title, &present)) return HG_API_ERR_JSON;
  }
  /* A hit with no title is unusable: it cannot be displayed or selected. */
  if (title[0] == '\0') return HG_API_ERR_MISSING;
  memcpy(out->title, title, strlen(title) + 1);

  /* cover: series_cover (measured), else cover, else cover_url. */
  if (have_video) {
    if (!opt_field(j, len, video, "series_cover", cover, sizeof cover, &present))
      return HG_API_ERR_JSON;
    if (cover[0] == '\0' &&
        !opt_field(j, len, video, "cover", cover, sizeof cover, &present)) return HG_API_ERR_JSON;
    if (cover[0] == '\0' &&
        !opt_field(j, len, video, "cover_url", cover, sizeof cover, &present)) return HG_API_ERR_JSON;
  }

  /* description: series_intro (measured), else the port's video_desc. */
  if (have_video) {
    if (!opt_field(j, len, video, "series_intro", desc, sizeof desc, &present))
      return HG_API_ERR_JSON;
    if (desc[0] == '\0') {
      if (hg_json_object_get(j, len, video, "video_desc", &v)) {
        if (!is_null_at(j, len, v)) {
          if (!str_field(j, len, v, desc, sizeof desc)) return HG_API_ERR_SMALL;
        }
      }
    }
  }

  /* badge: the count badge when episode_cnt > 0, else video.rec_text. The badge
   * format is "%d" wrapped in the Chinese characters U+5168 and U+96C6 (quan /
   * ji, i.e. "all N episodes"), which are written below as octal escapes so this
   * file stays pure ASCII on a GBK(936) machine. */
  cnt_present = 0;
  if (have_video && hg_json_object_get(j, len, video, "episode_cnt", &v)) {
    cnt_present = hg_json_int(j, len, v, &cnt);
  }
  if (cnt_present && cnt > 0) {
    static const char PREFIX[] = "\345\205\250 ";   /* U+5168 U+0020 */
    static const char SUFFIX[] = " \351\233\206";   /* U+0020 U+96C6 */
    size_t need = sizeof PREFIX - 1;
    int digits = 0;
    long long c = cnt;
    while (c > 0) { digits++; c /= 10; }
    need += (size_t)digits + sizeof SUFFIX;         /* includes the NUL */
    if (need > sizeof badge) return HG_API_ERR_SMALL;
    memcpy(badge, PREFIX, sizeof PREFIX - 1);
    {
      /* digits rendered back-to-front into the tail of the reserved space */
      size_t pos = sizeof PREFIX - 1 + (size_t)digits;
      c = cnt;
      while (c > 0) { badge[--pos] = (char)('0' + (int)(c % 10)); c /= 10; }
    }
    memcpy(badge + (sizeof PREFIX - 1) + (size_t)digits, SUFFIX, sizeof SUFFIX);
    (void)need;
  } else if (have_video) {
    if (!opt_field(j, len, video, "rec_text", badge, sizeof badge, &present)) return HG_API_ERR_JSON;
  }

  /* cleanUrl runs on the cover only, matching the reference, where the value is
   * about to become a request URL. */
  hg_api_clean_url(cover);
  memcpy(out->cover, cover, strlen(cover) + 1);
  memcpy(out->desc, desc, strlen(desc) + 1);
  memcpy(out->badge, badge, strlen(badge) + 1);

  return 0;
}

int hg_api_parse_search(const char *json, size_t len,
                        HgSeries *out, size_t cap, size_t *count,
                        int *has_more) {
  size_t data = 0, arr, elem;
  int more = 0;
  size_t n = 0;

  set_count(count, 0);
  if (has_more != NULL) *has_more = 0;

  /* The response must be an object with data.video_data[]. A response that is
   * valid JSON but of an unknown shape is MISSING, not an empty result: telling
   * the user "no results" for a changed upstream shape is the failure mode this
   * whole error vocabulary exists to avoid. */
  if (!hg_json_object_get(json, len, 0, "data", &data)) return HG_API_ERR_JSON;
  {
    size_t probe = data;
    while (probe < len && ascii_isspace(json[probe])) probe++;
    if (probe >= len || json[probe] != '{') return HG_API_ERR_JSON;
  }
  if (!hg_json_object_get(json, len, data, "video_data", &arr)) return HG_API_ERR_MISSING;
  {
    size_t probe = arr;
    while (probe < len && ascii_isspace(json[probe])) probe++;
    if (probe >= len || json[probe] != '[') return HG_API_ERR_JSON;
  }

  /* has_more lives beside video_data on the same data object. */
  if (has_more != NULL) {
    size_t hm;
    if (hg_json_object_get(json, len, data, "has_more", &hm)) {
      size_t p = hm;
      while (p < len && ascii_isspace(json[p])) p++;
      if (p + 4 <= len && memcmp(json + p, "true", 4) == 0) more = 1;
    }
  }

  /* An empty array is a legitimate "no results", and is reported as count 0 with
   * no error. */
  if (!hg_json_array_first(json, len, arr, &elem)) {
    if (has_more != NULL) *has_more = more;
    return 0;
  }

  for (;;) {
    int rc;
    if (n >= cap) return HG_API_ERR_SMALL;   /* the whole page is refused */
    rc = parse_search_row(json, len, elem, &out[n]);
    if (rc != 0) return rc;
    n++;
    if (!hg_json_array_next(json, len, arr, elem, &elem)) break;
  }

  set_count(count, n);
  if (has_more != NULL) *has_more = more;
  return (int)n;
}

/* ---- detail -------------------------------------------------------------- */

/* Walk loaderData -> seriesDetail. Returns 1 and writes the offset, or 0 when the
 * key is absent or is not an object. */
static int find_object(const char *j, size_t len, size_t obj, const char *key,
                       size_t *out) {
  size_t v, probe;
  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  probe = v;
  while (probe < len && ascii_isspace(j[probe])) probe++;
  if (probe >= len || j[probe] != '{') return 0;
  *out = v;
  return 1;
}

int hg_api_parse_detail(const char *json, size_t len, const char *series_id,
                        HgSeries *series, HgEpisode *out, size_t cap,
                        size_t *count) {
  size_t loader, detail, arr, elem;
  size_t n = 0;
  char seen[HG_API_MAX_EPISODES][HG_API_ID_MAX];
  int present;

  set_count(count, 0);
  if (series != NULL) {
    memset(series, 0, sizeof *series);
    if (!valid_num_id(series_id)) return HG_API_ERR_EPISODE;
    memcpy(series->id, series_id, strlen(series_id) + 1);
  }

  if (!find_object(json, len, 0, "loaderData", &loader)) return HG_API_ERR_JSON;
  if (!find_object(json, len, loader, "seriesDetail", &detail)) return HG_API_ERR_MISSING;
  if (!hg_json_object_get(json, len, detail, "vid_list", &arr)) return HG_API_ERR_MISSING;
  {
    size_t probe = arr;
    while (probe < len && ascii_isspace(json[probe])) probe++;
    if (probe >= len || json[probe] != '[') return HG_API_ERR_JSON;
  }

  if (series != NULL) {
    /* series_name, else series_title, else the page-level name. The page-level
     * `name` is what the reference falls back to for rows that carry no title of
     * their own. */
    series->title[0] = '\0';
    if (!opt_field(json, len, detail, "series_name", series->title, sizeof series->title, &present))
      return HG_API_ERR_JSON;
    if (series->title[0] == '\0') {
      if (!opt_field(json, len, detail, "series_title", series->title, sizeof series->title, &present))
        return HG_API_ERR_JSON;
    }
    if (series->title[0] == '\0') {
      size_t page;
      if (find_object(json, len, loader, "detail_page", &page)) {
        if (!opt_field(json, len, page, "name", series->title, sizeof series->title, &present))
          return HG_API_ERR_JSON;
      }
    }
    if (!opt_field(json, len, detail, "series_cover", series->cover, sizeof series->cover, &present))
      return HG_API_ERR_JSON;
    hg_api_clean_url(series->cover);
    if (!opt_field(json, len, detail, "series_intro", series->desc, sizeof series->desc, &present))
      return HG_API_ERR_JSON;
    /* The badge is derived exactly as the reference derives it for a detail card,
     * but that count is only available after the list is read; it is filled in
     * below once the true episode count is known. */
  }

  if (!hg_json_array_first(json, len, arr, &elem)) return HG_API_ERR_EMPTY;

  for (;;) {
    char id[HG_API_ID_MAX];
    int slen;
    size_t k;

    /* Entries are BARE strings, not objects. An entry that is an object here is a
     * shape change, and is reported as such rather than skipped. */
    slen = hg_json_string(json, len, elem, id, sizeof id);
    if (slen <= 0) return HG_API_ERR_EPISODE;
    if (slen >= (int)sizeof id) return HG_API_ERR_TOO_LONG;
    if (!valid_num_id(id)) return HG_API_ERR_EPISODE;

    /* Duplicates are refused, not de-duplicated: the reference throws, and a
     * silently deduped list shifts every later episode index. */
    for (k = 0; k < n; k++) {
      if (strcmp(seen[k], id) == 0) return HG_API_ERR_DUPLICATE;
    }

    if (n >= cap) return HG_API_ERR_SMALL;
    memcpy(out[n].id, id, (size_t)slen + 1);
    memcpy(seen[n], id, (size_t)slen + 1);
    n++;

    if (!hg_json_array_next(json, len, arr, elem, &elem)) break;
  }

  if (series != NULL) {
    /* The count badge with the real episode count, written as octal escapes:
     * this file is ASCII only and the machine's ANSI codepage is GBK. */
    static const char PREFIX[] = "\345\205\250 ";
    static const char SUFFIX[] = " \351\233\206";
    size_t digits = 0, m = n;
    char num[32];
    if (n > 0) {
      do { num[digits++] = (char)('0' + (int)(m % 10)); m /= 10; } while (m > 0);
      if (sizeof PREFIX - 1 + digits + sizeof SUFFIX > sizeof series->badge)
        return HG_API_ERR_SMALL;
      {
        size_t w = 0, i;
        memcpy(series->badge + w, PREFIX, sizeof PREFIX - 1);
        w += sizeof PREFIX - 1;
        for (i = digits; i > 0; i--) series->badge[w++] = num[i - 1];
        memcpy(series->badge + w, SUFFIX, sizeof SUFFIX);
      }
    }
  }

  set_count(count, n);
  return (int)n;
}

/* ---- video model --------------------------------------------------------- */

/* The codecs the target player cannot decode. The reference filters these by
 * name, case-insensitively, and keeps every OTHER label -- including an absent
 * one -- usable, because the legacy entries carry no codec_type at all and are
 * perfectly playable. */
static int incompatible_codec(const char *codec) {
  static const char *BAD[] = { "bytevc2", "bvc2" };
  size_t i;
  for (i = 0; i < 2; i++) {
    const char *a = codec, *b = BAD[i];
    while (*a != '\0' && *b != '\0') {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) break;
      a++; b++;
    }
    if (*a == '\0' && *b == '\0') return 1;
  }
  return 0;
}

int hg_api_parse_video_model(const char *json, size_t len,
                             HgVideoSource *out, size_t cap, size_t *count) {
  size_t arr, elem;
  size_t n = 0;
  int present;

  set_count(count, 0);

  /* NOTE: this entry point takes the INNER document -- the parsed result of
   * data[<vid>].video_model. The outer envelope is doubly encoded and unwrapping
   * it is hg_play_plan_from_model's job (hg_play.c), which owns the buffers for
   * it. Looking for video_list at the top level of the OUTER body finds nothing,
   * which is the bug that made playback fail silently. */
  if (!hg_json_object_get(json, len, 0, "video_list", &arr)) return HG_API_ERR_MISSING;
  {
    size_t probe = arr;
    while (probe < len && ascii_isspace(json[probe])) probe++;
    if (probe >= len || json[probe] != '[') return HG_API_ERR_JSON;
  }

  if (!hg_json_array_first(json, len, arr, &elem)) return HG_API_ERR_EMPTY;

  for (;;) {
    size_t v, enc = 0;
    int probe_ok = 1;
    int qi = 0, iv = 0;
    char url[2048];
    char codec[32];

    /* video_list entries are objects. A bare string here is a different shape. */
    {
      size_t probe = elem;
      while (probe < len && ascii_isspace(json[probe])) probe++;
      if (probe >= len || json[probe] != '{') return HG_API_ERR_JSON;
    }

    /* codec_type lives at video_meta.codec_type, NOT on the entry. Measured: the
     * entry has no top-level codec_type at all, so reading it there always
     * produced "" -- which this filter treats as "not filtered" and therefore
     * always ACCEPTED a bytevc2 source. That is a black screen, not an error. */
    codec[0] = '\0';
    if (hg_json_object_get(json, len, elem, "video_meta", &v)) {
      if (!opt_field(json, len, v, "codec_type", codec, sizeof codec, &present))
        return HG_API_ERR_JSON;
    }
    /* main_url is mandatory and must be non-empty; the reference skips a row
     * without one. That is a per-row property, not a page-level fault, so an
     * empty main_url SKIPS the row rather than failing the response -- an entry
     * with no URL is not a source the caller could ever use.
     *
     * The probe decodes into a LOCAL buffer. An earlier version decoded straight
     * into out[0].url, which meant probing a later row overwrote an entry that
     * had already been stored -- every filtered index shifted the earlier URLs
     * down and the list came back holding the wrong URLs with no error at all. */
    if (!hg_json_object_get(json, len, elem, "main_url", &v)) probe_ok = 0;
    else if (is_null_at(json, len, v)) probe_ok = 0;
    else {
      int slen = hg_json_string(json, len, v, url, sizeof url);
      if (slen < 0) return HG_API_ERR_SMALL;
      if (slen == 0) probe_ok = 0;
    }

    if (probe_ok) {
      /* The exclusion happens HERE, before the row occupies a slot, so `count`
       * is exactly the number of usable sources and a caller can never select a
       * bytevc2 entry by walking the list. */
      if (!incompatible_codec(codec)) {
        if (n >= cap) return HG_API_ERR_SMALL;
        /* the local `url` was already decoded and bounds-checked above */
        memcpy(out[n].url, url, strlen(url) + 1);
        if (!opt_field(json, len, elem, "key_seed", out[n].key_seed,
                       sizeof out[n].key_seed, &present)) return HG_API_ERR_JSON;
        /* spade_a lives at encrypt_info.spade_a, NOT on the entry. Same bug as
         * codec_type, same silent outcome: no key, no error, no picture. */
        out[n].spade_a[0] = '\0';
        if (hg_json_object_get(json, len, elem, "encrypt_info", &enc)) {
          if (!opt_field(json, len, enc, "spade_a", out[n].spade_a,
                         sizeof out[n].spade_a, &present)) return HG_API_ERR_JSON;
        }
        memcpy(out[n].codec_type, codec, strlen(codec) + 1);
        /* main_url arrives escaped often enough that the reference cleans it on
         * the detail path; doing it here keeps callers from having to remember. */
        hg_api_clean_url(out[n].url);

        /* THE QUALITY LADDER'S HALF. All six are optional -- a response that omits them
         * still yields a usable source -- so a missing field leaves the zero the struct
         * already started with rather than failing the row. See HgVideoSource for where each
         * one lives and why `quality_type` is a number on the ENTRY while the rest are in
         * video_meta. */
        out[n].quality_type = 0;
        out[n].has_quality_type = 0;
        out[n].definition[0] = '\0';
        out[n].vwidth = 0;
        out[n].vheight = 0;
        out[n].bitrate = 0;
        out[n].size_bytes = 0;
        if (opt_int_field(json, len, elem, "quality_type", &qi)) {
          out[n].quality_type = qi;
          out[n].has_quality_type = 1;
        }
        if (hg_json_object_get(json, len, elem, "video_meta", &v)) {
          if (!opt_field(json, len, v, "definition", out[n].definition,
                         sizeof out[n].definition, &present)) return HG_API_ERR_JSON;
          if (opt_int_field(json, len, v, "vwidth", &iv)) out[n].vwidth = iv;
          if (opt_int_field(json, len, v, "vheight", &iv)) out[n].vheight = iv;
          if (opt_int_field(json, len, v, "bitrate", &iv)) out[n].bitrate = iv;
          if (opt_int_field(json, len, v, "size", &iv)) out[n].size_bytes = (long long)iv;
        }
        n++;
      }
    }

    if (!hg_json_array_next(json, len, arr, elem, &elem)) break;
  }

  if (n == 0) return HG_API_ERR_EMPTY;
  set_count(count, n);
  return (int)n;
}

/* ---- the server-rendered pages ------------------------------------------- */

/* hg_router.h does not export skip_ws, so this file has its own. Declared here
 * because the object walkers below use it before its definition further down. */
static size_t skip_ws_off(const char *j, size_t len, size_t p);

/* Walk the members of the object at `obj`, calling back with each value offset.
 * hg_router.h has array iteration but not object iteration, and loaderData's key
 * names are dynamic -- there is no key to look up, so the members must be
 * enumerated. Kept local rather than added to hg_router.c because nothing outside
 * this file needs it and the reader there is deliberately minimal. */
static int obj_first(const char *j, size_t len, size_t obj, size_t *val,
                     char *key, size_t key_cap) {
  size_t p = skip_ws_off(j, len, obj), ks, ke, after;
  if (p >= len || j[p] != '{') return 0;
  p = skip_ws_off(j, len, p + 1);
  if (p >= len || j[p] != '"') return 0;
  ks = p + 1; ke = ks;
  while (ke < len && j[ke] != '"') { if (j[ke] == '\\') ke++; ke++; }
  if (ke >= len) return 0;
  if (key != NULL && key_cap > 0) {
    size_t n = ke - ks;
    if (n + 1 > key_cap) return 0;
    memcpy(key, j + ks, n);
    key[n] = '\0';
  }
  p = skip_ws_off(j, len, ke + 1);
  if (p >= len || j[p] != ':') return 0;
  p = skip_ws_off(j, len, p + 1);
  if (!hg_json_skip(j, len, p, &after)) return 0;
  *val = p;
  return 1;
}

static int obj_next(const char *j, size_t len, size_t val, size_t *next,
                    char *key, size_t key_cap) {
  size_t p, after, ks, ke;
  if (!hg_json_skip(j, len, val, &after)) return 0;
  p = skip_ws_off(j, len, after);
  if (p >= len || j[p] != ',') return 0;        /* end of object */
  p = skip_ws_off(j, len, p + 1);
  if (p >= len || j[p] != '"') return 0;
  ks = p + 1; ke = ks;
  while (ke < len && j[ke] != '"') { if (j[ke] == '\\') ke++; ke++; }
  if (ke >= len) return 0;
  if (key != NULL && key_cap > 0) {
    size_t n = ke - ks;
    if (n + 1 > key_cap) return 0;
    memcpy(key, j + ks, n);
    key[n] = '\0';
  }
  p = skip_ws_off(j, len, ke + 1);
  if (p >= len || j[p] != ':') return 0;
  p = skip_ws_off(j, len, p + 1);
  if (!hg_json_skip(j, len, p, &after)) return 0;
  *next = p;
  return 1;
}

/* hg_router.h does not export skip_ws, so this file has its own. */
static size_t skip_ws_off(const char *j, size_t len, size_t p) {
  while (p < len && ascii_isspace(j[p])) p++;
  return p;
}

/* 1 when the value at `pos` is an object or array literal. */static int is_container_at(const char *j, size_t len, size_t pos) {
  size_t p = skip_ws_off(j, len, pos);
  return (p < len && (j[p] == '{' || j[p] == '['));
}

/* Do the members of the object at `arr` value look like a series? Used to decide
 * whether an array is worth parsing, so a page whose first list happens to be
 * something else (home pages carry `req.semDeliveryConfig.items` before
 * `bannerList`, and its entries have a `default` object) is passed over rather
 * than reported as the content. */
static int array_looks_like_series(const char *j, size_t len, size_t arr) {
  size_t elem;
  int i;
  if (!hg_json_array_first(j, len, arr, &elem)) return 0;
  for (i = 0; i < 3; i++) {
    size_t p = skip_ws_off(j, len, elem);
    if (p < len && j[p] == '{') {
      size_t v;
      if (hg_json_object_get(j, len, elem, "series_id", &v)) return 1;
      if (hg_json_object_get(j, len, elem, "series_name", &v)) return 1;
      if (hg_json_object_get(j, len, elem, "series_cover", &v)) return 1;
    }
    if (!hg_json_array_next(j, len, arr, elem, &elem)) break;
  }
  return 0;
}

/* One series from a card object. `series_name` / `series_cover` / `series_intro`
 * are the MEASURED names. `cover_fallback` names a second cover key to try, for
 * mBannerList's `background_cover` and for older cards.
 *
 * Returns 0, 1 when the object is not a usable card (no valid series_id), or a
 * negative HG_API_ERR_*. A unusable card is skipped, not fatal: an ad slot or a
 * placeholder row is ordinary and must not empty the whole page. */
static int series_from_card(const char *j, size_t len, size_t card,
                            const char *cover_fallback, HgSeries *out) {
  int present;
  char id[HG_API_ID_MAX];

  id[0] = '\0';
  if (!opt_field(j, len, card, "series_id", id, sizeof id, &present))
    return HG_API_ERR_JSON;
  if (!valid_num_id(id)) {
    /* THE SEARCH PAGE PUTS THE CARD ONE LEVEL DOWN, UNDER `video_data`.
     *
     * MEASURED (live): a searchList[] item is
     *   { doc_type, name, keyword, video_data:{series_id, series_title, series_cover, ...}, ... }
     * so neither `series_id` nor `series_title` nor `series_cover` is at this level -- the title is
     * `name` and the id is `keyword`, and everything else lives inside `video_data`, spelled exactly
     * as on the home page.
     *
     * Rather than a second extractor for the same fields, the inner object is parsed with THIS
     * function: once we are inside `video_data` every field name is the one already handled below.
     * The recursion is bounded because a JSON document is a tree, so `video_data` cannot contain
     * itself. */
    size_t inner;
    if (hg_json_object_get(j, len, card, "video_data", &inner) && is_container_at(j, len, inner))
      return series_from_card(j, len, inner, cover_fallback, out);
    return 1;                 /* not a card we can address */
  }
  /* valid_num_id rejects anything past 30 digits, so every accepted id fits
   * HG_API_ID_MAX (31) with room for the NUL. */
  if (strlen(id) + 1 > sizeof out->id) return HG_API_ERR_TOO_LONG;

  memset(out, 0, sizeof *out);
  memcpy(out->id, id, strlen(id) + 1);

  /* series_name. `title` / `series_title` are tried after it: the SEARCH page's
   * cards are shaped differently from the home page's, and the detail page spells
   * the same value series_name. They are fallbacks in that order, not equals. */
  if (!opt_field_fit(j, len, card, "series_name", out->title, sizeof out->title, &present))
    return HG_API_ERR_JSON;
  if (out->title[0] == '\0' &&
      !opt_field_fit(j, len, card, "series_title", out->title, sizeof out->title, &present))
    return HG_API_ERR_JSON;

  if (!opt_field_fit(j, len, card, "series_cover", out->cover, sizeof out->cover, &present))
    return HG_API_ERR_JSON;
  if (out->cover[0] == '\0' && cover_fallback != NULL) {
    if (!opt_field_fit(j, len, card, cover_fallback, out->cover, sizeof out->cover, &present))
      return HG_API_ERR_JSON;
  }
  if (out->cover[0] == '\0' &&
      !opt_field_fit(j, len, card, "background_cover", out->cover, sizeof out->cover, &present))
    return HG_API_ERR_JSON;
  hg_api_clean_url(out->cover);

  if (!opt_field_fit(j, len, card, "series_intro", out->desc, sizeof out->desc, &present))
    return HG_API_ERR_JSON;

  /* TAGS: category_list[].name joined by '/'.
   *
   * MEASURED over 36 home cards: category_list is present on 36/36, while the
   * synopsis just above was present on 0/36 in one live capture (and on 2 cards
   * with >500 bytes in another -- the field comes and goes upstream). A card UI
   * that wants something descriptive beside the title therefore cannot depend on
   * the synopsis, and the tags are what is always there.
   *
   * The join is TRUNCATED AT A TAG BOUNDARY when it would not fit, rather than
   * cutting mid-tag: a half-rendered multi-byte tag is mojibake, and mojibake on
   * screen looks like a parser bug rather than a UI limit. It never fails the
   * parse -- tags are decoration, unlike the id or the title. */
  out->tags[0] = '\0';
  {
    size_t arr;
    if (hg_json_object_get(j, len, card, "category_list", &arr) &&
        is_container_at(j, len, arr)) {
      size_t elem;
      if (hg_json_array_first(j, len, arr, &elem) != 0) {
        for (;;) {
          size_t p = skip_ws_off(j, len, elem);
          if (p < len && j[p] == '{') {
            char name[48];
            int have = 0;
            name[0] = '\0';
            if (opt_field_fit(j, len, elem, "name", name, sizeof name, &have) && have &&
                name[0] != '\0') {
              size_t used = strlen(out->tags);
              size_t add = strlen(name);
              size_t need = used + (used ? 1 : 0) + add;
              if (need < sizeof out->tags) {
                if (used) out->tags[used++] = '/';
                memcpy(out->tags + used, name, add + 1);
              }
            }
          }
          if (!hg_json_array_next(j, len, arr, elem, &elem)) break;
        }
      }
    }
  }

  /* episode_right_text is the card's own "all N episodes" string (upstream sends
   * U+5168 ... U+96C6, e.g. for a 222-episode series). Preferred over a derived
   * badge when present because it is what the page itself displays. */
  if (!opt_field_fit(j, len, card, "episode_right_text", out->badge, sizeof out->badge, &present))
    return HG_API_ERR_JSON;
  if (out->badge[0] == '\0') {
    long long cnt = 0;
    size_t v;
    if (hg_json_object_get(j, len, card, "episode_cnt", &v) &&
        hg_json_int(j, len, v, &cnt) && cnt > 0) {
      static const char PREFIX[] = "\345\205\250 ";   /* U+5168 U+0020 */
      static const char SUFFIX[] = " \351\233\206";   /* U+0020 U+96C6 */
      char num[24];
      size_t d = 0, m = (size_t)cnt;
      do { num[d++] = (char)('0' + (char)(m % 10)); m /= 10; } while (m > 0);
      if (sizeof PREFIX - 1 + d + sizeof SUFFIX > sizeof out->badge)
        return HG_API_ERR_SMALL;
      {
        size_t w = 0, i;
        memcpy(out->badge + w, PREFIX, sizeof PREFIX - 1);
        w += sizeof PREFIX - 1;
        for (i = d; i > 0; i--) out->badge[w++] = num[i - 1];
        memcpy(out->badge + w, SUFFIX, sizeof SUFFIX);
      }
    }
  }
  return 0;
}

/* A card that yields a valid series_id but an empty title is refused rather than
 * accepted: it cannot be displayed or selected, and the caller would show a blank
 * row with no way to tell why. */
static int push_card(const char *j, size_t len, size_t card,
                     const char *cover_fallback, HgSeries *out, size_t cap,
                     size_t *n) {
  HgSeries tmp;
  int rc = series_from_card(j, len, card, cover_fallback, &tmp);
  size_t k;
  if (rc != 0) return rc;
  if (tmp.title[0] == '\0') return 1;              /* blank row: skip */
  for (k = 0; k < *n; k++) {
    if (strcmp(out[k].id, tmp.id) == 0) return 0;  /* already collected */
  }
  if (*n >= cap) return HG_API_ERR_SMALL;
  out[*n] = tmp;
  (*n)++;
  return 0;
}

/* Collect every series in the array at `arr`, recursing one level into arrays of
 * sections (homeSections[] holds its cards under each section's video_list). */
static int collect_array(const char *j, size_t len, size_t arr,
                         const char *cover_fallback, int sections_depth,
                         HgSeries *out, size_t cap, size_t *n) {
  size_t elem;
  if (!hg_json_array_first(j, len, arr, &elem)) return 0;

  for (;;) {
    size_t p = skip_ws_off(j, len, elem);
    int rc = 0;

    if (p < len && j[p] == '{') {
      size_t sub;
      size_t firstElem;
      /* DESCEND ONLY INTO A video_list THAT HAS SOMETHING IN IT.
       *
       * The home page's sections hold their cards under `video_list`, which is why this descends at
       * all. But a SEARCH hit also carries a `video_list` key -- always an EMPTY array (measured: every
       * item of searchList[] ends with `"video_list":[]`) -- and descending into it collected nothing
       * and returned without ever looking at the item itself. The search page therefore parsed as
       * HG_API_ERR_EMPTY: the page held 10 results and the parser reported none.
       *
       * An empty array has nothing to contribute either way, so the descent is skipped and the object
       * is treated as a card, which is what it is. (For a home section with an empty video_list this
       * changes nothing: push_card finds no series_id on it and skips it.) */
      if (hg_json_object_get(j, len, elem, "video_list", &sub) && is_container_at(j, len, sub) &&
          hg_json_array_first(j, len, sub, &firstElem)) {
        rc = collect_array(j, len, sub, cover_fallback, 0, out, cap, n);
      } else {
        rc = push_card(j, len, elem, cover_fallback, out, cap, n);
      }
    } else if (sections_depth && p < len && j[p] == '[') {
      rc = collect_array(j, len, elem, cover_fallback, 0, out, cap, n);
    }
    if (rc < 0) return rc;

    if (!hg_json_array_next(j, len, arr, elem, &elem)) break;
  }
  return 0;
}

int hg_api_find_page(const char *json, size_t len, size_t *page,
                     char *found_key, size_t key_cap) {
  size_t loader = 0, member;
  char key[128];

  if (found_key != NULL && key_cap > 0) found_key[0] = '\0';
  if (!find_object(json, len, 0, "loaderData", &loader)) return 0;

  /* First pass: a member whose own keys include a known content array. This is
   * the precise test -- home has `bannerList`, others have a bare `video_list`
   * or `seriesList` -- and it does not care what the member is called. */
  if (obj_first(json, len, loader, &member, key, sizeof key)) {
    for (;;) {
      size_t p = skip_ws_off(json, len, member);
      if (p < len && json[p] == '{') {
        static const char *MARKERS[] = {
          "bannerList", "mBannerList", "homeSections", "video_list",
          "seriesList", "seriesDetail", "vid_list", "list",
          /* MEASURED on the search page this round: its content array is `searchList`, which is why
           * the first pass used to miss that page entirely (and the second pass could not save it --
           * the items carry no series_id at their own level). See hg_api_parse_search_page. */
          "searchList"
        };
        size_t m;
        for (m = 0; m < sizeof MARKERS / sizeof MARKERS[0]; m++) {
          size_t v;
          if (hg_json_object_get(json, len, member, MARKERS[m], &v) &&
              is_container_at(json, len, v)) {
            *page = member;
            if (found_key != NULL && key_cap > 0) copy_field(found_key, key_cap, key, (int)strlen(key));
            return 1;
          }
        }
      }
      {
        size_t next;
        if (!obj_next(json, len, member, &next, key, sizeof key)) break;
        member = next;
      }
    }
  }

  /* Second pass: no marker matched (upstream renamed a key). Fall back to the
   * first object-valued member that holds an array of things shaped like a
   * series, which survives any renaming of the arrays themselves. */
  if (obj_first(json, len, loader, &member, key, sizeof key)) {
    for (;;) {
      size_t p = skip_ws_off(json, len, member);
      if (p < len && json[p] == '{') {
        size_t inner;
        if (obj_first(json, len, member, &inner, NULL, 0)) {
          for (;;) {
            size_t ip = skip_ws_off(json, len, inner);
            if (ip < len && json[ip] == '[' && array_looks_like_series(json, len, inner)) {
              *page = member;
              if (found_key != NULL && key_cap > 0)
                copy_field(found_key, key_cap, key, (int)strlen(key));
              return 1;
            }
            {
              size_t next;
              if (!obj_next(json, len, inner, &next, NULL, 0)) break;
              inner = next;
            }
          }
        }
      }
      {
        size_t next;
        if (!obj_next(json, len, member, &next, key, sizeof key)) break;
        member = next;
      }
    }
  }
  return 0;
}

int hg_api_parse_home(const char *json, size_t len,
                      HgSeries *out, size_t cap, size_t *count) {
  size_t page = 0, arr, n = 0;
  int rc;

  /* Validate before touching an out-parameter.
   *
   * `*count = 0` used to run first and `count` was never checked at all, so a
   * caller passing NULL -- which this signature invites, since the return value
   * is documented to be the count -- wrote to address 0 and aborted the process.
   * That is exactly how the first on-console run of the playback test died with
   * 2168-0002, one line after the home page parsed successfully.
   *
   * `count` is a convenience output; treating it as optional matches
   * hg_api_find_page, which already allows a NULL found_key. */
  if (count != NULL) *count = 0;
  if (json == NULL || out == NULL) return HG_API_ERR_JSON;
  if (!hg_api_find_page(json, len, &page, NULL, 0)) return HG_API_ERR_ROUTER;

  /* Banners first: they are the curated row, so when a series appears both there
   * and in a section the banner's copy is the one kept by push_card's de-dup. */
  if (hg_json_object_get(json, len, page, "bannerList", &arr) &&
      is_container_at(json, len, arr)) {
    rc = collect_array(json, len, arr, NULL, 0, out, cap, &n);
    if (rc < 0) { if (count != NULL) *count = 0; return rc; }
  }
  if (hg_json_object_get(json, len, page, "mBannerList", &arr) &&
      is_container_at(json, len, arr)) {
    rc = collect_array(json, len, arr, "background_cover", 0, out, cap, &n);
    if (rc < 0) { if (count != NULL) *count = 0; return rc; }
  }
  if (hg_json_object_get(json, len, page, "homeSections", &arr) &&
      is_container_at(json, len, arr)) {
    rc = collect_array(json, len, arr, NULL, 1, out, cap, &n);
    if (rc < 0) { if (count != NULL) *count = 0; return rc; }
  }

  /* Nothing found above: the page may be a category / search / rank page, whose
   * content sits in a differently named array directly on the page object. */
  if (n == 0) {
    size_t member;
    if (obj_first(json, len, page, &member, NULL, 0)) {
      for (;;) {
        size_t p = skip_ws_off(json, len, member);
        if (p < len && json[p] == '[' && array_looks_like_series(json, len, member)) {
          rc = collect_array(json, len, member, NULL, 0, out, cap, &n);
          if (rc < 0) { if (count != NULL) *count = 0; return rc; }
          if (n > 0) break;
        }
        {
          size_t next;
          if (!obj_next(json, len, member, &next, NULL, 0)) break;
          member = next;
        }
      }
    }
  }

  if (n == 0) return HG_API_ERR_EMPTY;
  set_count(count, n);
  return (int)n;
}

int hg_title_matches(const char *title, const char *query) {
  const char *start;
  size_t qlen, i;

  if (title == NULL || query == NULL) return 0;

  /* TRIM, and the trim is not cosmetic: " 短剧" and "短剧 " both come out of a keyboard, and a query that
   * matches nothing because of a space looks like a broken index rather than a stray keystroke. */
  while (*query == ' ' || *query == '\t' || *query == '\n' || *query == '\r') query++;
  start = query;
  while (*query != '\0') query++;
  while (query > start &&
         (query[-1] == ' ' || query[-1] == '\t' || query[-1] == '\n' || query[-1] == '\r'))
    query--;
  qlen = (size_t)(query - start);

  /* AN EMPTY QUERY MATCHES NOTHING. See the header: the caller uses this to FILTER, and "matches
   * everything" would turn an empty search box into "here is the entire catalogue". */
  if (qlen == 0) return 0;

  for (i = 0; title[i] != '\0'; i++) {
    size_t k;
    for (k = 0; k < qlen; k++) {
      unsigned char a = (unsigned char)title[i + k];
      unsigned char b = (unsigned char)start[k];
      if (a == '\0') break;                 /* the title ran out */
      /* ASCII-ONLY CASE FOLDING, deliberately: bytes >= 0x80 are UTF-8 lead or continuation bytes, and
       * folding those would corrupt a Chinese title rather than help anyone. */
      if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
      if (a != b) break;
    }
    if (k == qlen) return 1;
  }
  return 0;
}

/* ---- how a paged page says how far it goes ----------------------------------
 *
 * See hg_api.h for the measurement. One detail that is not obvious from the outside: the numbers here
 * are PLAIN JSON NUMBERS (`"total": 800`), while the search page's `totalCount` is a STRING ("201").
 * Both spellings are accepted, because the difference between the two pages is upstream's, not ours,
 * and reading a quoted number as absent would silently drop the end-of-list guard. */
static int pagination_int(const char *j, size_t len, size_t obj, const char *key, int *out) {
  size_t v;
  long long n = 0;
  char buf[32];
  int present = 0;

  if (!hg_json_object_get(j, len, obj, key, &v)) return 0;
  if (hg_json_int(j, len, v, &n)) {
    *out = (int)n;
    return 1;
  }
  buf[0] = '\0';
  if (opt_field_fit(j, len, obj, key, buf, sizeof buf, &present) && present && buf[0] != '\0') {
    *out = (int)strtol(buf, NULL, 10);
    return 1;
  }
  return 0;
}

int hg_api_parse_pagination(const char *json, size_t len, HgApiPagination *out) {
  size_t page = 0, pag = 0;
  int found = 0;

  if (out != NULL) {
    out->page = 0;
    out->page_size = 0;
    out->total = 0;
    out->total_pages = 0;
  }
  if (json == NULL || out == NULL) return 0;
  if (!hg_api_find_page(json, len, &page, NULL, 0)) return 0;
  if (!hg_json_object_get(json, len, page, "pagination", &pag) || !is_container_at(json, len, pag)) {
    return 0;
  }

  /* Each field is independent: a page that states the total but not the page size is still useful, and
   * `found` is the OR of them rather than an AND. */
  found |= pagination_int(json, len, pag, "pageNum", &out->page);
  found |= pagination_int(json, len, pag, "pageSize", &out->page_size);
  found |= pagination_int(json, len, pag, "total", &out->total);
  found |= pagination_int(json, len, pag, "totalPages", &out->total_pages);
  return found;
}

/* ---- the search page --------------------------------------------------------
 *
 * See hg_api.h for the measured shape. The implementation is short BECAUSE the two pieces that matter
 * already exist: hg_api_find_page locates the page object whatever its key is called, and
 * series_from_card unwraps `video_data` for the nested card shape. What is left is: find the array,
 * collect it, and read how many results the page claims to have.
 */
int hg_api_parse_search_page(const char *json, size_t len,
                             HgSeries *out, size_t cap, size_t *count, long long *total) {
  size_t page = 0, arr = 0, n = 0;

  /* Validate before touching an out-parameter -- the same rule and the same reason as
   * hg_api_parse_home's comment above. */
  if (count != NULL) *count = 0;
  if (total != NULL) *total = -1;
  if (json == NULL || out == NULL) return HG_API_ERR_JSON;
  if (cap == 0) return HG_API_ERR_SMALL;

  if (!hg_api_find_page(json, len, &page, NULL, 0)) return HG_API_ERR_ROUTER;

  if (!hg_json_object_get(json, len, page, "searchList", &arr) || !is_container_at(json, len, arr)) {
    /* The array was renamed. Find it by SHAPE instead of giving up: an array whose first element
     * carries a `video_data` object is this page's result list, whatever it is called. */
    size_t member;
    if (!obj_first(json, len, page, &member, NULL, 0)) return HG_API_ERR_MISSING;
    for (;;) {
      size_t mp = skip_ws_off(json, len, member);
      if (mp < len && json[mp] == '[') {
        size_t elem;
        if (hg_json_array_first(json, len, member, &elem)) {
          size_t vd;
          if (hg_json_object_get(json, len, elem, "video_data", &vd) && is_container_at(json, len, vd)) {
            arr = member;
            break;
          }
        }
      }
      {
        size_t next;
        if (!obj_next(json, len, member, &next, NULL, 0)) return HG_API_ERR_MISSING;
        member = next;
      }
    }
  }

  /* HOW MANY RESULTS EXIST IN TOTAL. `totalCount` is a STRING upstream ("201" for "ai"), so it is read
   * as text and converted. If upstream ever makes it a number this simply reports "unknown" (-1)
   * rather than failing the parse -- a page whose count cannot be read still has results worth
   * showing. */
  if (total != NULL) {
    char buf[32];
    int present = 0;
    buf[0] = '\0';
    if (opt_field_fit(json, len, page, "totalCount", buf, sizeof buf, &present) && present &&
        buf[0] != '\0') {
      /* strtol, NOT strtoll: TinyCC -- the compiler the host suite runs on -- has no strtoll, and this
       * is a result count, not a byte offset. `long` is 32 bits there and 64 on the target, and either
       * way a count that overflows it is not a count any more. */
      *total = (long long)strtol(buf, NULL, 10);
    }
  }

  /* collect_array walks the array, skips unusable entries and de-duplicates by id. A page whose list
   * is genuinely empty (a search that matched nothing) yields 0 here, which is EMPTY below. */
  {
    int rc = collect_array(json, len, arr, NULL, 0, out, cap, &n);
    if (rc < 0) return rc;
  }

  if (n == 0) return HG_API_ERR_EMPTY;
  if (count != NULL) *count = n;
  return (int)n;
}

/* ---- the home page WITH its tiers -----------------------------------------
 *
 * See hg_api.h for the measured shape and for why cross-section duplicates must
 * survive. The implementation leans on two facts about the existing code:
 *
 *   1. collect_array() already knows how to walk a section's video_list and to
 *      descend one level into homeSections[] itself. So each section is handed
 *      to collect_array directly.
 *   2. push_card() de-duplicates by scanning out[0..*n). Passing it THIS
 *      section's base pointer and a LOCAL counter therefore gives exactly
 *      per-section de-duplication, with no change to push_card and no chance of
 *      disturbing hg_api_parse_home's behaviour.
 */
int hg_api_parse_home_sections(const char *json, size_t len, HgHomePage *out) {
  size_t page = 0, arr, elem, total = 0, sections = 0;

  if (out == NULL) return HG_API_ERR_JSON;
  out->item_count = 0;
  out->section_count = 0;
  if (json == NULL) return HG_API_ERR_JSON;
  if (!hg_api_find_page(json, len, &page, NULL, 0)) return HG_API_ERR_ROUTER;
  if (!hg_json_object_get(json, len, page, "homeSections", &arr) ||
      !is_container_at(json, len, arr)) {
    return HG_API_ERR_MISSING;
  }

  if (!hg_json_array_first(json, len, arr, &elem)) return HG_API_ERR_EMPTY;

  for (;;) {
    size_t p = skip_ws_off(json, len, elem);

    if (p < len && json[p] == '{') {
      size_t sub, before = total, local = 0;
      int present = 0;

      if (sections >= HG_API_MAX_SECTIONS) { out->item_count = 0; return HG_API_ERR_SMALL; }

      if (hg_json_object_get(json, len, elem, "video_list", &sub) &&
          is_container_at(json, len, sub)) {
        int rc = collect_array(json, len, sub, NULL, 0, out->items + before,
                               (size_t)HG_API_MAX_HOME_ITEMS - before, &local);
        if (rc < 0) { out->item_count = 0; return rc; }
      }

      /* A section whose cards all failed to parse contributes nothing but an
       * empty heading, so it is dropped rather than rendered as a label with no
       * content under it. */
      if (local > 0) {
        HgHomeSection *hs = &out->sections[sections];
        hs->tab_type[0] = '\0';
        hs->tab_name[0] = '\0';
        /* The heading is display text, so a long one is truncated to fit rather
         * than allowed to fail the parse -- the same rule as a synopsis. */
        (void)opt_field_fit(json, len, elem, "tab_name", hs->tab_name, sizeof hs->tab_name, &present);
        present = 0;
        (void)opt_field_fit(json, len, elem, "tab_type", hs->tab_type, sizeof hs->tab_type, &present);
        hs->first = before;
        hs->count = local;
        sections++;
        total = before + local;
      }
    }

    if (!hg_json_array_next(json, len, arr, elem, &elem)) break;
  }

  if (sections == 0) return HG_API_ERR_EMPTY;
  out->item_count = total;
  out->section_count = sections;
  return (int)sections;
}

int hg_api_home_rows(const HgHomePage *page, HgHomeRow *out, size_t cap, size_t *count) {
  size_t i, n = 0;

  set_count(count, 0);
  if (page == NULL || out == NULL) return HG_API_ERR_JSON;

  for (i = 0; i < page->section_count; i++) {
    const HgHomeSection *s = &page->sections[i];
    size_t j;

    /* The section table must describe cards that are actually inside the item
     * array. A page built by anything other than hg_api_parse_home_sections can
     * hold a range that runs off the end, and emitting rows for it would put a
     * stale HgSeries in front of the user rather than an error. */
    if (s->first > page->item_count || s->count > page->item_count - s->first) {
      return HG_API_ERR_JSON;
    }

    if (n + 1 + s->count > cap) return HG_API_ERR_SMALL;
    out[n].kind = HG_API_ROW_HEADER;
    out[n].section = i;
    out[n].item = s->first;
    n++;

    for (j = 0; j < s->count; j++) {
      out[n].kind = HG_API_ROW_SERIES;
      out[n].section = i;
      out[n].item = s->first + j;
      n++;
    }
  }

  set_count(count, n);
  return (int)n;
}

int hg_api_parse_episode_ids(const char *json, size_t len,
                             HgEpisode *out, size_t cap, size_t *count) {  size_t page = 0, detail = 0, arr, elem;
  size_t n = 0;

  set_count(count, 0);
  if (json == NULL || out == NULL) return HG_API_ERR_JSON;

  if (!hg_api_find_page(json, len, &page, NULL, 0)) return HG_API_ERR_ROUTER;
  /* seriesDetail is nested INSIDE the content object (measured: the detail page's
   * loaderData is detail_layout + detail_page, and seriesDetail is a member of
   * detail_page). It is also possible for it to be a member of loaderData itself,
   * so both are tried before giving up. */
  if (!find_object(json, len, page, "seriesDetail", &detail)) {
    size_t loader = 0;
    if (!find_object(json, len, 0, "loaderData", &loader) ||
        !find_object(json, len, loader, "seriesDetail", &detail))
      return HG_API_ERR_MISSING;
  }
  if (!hg_json_object_get(json, len, detail, "vid_list", &arr))
    return HG_API_ERR_MISSING;
  if (!is_container_at(json, len, arr)) return HG_API_ERR_JSON;
  if (!hg_json_array_first(json, len, arr, &elem)) return HG_API_ERR_EMPTY;

  for (;;) {
    char id[HG_API_ID_MAX];
    int slen;
    size_t k;

    /* Entries are BARE STRINGS -- measured, 222 of them. An entry that is an
     * object here is a shape change and is reported as one rather than skipped,
     * because silently reading 0 episodes from a shifted shape looks like an
     * empty series. */
    slen = hg_json_string(json, len, elem, id, sizeof id);
    if (slen < 0) return HG_API_ERR_EPISODE;
    if (slen == 0) return HG_API_ERR_EPISODE;
    if (!valid_num_id(id)) return HG_API_ERR_EPISODE;

    for (k = 0; k < n; k++) {
      if (strcmp(out[k].id, id) == 0) return HG_API_ERR_DUPLICATE;
    }
    if (n >= cap) return HG_API_ERR_SMALL;
    memcpy(out[n].id, id, (size_t)slen + 1);
    n++;

    if (!hg_json_array_next(json, len, arr, elem, &elem)) break;
  }

  set_count(count, n);
  return (int)n;
}
