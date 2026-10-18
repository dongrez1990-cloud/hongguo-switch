/*
 * The last unverified assumption, tested directly on the console.
 *
 * Everything else in this port has been measured. One thing has not: whether mpv
 * actually honours `--demuxer-lavf-o=decryption_key=<hex>`. The option string was
 * found inside the prebuilt libavformat.a, which proves ffmpeg HAS the code path,
 * not that mpv passes it through, nor that it decrypts this stream.
 *
 * That single fact decides the architecture:
 *   - if mpv decrypts, playback is a few lines of glue and hg_cenc is redundant
 *   - if it does not, playback needs a decrypting stream reader built on hg_cenc,
 *     which is a completely different and much larger piece of work
 *
 * So this runs an A/B on the SAME url:
 *   A. with the derived CENC key
 *   B. without it
 * and reports what mpv says each time. If A reaches video parameters and B does
 * not, the key is doing the work. If neither does, the option is not being
 * honoured and the architecture has to change. If BOTH do, then the stream was
 * never encrypted in the range mpv touched, and that is worth knowing too.
 *
 * vo=null / ao=null so it decodes without needing a real output.
 *
 * Build: scripts/build-switch-test.ps1 ... -WithMpv
 */

#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <mpv/client.h>

#include "hg_http.h"
#include "hg_router.h"
#include "hg_api.h"
#include "hg_play.h"
#include "hg_request.h"

static char g_page[1024 * 1024];      /* detail pages measured at 530 KB */
static char g_router[512 * 1024];
static HgRequest g_req;
static HgPlayPlan g_plan;

/* Flush after every line.
 *
 * The first version of this program crashed the console with 2168-0002 and the
 * screen gave no clue how far it had got, because console output was still
 * buffered. A staged test whose progress is invisible is not a staged test. */
static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  consoleUpdate(NULL);
}

/* Bring up the console services the way borealis does for Switch.
 *
 * The first version called socketInitializeDefault(), which leaves
 * num_bsd_sessions at its default of 3. borealis raises it to 12 for a normal
 * applet precisely because a media player opens several sockets at once, and
 * running out of BSD sessions is a plausible cause of the 2168-0002 abort.
 * nifmInitialize is also required for network access and was missing entirely. */
static void init_services(void) {
  SocketInitConfig cfg = *(socketGetDefaultInitConfig());
  AppletType at = appletGetAppletType();
  if (at == AppletType_Application || at == AppletType_SystemApplication) {
    cfg.num_bsd_sessions = 12;
    cfg.sb_efficiency = 8;
  } else {
    cfg.num_bsd_sessions = 2;
    cfg.sb_efficiency = 1;
  }
  say("applet type    : %d\n", (int)at);
  say("socketInitialize(bsd_sessions=%u, sb_efficiency=%u) rc=%d\n",
      (unsigned)cfg.num_bsd_sessions, (unsigned)cfg.sb_efficiency, socketInitialize(&cfg));
  say("nifmInitialize  : %d\n", nifmInitialize(NifmServiceType_User));
  say("setsysInitialize: %d\n", setsysInitialize());
  say("plInitialize    : %d\n", plInitialize(PlServiceType_User));
}

static const char *BROWSER_UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

static int http_get(const char *url, char *out, size_t cap, size_t *len) {
  const char *h[4];
  char ua[256];
  long status = 0;
  int rc;
  snprintf(ua, sizeof ua, "User-Agent: %s", BROWSER_UA);
  h[0] = ua;
  h[1] = "Accept: text/html,application/xhtml+xml";
  h[2] = "Accept-Language: zh-CN,zh;q=0.9";
  h[3] = NULL;
  rc = hg_http_request(url, h, NULL, 0, out, cap, len, &status);
  if (rc != HG_HTTP_OK) {
    say("  GET %s -> transport rc=%d (%s)\n", url, rc, hg_http_last_error());
    return 0;
  }
  return 1;
}

