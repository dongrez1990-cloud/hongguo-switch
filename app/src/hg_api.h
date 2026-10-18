/*
 * Response parsing for the signed API and the scraped detail page.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV
 * kotlin-tv/core/src/main/kotlin/com/hongguotv/core/ContentRepository.kt
 * (GPL-3.0), derived from woshishiq1/drpys @22261ad (GPL-3.0). Combined work:
 * GPL-3.0.
 *
 * This layer turns three upstream shapes into plain fixed-size structs. It is the
 * counterpart to hg_request.c: that one builds what goes out, this one reads what
 * comes back. Nothing here allocates and nothing here performs I/O.
 *
 * The three shapes, and why each is treated the way it is:
 *
 *   1. SEARCH. Response is JSON; hits live at data.video_data[]. A hit may omit
 *      series_id and carry book_id / search_result_id instead, and its title may
 *      live only in search_high_light.title.text (the search endpoint returns the
 *      matched fragment highlighted, and for some rows that is the ONLY spelling
 *      of the title present). Both fallbacks are real upstream behaviour, so both
 *      are implemented rather than treated as malformed.
 *
 *   2. DETAIL. Not JSON: an HTML page with a _ROUTER_DATA blob. The episode list
 *      is seriesDetail.vid_list[], an array of BARE episode-id STRINGS, not
 *      objects. One bad element invalidates the WHOLE list (the reference throws
 *      an episode-data error); it does not skip the bad row. That is the
 *      fail-closed rule this file follows: a caller that gets
 *      HG_API_ERR_EPISODE knows the list is wrong, and cannot mistake a
 *      truncated list for a short series.
 *
 *   3. VIDEO MODEL. video_list[] carries the playable qualities. bytevc2/bvc2
 *      entries are EXCLUDED, not accepted-and-flagged: the target player cannot
 *      decode them, and the reference filters them for the same reason.
 *
 * REFUSE, DO NOT GUESS. Every entry point returns a distinct negative
 * HG_API_ERR_* rather than a partial result. When a caller passes a buffer with
 * room for N rows and the response holds N+1, the result is HG_API_ERR_SMALL and
 * *count is left at 0 -- there is no "first N of M" outcome, because the caller
 * cannot tell that from a genuinely short page.
 */

#ifndef HG_API_H
#define HG_API_H

#include <stddef.h>

/* Error codes (all negative). Anything below HG_API_ERR_BASE is reserved. */
#define HG_API_ERR_BASE      (-100)
#define HG_API_ERR_JSON      (-1)   /* top-level shape is not the expected object */
#define HG_API_ERR_MISSING   (-2)   /* a required key / array is absent */
#define HG_API_ERR_SMALL     (-3)   /* caller's array or a string buffer too small */
#define HG_API_ERR_EPISODE   (-4)   /* a vid_list entry is not [0-9]{1,30} */
#define HG_API_ERR_DUPLICATE (-5)   /* the same series / episode id appears twice */
#define HG_API_ERR_EMPTY     (-6)   /* structurally valid but nothing usable in it */
#define HG_API_ERR_TOO_LONG  (-7)   /* an item id itself exceeds HG_API_ID_MAX */
#define HG_API_ERR_ROUTER    (-8)   /* no loaderData, or no content object under it */

/* One id is [0-9]{1,30} plus a NUL. The bound is the reference's own regex, so a
 * longer id is rejected by the validator rather than silently collected. */
#define HG_API_ID_MAX     31

/* Bounded so that a caller can place an HgSeriesPage at file scope without a
 * megabyte of .bss; the search endpoint pages at 20 and nothing upstream
 * approaches this. */
#define HG_API_MAX_SERIES 64
#define HG_API_MAX_EPISODES 512
#define HG_API_MAX_SOURCES 16

