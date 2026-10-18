/*
 * Signed-request assembly: URL encoding, query building, and the header set.
 *
 * GPL-3.0-only. Mirrors N3urda/hongguoTV server/vendor/hongguo.cjs:25-27, 79-85,
 * 405-437 (GPL-3.0), derived from woshishiq1/drpys @22261ad (GPL-3.0). Combined
 * work: GPL-3.0.
 *
 * WHY THE ENCODER IS ITS OWN FUNCTION WITH ITS OWN VECTORS: the encoded query
 * string is an INPUT to the signature. One wrong byte changes x-gorgon and
 * x-medusa and the server rejects the request with no useful diagnostic. So the
 * encoder is pinned to the reference the same way the hash is, rather than being
 * assumed correct because it "looks like" percent-encoding.
 *
 * The exact safe set is A-Z a-z 0-9 - _ . ~ * . Two details are not what a
 * generic encoder would do:
 *   * `*` SURVIVES. Python's quote() would escape it; JS encodeURIComponent
 *     keeps it, and the reference encodes with encodeURIComponent.
 *   * `! ' ( )` are kept by encodeURIComponent and then escaped by an explicit
 *     .replace() that follows it, so they DO come out as %21 %27 %28 %29.
 *
 * SIZE: an HgRequest is about 16 KiB and holds the medusa header inline. It must
 * be allocated by the caller (file scope or heap) -- it is not a stack object.
 */

#ifndef HG_REQUEST_H
#define HG_REQUEST_H

#include "hg_tables.h"

#include <stddef.h>
#include <stdint.h>

/* A POST (video model) needs SEVENTEEN headers, not fourteen: it adds
 * Content-Type, x-vc-bdturing-sdk-version and x-ss-stub on top of the GET set.
 * This was 16, so the 17th silently failed to store and hg_request_video_model
 * returned -1 for every call -- a path no test covered until the request
 * assembly test below was added. */
#define HG_REQ_MAX_HEADERS 20
#define HG_REQ_STORE       16384
#define HG_REQ_URL_MAX     2048
#define HG_REQ_BODY_MAX    1024

typedef struct {
  char url[HG_REQ_URL_MAX];
  char body[HG_REQ_BODY_MAX];
  size_t body_len;
  const char *headers[HG_REQ_MAX_HEADERS + 1];
  char store[HG_REQ_STORE];      /* backs every header string */
  char query[HG_REQ_URL_MAX];    /* the encoded query, for signing */
} HgRequest;

/* Percent-encode one component into `out` (NUL-terminated). Returns the encoded
 * length, or -1 if it does not fit. */
int hg_url_encode(const char *in, char *out, size_t cap);

/* Percent-encode the device fingerprint followed by `extra` key/value pairs,
 * joined with '&', into `out`. Insertion order is preserved; the signature
 * depends on it. Returns the length or -1. */
int hg_url_encode_map(const HgKv *extra, size_t extra_count, char *out, size_t cap);

/* As above, but `overrides` replace device entries IN PLACE.
 *
 * This mirrors `{...DEVICE, k: v}` in the reference, which OVERWRITES: the key
 * keeps its original position and appears exactly once. Appending instead would
 * emit the key twice, and the signature covers the query string, so the server
 * would see a different request than the one the reference makes.
 * The search endpoint uses APP_DEVICE, which is DEVICE with five overrides. */
int hg_url_encode_map_ex(const HgKv *overrides, size_t override_count,
                         const HgKv *extra, size_t extra_count,
                         char *out, size_t cap);

/* Build a signed GET for the search endpoint. Returns 0 on success. */
int hg_request_search(HgRequest *r, const char *keyword,
                      uint32_t khronos, uint64_t ticket_ms,
                      uint32_t nonce);

/* Build a signed POST for the video model endpoint. `vid` is the episode id.
 * Returns 0 on success. Tries up to 32 ticket offsets to avoid branch 1, which
 * the server does not accept (hongguo.cjs:428).
 *
 * RETAINED BUT NO LONGER THE PLAYBACK SOURCE (build173). As of 2026-10-16 this route answers
 * `HTTP 200 + 0 bytes` to EVERY request -- signed, unsigned and deliberately corrupted alike, for any
 * body and any id on both hosts -- so nothing can be fetched through it. It is kept, not deleted,
 * because it is the reference the new route is compared against and because a future round may need to
 * prove again which of the two is live. The playback path is hg_request_play_info() below.
 * Evidence: docs/ (the play-interface change note dated 2026-10-16). */
int hg_request_video_model(HgRequest *r, const char *vid,
                           uint32_t khronos, uint64_t ticket_ms,
                           uint32_t nonce);

/* Build the WEB CLIENT's play-info POST (build173). This is the route that answers now.
 *
 * UNSIGNED, unlike every other request in this file: no gorgon/ladon/argus/helios/medusa, no device
 * query, no ticket jitter. The complete header set is Content-Type, X-Xs-From-Web: 1, a browser
 * User-Agent and Accept.
 *
 * `series_id` and `vid` must be [0-9]{1,30} (refused otherwise, never escaped into the body), and
 * `definition` must be one of "360p" / "480p" / "720p" -- the only three the service accepts.
 *
 * `cookie` is the "ttwid=<value>" pair to send, or NULL/"" for none (build175). It is a PARAMETER
 * rather than a lookup INTO THE TRANSPORT so this file keeps no dependency on hg_http.c -- the signer
 * test suite links this translation unit without it (it cannot link hg_http.c: that needs curl and
 * POSIX headers the host build does not provide). Passing it in is what keeps that suite building.
 *
 * `khronos`/`ticket_ms`/`nonce` are unused by the endpoint and exist so this can be called through the
 * same shape as its siblings. Returns 0 on success. */
int hg_request_play_info(HgRequest *r, const char *series_id, const char *vid,
                         const char *definition, const char *cookie, uint32_t khronos,
                         uint64_t ticket_ms, uint32_t nonce);

#endif /* HG_REQUEST_H */
