/*
 * Network self-test, second generation.
 *
 * The first version proved the signed search request works end to end: DNS, TLS,
 * the certificate chain, all four signature headers, and the device fingerprint
 * were accepted -- HTTP 200. It also proved the endpoint then returns
 * Content-Length: 0, which was NOT a client fault: running the reference
 * implementation itself, from another machine, produced the identical empty
 * response. Re-running that test cannot produce new information, so this version
 * tests the paths that actually serve data.
 *
 * What this checks, on the console:
 *
 *   1. HTTPS to hongguoduanju.com -- a DIFFERENT host from the API host, so a
 *      DNS or TLS problem here is its own finding, not an inherited one.
 *   2. hg_router_extract() against a real 170 KB page. The extractor is the only
 *      component that was built against a Kotlin test case rather than against
 *      real upstream output; this is where that gets checked.
 *   3. Flat lookups into the extracted blob for the fields the port depends on,
 *      with the real names (series_name / series_cover / series_intro, not the
 *      inferred title / cover / video_desc).
 *
 * Build: scripts/build-switch-test.ps1 -AppSrc <src> -MainC <this> -OutNro <nro> -WithCurl
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>

#include "hg_http.h"
#include "hg_router.h"

static char g_response[512 * 1024];
static char g_router[512 * 1024];

static const char *BROWSER_UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

int main(int argc, char **argv) {
  PadState pad;
  const char *headers[4];
  size_t body_len = 0;
  long status = 0;
  int rc;

  (void)argc; (void)argv;
  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  printf("hongguo network self-test (2) -- the paths that work\n");
  printf("==================================================\n\n");

  {
    int sockrc = socketInitializeDefault();
    printf("socketInitializeDefault : %d%s\n\n", sockrc, sockrc == 0 ? "  (ok)" : "  (FAILED)");
  }

  headers[0] = "User-Agent: ";   /* placeholders, replaced below */
  headers[1] = NULL;

  {
    char ua[256];
    snprintf(ua, sizeof ua, "User-Agent: %s", BROWSER_UA);
    headers[0] = ua;
    headers[1] = "Accept: text/html,application/xhtml+xml";
    headers[2] = "Accept-Language: zh-CN,zh;q=0.9";
    headers[3] = NULL;
  }

  printf("GET https://hongguoduanju.com/  (the live entry point)\n");
  hg_http_global_init();
  rc = hg_http_request("https://hongguoduanju.com/", headers, NULL, 0,
                       g_response, sizeof g_response, &body_len, &status);
  printf("  transport rc : %d   %s\n", rc,
         rc == HG_HTTP_OK ? "OK" :
         rc == HG_HTTP_ERR_TOO_BIG ? "(response too large -- refused)" : "(network error)");
  if (rc != HG_HTTP_OK) {
    printf("  curl says    : CURLcode %d  %s\n", hg_http_last_curl_code(), hg_http_last_error());
    goto done;
  }
  printf("  http status  : %ld\n", status);
  printf("  body length  : %u\n", (unsigned)body_len);
  if (body_len == 0) {
    printf("\n  The page returned nothing. Unlike the API host this one is\n");
    printf("  expected to answer, so this IS a finding.\n");
    goto done;
  }

  /* The extractor, against real upstream output for the first time. */
  printf("\n_ROUTER_DATA extraction (hg_router_extract, real page)\n");
  {
    int n = hg_router_extract(g_response, body_len, g_router, sizeof g_router);
    if (n < 0) {
      printf("  FAILED: rc=%d  (NO_DATA=%d FORMAT=%d INCOMPLETE=%d SMALL=%d)\n",
             n, HG_ROUTER_ERR_NO_DATA, HG_ROUTER_ERR_FORMAT,
             HG_ROUTER_ERR_INCOMPLETE, HG_ROUTER_ERR_SMALL);
      printf("  The blob is 40 KB and the page is 170 KB, so a SMALL result would\n");
      printf("  mean the extractor over-ran the object rather than that the page changed.\n");
    } else {
      printf("  extracted    : %d bytes\n", n);
      printf("  balanced     : %s\n",
             g_router[n - 1] == '}' ? "yes (ends with '}')" : "NO -- truncated");
    }

    if (n > 0) {
      /* Real field names, measured against the live page. */
      static const char *FIELDS[] = {
        "series_id", "series_name", "series_cover", "series_intro",
        "homeSections", "video_list", "tab_name", NULL
      };
      int i;
      char value[256];
      printf("\n  flat lookups into the blob:\n");
      for (i = 0; FIELDS[i] != NULL; i++) {
        int got = hg_json_find_string(g_router, FIELDS[i], value, sizeof value);
        if (got >= 0) {
          /* Print a bounded, ASCII-safe rendering: the console font has no CJK. */
          char safe[80];
          int j, k = 0;
          for (j = 0; j < got && k < 60; j++) {
            unsigned char c = (unsigned char)value[j];
            if (c >= 0x20 && c < 0x7f) safe[k++] = (char)c;
            else { safe[k++] = '.'; if (k < 58) safe[k++] = '.'; else break; }
          }
          safe[k] = '\0';
          printf("    %-16s found (%d chars)  \"%s\"\n", FIELDS[i], got, safe);
        } else {
          printf("    %-16s NOT FOUND (rc=%d)\n", FIELDS[i], got);
        }
      }
      printf("\n  (non-ASCII shows as '.' because the console font has no CJK glyphs;\n");
      printf("   the byte count is what matters.)\n");
    }
  }

  printf("\n--- response headers (first 900 chars) ---\n%.900s\n", hg_http_last_headers());

done:
  printf("\nPress + to exit.\n");
  consoleUpdate(NULL);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  socketExit();
  return 0;
}