typedef struct {
  char id[32];
  char title[192];
  char cover[512];
  char desc[512];
  /* The count badge, from episode_right_text or derived from episode_cnt. The literal is
   * upstream Chinese, of the form "<all><N><episodes>" or "<N><episodes>" -- it is DATA and is
   * rendered as-is, but this file must stay ASCII for the GBK build machine, so it is described
   * rather than quoted. Measured: present on 36/36 home cards. */
  char badge[64];
  /* Genre tags, joined by '/' -- three two-character genre names in the observed captures, also
   * upstream Chinese DATA (see the note on `badge` about why it is not quoted here).
   *
   * MEASURED, and the reason this field exists at all: category_list is present
   * on 36/36 home cards, while series_intro (which fills `desc`) was present on
   * 0/36 in one live capture and 2 cards with >500 bytes in another. So a card
   * UI that wants to show something descriptive next to the title cannot rely on
   * the synopsis; the tags are the part that is always there.
   *
   * Bounded so an upstream card with many categories cannot overflow: the join is
   * truncated at the last tag that fits, never mid-tag. */
  char tags[128];
} HgSeries;

typedef struct {
  char id[32];
} HgEpisode;

typedef struct {
  char url[2048];
  char key_seed[256];
  char spade_a[1024];
  char codec_type[32];

  /* ---- the quality ladder's half of a source -----------------------------
   *
   * These four were being discarded, which made a quality menu impossible: the caller could
   * see that a source EXISTED but not how good it was, so the best it could do was offer "the
   * sources" in whatever order upstream sent them -- and MEASURED, upstream sends them
   * WORST FIRST (quality_type 40 = 360p is entry 0, quality_type 1 = 1080p is entry 4).
   *
   * Where they live, measured against app/src/tests/fixtures/video_model.json:
   *
   *   quality_type  the ENTRY itself, and a NUMBER -- 40/30/20/10/1 for 360p..1080p, with 0
   *                 never sent. NOT a string, and NOT inside video_meta.
   *   definition    video_meta.definition, the display label ("1080p")
   *   vwidth        video_meta.vwidth   (640/854/1024/1280/1920)
   *   vheight       video_meta.vheight  (360/480/576/720/1080)
   *   bitrate       video_meta.bitrate; 0 when absent
   *   size_bytes    video_meta.size;    0 when absent
   *
   * `has_quality_type` exists for the same reason HgPlayPlan has one: 0 means absent, and a
   * caller comparing quality levels needs to be able to tell "absent" from "the tier whose
   * number happens to be 0", even though upstream never sends 0. */
  int quality_type;
  int has_quality_type;
  char definition[16];
  int vwidth;
  int vheight;
  int bitrate;
  long long size_bytes;
} HgVideoSource;

typedef struct {
  HgSeries items[HG_API_MAX_SERIES];
  size_t count;
  int has_more;                   /* 0/1; 0 when the key is absent or false */
} HgSeriesPage;

typedef struct {
  HgEpisode items[HG_API_MAX_EPISODES];
  size_t count;
} HgEpisodeList;

typedef struct {
  HgVideoSource items[HG_API_MAX_SOURCES];
  size_t count;
} HgVideoList;

/* In-place JSON/URL unescaping, mirroring the reference's cleanUrl():
 *   \/      -> /
 *   \u002F  -> /     (hex digits case-insensitive)
 *   \u0026  -> &
 *   &amp;   -> &
 * Applied to a NUL-terminated string in place. Cannot fail: every rule replaces
 * its match with a strictly shorter or equal-length sequence, so the result
 * always fits in the source buffer. Removing the name here rather than at each
 * call site matters because the search and detail paths both feed the result of
 * this into a URL that is later fetched. */
void hg_api_clean_url(char *s);

/* Search response. `json` need not be NUL-terminated; `len` is authoritative.
 * Returns the number of items written (>= 0) or a negative HG_API_ERR_*.
 * On any negative return *count is 0. */
int hg_api_parse_search(const char *json, size_t len,
                        HgSeries *out, size_t cap, size_t *count,
                        int *has_more);

/* Series detail, from the _ROUTER_DATA blob that hg_router_extract() produced.
 * Fills `series` (whose `id` is authoritatively the one passed in) and the
 * episode list. Returns the episode count or a negative HG_API_ERR_*. */
int hg_api_parse_detail(const char *json, size_t len, const char *series_id,
                        HgSeries *series,
                        HgEpisode *out, size_t cap, size_t *count);

