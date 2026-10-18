// hg_httpcfg.h -- the HTTP client's tunables, as parsed and validated values.
//
// GPL-3.0-only.
//
// WHY THESE VALUES HAVE A MODULE OF THEIR OWN. They used to be four literals inside
// hg_http_request:
//
//     CURLOPT_TIMEOUT,        20L
//     CURLOPT_CONNECTTIMEOUT, 10L
//     CURLOPT_FOLLOWLOCATION, 1L
//     CURLOPT_ACCEPT_ENCODING, "identity"
//
// and there was no proxy and no TLS-verify switch at all. That is fine as a default and wrong as
// a policy: this service is only reachable from some networks, and "how long do I wait" and "do I
// trust this certificate" are questions a user of a blocked or slow network has to answer. So the
// numbers become knobs -- and a knob whose PARSING is untestable is a knob that eventually gets
// set to something impossible, which is why the parsing lives here.
//
// WHAT IS VALIDATED, AND WHY EACH ONE MATTERS:
//
//   timeout 0 is REFUSED, not accepted as "no timeout". In libcurl CURLOPT_TIMEOUT 0 means "never
//   time out", so a user typing 0 to mean "give up immediately" would get the opposite: a request
//   that hangs until the console is switched off, with no error and no log. 0 therefore falls back
//   to the default and says so.
//
//   A CONNECT timeout above the total timeout is refused. The two are nested -- the connect phase
//   happens inside the total -- so a connect timeout of 60 with a total of 20 can never be
//   reached, and accepting it would leave the user believing a number that does nothing.
//
//   A PROXY with no scheme is refused rather than guessed. libcurl wants "http://host:port";
//   "192.168.1.x:7890" without a scheme is a different thing (and libcurl would try to treat it as
//   one), so it is reported and ignored instead of silently failing every request.
//
// ASCII-only source file.

#ifndef HG_HTTPCFG_H
#define HG_HTTPCFG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The defaults are the literals this replaces, so a cfg file with none of these keys behaves
 * exactly as the previous build did. */
#define HG_HTTPCFG_TIMEOUT_DEFAULT 20
#define HG_HTTPCFG_CONNECT_DEFAULT 10
#define HG_HTTPCFG_DNS_CACHE_DEFAULT 60

/* Bounds. The ceiling on the total timeout is deliberately generous -- a first request on a bad
 * mobile connection can legitimately take a while -- but finite, because "no timeout" is what 0
 * would have meant. */
#define HG_HTTPCFG_TIMEOUT_MIN 1
#define HG_HTTPCFG_TIMEOUT_MAX 600
#define HG_HTTPCFG_CONNECT_MIN 1
#define HG_HTTPCFG_CONNECT_MAX 600
#define HG_HTTPCFG_DNS_CACHE_MIN 0      /* 0 is VALID here and means libcurl's own default */
#define HG_HTTPCFG_DNS_CACHE_MAX 3600

/* How many cover fetches may run at once.
 *
 * THE VALUE IS 6 BECAUSE THAT IS THE HIGHEST NUMBER OBSERVED TO BE STABLE ON HARDWARE, and the two
 * failures on either side of it are both measured:
 *
 *   * With NO cap, the app CRASHED ON STARTUP. The home layout binds all 36 cells in one pass and
 *     every cell asks immediately, so 36 concurrent fetch threads plus their buffers is what this heap
 *     tolerates and then no more. The log showed exactly seven "IMG: fetching" lines and then nothing,
 *     with no install completing. (The cap was removed in the round that tried to fix "only six covers
 *     load", which turned out to be caused by the retry logic, not by the cap.)
 *   * With a cap of 6 but a BROKEN refcounted release, the count jammed at the limit after nine
 *     installs and every later request was refused for the rest of the session -- 402 refusals against
 *     9 installs.
 *
 * So 6 is not a tuned number, it is the number this has actually run at. Raising it is a reasonable
 * experiment; it just has to be an experiment on the console, and the failure mode of getting it wrong
 * is a crash at startup rather than a slow screen. Lowering it is always safe.
 *
 * The buffer reasoning that used to be here is still true but was never the binding constraint: a
 * fetch holds up to HG_IMAGE_RETRY_MAX_BYTES (1.5 MiB) while it runs, and the measured covers are
 * 71-168 KiB with one at 1.1 MiB.
 *
 * IT IS 8 (build101): 16 crashed the console on the first launch (the user reported it immediately) while 6 was
 * stable for a whole session, so this is the middle of the range being measured -- one number, nothing else.
 *
 * IT WAS 16 IN build100, AND THIS TIME IT IS THE ONLY CHANGE: the user confirmed build99 (which had 6, no
 * install throttle and no cull) is stable, and asked for exactly one thing -- "on that version, change it to 16 and
 * try again". So nothing else is touched: one number differs from the build they verified.
 *
 * AN EARLIER ATTEMPT AT 16, AT THE USER'S REQUEST: "it loads six covers at a time -- make it sixteen, which is exactly two
 * rows." Six was a guess made before the grid's shape was known; the grid draws eight per row, so sixteen fills
 * two rows in one wave and the screen stops looking like it is filling in stripes.
 *
 * RAISING THIS IS SAFE BECAUSE THE EXPENSIVE HALF IS THROTTLED ELSEWHERE: the fetches are network work, and what
 * used to crash the console was the TEXTURE UPLOADS landing together (see HG_IMAGE_INSTALL_GAP_MS in hg_image.cpp,
 * which keeps them to one per 120 ms whatever the fetch concurrency is). So more fetches in flight means more
 * bytes arriving sooner, not more deko3d commands in one frame. */