/* Decode without any output device: this is a decoder test, not a render test. */
static void run_mpv(const char *label, const char *url, const char *decryption_key, int verbose_log) {
  mpv_handle *m;
  const char *cmd[3];
  int i;
  int64_t w = 0, h = 0;
  double dur = 0.0;
  char fmt[64];
  char hwdec[64];
  int saw_error = 0;

  say("\n--- %s ---\n", label);
  m = mpv_create();
  if (m == NULL) { printf("  mpv_create FAILED\n"); return; }

  mpv_set_option_string(m, "vo", "null");
  mpv_set_option_string(m, "ao", "null");
  mpv_set_option_string(m, "terminal", "no");
  mpv_set_option_string(m, "msg-level", verbose_log ? "all=warn" : "all=no");
  mpv_set_option_string(m, "network-timeout", "20");
  mpv_set_option_string(m, "config", "no");
  mpv_set_option_string(m, "load-scripts", "no");
  if (g_plan.header_fields[0] != '\0')
    mpv_set_option_string(m, "http-header-fields", g_plan.header_fields);
  if (decryption_key != NULL && decryption_key[0] != '\0') {
    say("  demuxer-lavf-o : %s\n", decryption_key);
    mpv_set_option_string(m, "demuxer-lavf-o", decryption_key);
  } else {
    say("  demuxer-lavf-o : (not set)\n");
  }

  if (mpv_initialize(m) < 0) {
    say("  mpv_initialize FAILED (this build may require a real vo)\n");
    mpv_terminate_destroy(m);
    return;
  }
  mpv_request_log_messages(m, verbose_log ? "warn" : "no");

  cmd[0] = "loadfile"; cmd[1] = url; cmd[2] = NULL;
  say("  loadfile rc    : %d\n", mpv_command(m, cmd));

  /* Bounded poll. Waiting for width alone proves nothing: width comes from the
   * CONTAINER HEADER, which is not encrypted, so an all-garbage sample stream
   * still reports it. Wait for the playback clock to move instead -- if the
   * samples are encrypted and the key was not applied, the decoder produces no
   * frames and time-pos never advances. */
  for (i = 0; i < 28; i++) {
    mpv_event *ev = mpv_wait_event(m, 0.25);
    if (ev->event_id == MPV_EVENT_NONE) {
      double tp = 0.0;
      if (mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &tp) >= 0 && tp > 0.05) break;
      continue;
    }
    if (ev->event_id == MPV_EVENT_LOG_MESSAGE && !saw_error) {
      /* Only the FIRST message. Requesting the log stream and printing all of it
       * produced hundreds of "log message buffer overflow" lines that scrolled
       * the first attempt's result off the top of the console -- the most
       * important measurement in the project hidden by its own diagnostic. */
      mpv_event_log_message *lm = (mpv_event_log_message *)ev->data;
      say("    first log: [%s] %.140s\n",
          lm->prefix ? lm->prefix : "?", lm->text ? lm->text : "");
      saw_error = 1;
    } else if (ev->event_id == MPV_EVENT_END_FILE) {
      mpv_event_end_file *ef = (mpv_event_end_file *)ev->data;
      say("  end-file       : reason=%d error=%d\n", ef->reason, ef->error);
      break;
    }
  }

  fmt[0] = '\0';
  hwdec[0] = '\0';
  if (mpv_get_property(m, "width", MPV_FORMAT_INT64, &w) < 0) w = 0;
  if (mpv_get_property(m, "height", MPV_FORMAT_INT64, &h) < 0) h = 0;
  if (mpv_get_property(m, "duration", MPV_FORMAT_DOUBLE, &dur) < 0) dur = 0.0;
  mpv_get_property(m, "video-format", MPV_FORMAT_STRING, &fmt);
  mpv_get_property(m, "hwdec-current", MPV_FORMAT_STRING, &hwdec);

  /* THE REAL SIGNAL. A decoded frame exists only when the samples were usable;
   * header-derived values exist either way. */
  {
    double tp = -1.0;
    char *pt = NULL;
    char ptype[24];
    ptype[0] = '\0';
    if (mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &tp) < 0) tp = -1.0;
    if (mpv_get_property(m, "video-frame-info/picture-type", MPV_FORMAT_STRING, &pt) >= 0 &&
        pt != NULL)
      snprintf(ptype, sizeof ptype, "%.12s", pt);

    say("  >>> header : width=%lld height=%lld duration=%.2f format=%s\n",
        (long long)w, (long long)h, dur, fmt[0] ? fmt : "(none)");
    say("  >>> FRAMES : time-pos=%.2f picture-type=%s  hwdec=%s\n",
        tp, ptype[0] ? ptype : "(none)", hwdec[0] ? hwdec : "(none)");
    say("  >>> %s\n", tp > 0.05
        ? "FRAMES DECODED -- the samples were readable"
        : "NO FRAMES -- decoder produced nothing (header only)");
  }

  mpv_terminate_destroy(m);
}

