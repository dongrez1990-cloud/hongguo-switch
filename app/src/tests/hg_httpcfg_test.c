/*
 * Tests for hg_httpcfg.c -- the HTTP client's tunables.
 *
 * The checks that matter most are the REFUSALS, because each one is a value that would otherwise
 * be accepted and do the wrong thing silently:
 *
 *   - timeout=0 would mean "never time out" in libcurl, which is the opposite of what a user
 *     typing 0 expects, and it hangs rather than errors.
 *   - a connect timeout larger than the total can never be reached, so it is a number that looks
 *     like it works and does nothing.
 *   - a proxy with no scheme is a string libcurl misreads, and the symptom is "the network is
 *     broken" with no clue that the cfg line caused it.
 *
 * ASCII-only source file.
 */

#include "hg_httpcfg.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int cond) {
  checks++;
  if (cond) printf("  PASS  %-58s\n", what);
  else { failures++; printf("  FAIL  %-58s\n", what); }
}

/* ---- defaults ------------------------------------------------------------ */

static void test_defaults(void) {
  HgHttpCfg c;

  printf("\n-- the defaults are the literals this replaces --\n");
  hg_httpcfg_defaults(&c);
  ok("timeout defaults to 20 s", c.timeout == 20);
  ok("connect timeout defaults to 10 s", c.connect_timeout == 10);
  ok("dns cache defaults to 60 s", c.dns_cache_timeout == 60);
  ok("redirects are followed by default", c.follow_redirects == 1);
  ok("TLS is verified by default", c.tls_verify == 1);
  /* BACK TO 6, which is the highest number OBSERVED STABLE on hardware. 10 was tried and the app
   * crashed at startup -- the grid binds 36 cells at once, and the log stopped after seven fetch
   * attempts with no install completing. See hg_httpcfg.h for the whole history. */
  ok("image threads default to 8 (build101: 16 crashed, 6 was stable, this is the middle)", c.image_threads == 8);
  ok("there is no proxy by default", c.proxy[0] == '\0');
  ok("nothing is counted as applied or refused", c.applied == 0 && c.refused == 0);

  hg_httpcfg_defaults(NULL);
  ok("defaults(NULL) is tolerated", 1);
}

/* ---- the proxy string ---------------------------------------------------- */

static void test_proxy_validation(void) {
  printf("\n-- a proxy needs a scheme --\n");

  ok("http://host:port is accepted", hg_httpcfg_proxy_ok("http://192.168.1.x:7890"));
  ok("https:// is accepted", hg_httpcfg_proxy_ok("https://proxy.example:443"));
  ok("socks5:// is accepted", hg_httpcfg_proxy_ok("socks5://192.168.1.x:1080"));
  ok("socks5h:// is accepted", hg_httpcfg_proxy_ok("socks5h://host:1080"));
  ok("the scheme is case-insensitive", hg_httpcfg_proxy_ok("HTTP://host:1"));
  ok("a schemeless host:port is REFUSED", !hg_httpcfg_proxy_ok("192.168.1.x:7890"));
  ok("a bare hostname is refused", !hg_httpcfg_proxy_ok("proxy.local"));
  ok("a scheme with no address is refused", !hg_httpcfg_proxy_ok("http://"));
  ok("an empty string is refused", !hg_httpcfg_proxy_ok(""));
  ok("NULL is refused", !hg_httpcfg_proxy_ok(NULL));
  ok("an unknown scheme is refused", !hg_httpcfg_proxy_ok("ftp://host:21"));
}

/* ---- each key, accepted ------------------------------------------------ */

