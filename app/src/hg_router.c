/*
 * HTML scraping helpers. See hg_router.h for provenance and the fragility warning.
 */

#include "hg_router.h"

#include <string.h>

static const char MARKER[] = "_ROUTER_DATA";
#define MARKER_LEN 11

int hg_router_extract(const char *html, size_t len, char *out, size_t cap) {
  size_t i, first = 0, marker_at = 0;
  int depth = 0, quoted = 0, escaped = 0;
  int have_marker = 0, have_first = 0;

  /* locate the marker */
  if (len < MARKER_LEN) return HG_ROUTER_ERR_NO_DATA;
  for (i = 0; i + MARKER_LEN <= len; i++) {
    if (memcmp(html + i, MARKER, MARKER_LEN) == 0) { marker_at = i; have_marker = 1; break; }
  }
  if (!have_marker) return HG_ROUTER_ERR_NO_DATA;

  /* the object starts at the first '{' at or after the marker */
  for (i = marker_at; i < len; i++) {
    if (html[i] == '{') { first = i; have_first = 1; break; }
  }
  if (!have_first) return HG_ROUTER_ERR_FORMAT;

  /* brace matching, quote- and escape-aware. Inside a string, braces and
   * brackets are literal data and must not move the depth. */
  for (i = first; i < len; i++) {
    char ch = html[i];
    if (quoted) {
      if (escaped) escaped = 0;
      else if (ch == '\\') escaped = 1;
      else if (ch == '"') quoted = 0;
      continue;
    }
    if (ch == '"') quoted = 1;
    else if (ch == '{' || ch == '[') depth++;
    else if (ch == '}' || ch == ']') {
      depth--;
      if (depth == 0) {
        size_t n = i + 1 - first;
        if (n + 1 > cap) return HG_ROUTER_ERR_SMALL;
        memcpy(out, html + first, n);
        out[n] = '\0';
        return (int)n;
      }
    }
  }
  return HG_ROUTER_ERR_INCOMPLETE;
}

/* ---- JSON string decoding, shared by both entry points ------------------ */

/* Forward declaration: skip_ws is defined further down, in the JSON reader
 * section, but json_decode_string below needs it. Without this the compiler
 * assumes an int-returning function and then errors on the real definition. */
static size_t skip_ws(const char *j, size_t len, size_t p);

static int put_utf8(char *out, size_t cap, size_t *w, unsigned int cp) {
  if (cp < 0x80) {
    if (*w + 1 > cap) return 0;
    out[(*w)++] = (char)cp;
  } else if (cp < 0x800) {
    if (*w + 2 > cap) return 0;
    out[(*w)++] = (char)(0xC0 | (cp >> 6));
    out[(*w)++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    if (*w + 3 > cap) return 0;
    out[(*w)++] = (char)(0xE0 | (cp >> 12));
    out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*w)++] = (char)(0x80 | (cp & 0x3F));
  } else {
    if (*w + 4 > cap) return 0;
    out[(*w)++] = (char)(0xF0 | (cp >> 18));
    out[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[(*w)++] = (char)(0x80 | (cp & 0x3F));
  }
  return 1;
}