/* Verify the extracted blob really is a balanced JSON object, and say exactly
 * where it stops being one.
 *
 * The earlier self-test reported "balanced: yes (ends with '}')" -- which only
 * checked the LAST character. hg_router_extract guarantees that by construction,
 * so the check proved nothing. If the blob is in fact unbalanced, the parser
 * would return HG_API_ERR_JSON and the report would be "islands: yes" with no hint
 * that the extractor, not the parser, was at fault.
 *
 * The blob is also written to the SD card so it can be compared byte-for-byte
 * against what the same page yields elsewhere. */
static int verify_balance(const char *b, size_t len, size_t *fail_at) {
  size_t i;
  int depth = 0, q = 0, esc = 0;
  for (i = 0; i < len; i++) {
    char c = b[i];
    if (q) {
      if (esc) esc = 0;
      else if (c == '\\') esc = 1;
      else if (c == '"') q = 0;
      continue;
    }
    if (c == '"') q = 1;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      depth--;
      if (depth == 0) { *fail_at = i + 1; return (i + 1 == len); }
      if (depth < 0) { *fail_at = i; return 0; }
    }
  }
  *fail_at = i;
  return 0;
}

static void dump_blob(const char *b, size_t len) {
  static char page[65536];
  FILE *f = fopen("sdmc:/hongguo-router-dump.bin", "wb");
  if (f == NULL) { say("    could not open sdmc:/hongguo-router-dump.bin\n"); return; }
  fwrite(b, 1, len, f);
  fclose(f);
  say("    wrote sdmc:/hongguo-router-dump.bin (%u bytes)\n", (unsigned)len);
  (void)page;
}

/* Does the extracted object close where the file ends? */
static void report_balance(const char *b, size_t len) {
  size_t at = 0;
  int ok = verify_balance(b, len, &at);
  say("    balance      : %s (closes at %u, blob is %u)\n",
      ok ? "OK" : "BROKEN", (unsigned)at, (unsigned)len);
  if (!ok && at < len) {
    size_t s = at > 24 ? at - 24 : 0;
    size_t n = (len - s) < 48 ? (len - s) : 48;
    size_t i;
    say("    context hex  :");
    for (i = 0; i < n; i++) say("%02x", (unsigned char)b[s + i]);
    say("\n    depth here   : escaped=%d\n", 0);
  }
}

/* Walk the exact path find_page walks, one public call at a time.
 *
 * The same data and the same C succeed on i386/TinyCC and fail on aarch64/GCC -O2,
 * so this is a compiler- or architecture-dependent difference and guessing which
 * one is pointless. Each step reports its result; the first divergence localises
 * it. Byte values are printed because this console font has no CJK glyphs. */
static void probe_path(const char *b, size_t len) {
  size_t ld = 0, page = 0, v = 0, elem = 0;
  int rc;

  rc = hg_json_object_get(b, len, 0, "loaderData", &ld);
  say("    [a] loaderData : rc=%d off=%u\n", rc, (unsigned)ld);

  if (rc) {
    rc = hg_json_object_get(b, len, ld, "page", &page);
    say("    [b] page       : rc=%d off=%u\n", rc, (unsigned)page);

    if (rc) {
      rc = hg_json_object_get(b, len, page, "bannerList", &v);
      say("    [c] bannerList : rc=%d off=%u open='%02x'\n",
          rc, (unsigned)v, (unsigned)((unsigned char)b[v]));

      if (rc && b[v] == '[') {
        rc = hg_json_array_first(b, len, v, &elem);
        say("    [d] first elem : rc=%d off=%u\n", rc, (unsigned)elem);
        if (rc) {
          size_t idv = 0;
          char out[64];
          int n = hg_json_object_get(b, len, elem, "series_id", &idv);
          say("    [e] series_id  : rc=%d\n", n);
          if (n) {
            n = hg_json_string(b, len, idv, out, sizeof out);
            say("        value len  : %d  (expect 19)\n", n);
          }
        }
      }
    }
  }

  {
    size_t pg = 0;
    rc = hg_api_find_page(b, len, &pg, NULL, 0);
    say("    [f] find_page  : rc=%d off=%u\n", rc, (unsigned)pg);
  }

  {
    static HgSeries small[8];
    static HgSeries big[128];
    size_t got = 0;
    int r1 = hg_api_parse_home(b, len, small, 8, &got);
    say("    [g] parse cap=8   : rc=%d count=%u\n", r1, (unsigned)got);
    got = 0;
    r1 = hg_api_parse_home(b, len, big, 128, &got);
    say("    [h] parse cap=128 : rc=%d count=%u\n", r1, (unsigned)got);
    say("        (16=empty,12=router,11=small,9=json  -- see hg_api.h)\n");
  }
}

