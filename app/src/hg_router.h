/*
 * HTML scraping helpers for the 红果 website endpoints (home, detail, rank, and
 * the search fallback).
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV ContentRepository.kt (GPL-3.0),
 * derived from woshishiq1/drpys @22261ad (GPL-3.0). Combined work: GPL-3.0.
 *
 * Three of the four content endpoints do not return JSON at all: they are
 * server-rendered pages with the real payload embedded in a
 * `window._ROUTER_DATA = {...}` script literal. extractRouter pulls that object
 * out by brace matching, and the single hard part is that braces inside JSON
 * strings must not count -- a drama description containing "}" or an escaped
 * quote is entirely ordinary, and getting this wrong truncates the blob in a way
 * that looks like a successful parse of a shorter document.
 *
 * This is the most fragile part of the whole port: it depends on an upstream
 * page layout that nobody promised to keep stable. It fails loudly (distinct
 * error codes) rather than returning a half-parsed document.
 *
 * Deliberately NOT attempted here: a general JSON parser. hg_json_find_string is
 * a targeted helper for pulling one named string out of the router blob, which
 * is all the client needs for the fields it reads.
 */

#ifndef HG_ROUTER_H
#define HG_ROUTER_H

#include <stddef.h>

#define HG_ROUTER_ERR_NO_DATA    (-1)   /* no _ROUTER_DATA marker on the page */
#define HG_ROUTER_ERR_FORMAT     (-2)   /* marker present, no '{' after it */
#define HG_ROUTER_ERR_INCOMPLETE (-3)   /* braces never balanced */
#define HG_ROUTER_ERR_SMALL      (-4)   /* caller's buffer too small */
#define HG_ROUTER_ERR_NOT_FOUND  (-5)   /* hg_json_find_string: key absent */

/* Extract the balanced JSON object that follows the _ROUTER_DATA marker.
 * Writes a NUL-terminated copy to `out` and returns its length, or a negative
 * HG_ROUTER_ERR_*. */
int hg_router_extract(const char *html, size_t len, char *out, size_t cap);

/* Find the first `"key"` and return its string value with the usual JSON escapes
 * (\" \\ \/ \b \f \n \r \t) resolved. Returns the value length or a negative
 * HG_ROUTER_ERR_*. Intended for flat lookups such as a series title or cover URL
 * inside the router blob. */
int hg_json_find_string(const char *json, const char *key, char *out, size_t cap);

/* ------------------------------------------------------------------------ */
/* A position-based JSON reader.                                            */
/*                                                                          */
/* Needed because BOTH sides of this layer speak JSON: the signed API returns  */
/* it, and the scraped `_ROUTER_DATA` blob is a JSON object. The responses are */
/* arrays of objects (search hits, episode lists, video qualities), so a flat  */
/* key lookup is not enough -- the caller has to iterate.                     */
/*                                                                          */
/* "Positions" are byte offsets into the caller's buffer. Nothing is copied and */
/* nothing is allocated; the reader is read-only and never fails destructively. */
/* It is NOT a validating parser: it does not build a DOM and does not reject   */
/* malformed input beyond what it needs to keep advancing. That is deliberate -- */
/* upstream shapes are not ours to enforce, and a lenient reader that returns   */
/* "key absent" is more useful than one that refuses a whole response over a     */
/* trailing comma.                                                             */
/* ------------------------------------------------------------------------ */

/* Skip the value starting at `pos`; write the offset just past it. Returns 1. */
int hg_json_skip(const char *j, size_t len, size_t pos, size_t *next);

/* Look up `key` directly inside the object at `obj`; write the value's offset.
 * Returns 1 when found. */
int hg_json_object_get(const char *j, size_t len, size_t obj, const char *key, size_t *val);

/* Decode the JSON string at `pos` into `out` (NUL-terminated). Returns the
 * decoded length, or -1 if `pos` is not a string or it does not fit. */
int hg_json_string(const char *j, size_t len, size_t pos, char *out, size_t cap);

/* As hg_json_string, but a small buffer is NOT an error: it writes as much as
 * fits, backs off to a UTF-8 character boundary so the result stays valid text,
 * and NUL-terminates. Returns the bytes written, or -1 only when `pos` is not a
 * string at all.
 *
 * Needed because real display text does not fit fixed buffers. Two mBannerList
 * cards on the live home page carry series_intro of 553 and 570 bytes while
 * HgSeries.desc is 512; refusing to decode them made the parser report
 * HG_API_ERR_JSON and the ENTIRE home page fail to load. A synopsis that loses
 * its last line is not a reason to show the user nothing. */
int hg_json_string_fit(const char *j, size_t len, size_t pos, char *out, size_t cap);

/* Read an integer (or a bare literal) at `pos`. Returns 1 on success. */
int hg_json_int(const char *j, size_t len, size_t pos, long long *out);

/* Array iteration. `arr` is the offset of '['. Each returns 1 and writes the
 * element's offset, or 0 when there is no such element. */
int hg_json_array_first(const char *j, size_t len, size_t arr, size_t *elem);
int hg_json_array_next(const char *j, size_t len, size_t arr, size_t elem, size_t *next);

#endif /* HG_ROUTER_H */