/* Video model response. Entries whose codec_type is bytevc2/bvc2 are excluded
 * before filling `out`, so `*count` is the number of USABLE sources. Returns the
 * count or a negative HG_API_ERR_*. */
int hg_api_parse_video_model(const char *json, size_t len,
                             HgVideoSource *out, size_t cap, size_t *count);

/* ---- the server-rendered pages ------------------------------------------ */
/*
 * MEASURED, not inferred. Home / category / search / rank all answer 200 with
 * HTML carrying a `_ROUTER_DATA` object, and the Kotlin port's field names were
 * WRONG in two places. Verified against a live home page:
 *
 *   series_name   NOT title        (no `title` key on a banner or a video at all)
 *   series_cover  NOT cover        (and not cover_url)
 *   series_intro  NOT video_desc
 *   series_id     -- correct in the port already
 *
 * The `loaderData` KEY NAMES ARE DYNAMIC and per-page -- they embed the route:
 *   home     layout,                    page
 *   category category_layout,           category_page
 *   search   search_layout,             search_(keyword)/page
 *   rank     rank_layout,               rank_hot-comic-drama/page
 * so no fixed key can be hardcoded. Everything below therefore locates content by
 * walking the tree, exactly as the Kotlin does after
 * `router(url).getJSONObject("loaderData")`.
 */

/* The content object for one page, found structurally: every object-valued member
 * of loaderData is examined, and the first that yields at least one usable series
 * wins. `*found_key` optionally receives the key it was under, for diagnostics.
 *
 * Returns 1 when a content object was identified, 0 when loaderData holds nothing
 * usable. A key whose name is dynamic is exactly the kind of thing that must be
 * reported back rather than assumed. */
int hg_api_find_page(const char *json, size_t len, size_t *page,
                     char *found_key, size_t key_cap);

/* Home page: bannerList + mBannerList + every homeSections[].video_list[].
 * De-duplicated by series_id, banners first (they are the curated row).
 * `cap` bounds the output; more items than `cap` is HG_API_ERR_SMALL and *count
 * is 0 -- there is no "first N of M" result.
 * Returns the count, or a negative HG_API_ERR_*. */
int hg_api_parse_home(const char *json, size_t len,
                      HgSeries *out, size_t cap, size_t *count);

/* ---- how a paged page says how far it goes ------------------------------------
 *
 * MEASURED on the category page (app/src/tools/probe_category_paging.mjs):
 *
 *   GET /category/comic-drama?page=2   -> 200, a DIFFERENT list (page 1 and page 2 share no first id)
 *   GET /category/comic-drama?page=35  -> 404          (there is a hard end)
 *   loaderData.category_$.pagination   -> { total: 800, pageNum: 1, pageSize: 24, totalPages: 34 }
 *
 * So a screen that wants to keep loading does NOT have to guess when to stop: the page says how many
 * items there are and how many pages that is. `totalPages` is the number the category screen checks
 * before asking for another page, and a page that comes back with no items is the second guard.
 *
 * This lives in C, like every other parse, because an off-by-one here means either a page never loads
 * or the same page loads for ever -- both silent.
 *
 * Returns 1 when a `pagination` object was found, 0 when there is none (pages that do not page). Any
 * individual field that is absent is left at 0. */
typedef struct {
  int page;         /* pageNum */
  int page_size;    /* pageSize */
  int total;        /* total */
  int total_pages;  /* totalPages */
} HgApiPagination;

int hg_api_parse_pagination(const char *json, size_t len, HgApiPagination *out);

/* ---- matching a title against a query ------------------------------------------
 *
 * WHY THIS IS C AND NOT A `strstr` IN THE VIEW. Three decisions are buried in "does this title match":
 *
 *   1. CASE. The catalogue is Chinese with English words in it ("AI", "BOSS", "CEO"), and a user who
 *      types "ai" means "AI". ASCII letters fold; everything else is compared byte for byte, which is
 *      correct for UTF-8 (a Chinese query is a byte substring of a Chinese title).
 *   2. THE QUERY IS TRIMMED OF SPACES. The console's keyboard happily returns "  短剧 " and a search that
 *      silently matches nothing because of a trailing space looks exactly like a broken index.
 *   3. AN EMPTY QUERY MATCHES NOTHING. Not "everything": the caller asks this to filter a list, and
 *      "everything" would make an empty search return the whole catalogue as if it were a result.
 *
 * Returns 1 on a match, 0 otherwise. A null or empty query is 0. */
