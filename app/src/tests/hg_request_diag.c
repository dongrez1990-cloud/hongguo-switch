/*
 * Diagnostic for hg_request_search returning -1 on device.
 *
 * Temporary: it exists to localise a failure the console reported but could not
 * explain, by reproducing it on the host where each step can be inspected.
 */

#include <stdio.h>
#include <string.h>

#include "hg_request.h"
#include "hg_crypto.h"
/* hg_build_medusa is declared here. Without it TinyCC implicitly declares it and
 * the call silently returns 0 -- which made this diagnostic report "medusa is
 * broken" when medusa was fine. Fourth time an implicit declaration has caused a
 * wrong conclusion in this project; see hg_ror64, hg_cenc.h and hg_router_test.c. */
#include "hg_sign.h"
#include "sign_vectors_gen.h"

/* Same shape as hg_request.c's static default_rng. The first run of this
 * diagnostic passed NULL and both functions dutifully failed -- which said
 * nothing about the real path, since build_common always passes a live rng. */
static uint32_t diag_state = 1;
static uint32_t diag_rng(void *user) {
  (void)user;
  diag_state = diag_state * 1664525u + 1013904223u;
  return diag_state;
}

int main(void) {
  static HgRequest req;
  static char q[8192];
  HgKv extra[4];
  char ts[24], rt[24];
  int n = 0, rc, i;

  /* the exact values the console used */
  uint32_t khronos = 1791464625u;
  uint64_t ticket = (uint64_t)1791464625ull * 1000ull;

  printf("HG_DEVICE_COUNT      = %d\n", HG_DEVICE_COUNT);
  printf("sizeof(HgRequest)    = %u\n", (unsigned)sizeof(HgRequest));
  printf("HG_REQ_STORE         = %d\n", HG_REQ_STORE);
  printf("HG_REQ_URL_MAX       = %d\n", HG_REQ_URL_MAX);
  printf("HG_REQ_MAX_HEADERS   = %d\n", HG_REQ_MAX_HEADERS);
  printf("sizeof(devices)      = %u\n",
         (unsigned)(HG_DEVICE_COUNT * (sizeof(HgKv))));
  printf("HG_APP_UA len        = %u\n", (unsigned)strlen(HG_APP_UA));

  /* what the device map alone costs, before any extras */
  rc = hg_url_encode_map(NULL, 0, q, sizeof q);
  printf("\nhg_url_encode_map(device only) rc=%d len=%u\n",
         rc, rc >= 0 ? (unsigned)strlen(q) : 0u);

  snprintf(ts, sizeof ts, "%u", khronos);
  snprintf(rt, sizeof rt, "%llu", (unsigned long long)ticket);
  extra[n].key = "query";    extra[n].value = "\xe7\x9f\xad\xe5\x89\xa7"; n++;
  extra[n].key = "ts";       extra[n].value = ts; n++;
  extra[n].key = "_rticket"; extra[n].value = rt; n++;

  rc = hg_url_encode_map(extra, (size_t)n, q, sizeof q);
  printf("hg_url_encode_map(with extras) rc=%d len=%u\n",
         rc, rc >= 0 ? (unsigned)strlen(q) : 0u);
  if (rc >= 0) printf("  query head: %.120s...\n  query tail: ...%.80s\n", q, q + strlen(q) - 80);

  rc = hg_request_search(&req, "\xe7\x9f\xad\xe5\x89\xa7", khronos, ticket, 42131u);
  printf("\nhg_request_search rc=%d\n", rc);

  /* Narrow it down: run build_common's sub-steps by hand with the same inputs.
   * The theory is that x-medusa's length scales with the URL, so a buffer sized
   * from the short test vectors is too small for a real 600-char query. */
  {
    static char url[4096];
    static char medusa[8192];
    char helios[256];
    char gorgon[256];
    int ok;

    snprintf(url, sizeof url,
             "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?%s", q);
    printf("\nurl len = %u\n", (unsigned)strlen(url));

    ok = hg_helios(khronos, diag_rng, NULL, helios, sizeof helios);
    printf("hg_helios            rc=%d len=%u\n", ok, ok ? (unsigned)strlen(helios) : 0u);

    hg_xgorgon(q, NULL, 0, khronos, 42131u, gorgon);
    printf("hg_xgorgon           len=%u\n", (unsigned)strlen(gorgon));

    ok = hg_build_medusa(url, NULL, 0, khronos, diag_rng, NULL, medusa, sizeof medusa);
    printf("hg_build_medusa      rc=%d len=%u   (build_common gives it a 1600-byte buffer)\n",
           ok, ok ? (unsigned)strlen(medusa) : 0u);

    /* does medusa's length actually scale with the URL? compare against a stub */
    ok = hg_build_medusa("https://x/y", NULL, 0, khronos, diag_rng, NULL, medusa, sizeof medusa);
    printf("hg_build_medusa      rc=%d len=%u   (against a 12-char url)\n",
           ok, ok ? (unsigned)strlen(medusa) : 0u);

    /* CONTROL: a vector URL that the signer suite passes. If this succeeds here,
     * the binary and the call shape are fine and something about MY url is the
     * problem; if it also fails, the difference is the call itself. */
    {
      const SignVector *v = &SIGN_VECTORS[0];
      const uint8_t *vb = (const uint8_t *)v->body;
      int mok = hg_build_medusa(v->url, vb, v->body_len, v->ts, diag_rng, NULL,
                                medusa, sizeof medusa);
      printf("\nCONTROL medusa with SIGN_VECTORS[0].url (url len=%u, ts=%u) rc=%d len=%u\n",
             (unsigned)strlen(v->url), v->ts, mok, mok ? (unsigned)strlen(medusa) : 0u);
      printf("  vector url: %.100s\n", v->url);
    }

    /* branch sweep: does medusa fail only on some branches?
     * whenever the URL lands on a hash branch this project refuses. The 12
     * committed vectors were all searched for branch 0, so no test ever saw a
     * failing branch. Sweep the request ticket and correlate. */
    printf("\nticket sweep (branch vs medusa success):\n");
    {
      int t, mt_ok = 0, mt_fail = 0;
      int b0_ok = 0, b0_fail = 0, b1_fail = 0, b2_ok = 0, b2_fail = 0;
      for (t = 0; t < 32; t++) {
        HgKv e[4];
        char tsx[24], rtx[24], qx[4096], ux[4096];
        int nx = 0, mok;
        uint32_t br;
        snprintf(tsx, sizeof tsx, "%u", khronos);
        snprintf(rtx, sizeof rtx, "%u", (unsigned)(khronos * 1000u + (uint32_t)t));
        e[nx].key = "query";    e[nx].value = "\xe7\x9f\xad\xe5\x89\xa7"; nx++;
        e[nx].key = "ts";       e[nx].value = tsx; nx++;
        e[nx].key = "_rticket"; e[nx].value = rtx; nx++;
        if (hg_url_encode_map(e, (size_t)nx, qx, sizeof qx) < 0) continue;
        snprintf(ux, sizeof ux,
                 "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?%s", qx);
        mok = hg_build_medusa(ux, NULL, 0, khronos, diag_rng, NULL, medusa, sizeof medusa);
        br = hg_branch_of(qx, NULL, 0, khronos);
        if (mok) mt_ok++; else mt_fail++;
        if (br == 0) { if (mok) b0_ok++; else b0_fail++; }
        else if (br == 1) { if (!mok) b1_fail++; }
        else if (br == 2) { if (mok) b2_ok++; else b2_fail++; }
        if (t < 8) printf("  ticket+%d branch=%u medusa=%s\n", t, br, mok ? "ok" : "FAIL");
      }
      printf("  totals: medusa ok=%d fail=%d | branch0 ok=%d fail=%d | branch1 fail=%d | branch2 ok=%d fail=%d\n",
             mt_ok, mt_fail, b0_ok, b0_fail, b1_fail, b2_ok, b2_fail);
    }
  }

  if (rc == 0) {
    printf("  url len = %u\n", (unsigned)strlen(req.url));
    for (i = 0; req.headers[i] != NULL; i++) {
      const char *h = req.headers[i];
      const char *colon = strchr(h, ':');
      printf("  header %2d: %.*s  (%u chars)\n", i,
             colon ? (int)(colon - h) : (int)strlen(h), h,
             (unsigned)strlen(colon ? colon + 2 : ""));
    }
    printf("  store used = %u of %d\n",
           (unsigned)(req.headers[i - 1] - req.store + strlen(req.headers[i - 1]) + 1),
           HG_REQ_STORE);
  }
  return 0;
}
