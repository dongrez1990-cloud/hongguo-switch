/*
 * HTTP transport. See hg_http.h -- in particular the STATUS note: this is
 * written but unbuilt and unverified.
 */

#include "hg_http.h"
#include "hg_httpcfg.h"

#include <curl/curl.h>
#include <string.h>

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} HgSink;

/* libcurl write callback. Returning anything other than `size * nmemb` aborts
 * the transfer, which is how an oversized response is refused rather than
 * silently truncated: a truncated JSON body would parse "successfully" into a
 * document missing its tail, which is much harder to diagnose than an error. */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  HgSink *s = (HgSink *)userdata;
  size_t n = size * nmemb;
  if (s->len + n + 1 > s->cap) return 0;
  memcpy(s->buf + s->len, ptr, n);
  s->len += n;
  s->buf[s->len] = '\0';
  return n;
}

static int g_inited = 0;
static char g_last_error[256] = "";
static int g_last_code = 0;
static char g_headers[4096] = "";

/* ---- THE SITE'S DEVICE COOKIE (build175) --------------------------------------------------------
 *
 * Held in MEMORY ONLY. Measured, the cookie carries its own Expires (a year out) and the site reissues
 * one on any plain GET, so persisting it would add a file format, a migration and a "stale cookie on
 * disk" failure mode in exchange for saving one request per launch. The cheap and robust option is to
 * keep it for the session and take a fresh one when it is missing.
 *
 * `g_ttwid_len` is kept beside the buffer so "not captured yet" (0) is distinguishable from "captured
 * something empty", which is what the caller's re-fetch decision turns on. */
static char g_ttwid[256] = "";
static size_t g_ttwid_len = 0;

/* THE OPTIONS IN FORCE.
 *
 * Initialised to the built-in defaults at load time, so a request issued before
 * hg_http_set_options has been called behaves exactly as the previous build did. `g_have_options`
 * is there so hg_http_get_options can report "nothing was configured" rather than handing back a
 * struct nobody set -- the diagnostic page needs to be able to tell those apart. */
static HgHttpCfg g_cfg;
static int g_have_options = 0;

void hg_http_set_options(const HgHttpCfg *cfg) {
  if (cfg == NULL) {
    g_have_options = 0;
    return;
  }
  g_cfg = *cfg;
  g_have_options = 1;
}

const HgHttpCfg *hg_http_get_options(void) {
  return g_have_options ? &g_cfg : NULL;
}

const char *hg_http_last_error(void) { return g_last_error; }
int hg_http_last_curl_code(void) { return g_last_code; }
const char *hg_http_last_headers(void) { return g_headers; }

/* The captured device cookie as a "ttwid=<value>" pair, or "" when none has been seen yet.
 * See the note on g_ttwid: memory only, memory-wide, and captured from any response. */
const char *hg_http_ttwid(void) { return g_ttwid_len > 0 ? g_ttwid : ""; }
size_t hg_http_ttwid_len(void) { return g_ttwid_len; }

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t n = size * nmemb;
  size_t have = strlen(g_headers);
  (void)userdata;
  if (have + n + 1 < sizeof g_headers) {
    memcpy(g_headers + have, ptr, n);
    g_headers[have + n] = '\0';
  }
  /* ---- ttwid: THE SITE'S OWN DEVICE COOKIE, CAPTURED WHERE IT ARRIVES (build175) ----------------
   *
   * MEASURED (2026-10-16): GET https://hongguoduanju.com/ answers 200 with
   *     set-cookie: ttwid=1%7C<43 chars>%7C<expiry>%7C<64 hex>; Path=/; HttpOnly; Secure
   * and the player page issues the same cookie. It is the site's device identity, and the official web
   * client sends it back on every subsequent request -- so not sending it means this app is the only
   * client talking to the endpoint without one.
   *
   * IT IS CAPTURED HERE, IN THE HEADER CALLBACK, rather than by a second request that asks for it: the
   * cookie arrives on ANY response the site sends, so the home page fetch that already happens is
   * enough. That matters on a console -- one fewer round trip, and no new failure mode to handle.
   *
   * WHAT THIS DOES *NOT* CLAIM. The device-facing episodes (index 50 onward in one series, 6/11/12/13 in
   * others) answer 403 with or without this cookie -- measured both ways, interleaved and repeated. So
   * this is NOT a fix for "the episode will not open"; it is the app behaving like the site's own client
   * so that nothing ELSE is refused for the wrong reason. See hg_client.cpp for the failure the user
   * actually sees and why it is now reported differently.
   *
   * ONLY ttwid IS STORED, and only the name=value pair -- attributes (Path/Expires/HttpOnly) belong to a
   * cookie JAR, which this is not. Anything else the site sets is ignored rather than forwarded, so this
   * cannot leak a session cookie into a request that did not have one. */
  if (n >= 11) {
    /* Case-insensitive match on "set-cookie:" because header names are not case sensitive and this site
     * has been seen using both spellings. */
    size_t k;
    int is_set_cookie = 1;
    for (k = 0; k < 11 && is_set_cookie; k++) {
      char c = ptr[k];
      char want = "set-cookie:"[k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != want) is_set_cookie = 0;
    }
    if (is_set_cookie) {
      const char *p = ptr + 11;
      const char *end = ptr + n;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if ((size_t)(end - p) > 6 && strncmp(p, "ttwid=", 6) == 0) {
        /* Copy up to ';' (or end of line), bounded. The value is ~133 bytes on this site. */
        size_t v = 0;
        while (p + v < end && p[v] != ';' && p[v] != '\r' && p[v] != '\n' && v + 1 < sizeof g_ttwid) {
          g_ttwid[v] = p[v];
          v++;
        }
        if (v > 0) {
          g_ttwid[v] = '\0';
          g_ttwid_len = v;
        }
      }
    }
  }
  return n;   /* always claim it, so the body callback still runs */
}