/* Does mpv come up AT ALL, with no network and no URL?
 *
 * The crash this program caused could have been mpv's memory footprint, the
 * socket configuration, or the chain before it. Testing mpv alone, first,
 * separates those: if this stage crashes there is no point looking at the rest. */
static void mpv_selftest(void) {
  mpv_handle *m;
  const char *ver = NULL;

  say("\n[0] mpv alone -- create, initialize, read the version\n");
  m = mpv_create();
  if (m == NULL) { say("    mpv_create FAILED\n"); return; }
  say("    mpv_create       : ok\n");

  /* Keep mpv away from anything on the SD card and from its Lua scripts. */
  mpv_set_option_string(m, "config", "no");
  mpv_set_option_string(m, "load-scripts", "no");
  mpv_set_option_string(m, "terminal", "no");
  mpv_set_option_string(m, "vo", "null");
  mpv_set_option_string(m, "ao", "null");

  {
    int rc = mpv_initialize(m);
    say("    mpv_initialize   : %d\n", rc);
    if (rc < 0) { mpv_terminate_destroy(m); return; }
  }
  mpv_get_property(m, "mpv-version", MPV_FORMAT_STRING, &ver);
  say("    mpv-version      : %s\n", ver ? ver : "(null)");
  {
    char *ff = NULL;
    mpv_get_property(m, "ffmpeg-version", MPV_FORMAT_STRING, &ff);
    say("    ffmpeg-version   : %.80s\n", ff ? ff : "(null)");
  }
  mpv_terminate_destroy(m);
  say("    destroyed        : ok -- mpv itself is not the problem\n");
}