int hg_title_matches(const char *title, const char *query);

/* ---- the search page ----------------------------------------------------------
 *
 * MEASURED AGAINST THE LIVE PAGE THIS ROUND, not inferred from another page:
 *
 *   GET https://hongguoduanju.com/search/<urlencoded keyword>
 *     -> 200, ~215 KB of HTML, _ROUTER_DATA carrying
 *        loaderData["search_(keyword)/page"] = { req, query, isSuccess, searchList,
 *                                                totalCount, accessibleEpisodeCnt }
 *
 *   searchList[] has 10 items (the page's own first page), and each item is
 *
 *     { doc_type, name, keyword, video_data:{...}, user_info:{}, video_list:[] }
 *
 *   in which THE CARD FIELDS ARE NESTED ONE LEVEL DOWN, under `video_data`:
 *
 *     name    (item) == video_data.series_title   the title
 *     keyword (item) == video_data.series_id      the id
 *     video_data.series_cover / episode_cnt / episode_right_text / category_list -- as on the home page
 *
 * THAT NESTING IS WHY THIS PAGE USED TO LOOK EMPTY. The two shape-based searches in this file look
 * for `series_id` (or series_name/series_cover) ON the item, and the search page puts none of them
 * there: it was reported as "the search page has no content list" while the page carried 108 results.
 * series_from_card now unwraps `video_data` when the card level has no series_id, so this page parses
 * with the same field extraction as every other page.
 *
 * NO PAGINATION IS AVAILABLE, and that is measured rather than assumed: the signed search API is dead
 * (200 with a Content-Length of 0, reproduced with the reference implementation itself), and asking
 * the HTML for the next page returns the first page again (`/search/ai?page=2` came back item-for-item
 * identical; the documented API needs a `search_id`/`passback` cursor which the API no longer serves).
 * The page does know how many results exist in total (`totalCount`, a STRING, e.g. "201" for "ai"), so
 * *total receives it and may be NULL. The caller should say how many it is showing out of how many --
 * silently showing 10 of 201 would be the kind of lie this project keeps designing against.
 *
 * `count`/`total` may be NULL. Returns the number of items written (>= 0), or a negative HG_API_ERR_*:
 * HG_API_ERR_ROUTER when loaderData has no page, HG_API_ERR_MISSING when the page has no searchList,
 * HG_API_ERR_EMPTY when every item was unusable (a search that matched nothing is EXACTLY this case --
 * upstream answers 200 with an empty list, so EMPTY here means "no results", not "broken"). */
int hg_api_parse_search_page(const char *json, size_t len,
                             HgSeries *out, size_t cap, size_t *count, long long *total);

/* ---- the home page WITH its tiers ---------------------------------------------
 *
 * MEASURED, not inferred. Against a real captured home page
 * (app/src/tests/fixtures/home_router_full.json, inspected with
 * scripts/router-shape.mjs) the page is NOT a flat list:
 *
 *   loaderData.page.homeSections   4 entries, each { tab_type, tab_name, video_list }
 *       [0] tab_type="all"    9 cards
 *       [1] tab_type="human"  9 cards
 *       [2] tab_type="comic"  9 cards
 *       [3] tab_type="ai"     9 cards
 *   loaderData.page.bannerList     4 cards
 *   loaderData.page.mBannerList    5 cards
 *
 * 4 x 9 = 36, which is exactly the count hg_api_parse_home() reports. So the
 * "flat 36" was never the data's shape: that function merges the banners and the
 * sections and de-duplicates by series_id (45 items before de-duplication, 9
 * repeats, 36 out), which is also why its order looks arbitrary. The tiers were
 * always there.
 *
 * This variant keeps them, and it PRESERVES cross-section duplicates on purpose:
 * the live page really does open more than one section with the same series, so
 * de-duplicating across sections would silently shorten a section. De-duplication
 * still happens WITHIN a section.
 *
 * Refusals rather than truncation: more than HG_API_MAX_SECTIONS sections, or
 * more than HG_API_MAX_HOME_ITEMS cards, is HG_API_ERR_SMALL with the whole
 * structure zeroed -- a caller that silently received "the first 8 sections"
 * could not tell that from a page that has 8.
 *
 * Returns the number of sections (>= 1) or a negative HG_API_ERR_*. */