static void record_error(CURLcode rc) {
  const char *s = curl_easy_strerror(rc);
  size_t n;
  g_last_code = (int)rc;
  if (s == NULL) s = "(no message)";
  n = strlen(s);
  if (n >= sizeof g_last_error) n = sizeof g_last_error - 1;
  memcpy(g_last_error, s, n);
  g_last_error[n] = '\0';
}

void hg_http_global_init(void) {
  if (g_inited) return;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  /* The defaults, so a request made before anyone configures anything is exactly as it was. */
  hg_httpcfg_defaults(&g_cfg);
  g_have_options = 0;
  g_inited = 1;
}

/* ---- HOW MANY REQUESTS ARE IN FLIGHT, SO THE PROCESS CAN WAIT FOR THEM (build168) ----------------
 *
 * WHY THIS EXISTS, AND IT IS A HARDWARE CRASH, NOT A NICETY. Two Atmosphere crash reports from
 * 2026-10-14 (Data Abort / 2168-0002, fault address 0) show the same collision:
 *
 *     exit -> __syscall_exit -> serviceClose -> _bsdDispatchImpl          (libnx bsd.c:133)
 *     ...while another thread was still inside:
 *     curl_easy_perform -> Curl_readwrite -> Curl_poll -> poll()
 *                       -> _socketParseBsdResult -> _socketGetFd         (libnx socket.c:295)
 *
 * i.e. the process tore libnx's socket service down while a cover fetch was still running, and poll()
 * walked into a null pointer. build167 removed the app's own exit BUTTON, which made it easy to hit;
 * the race itself is only closed by waiting here, at the top of the process's exit path.
 *
 * The count is what makes the wait possible. It is deliberately a plain volatile int rather than an
 * atomic: every HTTP call is made from a worker thread or from the main thread while nothing else is
 * exiting, and the only reader is the bounded loop in hg::waitForHttpIdle (hg_thread.cpp). The failure
 * this protects against is a null socket service, not a torn integer. */
static volatile int g_in_flight = 0;

static int hg_http_request_inner(const char *url,
                                 const char *const *headers,
                                 const char *post_body, size_t post_len,
                                 char *out, size_t out_cap, size_t *out_len,
                                 long *status) {
  CURL *curl;
  CURLcode rc;
  struct curl_slist *list = NULL;
  HgSink sink;
  size_t i;

  if (url == NULL || out == NULL || out_cap == 0) return HG_HTTP_ERR_REQUEST;
  hg_http_global_init();

  curl = curl_easy_init();
  if (curl == NULL) return HG_HTTP_ERR_INIT;

  sink.buf = out;
  sink.cap = out_cap;
  sink.len = 0;
  out[0] = '\0';
  g_headers[0] = '\0';

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, g_cfg.follow_redirects ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_cfg.timeout);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)g_cfg.connect_timeout);
  /* 0 is libcurl's own "use the default cache behaviour", which is what hg_httpcfg allows 0 to
   * mean here -- unlike the two timeouts above, where 0 would have meant "never". */
  curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, (long)g_cfg.dns_cache_timeout);
  /* Do not negotiate encodings we would then have to decompress. */
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");

  /* THE PROXY, when one is configured. Empty means "no proxy", which is libcurl's own default --
   * setting CURLOPT_PROXY to "" would too, but not calling it at all is clearer. */
  if (g_cfg.proxy[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_PROXY, g_cfg.proxy);
  }

  /* TLS VERIFICATION IS ON UNLESS THE USER TURNED IT OFF, and turning it off is the one setting
   * here that makes the app less safe. Both options are set explicitly rather than relying on
   * libcurl's defaults, so the value in the log is the value in force. */
  if (g_cfg.tls_verify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  } else {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (headers != NULL) {
    for (i = 0; headers[i] != NULL; i++) {
      list = curl_slist_append(list, headers[i]);
      if (list == NULL) { curl_easy_cleanup(curl); return HG_HTTP_ERR_INIT; }
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  }

  if (post_body != NULL) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_len);
  }

  rc = curl_easy_perform(curl);

  if (status != NULL) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    *status = code;
  }

  if (list != NULL) curl_slist_free_all(list);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    record_error(rc);
    /* An aborted write is reported as CURLE_WRITE_ERROR; distinguish it so the
     * caller can tell "too big" from "network broke". */
    return (rc == CURLE_WRITE_ERROR) ? HG_HTTP_ERR_TOO_BIG : HG_HTTP_ERR_REQUEST;
  }
  g_last_error[0] = '\0';
  g_last_code = 0;
  if (out_len != NULL) *out_len = sink.len;
  return HG_HTTP_OK;
}

/* THE WRAPPER IS WHERE THE ACCOUNTING LIVES, so the inner function keeps its single-purpose body and
 * none of its five return paths has to remember to decrement anything. See the note on g_in_flight. */
int hg_http_request(const char *url,
                    const char *const *headers,
                    const char *post_body, size_t post_len,
                    char *out, size_t out_cap, size_t *out_len,
                    long *status) {
  int rc;
  g_in_flight++;
  rc = hg_http_request_inner(url, headers, post_body, post_len, out, out_cap, out_len, status);
  g_in_flight--;
  return rc;
}

int hg_http_in_flight(void) { return g_in_flight; }
