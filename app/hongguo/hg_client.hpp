/*
 * hg_client: the bridge between the verified C data layer and the C++ UI.
 *
 * GPL-3.0-only.
 *
 * WHY THIS FILE EXISTS. app/play-test/main.c already proves the whole chain on
 * hardware: home page -> 36 cards, detail page -> 76 episode ids, signed video
 * model -> a play plan with a CENC key, and mpv decoding frames with it. That
 * logic is MEASURED. The one thing it is not is reusable: it is a linear
 * printf-driven test whose every step is interleaved with console output.
 *
 * So this file re-runs exactly the same calls in exactly the same order, but
 * returns C++ objects instead of printing. It deliberately does NOT re-derive
 * anything: the URLs, the header set, the browser UA, the parse functions and
 * the plan builder are all the ones the test used. Where a number appears below
 * it was measured, not guessed.
 *
 * WHAT IS AND IS NOT VERIFIED ABOUT THIS FILE:
 *   - the C calls it makes: verified on hardware (see the doc header of
 *     the handover document, "the whole chain has run on hardware")
 *   - this file itself: compiled, but its behaviour on the console is NOT
 *     verified by me -- the console screen cannot be seen from here.
 *
 * THREADING. borealis runs the main loop on the main thread, so the network
 * work here BLOCKS the UI while it runs. Every call is made from a worker
 * thread by the activities; these functions are stateless apart from the shared
 * buffers below, which is why they must not be called concurrently.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include "hg_http.h"
#include "hg_httpcfg.h"
#include "hg_router.h"
#include "hg_api.h"
#include "hg_play.h"
#include "hg_quality.h"
#include "hg_request.h"
}

namespace hg {

/* One home card. Mirrors the fields hg_api_parse_home actually fills; `cover`
 * is the raw series_cover/background_cover URL and is often empty for
 * mBannerList entries, which carry only background_cover. */
struct Series {
    std::string id;
    std::string title;
    std::string cover;
    std::string desc;
    /* The "all N episodes" badge. The literal is upstream Chinese, of the form
     * "<all><N><episodes>" -- it is DATA and is rendered as-is, but this file must stay
     * ASCII for the GBK build machine, so it is described rather than quoted.
     * Measured present on 36/36 home cards. */
    std::string badge;
    /* Genre tags joined by '/', e.g. three two-character genre names separated by
     * slashes (also upstream Chinese DATA -- see the note on `badge`). Measured on
     * 36/36 home cards
     * while `desc` was present on 0/36 in the same capture -- so a card that wants
     * something descriptive under the title uses this, and falls back to `desc`
     * only when it happens to be there. */
    std::string tags;
};

/* One episode. The detail page yields BARE id strings (measured: 76 for a real
 * series), so there is no per-episode title upstream and none is invented here;
 * the UI labels them by position. */
struct Episode {
    std::string id;
};

/* One DISPLAY ROW of the home page.
 *
 * MEASURED, and the reason this exists: the home page is not a flat list. Its
 * _ROUTER_DATA carries homeSections[] -- 4 named tiers of 9 cards each (4 x 9 =
 * 36, which is the count fetchHome() reports), with more than one tier opening
 * on the same series. fetchHome() merges the banners in and de-duplicates by
 * series_id, which flattens the tiers into 36 unordered cards.
 *
 * The row arithmetic -- one header row per tier followed by that tier's cards --
 * lives in C (hg_api_home_rows) rather than here, because it is the part that can
 * be wrong SILENTLY and C code can be tested on the host. This struct is only the
 * transport: a header row carries a title, a series row carries a card. */
struct HomeRow {
    bool header = false;
    /* A "MORE" CARD, which fills the last slot of a full row instead of letting one extra card spill onto
     * a second row of its own.
     *
     * MEASURED: each tier has about nine cards and the grid draws eight per row, so the ninth sat alone on
     * a row -- a whole row of screen for one poster. The user's suggestion, and it is the right one: put
     * "more" in the eighth slot, so every tier ends exactly on a row boundary and the screen is denser.
     *
     * `sectionTitle` carries the tier this card belongs to, which is what a "more" view would need. */
    bool more = false;
    std::string sectionTitle;   /* header rows: the tier's heading; more rows: which tier */
    /* THE TIER'S UPSTREAM SLUG (all / human / comic / ai), on header rows and on the "more" row of that
     * tier. The "more" card needs it because it now opens a CATEGORY page, and the display heading is
     * not a URL -- see Client::categorySlugForTier, which maps it to the page the site actually
     * serves (measured by id overlap, not guessed from the names). */
    std::string tabType;
    Series series;              /* series rows only */
};