#define HG_HTTPCFG_IMG_THREADS_DEFAULT 8
#define HG_HTTPCFG_IMG_THREADS_MIN 1
#define HG_HTTPCFG_IMG_THREADS_MAX 16

typedef struct {
  int timeout;             /* CURLOPT_TIMEOUT, seconds; >= HG_HTTPCFG_TIMEOUT_MIN */
  int connect_timeout;     /* CURLOPT_CONNECTTIMEOUT, seconds; <= timeout */
  int dns_cache_timeout;   /* CURLOPT_DNS_CACHE_TIMEOUT, seconds; 0 = libcurl default */
  int follow_redirects;    /* CURLOPT_FOLLOWLOCATION */
  int tls_verify;          /* CURLOPT_SSL_VERIFYPEER/HOST; 1 = verify (default) */
  int image_threads;       /* how many cover fetches may run at once */

  /* The proxy, or an empty string for none. Always either "" or a URL libcurl accepts -- see the
   * note above about a missing scheme. */
  char proxy[256];

  /* How many of the fields above came from the config file rather than from the defaults, and
   * which of them were REFUSED. Both are reported so the log can say "your proxy line was
   * ignored and here is why" instead of the request failing with a curl error nobody can trace.
   *
   * `refused` is maintained by hg_httpcfg_apply itself -- it counts refused calls, so a caller
   * that logs each refusal does not also have to remember to increment it. (The first version of
   * this struct declared the field and never touched it, which is a number that always reads zero
   * and looks like health; hg_httpcfg_test now checks that it moves.) */
  int applied;
  int refused;
} HgHttpCfg;

/* Fill `cfg` with the defaults. */
void hg_httpcfg_defaults(HgHttpCfg *cfg);

/* True when `s` looks like a proxy URL libcurl will accept: a scheme, then something. The schemes
 * are the ones libcurl documents for CURLOPT_PROXY: http, https, socks4, socks4a, socks5,
 * socks5h. Case-insensitive, because a hand-typed cfg file will contain "HTTP://". */
int hg_httpcfg_proxy_ok(const char *s);

/* TRUE when `s` is a valid scheme-prefixed proxy string. See hg_httpcfg_proxy_ok. */
#define HG_HTTPCFG_PROXY_MAX 256

/* Parse one "key=value" pair into `cfg` and return 1 when the key was recognised, 0 when it was
 * not, or -1 when it was recognised and REFUSED (in which case the field is left at its previous
 * value and `*why` receives a short reason).
 *
 * `*why` may be NULL. It always points at a string literal, never at freed memory.
 *
 * A recognised key with an EMPTY value is refused rather than accepted as "clear this": the cfg
 * file's own convention is that a key that is absent means "default", and an empty value is far
 * more likely to be a typo than an intention. */
int hg_httpcfg_apply(HgHttpCfg *cfg, const char *key, const char *value, const char **why);

/* Parse a whole config file's text: one "key=value" per line, '#' comments, blank lines ignored.
 * Lines whose key is not one of ours are skipped silently -- the same file carries every other
 * setting in this app, so an unrecognised key is normal, not an error.
 *
 * Returns the number of recognised keys applied. */
int hg_httpcfg_parse(HgHttpCfg *cfg, const char *text);

/* A one-line summary for the log: the values in force, and whether any were refused. This is the
 * line that makes "the proxy did not take effect" answerable without a second console run. */
int hg_httpcfg_describe(const HgHttpCfg *cfg, char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* HG_HTTPCFG_H */