int main(int argc, char **argv) {
  PadState pad;
  size_t page_len = 0, router_len = 0;
  char url[512];
  int n_series = 0, n_eps = 0, rc;
  const char *series_id = NULL;
  const char *vid = NULL;

  (void)argc; (void)argv;
  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  say("hongguo playback test -- does mpv honour decryption_key?\n");
  say("=======================================================\n\n");

  /* borealis calls this on Switch; it stops HOME from tearing the applet down
   * mid-initialisation, which is its own crash source. */
  appletLockExit();
  init_services();
  hg_http_global_init();

  mpv_selftest();

  /* 1. home page -> a series id */
  say("\n[1] home page\n");
  if (!http_get("https://hongguoduanju.com/", g_page, sizeof g_page, &page_len)) goto done;
  router_len = (size_t)hg_router_extract(g_page, page_len, g_router, sizeof g_router);
  say("  page %u b -> router rc=%d\n", (unsigned)page_len, (int)router_len);
  if ((int)router_len < 0) goto done;
  report_balance(g_router, router_len);
  dump_blob(g_router, router_len);
  probe_path(g_router, router_len);
  {
    /* The real home page carries 36 cards. A cap smaller than the page is
     * HG_API_ERR_SMALL and yields NOTHING -- there is deliberately no "first N of
     * M" result -- so this must be at least as large as any page.
     * 64 x sizeof(HgSeries) = 84 KB, which is static by necessity. */
    static HgSeries series[HG_API_MAX_SERIES];
    size_t got = 0;
    n_series = hg_api_parse_home(g_router, router_len, series, HG_API_MAX_SERIES, &got);
    say("  parse_home rc  : %d  count=%u\n", n_series, (unsigned)got);
    if (n_series > 0) {
      series_id = series[0].id;
      say("  first series id: %s\n", series_id);
      say("  its title len  : %u  desc len=%u\n",
          (unsigned)strlen(series[0].title), (unsigned)strlen(series[0].desc));
    }
  }
  if (series_id == NULL) goto done;

  /* 2. detail page -> a vid */
  say("\n[2] detail page\n");
  snprintf(url, sizeof url, "https://hongguoduanju.com/detail?series_id=%s", series_id);
  if (!http_get(url, g_page, sizeof g_page, &page_len)) goto done;
  router_len = (size_t)hg_router_extract(g_page, page_len, g_router, sizeof g_router);
  say("  page %u b -> router rc=%d\n", (unsigned)page_len, (int)router_len);
  if ((int)router_len < 0) goto done;
  {
    static HgEpisode eps[HG_API_MAX_EPISODES];
    size_t got = 0;
    n_eps = hg_api_parse_episode_ids(g_router, router_len, eps, HG_API_MAX_EPISODES, &got);
    say("  parse_episodes : %d\n", n_eps);
    if (n_eps > 0) { vid = eps[0].id; printf("  first vid      : %s\n", vid); }
  }
  if (vid == NULL) goto done;

  /* 3. signed video model -> the play plan */
  say("\n[3] signed video model request\n");
  {
    uint32_t khronos = 1791466175u;   /* the console clock is sane; see net-test */
    uint64_t ticket = (uint64_t)khronos * 1000ull;
    size_t blen = 0;
    long status = 0;
    rc = hg_request_video_model(&g_req, vid, khronos, ticket, 1u);
    say("  build request  : rc=%d url=%u b body=%u b\n",
           rc, (unsigned)strlen(g_req.url), (unsigned)g_req.body_len);
    if (rc != 0) goto done;
    rc = hg_http_request(g_req.url, g_req.headers, g_req.body, g_req.body_len,
                         g_page, sizeof g_page, &blen, &status);
    say("  POST           : rc=%d status=%ld body=%u b\n",
           rc, status, (unsigned)blen);
    if (rc != HG_HTTP_OK || blen == 0) {
      say("  curl says      : %s\n", hg_http_last_error());
      goto done;
    }
    rc = hg_play_plan_from_model(g_page, blen, &g_plan);
    say("  play plan      : rc=%d\n", rc);
    if (rc != 0) goto done;
    say("  codec          : %s (%s)\n", g_plan.codec_type[0] ? g_plan.codec_type : "(absent)",
           g_plan.has_codec_type ? "reported" : "not reported");
    say("  definition     : %s %dx%d bitrate=%d\n", g_plan.definition,
           g_plan.vwidth, g_plan.vheight, g_plan.bitrate);
    say("  encrypted      : %d  kid=%s method=%s\n",
           g_plan.encrypted, g_plan.kid, g_plan.encryption_method);
    say("  url length     : %u\n", (unsigned)strlen(g_plan.url));
    say("  url head       : %.90s...\n", g_plan.url);
  }

  /* 4. the A/B */
  say("\n[4] mpv A/B on the SAME url\n");
  if (!g_plan.encrypted) {
    say("  the plan carries no key, so the A/B cannot distinguish anything.\n");
    say("  running the no-key case only.\n");
  }
  run_mpv("A. WITH the derived CENC key", g_plan.url,
          g_plan.encrypted ? g_plan.decryption_key : NULL, 0);
  run_mpv("B. WITHOUT any key", g_plan.url, NULL, 0);
  /* The control that actually answers the question. If the option reaches ffmpeg
   * and the stream is encrypted, a wrong key cannot decode: the ciphertext stays
   * ciphertext and no frame appears. If A and B behave identically instead, the
   * key is not being applied at all -- which the previous run could not tell,
   * because it judged success by header values that are readable either way. */
  run_mpv("C. WITH a deliberately WRONG key", g_plan.url,
          "decryption_key=00000000000000000000000000000000", 0);

  say("\n=== interpreting this ===\n");
  say("  A frames, B and C none -> decryption_key IS applied; playback is glue\n");
  say("  A, B and C identical  -> the option is NOT reaching ffmpeg; hg_cenc needed\n");
  say("  all three decode frames -> the samples were never encrypted after all\n");

done:
  say("\nPress + to exit.\n");
  consoleUpdate(NULL);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  /* appletLockExit() was called during init, following borealis. borealis pairs
   * it with this in userAppExit; leaving it locked and then returning from main
   * makes the exit itself fault, which is the error seen when pressing +. */
  appletUnlockExit();
  socketExit();
  return 0;
}