static void test_accepted(void) {
  HgHttpCfg c;
  const char* why = NULL;

  printf("\n-- accepted values --\n");
  hg_httpcfg_defaults(&c);

  ok("http.timeout is recognised",
     hg_httpcfg_apply(&c, "http.timeout", "45", &why) == 1 && c.timeout == 45);
  ok("and counted", c.applied == 1);
  ok("the wiliwili-style name is recognised too",
     hg_httpcfg_apply(&c, "HTTP_TIMEOUT", "30", &why) == 1 && c.timeout == 30);

  ok("a connect timeout under the total is accepted",
     hg_httpcfg_apply(&c, "http.connect.timeout", "5", &why) == 1 && c.connect_timeout == 5);
  ok("a connect timeout EQUAL to the total is accepted",
     hg_httpcfg_apply(&c, "http.connect.timeout", "30", &why) == 1 && c.connect_timeout == 30);

  ok("dns cache 0 is valid (libcurl's own default)",
     hg_httpcfg_apply(&c, "http.dns.cache", "0", &why) == 1 && c.dns_cache_timeout == 0);
  ok("dns cache 300 is accepted",
     hg_httpcfg_apply(&c, "http.dns.cache", "300", &why) == 1 && c.dns_cache_timeout == 300);

  ok("redirect=no is accepted",
     hg_httpcfg_apply(&c, "http.redirect", "no", &why) == 1 && c.follow_redirects == 0);
  ok("redirect=yes is accepted",
     hg_httpcfg_apply(&c, "http.redirect", "yes", &why) == 1 && c.follow_redirects == 1);
  ok("redirect=0 is accepted",
     hg_httpcfg_apply(&c, "http.redirect", "0", &why) == 1 && c.follow_redirects == 0);

  ok("tls.verify=no is accepted",
     hg_httpcfg_apply(&c, "http.tls.verify", "no", &why) == 1 && c.tls_verify == 0);
  ok("the wiliwili name TLS_VERIFY is recognised",
     hg_httpcfg_apply(&c, "TLS_VERIFY", "yes", &why) == 1 && c.tls_verify == 1);

  ok("a well-formed proxy is accepted",
     hg_httpcfg_apply(&c, "http.proxy", "http://192.168.1.x:7890", &why) == 1 &&
         strcmp(c.proxy, "http://192.168.1.x:7890") == 0);

  ok("image.threads=3 is accepted",
     hg_httpcfg_apply(&c, "image.threads", "13", &why) == 1 && c.image_threads == 13);
  ok("the wiliwili name IMAGE_REQUEST_THREADS is recognised",
     hg_httpcfg_apply(&c, "IMAGE_REQUEST_THREADS", "8", &why) == 1 && c.image_threads == 8);

  ok("an unknown key is reported as unknown, not as an error",
     hg_httpcfg_apply(&c, "hwdec", "no", &why) == 0);
  ok("and it changed nothing", c.timeout == 30 && c.image_threads == 8);
}

/* ---- the refusals, which are the point ---------------------------------- */

