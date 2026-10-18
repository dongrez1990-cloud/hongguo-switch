/*
 * HTTP transport for the hongguo client, on libcurl over libnx's TLS.
 *
 * GPL-3.0-only.
 *
 * WHY libcurl AND NOT SOMETHING SMALLER: the Switch already has a working
 * libcurl built against libnx's own TLS backend (this project built it in an
 * earlier round precisely because devkitPro's copy was unreachable). wiliwili
 * reaches the same library through cpr. Writing a bespoke TLS client would mean
 * re-implementing certificate handling for no gain, and the whole point of
 * reusing wiliwili's stack is not to redo the parts that already work.
 *
 * STATIC LINKING NEEDS -DCURL_STATICLIB. Without it the curl headers decorate
 * every symbol with dllimport and the link fails on Windows-hosted toolchains.
 * The Switch build is one of those, so the flag is not optional -- libcurl.pc
 * carries it in Cflags for exactly this reason.
 *
 * STATUS: VERIFIED ON HARDWARE. This paragraph used to say the opposite -- that
 * the file had never been built or run -- which was true when it was written and
 * became false once the self-test NRO reached the console. app/net-test/main.c
 * exercises this transport against the live site and ran on a real Switch, and
 * the shipped UI app fetches the same pages through it (its breadcrumb log shows
 * "fetchHome returned ok=1 n=36"). A stale "unverified" note is worse than none:
 * it invites re-verifying what is already known and invites distrusting what is
 * not.
 */

#ifndef HG_HTTP_H
#define HG_HTTP_H

#include <stddef.h>

/* For HgHttpCfg, which hg_http_set_options takes. Included rather than forward-declared: a
 * forward declaration of a typedef'd anonymous struct does not exist in C, and the first version
 * of this header tried it and produced "conflicting types" for every function that mentioned it. */
#include "hg_httpcfg.h"

/* ---- C LINKAGE FOR C++ CALLERS (build168) ---------------------------------------------------------
 *
 * THIS HEADER DID NOT HAVE THIS GUARD, AND IT COST A LINK ERROR. hg_thread.cpp (C++) included it
 * plainly and called hg_http_in_flight(), and the linker answered:
 *
 *     undefined reference to `hg_http_in_flight()'
 *
 * -- with the parentheses, i.e. the C++ MANGLED name, because a C function declared in C++ gets C++
 * linkage unless something says otherwise. hg_image.cpp had worked around it by wrapping its own
 * #include in `extern "C" { ... }`; hg_selfcheck.cpp included the same header bare and happened to
 * link, because hg_httpcfg.h (included just above) opens an extern "C" block of its own and this
 * header's declarations therefore landed inside it -- an accident of inclusion order, not a design.
 *
 * The guard belongs HERE, in the header, where it holds for every caller and every order. Every
 * declaration below is C. The #ifdef keeps it a no-op for the C compiler and for the host tests. */