#define HG_API_MAX_SECTIONS   8
#define HG_API_MAX_HOME_ITEMS 128

typedef struct {
  char tab_type[32];   /* upstream's slug: all / human / comic / ai; "" if absent */
  char tab_name[64];   /* the display heading, verbatim from upstream */
  size_t first;        /* index of this section's first card in HgHomePage.items */
  size_t count;
} HgHomeSection;

typedef struct {
  HgSeries items[HG_API_MAX_HOME_ITEMS];
  size_t item_count;
  HgHomeSection sections[HG_API_MAX_SECTIONS];
  size_t section_count;
} HgHomePage;

int hg_api_parse_home_sections(const char *json, size_t len, HgHomePage *out);

/* ---- the page flattened into DISPLAY ROWS -------------------------------------
 *
 * A tiered screen needs one header row per section followed by that section's
 * cards. That sounds like view code, and it was going to live there -- but the
 * indexing is the part that can be wrong silently, and view code cannot be
 * tested on the host. This project has already paid for that lesson once: a
 * recycler fed a zero-row list died on the console with no message
 * (RecyclerFrame::selectRowAt reading cacheFramesData[1] on a one-entry vector).
 *
 * So the arithmetic lives here, where the host suite can check it, and the view
 * only has to dispatch on `kind`.
 *
 * Layout for a page with S sections and N cards:  N + S rows,
 *   row 0                      HEADER  section 0
 *   rows 1 .. c0               SERIES  section 0, items 0 .. c0-1
 *   row c0+1                   HEADER  section 1
 *   ...
 *
 * `count` is a convenience output and may be NULL. Returns the row count, or a
 * negative HG_API_ERR_*. More rows than `cap` is HG_API_ERR_SMALL with *count 0
 * -- never a truncated list, because a caller handed "the first 40 of 43 rows"
 * cannot tell that from a page that has 40.
 *
 * A page with no sections produces 0 rows and HG_API_OK, NOT an error: this
 * function describes a page, and saying "no rows" is a description. The caller
 * is responsible for never handing a zero-row list to a RecyclerFrame. */
#define HG_API_ROW_HEADER 0
#define HG_API_ROW_SERIES 1

typedef struct {
  int kind;        /* HG_API_ROW_HEADER | HG_API_ROW_SERIES */
  size_t section;  /* index into HgHomePage.sections */
  size_t item;     /* index into HgHomePage.items; only for HG_API_ROW_SERIES */
} HgHomeRow;

int hg_api_home_rows(const HgHomePage *page, HgHomeRow *out, size_t cap, size_t *count);

/* Episode ids from a detail page's seriesDetail.vid_list[].
 *
 * MEASURED: the entries are BARE STRINGS (~222 of them for a real series), not
 * objects with a `vid` field. Each must be [0-9]{1,30}; one bad entry invalidates
 * the WHOLE list rather than being skipped, because a caller that silently got
 * 221 ids cannot tell that from a 221-episode series.
 * Returns the count, or a negative HG_API_ERR_*. */
int hg_api_parse_episode_ids(const char *json, size_t len,
                             HgEpisode *out, size_t cap, size_t *count);

/* Validators, exposed because the callers need them independently of any parse:
 * a series id arrives from the UI before a request is built, and the episode
 * count of an already-parsed list is checked again before playback. */
int hg_api_valid_id(const char *s);          /* 1 when s is [0-9]{1,30} */
int hg_api_valid_episode_id(const char *s);  /* same rule; separate name for intent */

#endif /* HG_API_H */