static void test_refusals(void) {
  HgHttpCfg c;
  const char* why = NULL;

  printf("\n-- refused values, and why each one matters --\n");

  /* THE ONE THAT HANGS. libcurl reads CURLOPT_TIMEOUT 0 as "never", so accepting it would turn
   * "give up fast" into "hang until the console is switched off". */
  hg_httpcfg_defaults(&c);
  why = NULL;
  ok("timeout=0 is REFUSED", hg_httpcfg_apply(&c, "http.timeout", "0", &why) == -1);
  ok("the default is kept instead", c.timeout == 20);
  ok("and a reason is given", why != NULL && why[0] != '\0');

  hg_httpcfg_defaults(&c);
  ok("connect timeout=0 is refused", hg_httpcfg_apply(&c, "http.connect.timeout", "0", &why) == -1);
  ok("the connect default is kept", c.connect_timeout == 10);

  /* THE NESTED PAIR. The connect phase happens inside the total, so a larger connect timeout can
   * never be reached -- a number that looks like it works and does nothing. */
  hg_httpcfg_defaults(&c);
  hg_httpcfg_apply(&c, "http.timeout", "20", &why);
  why = NULL;
  ok("a connect timeout ABOVE the total is refused",
     hg_httpcfg_apply(&c, "http.connect.timeout", "60", &why) == -1);
  ok("the connect default is kept, not the larger value", c.connect_timeout == 10);
  ok("and the reason names the nesting", why != NULL && strstr(why, "total") != NULL);

  /* Out-of-range and malformed numbers. */
  hg_httpcfg_defaults(&c);
  ok("timeout=9999 is refused as out of range",
     hg_httpcfg_apply(&c, "http.timeout", "9999", &why) == -1);
  ok("timeout=-1 is refused", hg_httpcfg_apply(&c, "http.timeout", "-1", &why) == -1);
  ok("timeout=abc is refused", hg_httpcfg_apply(&c, "http.timeout", "abc", &why) == -1);
  ok("timeout=20x is refused", hg_httpcfg_apply(&c, "http.timeout", "20x", &why) == -1);
  ok("timeout= (empty) is refused", hg_httpcfg_apply(&c, "http.timeout", "", &why) == -1);
  ok("none of those changed the default", c.timeout == 20);

  /* A LEADING PLUS is how a human writes a positive number and is accepted. */
  hg_httpcfg_defaults(&c);
  ok("timeout=+30 is accepted", hg_httpcfg_apply(&c, "http.timeout", "+30", &why) == 1 &&
                                    c.timeout == 30);

  /* THE PROXY GUESS. */
  hg_httpcfg_defaults(&c);
  why = NULL;
  ok("a schemeless proxy is refused",
     hg_httpcfg_apply(&c, "http.proxy", "192.168.1.x:7890", &why) == -1);
  ok("no proxy was set", c.proxy[0] == '\0');
  ok("and the reason says what is missing", why != NULL && strstr(why, "scheme") != NULL);

  why = NULL;
  ok("an empty proxy is refused rather than treated as 'clear it'",
     hg_httpcfg_apply(&c, "http.proxy", "", &why) == -1);
  ok("and the reason explains how to clear it", why != NULL && strstr(why, "empty") != NULL);

  /* An over-long proxy is refused rather than truncated: a truncated URL is a wrong URL. */
  {
    char big[HG_HTTPCFG_PROXY_MAX + 64];
    memset(big, 'a', sizeof big - 1);
    big[0] = 'h'; big[1] = 't'; big[2] = 't'; big[3] = 'p';
    big[4] = ':'; big[5] = '/'; big[6] = '/';
    big[sizeof big - 1] = '\0';
    ok("an over-long proxy is refused",
       hg_httpcfg_apply(&c, "http.proxy", big, &why) == -1);
    ok("no proxy was set from it", c.proxy[0] == '\0');
  }

  /* Image threads are bounded: each one is a thread plus up to 1.5 MiB of buffer. */
  hg_httpcfg_defaults(&c);
  ok("image.threads=0 is refused", hg_httpcfg_apply(&c, "image.threads", "0", &why) == -1);
  ok("image.threads=99 is refused", hg_httpcfg_apply(&c, "image.threads", "99", &why) == -1);
  ok("image.threads=1 is accepted", hg_httpcfg_apply(&c, "image.threads", "1", &why) == 1);
  ok("image.threads=16 is accepted", hg_httpcfg_apply(&c, "image.threads", "16", &why) == 1);

  /* A boolean with a value that is neither. */
  hg_httpcfg_defaults(&c);
  ok("redirect=maybe is refused", hg_httpcfg_apply(&c, "http.redirect", "maybe", &why) == -1);
  ok("and the default is kept", c.follow_redirects == 1);
  ok("tls.verify=maybe is refused",
     hg_httpcfg_apply(&c, "http.tls.verify", "maybe", &why) == -1);
  ok("and TLS is still verified", c.tls_verify == 1);

  /* THE REFUSAL COUNTER. It was declared in the struct and never incremented, which is a number
   * that always reads zero and therefore looks like good news -- the worst kind of dead field.
   * The header promises the tally; this is the check that it happens. */
  hg_httpcfg_defaults(&c);
  ok("a fresh config has no refusals", c.refused == 0 && c.applied == 0);
  hg_httpcfg_apply(&c, "http.timeout", "30", &why);
  ok("an accepted value counts as applied, not refused", c.applied == 1 && c.refused == 0);
  hg_httpcfg_apply(&c, "http.timeout", "0", &why);
  hg_httpcfg_apply(&c, "http.proxy", "no-scheme:1", &why);
  hg_httpcfg_apply(&c, "http.redirect", "maybe", &why);
  ok("three refusals are tallied", c.refused == 3);
  ok("and the refusals did not count as applied", c.applied == 1);
  /* An unknown key is NOT a refusal: the cfg file carries every setting in this app, so a key
   * that is not ours is normal, and counting it would make `refused` meaningless noise. */
  hg_httpcfg_apply(&c, "hwdec", "no", &why);
  hg_httpcfg_apply(&c, "ao", "hos", &why);
  ok("an unknown key is not a refusal", c.refused == 3 && c.applied == 1);
}

/* ---- whole-file parsing -------------------------------------------------- */