/* The outcome of one blocking operation, with the upstream detail preserved.
 * The reference behavior for every failure in this project is "refuse, do not
 * guess", so a failure carries a human-readable reason rather than a silent
 * empty list. */
struct Result {
    bool ok = false;
    std::string error;   /* empty when ok */

    /* ---- WAS THIS THE SERVICE SAYING NO, OR THE NETWORK FAILING? (build175) ----------------------
     *
     * The two need different words in front of a user. "The server refused this episode" is actionable
     * and true; showing a network-failure phrasing for a refusal (or the reverse) sends the viewer to
     * check their connection over something no connection can fix.
     *
     * It is a FLAG ON THE RESULT rather than a substring test at the call site because the string is
     * translated for display while this stays machine-readable -- and a UI that grepped a localised
     * sentence to decide what to say next would break the moment the sentence changed.
     *
     * MEASURED BASIS: the endpoint answers a refused episode with HTTP 403 and
     * {"error":"protected_playback_unavailable"} -- with or without the device cookie, on every tier,
     * every attempt (0 of 12). A transport failure never carries that body. */
    bool serverRefused = false;
};

/* WHAT ONE NETWORK PROBE MEASURED.
 *
 * WHY THIS EXISTS AS A STRUCT AND NOT A BOOL. The settings screen's network page
 * (wiliwili's fragment/setting_network.cpp) answers one question -- "does the API
 * work" -- with a green or red word. That is not enough to diagnose anything: on this
 * project's five distinct failures so far (DNS, TLS, connect, HTTP status, and "200
 * with an empty body"), four of them look identical through a boolean.
 *
 * So a probe reports each stage separately, and every field is something that was
 * MEASURED on this request rather than derived:
 *
 *   transportRc   libcurl's result. Non-zero means nothing was ever spoken to, and
 *                 `transportError` is curl_easy_strerror of it (DNS vs TLS vs
 *                 connect vs timeout are four different fixes).
 *   httpStatus    what the server said, when the transport succeeded.
 *   bodyBytes     how much arrived. A 200 with 0 bytes is the failure mode this
 *                 service produced on its search endpoint, twice, and it is
 *                 invisible without this number.
 *   routerBytes   bytes of _ROUTER_DATA extracted from the body, or a negative
 *                 HG_ROUTER_ERR_*. This separates "the page loaded but is not the
 *                 page we expect" (upstream layout change) from "the page did not
 *                 load" (network).
 *   cardCount     how many cards the home parser found. The end-to-end answer: the
 *                 number the app's own screen would show.
 *
 * `ServerDate` is the value of the response's Date header, if the server sent one.
 * It is here because this project's most confusing class of failure is a clock that
 * is wrong on the console and right everywhere else -- the signed requests carry
 * khronos/ticket from the system clock, so a skewed clock makes every request fail
 * for a reason that looks like a server problem. wiliwili has a whole network-time
 * request for this (Bilibili's get_unix_time); this service was never observed to
 * offer one, so the HTTP Date header is the honest substitute -- it is genuinely
 * present and needs no invented endpoint. */
struct NetworkProbe {
    bool ok = false;              /* the whole chain worked */
    std::string error;            /* the first stage that failed, in words */
    int transportRc = 0;          /* HG_HTTP_OK or a negative HG_HTTP_ERR_* */
    std::string transportError;   /* hg_http_last_error(), when transportRc != 0 */
    long httpStatus = 0;
    size_t bodyBytes = 0;
    long routerBytes = 0;         /* _ROUTER_DATA length, or negative HG_ROUTER_ERR_* */
    size_t cardCount = 0;
    std::string serverDate;       /* the response's Date header, or "" */
    long clockSkewSeconds = 0;    /* Date minus the console clock; 0 when no Date */
    bool clockSkewKnown = false;
    int msElapsed = 0;            /* wall time of the probe, for "is it just slow" */
};

class Client {
  public:
    /* One-time process init. Must run before any request; safe to call twice.
     * Wraps hg_http_global_init, which the test called once at startup. */
    static void globalInit();

    /* GET https://hongguoduanju.com/ and parse the _ROUTER_DATA blob.
     * Measured: the real home page carries 36 cards, so the output array is
     * sized HG_API_MAX_SERIES -- a smaller cap makes hg_api_parse_home return
     * HG_API_ERR_SMALL and yield NOTHING (there is no "first N of M"). */
    static Result fetchHome(std::vector<Series>& out);