static int hex4(const char *j, size_t len, size_t p, unsigned int *out) {
  unsigned int v = 0;
  int i;
  if (p + 4 > len) return 0;
  for (i = 0; i < 4; i++) {
    char c = j[p + (size_t)i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return 0;
    v = (v << 4) | (unsigned int)d;
  }
  *out = v;
  return 1;
}

/* Decode the JSON string starting at `pos` (which must be the opening quote).
 *
 * \uXXXX IS decoded, into UTF-8, and surrogate pairs are combined. This is not a
 * nicety: the content this layer exists to read is Chinese drama titles, and the
 * upstream emits them escaped. A reader that skipped \u would hand back
 * "u77edu5267" instead of a title -- and it would look like a parsing bug
 * somewhere else entirely. */
static int json_decode_string_ex(const char *j, size_t len, size_t pos, char *out,
                                 size_t cap, int truncate) {
  size_t p = skip_ws(j, len, pos), w = 0;
  if (p >= len || j[p] != '"') return -1;
  p++;
  while (p < len) {
    char ch = j[p];
    if (ch == '"') {
      if (w + 1 > cap) return truncate ? (int)w : -1;
      out[w] = '\0';
      return (int)w;
    }
    if (ch == '\\' && p + 1 < len) {
      char e = j[++p];
      if (e == 'u') {
        unsigned int cp;
        if (!hex4(j, len, p + 1, &cp)) return -1;
        p += 4;
        /* surrogate pair: high followed by \uDC00-\uDFFF */
        if (cp >= 0xD800u && cp <= 0xDBFFu && p + 6 < len &&
            j[p + 1] == '\\' && j[p + 2] == 'u') {
          unsigned int lo;
          if (hex4(j, len, p + 3, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            p += 6;
          }
        }
        /* Back off a whole character rather than emit a partial sequence: half
         * a UTF-8 character is not displayable text. */
        {
          size_t need = cp < 0x80u ? 1u : cp < 0x800u ? 2u : cp < 0x10000u ? 3u : 4u;
          if (w + need + 1 > cap) {
            out[w] = '\0';
            return truncate ? (int)w : -1;
          }
        }
        if (!put_utf8(out, cap, &w, cp)) return -1;
        p++;
        continue;
      }
      switch (e) {
        case 'n': ch = '\n'; break;
        case 't': ch = '\t'; break;
        case 'r': ch = '\r'; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        default:  ch = e;    break;   /* \" \\ \/ and anything else: literal */
      }
      p++;
    } else {
      p++;
    }
    if (w + 1 > cap) {
      if (truncate) { out[w] = '\0'; return (int)w; }
      return -1;
    }
    out[w++] = ch;
  }
  if (truncate) { if (cap > 0) out[w < cap ? w : cap - 1] = '\0'; return (int)w; }
  return -1;
}

static int json_decode_string(const char *j, size_t len, size_t pos, char *out, size_t cap) {
  return json_decode_string_ex(j, len, pos, out, cap, 0);
}

int hg_json_string_fit(const char *j, size_t len, size_t pos, char *out, size_t cap) {
  if (cap == 0) return -1;
  return json_decode_string_ex(j, len, pos, out, cap, 1);
}

int hg_json_find_string(const char *json, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key), jlen = strlen(json), i;
  size_t value_start;

  /* find "key" followed by optional space, ':', optional space, '"' */
  for (i = 0; i + klen + 2 <= jlen; i++) {
    size_t p;
    if (json[i] != '"') continue;
    if (memcmp(json + i + 1, key, klen) != 0) continue;
    if (json[i + 1 + klen] != '"') continue;
    p = i + 2 + klen;
    while (p < jlen && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
    if (p >= jlen || json[p] != ':') continue;
    p++;
    while (p < jlen && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
    if (p >= jlen || json[p] != '"') continue;
    value_start = p + 1;
    goto found;
  }
  return HG_ROUTER_ERR_NOT_FOUND;

found:
  /* value_start is one past the opening quote; json_decode_string expects the
   * quote itself. Passing value_start made it reject every value and the caller
   * get an empty string -- a silent, total failure of this whole entry point. */
  return json_decode_string(json, jlen, value_start - 1, out, cap);
}

/* ------------------------------------------------------------------------ */
/* JSON reader                                                               */
/* ------------------------------------------------------------------------ */

static size_t skip_ws(const char *j, size_t len, size_t p) {
  while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
  return p;
}

/* Advance past a quoted string starting at j[p] == '"'. */
static size_t skip_string(const char *j, size_t len, size_t p) {
  p++;                                  /* opening quote */
  while (p < len) {
    if (j[p] == '\\') { p += 2; continue; }
    if (j[p] == '"') return p + 1;
    p++;
  }
  return len;
}

int hg_json_skip(const char *j, size_t len, size_t pos, size_t *next) {
  size_t p = skip_ws(j, len, pos);
  if (p >= len) return 0;

  if (j[p] == '"') { *next = skip_string(j, len, p); return 1; }

  if (j[p] == '{' || j[p] == '[') {
    int depth = 0;
    while (p < len) {
      char c = j[p];
      if (c == '"') { p = skip_string(j, len, p); continue; }
      if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') {
        depth--;
        p++;
        if (depth == 0) { *next = p; return 1; }
        continue;
      }
      p++;
    }
    return 0;                           /* unbalanced */
  }

  /* number / true / false / null */
  {
    size_t start = p;
    while (p < len && j[p] != ',' && j[p] != '}' && j[p] != ']' &&
           j[p] != ' ' && j[p] != '\t' && j[p] != '\n' && j[p] != '\r') p++;
    if (p == start) return 0;
    *next = p;
    return 1;
  }
}

int hg_json_object_get(const char *j, size_t len, size_t obj, const char *key, size_t *val) {
  size_t p = skip_ws(j, len, obj);
  size_t klen = strlen(key);

  if (p >= len || j[p] != '{') return 0;
  p = skip_ws(j, len, p + 1);

  while (p < len && j[p] != '}') {
    size_t ks, ke, after;
    if (j[p] != '"') return 0;
    ks = p + 1;
    ke = skip_string(j, len, p) - 1;    /* offset of the closing quote */
    if (ke < ks) return 0;
    p = ke + 1;

    if (ke - ks == klen && memcmp(j + ks, key, klen) == 0) {
      p = skip_ws(j, len, p);
      if (p >= len || j[p] != ':') return 0;
      p = skip_ws(j, len, p + 1);
      if (!hg_json_skip(j, len, p, &after)) return 0;
      *val = p;
      return 1;
    }

    p = skip_ws(j, len, p);
    if (p >= len || j[p] != ':') return 0;
    p = skip_ws(j, len, p + 1);
    if (!hg_json_skip(j, len, p, &after)) return 0;
    p = skip_ws(j, len, after);
    if (p < len && j[p] == ',') { p = skip_ws(j, len, p + 1); continue; }
    break;
  }
  return 0;
}

int hg_json_string(const char *j, size_t len, size_t pos, char *out, size_t cap) {
  return json_decode_string(j, len, pos, out, cap);
}

int hg_json_int(const char *j, size_t len, size_t pos, long long *out) {
  size_t p = skip_ws(j, len, pos), start;
  int neg = 0;
  long long v = 0;
  if (p >= len) return 0;
  if (j[p] == '"') {                    /* some upstream counters arrive quoted */
    char tmp[32];
    int n = hg_json_string(j, len, p, tmp, sizeof tmp);
    const char *q = tmp;
    if (n <= 0) return 0;
    if (*q == '-') { neg = 1; q++; }
    if (*q < '0' || *q > '9') return 0;
    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
    *out = neg ? -v : v;
    return 1;
  }
  if (j[p] == '-') { neg = 1; p++; }
  start = p;
  while (p < len && j[p] >= '0' && j[p] <= '9') { v = v * 10 + (j[p] - '0'); p++; }
  if (p == start) return 0;
  *out = neg ? -v : v;
  return 1;
}

int hg_json_array_first(const char *j, size_t len, size_t arr, size_t *elem) {
  size_t p = skip_ws(j, len, arr);
  if (p >= len || j[p] != '[') return 0;
  p = skip_ws(j, len, p + 1);
  if (p >= len || j[p] == ']') return 0;
  *elem = p;
  return 1;
}

int hg_json_array_next(const char *j, size_t len, size_t arr, size_t elem, size_t *next) {
  size_t p, after;
  if (!hg_json_skip(j, len, elem, &after)) return 0;
  p = skip_ws(j, len, after);
  if (p >= len || j[p] != ',') return 0; /* end of array (or trailing junk) */
  p = skip_ws(j, len, p + 1);
  if (p >= len || j[p] == ']') return 0;
  (void)arr;
  *next = p;
  return 1;
}