#ifdef __cplusplus
extern "C" {
#endif

#define HG_HTTP_OK            0
#define HG_HTTP_ERR_INIT     (-1)
#define HG_HTTP_ERR_REQUEST  (-2)
#define HG_HTTP_ERR_TOO_BIG  (-3)   /* response exceeded the caller's buffer */

/* Perform one request. `headers` is a NULL-terminated array of "Name: value"
 * strings (or NULL for none). `post_body` NULL means GET.
 *
 * On success writes a NUL-terminated body to `out` and the HTTP status to
 * `*status`; returns HG_HTTP_OK. A non-2xx status is still HG_HTTP_OK -- the
 * caller decides what a 403 means, because the body is usually the explanation.
 *
 * `out_len` receives the body length (excluding the terminator) when non-NULL. */
int hg_http_request(const char *url,
                    const char *const *headers,
                    const char *post_body, size_t post_len,
                    char *out, size_t out_cap, size_t *out_len,
                    long *status);

/* One-time global init. Safe to call more than once. */
void hg_http_global_init(void);

/* THE CLIENT'S TUNABLES, applied to every subsequent request.
 *
 * These used to be four literals inside hg_http_request (a 20 s timeout, a 10 s connect timeout,
 * redirects on, identity encoding) and there was no proxy and no TLS switch at all. They are now
 * settable because this service is only reachable from some networks, and "how long do I wait"
 * and "do I trust this certificate" are questions a user on a blocked or slow network has to be
 * able to answer.
 *
 * THE VALUES ARE VALIDATED BEFORE THEY GET HERE. hg_httpcfg.h does the parsing, the range checks
 * and the refusals (timeout 0, a connect timeout above the total, a proxy with no scheme) and its
 * suite covers them on the host. This function therefore takes already-sane numbers and does no
 * checking of its own beyond NULL -- one place decides what is valid, not two.
 *
 * Call it before the first request. Safe to call more than once: the settings are stored, not
 * applied to a live connection. */
void hg_http_set_options(const HgHttpCfg *cfg);

/* The options in force, for the log and for the network diagnostic page. NULL before the first
 * hg_http_set_options call and after hg_http_set_options(NULL), in which case the built-in
 * defaults are what a request uses. */
const HgHttpCfg *hg_http_get_options(void);

/* Why the last request failed.
 *
 * hg_http_request collapses every transport failure into one code, which is
 * useless for diagnosis: DNS, TLS, connect and timeout are four different
 * problems with four different fixes. These return the underlying libcurl
 * message and codes so the caller can print something actionable. */
const char *hg_http_last_error(void);   /* curl_easy_strerror() text, or "" */
int hg_http_last_curl_code(void);       /* the raw CURLcode */

/* The response headers from the last request, as received.
 *
 * Added because an HTTP 200 with a zero-length body is not self-explanatory: the
 * headers say whether the server really sent nothing, sent a redirect, or sent a
 * Content-Type that is not JSON at all. Without them the only way forward is
 * guessing at the response, which is not diagnosis. */
const char *hg_http_last_headers(void);

/* ---- THE SITE'S DEVICE COOKIE (build175) --------------------------------------------------------
 *
 * Returns "ttwid=<value>" -- ready to use as a Cookie header value -- or "" when no Set-Cookie carrying
 * one has been seen yet. hg_http_ttwid_len() gives the length, so a caller can log that it HAS one
 * without putting the token itself in the log.
 *
 * WHERE IT COMES FROM. Measured 2026-10-16: GET https://hongguoduanju.com/ and the player page both
 * answer with `set-cookie: ttwid=1%7C...%7C<expiry>%7C<64 hex>`. It is captured by the header callback
 * on ANY response, so the home-page fetch the app already makes is enough -- no extra request, and no
 * new failure mode.
 *
 * WHAT IT IS NOT. It does NOT unlock the episodes that answer 403: measured with and without it,
 * interleaved and repeated, those answer 403 either way. It exists so this app talks to the endpoint
 * the way the site's own client does, so nothing is refused for the wrong reason.
 *
 * MEMORY ONLY, and only `ttwid`: attributes (Path/Expires/HttpOnly) are not stored because this is not a
 * cookie jar, and no other cookie is captured or forwarded. */
const char *hg_http_ttwid(void);
size_t hg_http_ttwid_len(void);

/* HOW MANY REQUESTS ARE RUNNING RIGHT NOW (build168).
 *
 * The process MUST NOT tear libnx's socket service down while a request is in flight: the 2026-10-14
 * crash reports show exit() closing bsd while another thread was still inside curl's poll(), and
 * poll() then used a socket the teardown had already destroyed (Data Abort, fault address 0).
 *
 * A caller that wants to wait does not have to write the loop: hg::waitForHttpIdle() in hg_thread.hpp
 * is that loop, with the logging and a sleep, in the layer that has a sleep to call. This accessor
 * exists so the count stays where the counting happens. */
int hg_http_in_flight(void);

#ifdef __cplusplus
}   /* extern "C" -- see the note at the top of this header */
#endif

#endif /* HG_HTTP_H */