    /* The same page, keeping its tiers: rows are `header, cards..., header,
     * cards...`. Measured shape is 4 tiers x 9 cards = 40 rows (36 cards + 4
     * headings).
     *
     * Refuses rather than truncates: too many rows, a section that did not parse,
     * or zero rows are all failures with a reason. Zero rows especially -- a
     * zero-row list must never reach a RecyclerFrame (that is the crash this
     * project already paid for), so the refusal is enforced here rather than left
     * to the caller's discipline. */
    static Result fetchHomeRows(std::vector<HomeRow>& out);

    /* GET /detail?series_id=<id> and parse seriesDetail.vid_list[].
     * Measured: 76 episode ids for the series the test used. */
    static Result fetchEpisodes(const std::string& seriesId, std::vector<Episode>& out);

    /* SEARCH. GET https://hongguoduanju.com/search/<urlencoded keyword> and parse the
     * server-rendered result list.
     *
     * THE HTML PAGE, NOT THE SIGNED API: the signed search endpoint answers 200 with a
     * Content-Length of 0 (reproduced with the reference implementation itself), so there is
     * nothing to parse there. The page is what the site itself shows.
     *
     * WHAT THE CALLER GETS, and why `total` matters: the page carries ONE page of results (10
     * items, measured) and no way to ask for the next one -- `/search/ai?page=2` returns page 1
     * again, item for item. It does report how many exist in total (`totalCount`, 201 for "ai"),
     * so the screen can say "showing 10 of 201" instead of implying it showed everything. `total`
     * is -1 when the page did not state one.
     *
     * The keyword is percent-encoded with hg_url_encode, which the signer's tests already pin to
     * encodeURIComponent's behaviour -- the path segment is the one place in this app where a
     * user's own text goes into a URL, so it is the one place that matters. */
    static Result fetchSearch(const std::string& keyword, std::vector<Series>& out,
                              long long& total);

    /* THE CATEGORY PAGE, which is what a tier's "more" card opens.
     *
     * MEASURED (app/src/tools/probe_category_page.mjs): `GET /category` answers 200 with
     * loaderData.category_page.recommendList[] -- 24 items whose fields are the HOME page's
     * (series_id / series_name / series_cover / tags / episode_right_text), which is why the existing
     * hg_api_parse_home reads it with no new parser at all (verified against a saved router blob
     * before any UI was written: find_page -> "category_page", parse_home -> 24 series).
     *
     * `slug` is a path segment: "real-drama" / "comic-drama" / "ai-drama" / "comic". An empty slug is
     * the bare /category page. See categorySlugForTier.
     *
     * IT PAGES, AND IT SAYS HOW FAR. MEASURED (probe_category_paging.mjs): `?page=2` and `?page=3`
     * return DIFFERENT lists (compared by first id, not by count), `?page=35` is a 404, and the page
     * carries `loaderData.category_$.pagination` = { total: 800, pageNum, pageSize: 24, totalPages: 34 }.
     * So `page` selects the page and `*info` reports that block, which is what the category screen
     * checks before asking for another one -- a full comic-drama category is 800 series over 34 pages.
     *
     * `page` is 1-based; anything below 1 is clamped to 1. `info` may be NULL. */
    static Result fetchCategory(const std::string& slug, int page, std::vector<Series>& out,
                                HgApiPagination* info = nullptr);

    /* A HOME TIER'S UPSTREAM SLUG -> THE CATEGORY PAGE THAT HOLDS THAT TIER.
     *
     * MEASURED BY ID OVERLAP, not by matching the words:
     *
     *   home tier          category page            series ids shared
     *   human  (热播真人剧)  /category/real-drama    9 of 9
     *   comic  (热播漫剧)    /category/comic-drama   9 of 9
     *   ai     (热播AI剧)    /category/ai-drama      9 of 9
     *   all    (热播短剧)    (no exact page)         -- 2 of its 9 also appear in real-drama, so this
     *                                               tier is a cross-category selection upstream makes
     *                                               and there is no single page behind it
     *
     * `/category/comic` exists and is a DIFFERENT list (0 overlap with every home tier): it is the
     * site's 漫画 category, not 漫剧. And the bare `/category` returns exactly what `/category/real-drama`
     * returns (identical 24 ids), which is why "all" falls back to it -- the site's own general page is
     * the closest thing to "everything" that it serves.
     *
     * Returns the slug to use, or "" for the bare /category page. */
    static std::string categorySlugForTier(const std::string& homeTabType);