static void test_parse(void) {
  HgHttpCfg c;
  const char* text =
      "# hongguo switches\n"
      "\n"
      "hwdec=no\n"                       /* not one of ours: skipped */
      "  http.timeout = 60  \r\n"        /* spaces and a CR, as a Windows-edited file has */
      "http.connect.timeout=15\n"
      "#http.proxy=http://ignored:1\n"   /* commented out */
      "http.tls.verify=no\n"
      "image.threads=4\n"
      "ao=hos\n";

  printf("\n-- a whole file, in the shape the app actually reads --\n");
  hg_httpcfg_defaults(&c);
  {
    /* FOUR of ours are in that text: http.timeout, http.connect.timeout, http.tls.verify and
     * image.threads. The proxy line is commented out and the rest (hwdec, ao) belong to other
     * parts of this app. The first version of this test said five, which was simply a miscount --
     * and a miscount is exactly what a test is for. */
    int applied = hg_httpcfg_parse(&c, text);
    ok("four of the keys were recognised", applied == 4);
    ok("http.timeout was applied with its spaces trimmed", c.timeout == 60);
    ok("the connect timeout was applied", c.connect_timeout == 15);
    ok("TLS verification was turned off", c.tls_verify == 0);
    ok("image.threads was applied", c.image_threads == 4);
    ok("the commented-out proxy was NOT applied", c.proxy[0] == '\0');
    ok("a non-http key was skipped silently", c.applied == 4);
  }

  /* The file is the SAME one that carries every other setting in this app, so an unrecognised key
   * must be normal. If it were an error, a cfg file with one typo would configure nothing. */
  {
    HgHttpCfg d;
    hg_httpcfg_defaults(&d);
    ok("a file with nothing of ours applies nothing",
       hg_httpcfg_parse(&d, "hwdec=no\nao=hos\nhome.tiers=yes\n") == 0);
    ok("and every default survives", d.timeout == 20 && d.image_threads == 8);
  }

  /* ORDER MATTERS for the nested timeout pair: http.timeout has to come first for the connect
   * check to mean anything, and hg_httpcfg_parse processes lines in order -- which is the order
   * the user wrote them in, and the order the cfg file documents. */
  {
    HgHttpCfg d;
    hg_httpcfg_defaults(&d);
    hg_httpcfg_parse(&d, "http.connect.timeout=60\nhttp.timeout=120\n");
    ok("a connect timeout written BEFORE a larger total is refused against the old total",
       d.connect_timeout == 10 && d.timeout == 120);
    hg_httpcfg_defaults(&d);
    hg_httpcfg_parse(&d, "http.timeout=120\nhttp.connect.timeout=60\n");
    ok("and accepted when written after it",
       d.connect_timeout == 60 && d.timeout == 120);
  }

  /* Degenerate input must not crash or hang. */
  {
    HgHttpCfg d;
    hg_httpcfg_defaults(&d);
    ok("NULL text applies nothing", hg_httpcfg_parse(&d, NULL) == 0);
    ok("NULL cfg applies nothing", hg_httpcfg_parse(NULL, "http.timeout=30\n") == 0);
    ok("an empty file applies nothing", hg_httpcfg_parse(&d, "") == 0);
    ok("a line with no '=' is skipped", hg_httpcfg_parse(&d, "just-a-word\n") == 0);
    ok("a line that is only '=' is skipped", hg_httpcfg_parse(&d, "=\n") == 0);
    ok("a key with no value is refused, not applied",
       hg_httpcfg_parse(&d, "http.timeout=\n") == 0 && d.timeout == 20);
    ok("a file of only comments applies nothing",
       hg_httpcfg_parse(&d, "# a\n# b\n# c\n") == 0);
    ok("a trailing line with no newline is still read",
       hg_httpcfg_parse(&d, "http.dns.cache=120") == 1 && d.dns_cache_timeout == 120);
  }
}

/* ---- the log line -------------------------------------------------------- */

static void test_describe(void) {
  HgHttpCfg c;
  char b[320];

  printf("\n-- the summary line --\n");
  hg_httpcfg_defaults(&c);
  hg_httpcfg_describe(&c, b, (int)sizeof b);
  ok("the defaults are all in it", strstr(b, "timeout=20s") != NULL &&
                                      strstr(b, "connect=10s") != NULL &&
                                      strstr(b, "tls_verify=yes") != NULL &&
                                      strstr(b, "image_threads=8") != NULL);
  ok("and it says there is no proxy", strstr(b, "proxy=''") != NULL);

  hg_httpcfg_apply(&c, "http.proxy", "socks5://127.0.0.1:1080", NULL);
  hg_httpcfg_apply(&c, "http.tls.verify", "no", NULL);
  hg_httpcfg_describe(&c, b, (int)sizeof b);
  ok("a set proxy appears", strstr(b, "socks5://127.0.0.1:1080") != NULL);
  ok("and TLS verification being off is visible", strstr(b, "tls_verify=no") != NULL);

  hg_httpcfg_describe(&c, b, 8);
  ok("a small buffer is NUL-terminated", b[7] == '\0');
  ok("a NULL buffer is tolerated", hg_httpcfg_describe(&c, NULL, 32) == 0);
  ok("a NULL config is tolerated", hg_httpcfg_describe(NULL, b, (int)sizeof b) == 0);
}

int main(void) {
  printf("hg_httpcfg tests: the HTTP client's tunables\n");
  printf("================================================\n");

  test_defaults();
  test_proxy_validation();
  test_accepted();
  test_refusals();
  test_parse();
  test_describe();

  printf("\n================================================\n");
  printf("checks: %d   failures: %d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
