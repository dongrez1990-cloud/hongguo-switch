// hg_httpcfg implementation. See hg_httpcfg.h for what is validated and why.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_httpcfg.h"

#include <stdio.h>
#include <string.h>

/* Bounded copy that always terminates. snprintf's truncation behaviour is not portable in this
 * project's toolchain -- MEASURED: TinyCC returns -1 and leaves the buffer unterminated for an
 * exactly-sized buffer -- so it is used only to build private strings. */
static void putStr(char *dst, int cap, const char *src) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (!src) src = "";
  while (src[i] != '\0' && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int strEqCI(const char *a, const char *b) {
  size_t i;
  if (!a || !b) return 0;
  for (i = 0; a[i] && b[i]; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return a[i] == '\0' && b[i] == '\0';
}

/* Parse a decimal integer. Returns 1 on success. Rejects an empty string, a trailing non-digit,
 * and anything outside [lo, hi] -- reporting the range rather than clamping silently, because a
 * clamped timeout is a value the user did not ask for and would have no way to notice. */
static int parseInt(const char *s, int lo, int hi, int *out, const char **why) {
  long v = 0;
  size_t i = 0;
  if (!s || s[0] == '\0') {
    if (why) *why = "empty value";
    return 0;
  }
  /* A leading '+' is how a human writes a positive number; a leading '-' is a mistake. */
  if (s[0] == '+') i = 1;
  for (; s[i]; i++) {
    if (s[i] < '0' || s[i] > '9') {
      if (why) *why = "not a whole number";
      return 0;
    }
    v = v * 10 + (s[i] - '0');
    if (v > 1000000L) {
      if (why) *why = "far out of range";
      return 0;
    }
  }
  if (v < (long)lo || v > (long)hi) {
    if (why) *why = "out of range";
    return 0;
  }
  *out = (int)v;
  return 1;
}

void hg_httpcfg_defaults(HgHttpCfg *cfg) {
  if (!cfg) return;
  cfg->timeout = HG_HTTPCFG_TIMEOUT_DEFAULT;
  cfg->connect_timeout = HG_HTTPCFG_CONNECT_DEFAULT;
  cfg->dns_cache_timeout = HG_HTTPCFG_DNS_CACHE_DEFAULT;
  cfg->follow_redirects = 1;
  cfg->tls_verify = 1;
  cfg->image_threads = HG_HTTPCFG_IMG_THREADS_DEFAULT;
  cfg->proxy[0] = '\0';
  cfg->applied = 0;
  cfg->refused = 0;
}

/* Case-insensitive prefix test: does `s` start with `prefix`? */
static int startsWithCI(const char *s, const char *prefix) {
  size_t i;
  if (!s || !prefix) return 0;
  for (i = 0; prefix[i]; i++) {
    char ca = s[i], cb = prefix[i];
    if (ca == '\0') return 0;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return 1;
}

int hg_httpcfg_proxy_ok(const char *s) {
  /* The schemes libcurl documents for CURLOPT_PROXY. A string with none of them is refused rather
   * than passed through: libcurl would accept "192.168.1.x:7890" as a URL of sorts and every
   * request would then fail in a way that looks like a broken network. */
  static const char *const SCHEMES[] = {"http://", "https://", "socks4://", "socks4a://",
                                        "socks5://", "socks5h://"};
  size_t i;
  if (!s) return 0;
  for (i = 0; i < sizeof SCHEMES / sizeof SCHEMES[0]; i++) {
    if (startsWithCI(s, SCHEMES[i])) {
      /* The scheme alone is not an address. */
      return s[strlen(SCHEMES[i])] != '\0';
    }
  }
  return 0;
}

/* The body, forward-declared so the public wrapper can tally its refusals. The wrapper is the
 * only caller: `apply` is the public name, and the tally lives there so that a caller which logs
 * each refusal does not also have to remember to count it. */
static int hg_httpcfg_apply_body(HgHttpCfg *cfg, const char *key, const char *value,
                                 const char **why);

int hg_httpcfg_apply(HgHttpCfg *cfg, const char *key, const char *value, const char **why) {
  int rc;
  if (why) *why = NULL;
  if (!cfg || !key) return 0;

  /* THE COUNTER IS MAINTAINED WHERE THE REFUSAL IS DECIDED, not by the caller: a caller that had
   * to remember to increment it would eventually not, and a `refused` that is always zero reads
   * as health. (The first version of this declared the field and never touched it -- which is
   * exactly the kind of number that looks like good news, and hg_httpcfg_test now checks it
   * moves.) */
  rc = hg_httpcfg_apply_body(cfg, key, value, why);
  if (rc == -1) cfg->refused++;
  return rc;
}

static int hg_httpcfg_apply_body(HgHttpCfg *cfg, const char *key, const char *value,
                                 const char **why) {
  int v;
  if (why) *why = NULL;
  if (!cfg || !key) return 0;

  /* THE TOTAL TIMEOUT. 0 is explicitly refused: libcurl reads CURLOPT_TIMEOUT 0 as "never", so
   * accepting it would turn "give up fast" into "hang until the console is switched off". */
  if (strcmp(key, "http.timeout") == 0 || strcmp(key, "HTTP_TIMEOUT") == 0) {
    if (strcmp(value, "0") == 0) {
      if (why) *why = "0 would mean NO timeout in libcurl, which is the opposite of what it reads "
                      "as; the default is kept";
      return -1;
    }
    if (!parseInt(value, HG_HTTPCFG_TIMEOUT_MIN, HG_HTTPCFG_TIMEOUT_MAX, &v, why)) return -1;
    cfg->timeout = v;
    cfg->applied++;
    return 1;
  }

  if (strcmp(key, "http.connect.timeout") == 0 || strcmp(key, "HTTP_CONNECTION_TIMEOUT") == 0) {
    if (strcmp(value, "0") == 0) {
      if (why) *why = "0 would mean NO connect timeout in libcurl; the default is kept";
      return -1;
    }
    if (!parseInt(value, HG_HTTPCFG_CONNECT_MIN, HG_HTTPCFG_CONNECT_MAX, &v, why)) return -1;
    /* THE NESTED PAIR. The connect phase happens inside the total, so a connect timeout larger
     * than the total can never be reached -- and a number that does nothing is worse than a
     * number that was refused, because it looks like it worked. The check is against the CURRENT
     * total, and hg_httpcfg_apply is order-dependent on purpose: http.timeout must be set first
     * for this to mean anything, which hg_httpcfg_parse guarantees by processing lines in order. */
    if (v > cfg->timeout) {
      if (why) *why = "the connect timeout is larger than the total timeout, so it can never be "
                      "reached";
      return -1;
    }
    cfg->connect_timeout = v;
    cfg->applied++;
    return 1;
  }

  if (strcmp(key, "http.dns.cache") == 0 || strcmp(key, "HTTP_DNS_CACHE_TIMEOUT") == 0) {
    if (!parseInt(value, HG_HTTPCFG_DNS_CACHE_MIN, HG_HTTPCFG_DNS_CACHE_MAX, &v, why)) return -1;
    cfg->dns_cache_timeout = v;
    cfg->applied++;
    return 1;
  }

  if (strcmp(key, "http.redirect") == 0) {
    if (strEqCI(value, "yes") || strEqCI(value, "on") || strcmp(value, "1") == 0) {
      cfg->follow_redirects = 1;
    } else if (strEqCI(value, "no") || strEqCI(value, "off") || strcmp(value, "0") == 0) {
      cfg->follow_redirects = 0;
    } else {
      if (why) *why = "expected yes or no";
      return -1;
    }
    cfg->applied++;
    return 1;
  }

  /* TLS VERIFY. Defaults to ON, and turning it off is the one setting here that makes the app
   * less safe, so it is spelled exactly and the reason is logged by the caller. */
  if (strcmp(key, "http.tls.verify") == 0 || strcmp(key, "TLS_VERIFY") == 0) {
    if (strEqCI(value, "yes") || strEqCI(value, "on") || strcmp(value, "1") == 0) {
      cfg->tls_verify = 1;
    } else if (strEqCI(value, "no") || strEqCI(value, "off") || strcmp(value, "0") == 0) {
      cfg->tls_verify = 0;
    } else {
      if (why) *why = "expected yes or no";
      return -1;
    }
    cfg->applied++;
    return 1;
  }

  if (strcmp(key, "http.proxy") == 0 || strcmp(key, "HTTP_PROXY") == 0) {
    if (value[0] == '\0') {
      if (why) *why = "empty; write it out of the file to clear the proxy rather than leaving it "
                      "blank";
      return -1;
    }
    if (strlen(value) >= HG_HTTPCFG_PROXY_MAX) {
      if (why) *why = "longer than the buffer";
      return -1;
    }
    /* A PROXY WITH NO SCHEME IS REFUSED RATHER THAN GUESSED. libcurl wants "http://host:port";
     * "192.168.1.x:7890" is a different string that libcurl would misread, and the failure would
     * appear as every request breaking for no visible reason. */
    if (!hg_httpcfg_proxy_ok(value)) {
      if (why) *why = "needs a scheme, e.g. http://host:port or socks5://host:port";
      return -1;
    }
    putStr(cfg->proxy, (int)sizeof cfg->proxy, value);
    cfg->applied++;
    return 1;
  }

  /* How many cover fetches may run at once. Bounded because each one is a thread plus up to
   * 1.5 MiB of buffer, and an unbounded page would turn scrolling into allocation. */
  if (strcmp(key, "image.threads") == 0 || strcmp(key, "IMAGE_REQUEST_THREADS") == 0) {
    if (!parseInt(value, HG_HTTPCFG_IMG_THREADS_MIN, HG_HTTPCFG_IMG_THREADS_MAX, &v, why)) {
      return -1;
    }
    cfg->image_threads = v;
    cfg->applied++;
    return 1;
  }

  return 0;   /* not one of ours, which is normal */
}

/* Trim leading and trailing spaces in place, and strip a trailing CR (the file is usually written
 * from Windows). Returns the trimmed pointer. */
static char *trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    end--;
  }
  *end = '\0';
  return s;
}

int hg_httpcfg_parse(HgHttpCfg *cfg, const char *text) {
  int applied = 0;
  const char *p;
  if (!cfg || !text) return 0;

  p = text;
  while (*p) {
    const char *lineEnd = strchr(p, '\n');
    char line[512];
    size_t n = lineEnd ? (size_t)(lineEnd - p) : strlen(p);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, p, n);
    line[n] = '\0';

    {
      char *s = trim(line);
      /* '#' starts a comment, and a blank line is skipped. */
      if (s[0] != '\0' && s[0] != '#') {
        char *eq = strchr(s, '=');
        if (eq) {
          char *key;
          char *val;
          *eq = '\0';
          key = trim(s);
          val = trim(eq + 1);
          if (hg_httpcfg_apply(cfg, key, val, NULL) == 1) applied++;
        }
      }
    }

    if (!lineEnd) break;
    p = lineEnd + 1;
  }

  return applied;
}

int hg_httpcfg_describe(const HgHttpCfg *cfg, char *buf, int bufsize) {
  char tmp[320];
  int n = 0;
  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';
  if (!cfg) return 0;

  snprintf(tmp, sizeof tmp, "timeout=%ds connect=%ds dns_cache=%ds redirect=%s tls_verify=%s "
                            "proxy='%s' image_threads=%d",
           cfg->timeout, cfg->connect_timeout, cfg->dns_cache_timeout,
           cfg->follow_redirects ? "yes" : "no", cfg->tls_verify ? "yes" : "no", cfg->proxy,
           cfg->image_threads);
  tmp[sizeof tmp - 1] = '\0';

  putStr(buf, bufsize, tmp);
  while (buf[n] != '\0') n++;
  return n;
}