    /* Build the signed video-model request for one episode id, POST it, and
     * turn the response into a play plan (URL + CENC key + header fields).
     *
     * The khronos/ticket pair is the console's own clock. The test hardcoded
     * 1791466175 because the console clock was sane; using time(NULL) here is
     * the same value by a more direct route, and _rticket is that in ms.
     *
     * RETAINED, BUT THE DEAD ROUTE (build173). As of 2026-10-16 the endpoint behind this answers
     * `HTTP 200 + 0 bytes` to every request, so this always fails with "POST 200 with 0 bytes". It is
     * kept because fetchPlayPlanForSource and the quality ladder still call it and because the failure
     * line it writes is the evidence that the route is dead. Playback goes through
     * fetchPlayInfoPlan() below. */
    static Result fetchPlayPlan(const std::string& episodeId, HgPlayPlan& out);

    /* THE ROUTE THAT ANSWERS NOW (build173): the web client's play-info.
     *
     * `seriesId` is REQUIRED and must be the real series: measured, a wrong, empty or absent series_id
     * is rejected with HTTP 400 while the same vid under its own series returns 200. (The player screen
     * only ever had the episode id, which is why PlayerView now carries the series id too.)
     *
     * THE TIERS ARE TRIED IN ORDER, BEST FIRST -- AND 720p IS THE CEILING OF THIS ROUTE, not a choice
     * this app made. MEASURED: 360p / 480p / 720p answer, while 540p / 1080p / 1080p+ / 2k / 4k ALL
     * answer 400 {"error":"protected_playback_unavailable"} -- a stable refusal that retrying cannot
     * change. So 1080p is never requested, and the saved "default 1080P" preference (build143) CANNOT be
     * honoured while playback comes from here. That is a property of the endpoint, not a regression in
     * this file: the previous route did offer 1080p, this one does not, and no amount of tier preference
     * brings it back. The quality menu is built from what this route can actually return, so a tier that
     * would 400 is not shown at all rather than offered and then refused.
     *
     * EACH TIER IS RETRIED, AND THAT IS NOT A WORKAROUND FOR A REFUSAL -- it is the shape of this
     * service. MEASURED: the SAME request (same vid, same definition) repeated eight times answers
     * `403 200 200 403 200 403 403 403` on one attempt and `200 200 403 403 403 403 200 403` on the
     * next, so a 403 usually means this attempt hit an unlucky backend rather than that the device is
     * not allowed. Retrying raised reachability from 20/76 episodes to ~50/76 in the same series.
     *
     * THE BUDGET IS BOUNDED AND SMALL on purpose: 6 tries for 720p then 3 and 3 for the fallbacks --
     * 12 requests at most, ~300 ms apart, giving up the moment a tier answers. Roughly four seconds in
     * the worst case, which rides out the jitter while still failing an unavailable episode promptly.
     * A genuine refusal is not retried into success, so the failure is reported with the service's own
     * words and no invented explanation.
     *
     * `out` is an HgPlayInfo (video URL, separate audio URL, CENC key), not an HgPlayPlan: this route
     * returns a different shape, and pretending otherwise would mean inventing a ladder that does not
     * exist here. */
    static Result fetchPlayInfoPlan(const std::string& seriesId, const std::string& episodeId,
                                    HgPlayInfo& out);

    /* The quality menu and the per-tier fetch built on the same route are declared AFTER QualityTier
     * below, because their signatures name it. */

    /* Human-readable one-liner about a plan, for the player overlay. */
    static std::string describePlan(const HgPlayPlan& plan);

    /* ONE QUALITY TIER, as the player's menu needs it: what to call it, how good it is, and how to ask
     * for it.
     *
     * `sourceIndex` HAS TWO MEANINGS AND THEY DEPEND ON WHICH ROUTE BUILT THE TIER (build173):
     *   - from fetchQualityLadder (the RETIRED video_model route): the index into that response's source
     *     list, which is what fetchPlayPlanForSource takes. That list no longer exists.
     *   - from fetchPlayInfoTiers (the route that answers now): the INDEX INTO THE DEFINITION LIST --
     *     0 = 720p, 1 = 480p, 2 = 360p -- which is what fetchPlayInfoPlanForTier takes, because on this
     *     route a tier IS the `definition` parameter rather than a separate source object.
     * The field is shared rather than duplicated because the MENU and the switch logic are shared: one
     * index that the menu displays and the switch consumes, resolved by whichever route supplied it. */
    struct QualityTier {
        std::string label;      /* the service's own name for the tier, e.g. "720p" */
        int qualityType = 0;    /* upstream's number; 0 = absent (always 0 on the play-info route) */
        int width = 0;
        int height = 0;
        int bitrate = 0;
        long long sizeBytes = 0;
        /* Upstream's own codec token for this tier ("h264" / "bytevc1"), shown in the label. */
        std::string codec;
        int sourceIndex = 0;
    };

