/*
 * Does hg_api_parse_home fail on the HOST with the device's exact blob?
 *
 * The earlier diagnostic walked the JSON reader and find_page but never called
 * parse_home, so the conclusion "i386 passes, aarch64 fails" was never actually
 * tested. If the host fails here too, this is a data/shape problem that can be
 * debugged entirely on the host, and the architecture theory was a phantom.
 */

#include <stdio.h>
#include <string.h>

#include "hg_api.h"
#include "hg_router.h"

static char buf[1 << 20];
static HgSeries items[256];

/* Walk the arrays the way collect_array does, reporting per card. */
static void probe_cards(const char *j, size_t len, size_t page) {
  static const char *LISTS[3] = { "bannerList", "mBannerList", "homeSections" };
  int li;
  for (li = 0; li < 3; li++) {
    size_t arr = 0, elem = 0;
    int idx = 0;
    if (!hg_json_object_get(j, len, page, LISTS[li], &arr)) {
      printf("  %-14s : key absent\n", LISTS[li]);
      continue;
    }
    printf("  %-14s : found\n", LISTS[li]);
    if (!hg_json_array_first(j, len, arr, &elem)) continue;
    for (;;) {
      size_t p = elem;
      while (p < len && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
      if (p < len && j[p] == '{') {
        size_t sub = 0;
        int has_vl = hg_json_object_get(j, len, elem, "video_list", &sub);
        if (idx < 4 || idx > 40) {
          printf("    [%d] off=%u  video_list=%s\n", idx, (unsigned)elem,
                 has_vl ? "PRESENT" : "no");
        }
      }
      idx++;
      if (!hg_json_array_next(j, len, arr, elem, &elem)) break;
    }
    printf("    total %d entries\n", idx);
  }
}

int main(int argc, char **argv) {
  FILE *f;
  size_t len, page = 0, got = 0;
  int rc;

  if (argc < 2) { fprintf(stderr, "usage: diag_home3 <router.bin>\n"); return 2; }
  f = fopen(argv[1], "rb");
  if (f == NULL) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  len = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[len] = '\0';
  printf("blob %u bytes\n", (unsigned)len);

  rc = hg_api_find_page(buf, len, &page, NULL, 0);
  printf("find_page    : rc=%d off=%u\n", rc, (unsigned)page);
  if (rc) probe_cards(buf, len, page);

  got = 0;
  rc = hg_api_parse_home(buf, len, items, 128, &got);
  printf("parse cap=128: rc=%d count=%u\n", rc, (unsigned)got);
  got = 0;
  rc = hg_api_parse_home(buf, len, items, 8, &got);
  printf("parse cap=8  : rc=%d count=%u\n", rc, (unsigned)got);
  if (got > 0) {
    size_t i;
    for (i = 0; i < (got < 5 ? got : 5); i++)
      printf("   [%u] id=%.24s title_len=%u cover_len=%u badge_len=%u desc_len=%u\n",
             (unsigned)i, items[i].id, (unsigned)strlen(items[i].title),
             (unsigned)strlen(items[i].cover), (unsigned)strlen(items[i].badge),
             (unsigned)strlen(items[i].desc));
  }
  return 0;
}