    /* ---- THE QUALITY MENU, BUILT FROM WHAT THE LIVE ROUTE CAN ACTUALLY RETURN (build173) ------------
     *
     * WHY THIS REPLACES fetchQualityLadder. The old ladder came from the retired video_model endpoint, so
     * it now returns nothing -- and a menu whose source no longer answers is worse than no menu, because
     * the row is drawn from whatever was last parsed successfully.
     *
     * WHAT IT CONTAINS, AND WHAT IT OMITS ON PURPOSE. At most three entries -- 720p, 480p, 360p, best
     * first -- and ONLY those that answered just now. 1080p is never listed: measured, 540p / 1080p /
     * 1080p+ / 2k / 4k are a stable 400 from this endpoint, so it is not even asked for. Offering a tier
     * that is refused the moment it is picked is worse than not offering it.
     *
     * Each definition is probed ONCE (not with the plan request's retry budget -- three tiers x six tries
     * would be eighteen requests just to label a row). The cost of that is the menu can under-report: the
     * 403 is intermittent, so a one-shot probe may miss a tier the retrying plan request would have
     * taken. Under-promising is the right direction to be wrong in here, and the tier actually playing is
     * always shown regardless.
     *
     * `out` EMPTY WITH ok=true MEANS "NO MENU", not a failure: the caller keeps playing. */
    static Result fetchPlayInfoTiers(const std::string& seriesId, const std::string& episodeId,
                                     std::vector<QualityTier>& out);

    /* Fetch the plan for ONE tier from the same route. `tierIndex` is QualityTier::sourceIndex.
     *
     * `tierIndex` == -1 means "the best tier that answers", which is what a fresh episode wants, and is
     * exactly what fetchPlayInfoPlan does.
     *
     * A named tier that fails is REPORTED, not silently downgraded: a user who picks 480p and gets 360p
     * has been disobeyed without being told, which is worse than being told it failed while the current
     * stream keeps playing. */
    static Result fetchPlayInfoPlanForTier(const std::string& seriesId, const std::string& episodeId,
                                           int tierIndex, HgPlayInfo& out);

    /* Fetch every quality tier this episode offers, BEST FIRST.
     *
     * MEASURED, and the reason the ordering is not taken from the response: upstream sends
     * the tiers WORST FIRST (`quality_type` 40 = 360p is entry 0, `quality_type` 1 = 1080p is
     * entry 4), so a menu built in response order offers the worst picture as the default.
     * The ordering is hg_quality_build's job and it sorts by pixel height.
     *
     * `currentQualityType` is the tier the player is on now; it is only used for the log.
     * Reports a reason rather than an empty list when the response has no ladder, because
     * "no quality menu" and "the fetch failed" are different facts. */
    static Result fetchQualityLadder(const std::string& episodeId,
                                     std::vector<QualityTier>& out);

    /* Build a play plan from ONE ENTRY of the same response fetchQualityLadder read.
     * `sourceIndex` is QualityTier::sourceIndex.
     *
     * This re-fetches the response rather than caching it. That is deliberate: the response
     * carries a signed URL with a limited life, so a cached copy would be the wrong thing to
     * switch with after a few minutes -- and re-fetching also means a switch cannot land on a
     * source whose signature has expired. The cost is one request per quality change. */
    static Result fetchPlayPlanForSource(const std::string& episodeId, int sourceIndex,
                                         HgPlayPlan& out);

    /* Fetch the home page and report every stage of what happened. Blocking, so it
     * belongs on a worker thread -- see hg_thread.hpp. See NetworkProbe for why it
     * returns measurements rather than a boolean. */
    static NetworkProbe networkProbe();

    /* THE SERVER'S OWN LAST WORDS, as measured rather than summarised: the "HTTP 200 | 125 bytes |
     * first 512: {...}" line that a failed play request writes to the log. Empty when nothing has
     * failed.
     *
     * WHY IT IS AN ACCESSOR AND NOT A FIELD ON Result. The string exists because of the build155
     * failure logging (see logFailureResponse in hg_client.cpp), and the play-chain self check needs
     * the SAME bytes on screen -- a user should not have to fetch a log file off the console to be
     * told what the server said. Duplicating the formatting would let the two drift, so there is one
     * producer and this reader. */
    static std::string lastFailureDetail();
};

} // namespace hg
