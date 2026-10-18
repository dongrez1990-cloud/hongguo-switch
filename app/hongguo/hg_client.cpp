/*
 * hg_client implementation: the same calls app/play-test/main.c makes, in the
 * same order, returning objects instead of printf.
 *
 * GPL-3.0-only.
 *
 * Every URL, header and buffer size below is copied from the hardware-verified
 * test rather than re-derived. The comments say which measurement each one
 * comes from, because a number that was measured on the console is worth far
 * more than one that looks reasonable.
 */

#include "hg_client.hpp"
#include "hg_cfg.hpp"
#include "hg_log.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <chrono>
#include <thread>

namespace hg {

/* Buffer sizes from the test, which measured against real pages:
 *   home page        186 KB HTML -> 45,598 B _ROUTER_DATA
 *   detail page      509 KB HTML -> the episode list (222 ids on the longest
 *                                  series measured; this client's first series
 *                                  yielded 76)
 * The test used 1 MiB for the page and 512 KB for the router blob. A client
 * cannot be smaller than what its own inputs measured, so these are unchanged. */
static char g_page[1024 * 1024];
static char g_router[512 * 1024];
static HgRequest g_req;

/* ---- ONE CLIENT CALL AT A TIME (build157) ----------------------------------------------------------
 *
 * THE TWO BUFFERS ABOVE ARE SHARED BY EVERY REQUEST THIS CLASS MAKES, so two calls running at once do
 * not merely race on a counter -- each one overwrites the other's whole response and both parsers then
 * read a MIXTURE of two different pages. The failure is a wrong answer, not a crash, which is the worse
 * kind: with build156 the player held a quality-ladder request and a plan request in flight together,
 * and the only thing keeping them apart was a mutex in player_view.cpp (`g_ladderMutex`, build144,
 * written after a crash).
 *
 * build157 adds a third caller -- the play-chain self check, which runs on its own thread while the
 * player may already be starting another episode. Rather than add a fourth place that has to know about
 * the other three, the serialisation lives HERE, where the buffers are, and every public entry point
 * takes it. It is RECURSIVE because a public entry point calls the private helpers that also need it
 * (fetchPlayPlan -> postVideoModel); a plain mutex would deadlock the first time.
 *
 * Cost: requests that used to overlap now queue. On this service that is closer to what the app's own
 * behaviour was anyway (one ladder and one plan at a time), and a queued request is correct while a
 * mixed response is not. */
static std::recursive_mutex g_clientMutex;

/* The measured detail of the last failed response -- the same "HTTP 200 | 125 bytes | first 512: ..."
 * string that goes to the log. Kept so the self check can put the evidence on screen, not just in a file
 * the user has to fetch. Guarded by g_clientMutex (recursive, so the locked callers can call it). */
static std::string g_lastFailure;

std::string Client::lastFailureDetail() {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    return g_lastFailure;
}

/* The browser UA the detail/home pages were measured with. The pages are
 * server-rendered and the site's own XHR path differs by UA, so this is part
 * of the measured configuration, not a cosmetic detail. */
static const char *BROWSER_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

static const char *HOME_URL = "https://hongguoduanju.com/";

void Client::globalInit() {
    hg_http_global_init();

    /* THE HTTP CLIENT IS CONFIGURED FROM THE SAME FILE AS EVERYTHING ELSE.
     *
     * hg_httpcfg owns the parsing, the ranges and the refusals (timeout 0, a connect timeout above
     * the total, a proxy with no scheme) and its host suite covers all of them; this function only
     * hands it the cfg text and passes the result on. That split is deliberate: value validation
     * is the part that can be tested off the console, and it is the part that goes wrong quietly.
     *
     * THE REFUSALS ARE LOGGED, ONE LINE EACH. A cfg line that was ignored is worse than one that
     * was never written, because the user believes it took effect -- so the reason goes to the
     * log next to the value that was kept. */
    {
        HgHttpCfg cfg;
        hg_httpcfg_defaults(&cfg);

        /* Applied pair by pair rather than through hg_httpcfg_parse, because parse() discards the
         * reasons and the reasons are the point here. The iteration is the same: one key=value per
         * line, '#' comments, unknown keys skipped. */
        std::string text = hg::cfgText();
        size_t pos = 0;
        int applied = 0, refused = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            /* Trim, because the file is usually written from Windows. */
            size_t k0 = key.find_first_not_of(" \t");
            size_t k1 = key.find_last_not_of(" \t\r");
            size_t v0 = val.find_first_not_of(" \t");
            size_t v1 = val.find_last_not_of(" \t\r");
            if (k0 == std::string::npos || v0 == std::string::npos) continue;
            key = key.substr(k0, k1 - k0 + 1);
            val = val.substr(v0, v1 - v0 + 1);
            if (key.empty() || key[0] == '#') continue;

            const char* why = nullptr;
            int rc = hg_httpcfg_apply(&cfg, key.c_str(), val.c_str(), &why);
            if (rc == 1) {
                applied++;
            } else if (rc == -1) {
                refused++;
                hg::logPrintf("HTTPCFG: %s=%s REFUSED -- %s", key.c_str(), val.c_str(),
                              why ? why : "(no reason given)");
            }
        }

        char summary[320];
        hg_httpcfg_describe(&cfg, summary, (int)sizeof summary);
        hg::logPrintf("HTTPCFG: %d applied, %d refused -- %s", applied, refused, summary);
        if (!cfg.tls_verify) {
            /* Said out loud, because it is the one setting here that makes the app less safe and a
             * user should not discover it by accident. */
            hg::logPrintf("HTTPCFG: TLS verification is OFF (http.tls.verify=no) -- the connection "
                          "is encrypted but the certificate is not checked");
        }
        if (cfg.proxy[0] != '\0') {
            hg::logPrintf("HTTPCFG: using proxy %s", cfg.proxy);
        }

        hg_http_set_options(&cfg);
    }
}

/* Plain GET with the measured header set. Returns the body length, or 0 with
 * *err filled. */
static size_t httpGet(const char *url, std::string& err) {
    const char *h[4];
    char ua[256];
    long status = 0;
    size_t len = 0;
    int rc;

    snprintf(ua, sizeof ua, "User-Agent: %s", BROWSER_UA);
    h[0] = ua;
    h[1] = "Accept: text/html,application/xhtml+xml";
    h[2] = "Accept-Language: zh-CN,zh;q=0.9";
    h[3] = nullptr;

    rc = hg_http_request(url, h, nullptr, 0, g_page, sizeof g_page, &len, &status);
    if (rc != HG_HTTP_OK) {
        char buf[256];
        snprintf(buf, sizeof buf, "transport rc=%d (%s)", rc, hg_http_last_error());
        err = buf;
        return 0;
    }
    if (status < 200 || status >= 300) {
        char buf[128];
        snprintf(buf, sizeof buf, "HTTP %ld", status);
        err = buf;
        return 0;
    }
    return len;
}

/* Wraps the shared "GET a page, extract _ROUTER_DATA" step that both the home
 * and detail paths perform identically. */
static Result fetchRouter(const std::string& url, size_t& routerLen, std::string& err) {
    Result r;
    size_t pageLen = httpGet(url.c_str(), err);
    if (pageLen == 0) {
        if (err.empty()) err = "empty response";
        return r;
    }
    int n = hg_router_extract(g_page, pageLen, g_router, sizeof g_router);
    if (n < 0) {
        char buf[128];
        snprintf(buf, sizeof buf, "no _ROUTER_DATA in %u bytes (rc=%d)",
                 (unsigned)pageLen, n);
        err = buf;
        return r;
    }
    routerLen = (size_t)n;
    r.ok = true;
    return r;
}

Result Client::fetchHome(std::vector<Series>& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();
    if (!r.error.empty()) r.error.clear();

    std::string err;
    size_t routerLen = 0;
    Result got = fetchRouter(HOME_URL, routerLen, err);
    if (!got.ok) { r.error = err; return r; }

    /* HG_API_MAX_SERIES, not a smaller cap: the measured home page carries 36
     * cards and hg_api_parse_home returns HG_API_ERR_SMALL with *count = 0 when
     * the cap is too small. There is deliberately no "first N of M". */
    static HgSeries buf[HG_API_MAX_SERIES];
    size_t count = 0;
    int rc = hg_api_parse_home(g_router, routerLen, buf, HG_API_MAX_SERIES, &count);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_home rc=%d (see hg_api.h)", rc);
        r.error = b;
        return r;
    }

    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
        Series s;
        s.id = buf[i].id;
        s.title = buf[i].title;
        s.cover = buf[i].cover;
        s.desc = buf[i].desc;
        s.badge = buf[i].badge;
        s.tags = buf[i].tags;
        out.push_back(std::move(s));
    }
    r.ok = true;
    return r;
}

Result Client::fetchHomeRows(std::vector<HomeRow>& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();

    std::string err;
    size_t routerLen = 0;
    Result got = fetchRouter(HOME_URL, routerLen, err);
    if (!got.ok) { r.error = err; return r; }

    /* STATIC, not stack. HgHomePage is 128 cards x ~1.3 KB = 168 KB, and this
     * runs on a worker thread whose stack is 128 KiB (hg_thread.cpp). A stack
     * object of that size would silently overflow the thread and take the process
     * down with no message -- the same class of failure as the std::thread
     * problem this project already fixed. These are also why the class comment
     * says the fetchers must not be called concurrently. */
    static HgHomePage page;
    static HgHomeRow rows[HG_API_MAX_HOME_ITEMS + HG_API_MAX_SECTIONS];
    size_t rowCount = 0;

    int rc = hg_api_parse_home_sections(g_router, routerLen, &page);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_home_sections rc=%d (see hg_api.h)", rc);
        r.error = b;
        return r;
    }
    rc = hg_api_home_rows(&page, rows, sizeof rows / sizeof rows[0], &rowCount);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "home_rows rc=%d", rc);
        r.error = b;
        return r;
    }
    /* ZERO ROWS IS REFUSED HERE, ON PURPOSE. A zero-row RecyclerFrame is the
     * crash this project already paid for (RecyclerFrame::reloadData calls
     * selectRowAt({0,0}), which indexes cacheFramesData[1] on a one-entry
     * vector). Refusing in the fetcher means no caller can forget. */
    if (rc == 0) {
        r.error = "the home page has no sections to display";
        return r;
    }

    out.reserve(rowCount);
    for (size_t i = 0; i < rowCount; i++) {
        HomeRow row;
        row.header = (rows[i].kind == HG_API_ROW_HEADER);
        if (row.header) {
            row.sectionTitle = page.sections[rows[i].section].tab_name;
            /* THE TIER'S SLUG TRAVELS WITH THE ROW, because the "more" card has to name a category
             * page and the display heading ("热播真人剧") is not a URL. The slug IS upstream's own
             * (`all` / `human` / `comic` / `ai`) and Client::categorySlugForTier turns it into the
             * page the site actually serves -- measured by id overlap, see that function. */
            row.tabType = page.sections[rows[i].section].tab_type;
        } else {
            const HgSeries& s = page.items[rows[i].item];
            row.series.id = s.id;
            row.series.title = s.title;
            row.series.cover = s.cover;
            row.series.desc = s.desc;
            row.series.badge = s.badge;
            row.series.tags = s.tags;
        }
        out.push_back(std::move(row));
    }
    r.ok = true;

    /* CAP EACH TIER AT A FULL ROW, WITH THE LAST SLOT AS "MORE".
     *
     * MEASURED: each tier carries about nine cards and the grid draws eight per row, so the ninth sat alone
     * on a row of its own -- an entire row of screen for one poster, and the screen looked like it had a
     * stray card at the bottom of every section.
     *
     * The user's suggestion, and it is the right one: keep seven posters and put "more" in the eighth slot,
     * so every tier ends exactly on a row boundary. Nothing is lost -- the eighth card is replaced by an
     * entry point to everything else in that tier, which is what the eighth slot should have been anyway.
     *
     * DONE HERE RATHER THAN IN THE VIEW because this is the part that can be wrong SILENTLY (an off-by-one
     * drops a card or duplicates the "more" row), and this function's logic is covered by host tests. The
     * view just draws whatever rows it is given.
     *
     * The cap is generous (99) so it only applies to the pathological case: a tier with hundreds of items
     * would otherwise produce hundreds of rows, and the "more" card is exactly the right answer for that. */
    {
        const size_t CARDS_PER_ROW = 7;
        std::vector<HomeRow> capped;
        capped.reserve(out.size());
        size_t i = 0;
        while (i < out.size()) {
            if (out[i].header) {
                capped.push_back(out[i]);
                const std::string tier = out[i].sectionTitle;
                const std::string tierSlug = out[i].tabType;
                i++;
                size_t kept = 0;
                size_t seen = 0;
                while (i < out.size() && !out[i].header) {
                    if (seen < CARDS_PER_ROW) {
                        capped.push_back(out[i]);
                        kept++;
                    }
                    seen++;
                    i++;
                }
                /* Only when there IS something past the visible row: a tier of seven cards would otherwise
                 * get a "more" card that leads to nothing. */
                if (seen > CARDS_PER_ROW) {
                    HomeRow more;
                    more.more = true;
                    more.sectionTitle = tier;
                    more.tabType = tierSlug;
                    capped.push_back(std::move(more));
                }
                hg::logPrintf("HOME rows: tier '%s' had %u cards, kept %u + more=%d", tier.c_str(),
                              (unsigned)seen, (unsigned)kept, seen > CARDS_PER_ROW ? 1 : 0);
            } else {
                /* A card with no header before it: leave the order alone rather than guessing. */
                capped.push_back(out[i]);
                i++;
            }
        }
        out = std::move(capped);
    }

    return r;
}

Result Client::fetchEpisodes(const std::string& seriesId, std::vector<Episode>& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();

    /* hg_api_valid_id is the reference's own [0-9]{1,30} rule. Validating here
     * means a bad id fails with a clear message instead of being interpolated
     * into a URL and coming back as a confusing parse error. */
    if (!hg_api_valid_id(seriesId.c_str())) {
        r.error = "series id is not [0-9]{1,30}";
        return r;
    }

    std::string url = "https://hongguoduanju.com/detail?series_id=" + seriesId;
    std::string err;
    size_t routerLen = 0;
    Result got = fetchRouter(url, routerLen, err);
    if (!got.ok) { r.error = err; return r; }

    static HgEpisode buf[HG_API_MAX_EPISODES];
    size_t count = 0;
    int rc = hg_api_parse_episode_ids(g_router, routerLen, buf, HG_API_MAX_EPISODES, &count);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_episode_ids rc=%d", rc);
        r.error = b;
        return r;
    }

    out.reserve(count);
    for (size_t i = 0; i < count; i++) out.push_back(Episode{buf[i].id});
    r.ok = true;
    return r;
}

Result Client::fetchSearch(const std::string& keyword, std::vector<Series>& out, long long& total) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();
    total = -1;

    if (keyword.empty()) {
        r.error = "empty search keyword";
        return r;
    }

    /* PERCENT-ENCODE THE KEYWORD. This is user text going into a URL path segment, and the keyword
     * is Chinese most of the time (the site's own search box takes pinyin or hanzi). hg_url_encode is
     * the same encoder the signer uses, checked against encodeURIComponent's behaviour in
     * hg_sign_test -- the reference implementation builds this URL with encodeURIComponent
     * (probe_search_page.mjs does the same), so the two agree by construction. */
    char enc[512];
    if (hg_url_encode(keyword.c_str(), enc, sizeof enc) < 0) {
        r.error = "search keyword is too long to encode";
        return r;
    }
    std::string url = std::string("https://hongguoduanju.com/search/") + enc;

    std::string err;
    size_t routerLen = 0;
    Result got = fetchRouter(url, routerLen, err);
    if (!got.ok) { r.error = err; return r; }

    /* STATIC, not stack: same reason as the other fetchers in this file (a worker thread has a
     * 128 KiB stack, and this array is sized by the largest list any page parses into it). */
    static HgSeries buf[HG_API_MAX_SERIES];
    size_t count = 0;
    int rc = hg_api_parse_search_page(g_router, routerLen, buf, HG_API_MAX_SERIES, &count, &total);
    if (rc < 0) {
        /* EMPTY IS A RESULT, NOT AN ERROR: a keyword that matches nothing comes back as a 200 with an
         * empty list, and turning that into "search failed" would be a lie. It still is not a
         * success -- the caller has to say "no results" -- so it is reported as an ok Result with an
         * empty vector, which is the only combination that says exactly that. */
        if (rc == HG_API_ERR_EMPTY) {
            hg::logPrintf("SEARCH: '%s' matched nothing (rc=%d, total=%lld)", keyword.c_str(), rc,
                          total);
            r.ok = true;
            return r;
        }
        char b[128];
        snprintf(b, sizeof b, "parse_search_page rc=%d (see hg_api.h)", rc);
        r.error = b;
        return r;
    }

    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
        Series s;
        s.id = buf[i].id;
        s.title = buf[i].title;
        s.cover = buf[i].cover;
        s.desc = buf[i].desc;
        s.badge = buf[i].badge;
        s.tags = buf[i].tags;
        out.push_back(std::move(s));
    }
    r.ok = true;
    return r;
}

std::string Client::categorySlugForTier(const std::string& homeTabType) {
    /* See the header for the measurement table this comes from. Two of the four slugs are NOT the
     * tier's own name (`comic` -> comic-drama, and `/category/comic` is a different, unrelated list),
     * which is exactly why this is a table with measured evidence rather than a string concatenation. */
    if (homeTabType == "human") return "real-drama";
    if (homeTabType == "comic") return "comic-drama";
    if (homeTabType == "ai") return "ai-drama";
    /* "all" and anything unknown: the bare category page, which is the same 24 items the site serves
     * for real-drama and the closest thing it has to "everything". */
    return "";
}

Result Client::fetchCategory(const std::string& slug, int page, std::vector<Series>& out,
                             HgApiPagination* info) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();
    if (info) {
        info->page = 0;
        info->page_size = 0;
        info->total = 0;
        info->total_pages = 0;
    }
    if (page < 1) page = 1;

    /* The slug is interpolated into a URL path, so it is checked rather than trusted: it comes from a
     * fixed table today, and a caller that passed something else must not be able to build a different
     * URL by accident. */
    if (!slug.empty()) {
        bool ok = true;
        for (size_t i = 0; i < slug.size(); i++) {
            const char c = slug[i];
            if (!((c >= 'a' && c <= 'z') || c == '-')) ok = false;
        }
        if (!ok) {
            r.error = "category slug is not [a-z-]+: " + slug;
            return r;
        }
    }

    std::string url = "https://hongguoduanju.com/category";
    if (!slug.empty()) url += "/" + slug;
    /* PAGE 1 IS THE BARE URL: adding ?page=1 was never measured, and the site's own links start at
     * ?page=2 (its pager omits the parameter for the first page). */
    if (page > 1) {
        char b[32];
        snprintf(b, sizeof b, "?page=%d", page);
        url += b;
    }

    std::string err;
    size_t routerLen = 0;
    Result got = fetchRouter(url, routerLen, err);
    if (!got.ok) { r.error = err; return r; }

    /* HOW FAR THIS CATEGORY GOES, read from the page rather than guessed: the screen needs it to know
     * when to stop asking. A page with no pagination block leaves the struct zeroed and the screen then
     * stops when a page comes back empty. */
    if (info) (void)hg_api_parse_pagination(g_router, routerLen, info);

    /* THE EXISTING PARSER, NOT A NEW ONE. The category page's items carry the home page's field names,
     * and this was verified against a live router blob before any UI existed (see the header). If
     * upstream ever reshapes this page, the parse fails with a reason here rather than drawing an empty
     * grid. */
    static HgSeries buf[HG_API_MAX_SERIES];
    size_t count = 0;
    int rc = hg_api_parse_home(g_router, routerLen, buf, HG_API_MAX_SERIES, &count);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_home rc=%d on /category/%s page %d (see hg_api.h)", rc, slug.c_str(),
                 page);
        r.error = b;
        return r;
    }

    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
        Series s;
        s.id = buf[i].id;
        s.title = buf[i].title;
        s.cover = buf[i].cover;
        s.desc = buf[i].desc;
        s.badge = buf[i].badge;
        s.tags = buf[i].tags;
        out.push_back(std::move(s));
    }
    hg::logPrintf("CATEGORY: /category/%s page %d -> %u series (total=%d, pages=%d)", slug.c_str(), page,
                  (unsigned)count, info ? info->total : -1, info ? info->total_pages : -1);
    r.ok = true;
    return r;
}

/* ---- WHAT THE SERVER ACTUALLY SENT, ON FAILURE ONLY (build155) --------------------------------
 *
 * WHY THIS EXISTS. Pressing Play started answering "Playback failed: play_plan rc=-5" -- that is
 * HG_PLAY_ERR_NOMODEL, "the response carried no outer / video_model / video_list" (hg_play.h) --
 * and it does so on build154, on build145 and on the weeks-old build33, with the config deleted
 * and with the config file from the days when playback worked. That rules out this program, this
 * configuration and this version between them, and it leaves exactly ONE unknown: what the server
 * returns. Every failure path below used to throw the body away, so a console trip could only
 * report that the parse had failed, never what it had failed to parse.
 *
 * THE TWO ANSWERS NEED DIFFERENT FIXES, AND THE STATUS CODE IS NOT ENOUGH TO SEPARATE THEM:
 *   - 403 / 429 / 5xx  -> the request is being REFUSED, so what changed is the request (a new
 *                         signature, a new header, a new parameter);
 *   - 200 with a body that has no video_model -> the SHAPE changed, and this body can be compared
 *                         field by field against a response recorded when playback worked.
 * So both the status and the head of the body are written, on one line, together.
 *
 * ON FAILURE ONLY, AND ONLY THE FIRST 512 BYTES. A success needs no explanation -- and playback
 * itself writes a few lines a second, so a line per request would be its own problem. The measured
 * response is about 19 KB while this log is appended to across sessions under a 512 KiB cap; the
 * envelope, the error fields and the key names are all at the front.
 *
 * WHY THE HEAD IS SANITISED: these are third-party bytes going into a line-oriented log. A newline
 * would split one failure across two lines (the log's whole protocol is one line per event) and an
 * embedded NUL would silently truncate the line instead of ending it. So control bytes become '.'.
 * Everything from 0x20 up is passed through unchanged, including UTF-8 continuation bytes, because
 * the payload's own text is part of the evidence.
 *
 * `blen` is the length the transport reported. On a FAILED transfer that number is never written
 * (hg_http.c only sets out_len on success), so those call sites pass strlen() of the buffer: a
 * transfer that died mid-body has still delivered the start of one, and reporting "0 bytes" there
 * would read as "the server sent nothing", which is a different finding. */
static void logFailureResponse(const char* where, const char* why, long status, const char* body,
                               size_t blen) {
    char head[513];
    char line[900];
    size_t shown = 0;
    if (body != nullptr) {
        for (size_t i = 0; i < 512 && body[i] != '\0'; i++) {
            const unsigned char c = (unsigned char)body[i];
            head[shown++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
        }
    }
    head[shown] = '\0';

    if (body == nullptr) {
        /* No buffer at all: say so rather than printing whatever the previous request left there. */
        snprintf(line, sizeof line, "%s FAILED [build155]: %s | HTTP %ld | no response buffer", where,
                 why, status);
    } else {
        snprintf(line, sizeof line,
                 "%s FAILED [build155]: %s | HTTP %ld | %u bytes | first %u: %s%s", where, why, status,
                 (unsigned)blen, (unsigned)shown, head, blen > (size_t)shown ? " ..." : "");
    }
    logPrintf("%s", line);

    /* The same evidence, kept for the on-screen self check -- a user should not have to pull a log file
     * off the console to be told what the server said. */
    {
        std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
        g_lastFailure = line;
    }
}

Result Client::fetchPlayPlan(const std::string& episodeId, HgPlayPlan& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;

    if (!hg_api_valid_episode_id(episodeId.c_str())) {
        r.error = "episode id is not [0-9]{1,30}";
        return r;
    }

    /* The console clock. The test hardcoded 1791466175 with the note that the
     * console clock is sane; time(NULL) is that value from the real source.
     * _rticket is the same instant in milliseconds, as the signer requires. */
    uint32_t khronos = (uint32_t)time(nullptr);
    uint64_t ticket = (uint64_t)khronos * 1000ull;

    int rc = hg_request_video_model(&g_req, episodeId.c_str(), khronos, ticket, 1u);
    if (rc != 0) {
        char b[128];
        snprintf(b, sizeof b, "build video-model request rc=%d", rc);
        r.error = b;
        return r;
    }

    size_t blen = 0;
    long status = 0;
    rc = hg_http_request(g_req.url, g_req.headers, g_req.body, g_req.body_len,
                         g_page, sizeof g_page, &blen, &status);
    if (rc != HG_HTTP_OK) {
        char b[256];
        snprintf(b, sizeof b, "POST transport rc=%d (%s)", rc, hg_http_last_error());
        logFailureResponse("PLAY-PLAN", b, status, g_page, strlen(g_page));
        r.error = b;
        return r;
    }
    if (status != 200) {
        char b[96];
        snprintf(b, sizeof b, "POST HTTP %ld", status);
        logFailureResponse("PLAY-PLAN", b, status, g_page, blen);
        r.error = b;
        return r;
    }
    /* Measured response was 19,381 bytes. A zero-length 200 is what the DEAD
     * search endpoint returned, so this is worth distinguishing explicitly. */
    if (blen == 0) {
        logFailureResponse("PLAY-PLAN", "POST 200 with 0 bytes", status, g_page, 0);
        r.error = "POST 200 with 0 bytes (see docs: the dead endpoint does this)";
        return r;
    }

    rc = hg_play_plan_from_model(g_page, blen, &out);
    if (rc != 0) {
        char b[128];
        snprintf(b, sizeof b, "play_plan rc=%d (see hg_play.h)", rc);
        logFailureResponse("PLAY-PLAN", b, status, g_page, blen);
        r.error = b;
        return r;
    }

    r.ok = true;
    return r;
}

/* ---- THE PLAY-INFO ROUTE: THE ONE THAT ANSWERS NOW (build173) -------------------------------------
 *
 * WHAT WAS WRONG. Pressing Play answered "PLAY-PLAN FAILED [build155]: POST 200 with 0 bytes" and the
 * status light went RED, while the home page (36 cards) and the detail page (76 episodes) both worked.
 * That is the signature of ONE route dying rather than of a network or signing problem, and it is what
 * the /novel/player/video_model/v1/ endpoint does now: measured with the reference implementation's own
 * signer, EVERY request answers 200 with an empty body -- signed, unsigned and deliberately corrupted
 * x-gorgon alike, for any body and any id, on both hosts. No parameter change can revive it.
 * Evidence: docs/ (the play-interface change note dated 2026-10-16).
 *
 * THE REPLACEMENT is the route the site's own web player calls (read out of its live bundle). It needs
 * NO SIGNATURE at all -- see hg_request_play_info in hg_request.c -- which is why nothing in this
 * function touches the signer.
 *
 * WHY THE TIERS AND THE RETRIES ARE BOTH HERE, in one place, rather than at the call sites:
 *
 *   1. ONLY THREE DEFINITIONS EXIST. Measured across every tier: 360p / 480p / 720p answer, and
 *      540p / 1080p / 1080p+ / 2k / 4k answer 400 {"error":"protected_playback_unavailable"}. So the
 *      ladder is a fixed list of three, tried best first. In particular the app's saved "default 1080P"
 *      preference (build143) CANNOT be honoured on this route and must not be passed through: asking
 *      for 1080p is a guaranteed 400.
 *
 *   2. THE 403 IS INTERMITTENT, SO RETRYING IS THE FIX, not a workaround. MEASURED, the identical
 *      request repeated eight times: `403 200 200 403 200 403 403 403`, and on another run
 *      first measurement saw 20 of 76 episodes usable, and retrying the same set saw ~50 of 76. The budget
 *      is deliberately small (see the note below) because a handful of attempts already covers the
 *      observed jitter, and an episode the service will not serve never succeeds however often it is asked.
 *
 *   3. A FAILURE THAT SURVIVES THE BUDGET IS RELAYED, NOT EXPLAINED. MEASURED: some episodes fail on EVERY
 *      tier on EVERY attempt (in the 76-episode series that was measured, everything from index 50 onward),
 *      while earlier episodes succeed within a few tries -- so "budget exhausted" genuinely has two causes.
 *      But WHICH episodes those are is the service's business, it differs per series, and it is not a
 *      documented rule. Naming a cause in the UI would turn today's measurement into a promise the code
 *      cannot keep, so the server's own refusal text is what travels to the user. See the failure site.
 */
/* ---- THE REQUEST BUDGET (build173) ---------------------------------------------------------------
 *
 * THE SHAPE IS DICTATED BY MEASUREMENT, NOT BY TASTE. Two facts from the live service:
 *
 *   1. 720p IS THE BEST TIER THIS ROUTE OFFERS, and it is worth more attempts than the others because
 *      it is the one the user sees. Measured, 540p / 1080p / 1080p+ / 2k / 4k are ALL a stable 400 --
 *      retrying them is pure delay -- so only three tiers are ever asked for.
 *
 *   2. THE 403 IS INTERMITTENT. The SAME request repeated eight times came back
 *      `403 200 200 403 200 403 403 403` on one run and `200 200 403 403 403 403 200 403` on another,
 *      so a refusal usually means "this attempt landed on an unlucky backend", not "this device may not
 *      watch this". Retrying lifted reachability from 20/76 episodes to ~50/76 in the same series.
 *
 * So the budget is 6 tries for the preferred tier, then 3 and 3 for the fallbacks: 12 requests at most,
 * and the loop gives up early the moment a tier answers. The 300 ms gap is what makes consecutive
 * attempts land on different backends rather than inside one. Measured against the live endpoint, this
 * bounds the worst case at roughly four seconds -- long enough to ride out the jitter, short enough that
 * a genuinely unavailable episode fails while the user is still watching the spinner.
 *
 * build175 NARROWED WHEN THAT BUDGET IS SPENT. A DEFINITIVE refusal (the server understood the request
 * and answered protected_playback_unavailable) now ends a tier at the FIRST such answer instead of the
 * sixth, because repeating it cannot change it -- measured 0 of 12 across all three tiers. The budget
 * above still governs everything that is genuinely intermittent, which is what it was built for. The
 * worst case for a refused episode therefore fell from ~12 requests to 3 (one per tier), and the message
 * the user sees reports the count actually spent rather than the budget. */
#define HG_PLAY_INFO_TRIES_BEST 6     /* 720p -- the tier we actually want */
#define HG_PLAY_INFO_TRIES_REST 3     /* 480p, 360p -- fallbacks */
#define HG_PLAY_INFO_GAP_MS 300
#define HG_PLAY_INFO_TIERS 3
static const char* const HG_PLAY_INFO_DEFS[HG_PLAY_INFO_TIERS] = { "720p", "480p", "360p" };

/* The number of attempts for tier `t`, so the budget lives in one place and the log can print it. */
static int playInfoTriesFor(int tier) {
  return (tier == 0) ? HG_PLAY_INFO_TRIES_BEST : HG_PLAY_INFO_TRIES_REST;
}

/* ---- ONE TIER, RETRIED (build173) -----------------------------------------------------------------
 *
 * The single place a play-info request is actually issued, so the signing-less POST, the status policy,
 * the retry gap and the log lines exist once and not three times. Everything else -- the ladder walk in
 * fetchPlayInfoPlan, the menu probe in fetchPlayInfoTiers, the switch in fetchPlayInfoPlanForTier -- is
 * built on this.
 *
 * `def` is the definition to ask for and `tries` how many times; `outDetail` receives the service's own
 * last refusal text (or "" when the transport failed). Returns:
 *   HG_PLAY_INFO_OK        a plan was parsed into `out`
 *   HG_PLAY_INFO_REFUSED   every attempt was refused; try the next tier
 *   HG_PLAY_INFO_FATAL     a transport error or a refused CENC key; do NOT try other tiers, because the
 *                          next tier would fail for the same reason
 * A refused key is fatal on purpose: the URLs are good but playback would be a black screen with no
 * error anywhere, so continuing to another tier would hide the real fault.
 *
 * The caller holds g_clientMutex. */
#define HG_PLAY_INFO_OK 0
#define HG_PLAY_INFO_REFUSED 1
#define HG_PLAY_INFO_FATAL 2

/* ---- A SERVER REFUSAL IS NOT JITTER, AND MUST NOT BE RETRIED LIKE ONE (build175) -----------------
 *
 * THE USER'S OWN REPORT IS THE SPEC. On the console, an episode past the boundary produced:
 *
 *   Playback failed, unavailable from this source after 20 attempts over 3 quality tiers.
 *   The server said HTTP 403-42 bytes-{"error":"protected_playback_unavailable"}
 *
 * Two things are wrong with that, and both are in this file rather than in the service:
 *
 *   1. IT SPENT THE WHOLE BUDGET ON A NO. MEASURED 2026-10-16, exhaustively: such an episode answers
 *      403 on EVERY tier with EVERY attempt (0 of 12 across 720p/480p/360p), while an episode inside the
 *      boundary succeeds within a few tries. The retry loop exists for the INTERMITTENT 403 (the same
 *      request repeated 8 times gives `403 200 200 403 200 403 403 403`), and that intermittency is
 *      real -- but once the server has answered the same way on several tiers in a row, the budget is
 *      grinding through a decision, not riding out a flake. It costs ~4 seconds of spinner to reach an
 *      answer that was available almost immediately.
 *
 *   2. IT TOLD THE USER A MACHINE'S SENTENCE. "unavailable from this source after 20 attempts over 3
 *      quality tiers. The server said HTTP 403-42 bytes-{...}" is a diagnostic, not an explanation. The
 *      reason is knowable and specific: the service refused this episode for this device.
 *
 * SO: the body is classified. `protected_playback_unavailable` means the request was UNDERSTOOD and
 * REFUSED -- the service is not confused, so repeating it is pointless. That is a DEFINITIVE refusal and
 * stops the walk at once. Anything else non-200 (a 5xx, an empty body, a gateway error) stays on the
 * retry path, because that IS the intermittent case the budget was built for.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO. It does not say "free range", "paid", or any episode count. The
 * measured boundaries differ per series (6, 11, 12, 13 and 50 across five series sampled on 2026-10-16),
 * so the code cannot know the rule and must not imply one. It reports that the service refused, which is
 * the fact it actually has. */
static int playInfoBodyIsRefusal(const char *body) {
  /* The one token measured on this endpoint. A substring test is right here because the body is a tiny
   * JSON object and the field name has been stable across every sample. */
  if (body == NULL) return 0;
  return strstr(body, "protected_playback_unavailable") != NULL;
}

static int playInfoTryTier(const std::string& seriesId, const std::string& episodeId, const char* def,
                           int tries, HgPlayInfo& out, Result& err, char* outDetail,
                           size_t outDetailCap, int* outStatus, int* outRefusal) {
  int attempt;
  for (attempt = 1; attempt <= tries; attempt++) {
    uint32_t khronos = (uint32_t)time(nullptr);
    uint64_t ticket = (uint64_t)khronos * 1000ull;
    size_t blen = 0;
    long status = 0;

    int rc = hg_request_play_info(&g_req, seriesId.c_str(), episodeId.c_str(), def,
                                  hg_http_ttwid(), khronos, ticket, (uint32_t)attempt);
    if (rc != 0) {
      char b[128];
      snprintf(b, sizeof b, "build play-info request rc=%d", rc);
      err.error = b;
      return HG_PLAY_INFO_FATAL;
    }

    rc = hg_http_request(g_req.url, g_req.headers, g_req.body, g_req.body_len, g_page, sizeof g_page,
                         &blen, &status);
    if (rc != HG_HTTP_OK) {
      /* A transport failure is NOT retried: the socket, DNS or TLS is the problem, and repeating it
       * would just delay the same answer. It is not tier-specific either, so it is fatal. */
      char b[256];
      snprintf(b, sizeof b, "POST transport rc=%d (%s)", rc, hg_http_last_error());
      logFailureResponse("PLAY-INFO", b, status, g_page, strlen(g_page));
      err.error = b;
      return HG_PLAY_INFO_FATAL;
    }

    if (status == 200 && blen > 0) {
      int prc = hg_play_info_parse(g_page, blen, &out);
      if (prc == 0) {
        /* The definition and the expiry decide whether playback will still work in a minute; the audio
         * flag says whether sound is expected at all. */
        hg::logPrintf("PLAY-INFO [build173]: %s attempt %d/%d -> OK expires_at=%lld audio=%s "
                      "encrypted=%d codec=%s [build173]",
                      def, attempt, tries, (long long)out.expires_at, out.audio_url[0] ? "yes" : "NO",
                      out.encrypted, out.codec_type[0] ? out.codec_type : "(absent)");
        return HG_PLAY_INFO_OK;
      }
      if (prc == HG_PLAY_ERR_KEY) {
        char b[192];
        snprintf(b, sizeof b, "play_info parsed but the CENC key was refused (unknown method tag?)");
        logFailureResponse("PLAY-INFO", b, status, g_page, blen);
        err.error = b;
        return HG_PLAY_INFO_FATAL;
      }
      {
        char b[128];
        snprintf(b, sizeof b, "play_info parse rc=%d (see hg_play.h)", prc);
        logFailureResponse("PLAY-INFO", b, status, g_page, blen);
        err.error = b;
        return HG_PLAY_INFO_FATAL;
      }
    }

    /* Non-200 or an empty body. Keep the service's own words -- they are what travels to the user. */
    if (outStatus != NULL) *outStatus = (int)status;
    if (outDetail != NULL && outDetailCap > 0) {
      char head[120];
      size_t shown = 0;
      size_t i;
      for (i = 0; i < sizeof head - 1 && g_page[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)g_page[i];
        head[shown++] = (c < 0x20 || c == 0x7f) ? '.' : (char)c;
      }
      head[shown] = '\0';
      snprintf(outDetail, outDetailCap, "HTTP %ld | %u bytes | %s", status, (unsigned)blen,
               shown ? head : "(empty)");
    }
    hg::logPrintf("PLAY-INFO [build173]: %s attempt %d/%d -> %s%s [build173]", def, attempt, tries,
                  outDetail != NULL ? outDetail : "(refused)",
                  attempt < tries ? " (retrying)" : "");

    /* ---- A REFUSAL IS RECORDED, NOT OBEYED (build176 -- build175 got this wrong, and the console proved it) ---
     *
     * build175 RETURNED HERE, on the theory that a definitive refusal will answer the same way again. The
     * theory came from one measurement (0 of 12 on an episode past the boundary) and it was WRONG about the
     * episodes that matter: the user's report is that build175 left "many episodes under fifty unplayable",
     * while build174 -- which simply spent the whole retry budget -- played them. Inside the free window this
     * server refuses intermittently, exactly like it does everything else, and the refusal reason has nothing
     * to do with whether the next attempt would have worked.
     *
     * So the refusal is still DETECTED (it decides the wording the user sees, which is the half of build175
     * worth keeping) but it no longer ends the retry budget. The one-time line below says the server refused,
     * so the log still distinguishes "refused" from "could not reach the network"; the retrying decides
     * whether it was really final.
     *
     * `outRefusal` IS STICKY: once any attempt of this pass was a refusal, the caller reports the failure as a
     * refusal rather than as a network fault, which is what keeps the message honest even when the last
     * attempt happened to be a timeout. */
    if (playInfoBodyIsRefusal(g_page)) {
      if (outRefusal != NULL) *outRefusal = 1;
      if (attempt == 1) {
        hg::logPrintf("PLAY-INFO [build176]: %s answered with a refusal (%s) -- retrying anyway, this server "
                      "refuses intermittently inside the free window too [build176]",
                      def, outDetail != NULL ? outDetail : "(refused)");
      }
    }

    /* The gap is what makes consecutive attempts land on different backends, which is the whole reason
     * retrying works (see the budget note). Skipped after the last attempt of a tier. */
    if (attempt < tries) std::this_thread::sleep_for(std::chrono::milliseconds(HG_PLAY_INFO_GAP_MS));
  }
  return HG_PLAY_INFO_REFUSED;
}

Result Client::fetchPlayInfoPlan(const std::string& seriesId, const std::string& episodeId,
                                 HgPlayInfo& out) {
  std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
  Result r;

  if (!hg_api_valid_episode_id(seriesId.c_str())) {
    r.error = "series id is not [0-9]{1,30}";
    return r;
  }
  if (!hg_api_valid_episode_id(episodeId.c_str())) {
    r.error = "episode id is not [0-9]{1,30}";
    return r;
  }

  /* ---- THE LADDER WALK: BEST TIER FIRST, STOPPING AT THE FIRST THAT ANSWERS ------------------------
   *
   * 720p is tried hardest because it is the one the user wants (see the budget note above). A FATAL
   * result stops the walk immediately -- a transport failure or a refused key is not tier-specific, so
   * asking two more tiers would only add requests that fail the same way. A REFUSAL moves to the next
   * tier, which is the case the ladder exists for: the service does refuse individual tiers per request.
   *
   * A DEFINITIVE REFUSAL (build175) ends the CURRENT tier's retries at once but still lets the walk
   * continue to the other tiers, because the service refuses per tier and 720p can be refused while 360p
   * answers. What it no longer does is spend six attempts establishing what one already said. */
  char lastDetail[192];
  lastDetail[0] = '\0';
  int lastStatus = 0;
  int tier;
  int usedAttempts = 0;          /* how many requests were ACTUALLY spent, not the budget */
  int sawRefusal = 0;            /* at least one tier answered protected_playback_unavailable */
  Result transportErr;           /* set when the walk ended on a transport failure instead */
  int transportFailed = 0;

  /* ---- MAKE SURE THE DEVICE COOKIE IS IN HAND FIRST (build175) ------------------------------------
   *
   * The site issues ttwid on any plain GET and its own web client sends it back; hg_http.c captures it
   * from whatever response happens to carry it. On a cold start -- the app launched straight into the
   * player -- nothing may have been fetched yet, so this asks the home page ONCE.
   *
   * IT IS BEST-EFFORT AND MUST NOT BECOME A NEW FAILURE MODE. If the fetch fails, playback continues
   * without the cookie: measured, the episodes that answer 403 do so either way, so a missing cookie
   * cannot be the reason an episode will not play, and refusing to try would turn a cosmetic header
   * gap into a total outage. That is why the result of this request is not checked.
   *
   * IT IS ONLY ATTEMPTED WHEN THE COOKIE IS ABSENT. Once captured it lives for the session, so in the
   * normal case (home page visited, then an episode) this costs nothing at all. */
  if (hg_http_ttwid_len() == 0) {
    static const char* homeHeaders[] = {
      "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
      "Accept: text/html,application/xhtml+xml",
      nullptr
    };
    size_t hlen = 0;
    long hstatus = 0;
    int hrc = hg_http_request("https://hongguoduanju.com/", homeHeaders, nullptr, 0, g_page,
                              sizeof g_page, &hlen, &hstatus);
    /* LENGTH ONLY, NEVER THE TOKEN. A device identity in a log file that gets pasted into a chat is a
     * credential leak; the length is enough to tell "captured something" from "captured nothing". */
    hg::logPrintf("COOKIE [build175]: cold start, asked the home page for the device cookie: rc=%d "
                  "status=%ld bytes=%u -> ttwid %s (len=%u) [build175]",
                  hrc, hstatus, (unsigned)hlen,
                  hg_http_ttwid_len() > 0 ? "captured" : "NOT captured",
                  (unsigned)hg_http_ttwid_len());
  } else {
    hg::logPrintf("COOKIE [build175]: sending the captured device cookie (len=%u) [build175]",
                  (unsigned)hg_http_ttwid_len());
  }

  for (tier = 0; tier < HG_PLAY_INFO_TIERS; tier++) {
    int refusal = 0;
    const int planned = playInfoTriesFor(tier);
    int outcome = playInfoTryTier(seriesId, episodeId, HG_PLAY_INFO_DEFS[tier], planned, out, r,
                                  lastDetail, sizeof lastDetail, &lastStatus, &refusal);
    /* DEFINITIVE REFUSALS STOP THE TIER EARLY, so the budget is not what was spent -- the log and the
     * user-facing message must say the REAL count, which is why this is accumulated from `refusal`
     * rather than summed from the budget. */
    usedAttempts += (refusal && outcome == HG_PLAY_INFO_REFUSED) ? 1 : planned;

    if (outcome == HG_PLAY_INFO_OK) {
      r.ok = true;
      return r;
    }
    if (outcome == HG_PLAY_INFO_FATAL) {
      /* A transport failure is not tier-specific (see the note at the call), so the remaining tiers are
       * not asked. WHICH of the two terminal causes this is decides what the user is told, so it is
       * recorded rather than thrown away. */
      transportFailed = 1;
      transportErr = r;
      break;
    }
    if (refusal) sawRefusal = 1;
  }

  /* ---- THE WALK IS OVER, AND THE CAUSE DECIDES WHAT THE USER IS TOLD (build175) --------------------
   *
   * TWO OUTCOMES THAT USED TO BE ONE MESSAGE, AND SHOULD NOT BE:
   *
   *   * THE SERVER REFUSED. It understood the request and said no
   *     ({"error":"protected_playback_unavailable"} -- measured). The user needs to know the episode is
   *     not available to them, not how many HTTP requests were made. The reason is reported through the
   *     language table (hg_lang.h) so the UI says a sentence rather than a diagnostic, and the server's
   *     own words stay in the LOG where the next diagnosis needs them.
   *
   *   * THE NETWORK FAILED. Nothing was refused -- the request did not complete. That keeps its existing
   *     "network" wording, because telling the user an episode is unavailable when the console simply
   *     has no connection would be wrong in the opposite direction.
   *
   * NEITHER MESSAGE NAMES AN EPISODE COUNT OR A REASON THE CODE CANNOT SEE. The measured boundaries
   * differ per series (6, 11, 12, 13 and 50 across five series sampled on 2026-10-16), so any number here
   * would be today's coincidence presented as a rule. */
  if (transportFailed && !sawRefusal) {
    /* Pure transport: keep the old wording and the underlying reason. serverRefused stays FALSE, which
     * is what tells the UI to say "network" rather than "the service refused" -- the opposite mistake
     * would blame the service for the console having no connection. */
    logFailureResponse("PLAY-INFO", "every tier failed at the transport", lastStatus, g_page, 0);
    r = transportErr;
    r.serverRefused = false;
    return r;
  }

  {
    char b[384];
    if (sawRefusal) {
      /* Accurate, user-facing, and free of machine detail. The count is the real one. */
      snprintf(b, sizeof b,
               "the server refused this episode for this device (%d attempts, %d quality tiers)",
               usedAttempts, HG_PLAY_INFO_TIERS);
    } else {
      snprintf(b, sizeof b, "unavailable from this source after %d attempts over %d quality tiers",
               usedAttempts, HG_PLAY_INFO_TIERS);
    }
    /* THE SERVER'S OWN WORDS GO TO THE LOG, ALWAYS -- including the exact body and status, because the
     * message the user sees is now a sentence and would otherwise lose the evidence. */
    logFailureResponse("PLAY-INFO", "every tier and attempt failed", lastStatus, g_page, 0);
    r.error = b;
    /* The machine-readable half of the same fact, so the UI can pick its wording without parsing a
     * sentence (see Result::serverRefused). */
    r.serverRefused = (sawRefusal != 0);
    return r;
  }
}

/* ---- THE QUALITY MENU, BUILT FROM WHAT THIS ROUTE CAN ACTUALLY RETURN (build173) ------------------
 *
 * WHY NOT Client::fetchQualityLadder. That one reads the video_model response, which is the retired
 * endpoint, so it now returns nothing -- and a menu whose data source no longer answers is worse than no
 * menu, because the row is drawn from whatever was last parsed successfully.
 *
 * WHAT IT PROBES AND WHAT IT KEEPS. Each definition is asked for ONCE (not with the full retry budget:
 * this is a menu, and three tiers x six tries would be 18 requests just to label a row). A definition
 * that answers becomes an entry; one that is refused does not. That is what makes the menu honest -- it
 * cannot offer 1080p, because 1080p is not in the list of definitions asked for at all, and it cannot
 * offer a tier that just failed.
 *
 * A TIER THAT FAILS THE PROBE BUT WOULD SUCCEED LATER is possible -- the 403 is intermittent (measured),
 * so a one-shot probe can miss a tier that the retrying plan request would have taken. That is accepted
 * deliberately: the menu then lists fewer tiers than are playable, which under-promises rather than
 * offering a tier whose switch would fail in the user's face. The TIER ACTUALLY PLAYING is always shown
 * regardless (the caller sets it from the plan), so the row never misrepresents what is on screen.
 *
 * `out` empty with ok=true means "no menu", NOT a failure: the caller keeps playing. */
Result Client::fetchPlayInfoTiers(const std::string& seriesId, const std::string& episodeId,
                                  std::vector<QualityTier>& out) {
  std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
  Result r;
  out.clear();

  if (!hg_api_valid_episode_id(seriesId.c_str())) {
    r.error = "series id is not [0-9]{1,30}";
    return r;
  }
  if (!hg_api_valid_episode_id(episodeId.c_str())) {
    r.error = "episode id is not [0-9]{1,30}";
    return r;
  }

  int tier;
  for (tier = 0; tier < HG_PLAY_INFO_TIERS; tier++) {
    HgPlayInfo info;
    Result probeErr;
    char detail[192];
    detail[0] = '\0';

    int outcome = playInfoTryTier(seriesId, episodeId, HG_PLAY_INFO_DEFS[tier], 1, info, probeErr,
                                  detail, sizeof detail, NULL, NULL);
    if (outcome == HG_PLAY_INFO_OK) {
      QualityTier t;
      /* The label is the definition the service echoed back, so the menu shows the server's own name for
       * the tier rather than a string invented here. */
      t.label = info.definition[0] ? info.definition : HG_PLAY_INFO_DEFS[tier];
      /* sourceIndex carries the INDEX INTO THE DEFINITION LIST, which is what
       * fetchPlayInfoPlanForTier() takes. It is NOT an index into a video_model response -- that list
       * does not exist on this route, and reusing the field this way is why the switch path needed no
       * new plumbing. */
      t.sourceIndex = tier;
      t.width = info.vwidth;
      t.height = info.vheight;
      t.codec = info.codec_type;
      out.push_back(t);
    } else if (outcome == HG_PLAY_INFO_FATAL) {
      /* A transport failure is not per-tier, so there is nothing to learn from the remaining ones. Keep
       * whatever was already found and report the reason; the caller still has a working player. */
      if (out.empty()) {
        r.error = probeErr.error;
        return r;
      }
      break;
    }
    /* HG_PLAY_INFO_REFUSED: this tier is simply not on offer right now. Not an error -- skip it. */
  }

  hg::logPrintf("PLAY-INFO [build173]: quality menu built from play-info: %u of %d tiers available "
                "(1080p is not requested -- the endpoint refuses it) [build173]",
                (unsigned)out.size(), HG_PLAY_INFO_TIERS);
  r.ok = true;
  return r;
}

Result Client::fetchPlayInfoPlanForTier(const std::string& seriesId, const std::string& episodeId,
                                        int tierIndex, HgPlayInfo& out) {
  std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
  Result r;

  if (!hg_api_valid_episode_id(seriesId.c_str())) {
    r.error = "series id is not [0-9]{1,30}";
    return r;
  }
  if (!hg_api_valid_episode_id(episodeId.c_str())) {
    r.error = "episode id is not [0-9]{1,30}";
    return r;
  }
  /* Out of range is caught here rather than left to the array: an unclamped index would ask for the
   * wrong definition and play the wrong picture with no error at all. */
  if (tierIndex < -1 || tierIndex >= HG_PLAY_INFO_TIERS) {
    r.error = "quality tier index out of range";
    return r;
  }

  char lastDetail[192];
  lastDetail[0] = '\0';
  int lastStatus = 0;

  if (tierIndex >= 0) {
    /* ONE tier, the one asked for. No fallback: a user who picks 480p and gets 360p instead has been
     * silently disobeyed, and the previous behaviour on this screen was to report the failure and keep
     * the old stream playing -- which is what happens here too. */
    int outcome = playInfoTryTier(seriesId, episodeId, HG_PLAY_INFO_DEFS[tierIndex],
                                  playInfoTriesFor(tierIndex), out, r, lastDetail, sizeof lastDetail,
                                  &lastStatus, NULL);
    if (outcome == HG_PLAY_INFO_OK) {
      r.ok = true;
      return r;
    }
    if (outcome == HG_PLAY_INFO_FATAL) return r;
    /* A hand-picked tier that the server refuses gets the same plain sentence as a whole-episode
     * refusal would, rather than a diagnostic -- the user asked for this tier and the answer is no. */
    r.serverRefused = (playInfoBodyIsRefusal(g_page) != 0);
    r.error = r.serverRefused
                  ? std::string("the server refused this quality for this device")
                  : (lastDetail[0] != '\0' ? std::string("that quality is unavailable: ") + lastDetail
                                           : std::string("that quality is unavailable from this source"));
    return r;
  }

  /* tierIndex == -1: "the best that answers", i.e. the same walk a fresh episode does. */
  return fetchPlayInfoPlan(seriesId, episodeId, out);
}

std::string Client::describePlan(const HgPlayPlan& plan) {
    char b[512];
    snprintf(b, sizeof b, "%s %dx%d  codec=%s  %s",
             plan.definition[0] ? plan.definition : "?",
             plan.vwidth, plan.vheight,
             plan.codec_type[0] ? plan.codec_type : "(absent)",
             plan.encrypted ? "CENC key derived" : "not encrypted");
    return std::string(b);
}

/* ---- the quality ladder ---------------------------------------------------
 *
 * Both of these re-fetch the video model. That is one request each, and it is the right trade:
 * the response carries SIGNED URLs with a limited life, so anything cached would be the wrong
 * thing to switch, decode or play a few minutes later. The measured response is ~13 KB. */

/* One request of the video model, shared by the two entry points below so the signing, the POST
 * and the status policy exist in exactly one place. */
static int postVideoModel(const std::string& episodeId, size_t& blen, std::string& err) {
    uint32_t khronos = (uint32_t)time(nullptr);
    uint64_t ticket = (uint64_t)khronos * 1000ull;

    blen = 0;
    int rc = hg_request_video_model(&g_req, episodeId.c_str(), khronos, ticket, 1u);
    if (rc != 0) {
        char b[128];
        snprintf(b, sizeof b, "build video-model request rc=%d", rc);
        err = b;
        return -1;
    }

    long status = 0;
    rc = hg_http_request(g_req.url, g_req.headers, g_req.body, g_req.body_len, g_page,
                         sizeof g_page, &blen, &status);
    if (rc != HG_HTTP_OK) {
        char b[256];
        snprintf(b, sizeof b, "POST transport rc=%d (%s)", rc, hg_http_last_error());
        logFailureResponse("VIDEO-MODEL", b, status, g_page, strlen(g_page));
        err = b;
        return -1;
    }
    if (status != 200) {
        char b[96];
        snprintf(b, sizeof b, "POST HTTP %ld", status);
        logFailureResponse("VIDEO-MODEL", b, status, g_page, blen);
        err = b;
        return -1;
    }
    if (blen == 0) {
        logFailureResponse("VIDEO-MODEL", "POST 200 with 0 bytes", status, g_page, 0);
        err = "POST 200 with 0 bytes (see docs: the dead endpoint does this)";
        return -1;
    }
    return 0;
}

Result Client::fetchQualityLadder(const std::string& episodeId, std::vector<QualityTier>& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;
    out.clear();

    if (!hg_api_valid_episode_id(episodeId.c_str())) {
        r.error = "episode id is not [0-9]{1,30}";
        return r;
    }

    size_t blen = 0;
    std::string err;
    if (postVideoModel(episodeId, blen, err) != 0) {
        r.error = err;
        return r;
    }

    /* The response is DOUBLY ENCODED: data[vid].video_model is a JSON *string* that has to be
     * parsed a second time before video_list is reachable. The unwrapping is shared with the
     * plan path rather than repeated here -- see the note in hg_play.h about the version of
     * this project that looked for video_list at the top level and could never find it. */
    const char* inner = nullptr;
    size_t innerLen = 0;
    if (hg_play_model_inner(g_page, blen, &inner, &innerLen) != 0) {
        logFailureResponse("LADDER", "no video_model in the response", 200, g_page, blen);
        r.error = "no video_model in the response (see hg_play.h)";
        return r;
    }

    static HgVideoSource src[HG_API_MAX_SOURCES];
    size_t count = 0;
    int rc = hg_api_parse_video_model(inner, innerLen, src, HG_API_MAX_SOURCES, &count);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_video_model rc=%d", rc);
        logFailureResponse("LADDER", b, 200, g_page, blen);
        r.error = b;
        return r;
    }

    /* The parallel arrays hg_quality_build takes. Sized by HG_QUALITY_MAX, which matches
     * hg_api.h's own ceiling on the source list, so the ladder can never be longer. */
    int qt[HG_QUALITY_MAX];
    const char* def[HG_QUALITY_MAX];
    int w[HG_QUALITY_MAX];
    int h[HG_QUALITY_MAX];
    int br[HG_QUALITY_MAX];
    long long sz[HG_QUALITY_MAX];
    int dec[HG_QUALITY_MAX];
    /* THE CODEC PER SOURCE, for the tier label: see HgQualityTier::codec for the measurement that made this a
     * display rather than a choice. */
    const char* cod[HG_QUALITY_MAX];
    size_t n = count;
    if (n > HG_QUALITY_MAX) n = HG_QUALITY_MAX;

    for (size_t i = 0; i < n; i++) {
        qt[i] = src[i].has_quality_type ? src[i].quality_type : 0;
        def[i] = src[i].definition;
        w[i] = src[i].vwidth;
        h[i] = src[i].vheight;
        br[i] = src[i].bitrate;
        sz[i] = src[i].size_bytes;
        /* Every source hg_api_parse_video_model returns is decodable by construction: it
         * FILTERS bytevc2 rather than flagging it, so an undecodable row never reaches here.
         * Passing 1 states that rather than re-deriving it. */
        dec[i] = 1;
        /* `codec_type` is EMPTY when upstream omitted it, so no separate presence flag is needed here. */
        cod[i] = src[i].codec_type;
    }

    HgQualityTier tiers[HG_QUALITY_MAX];
    int tn = hg_quality_build(qt, def, w, h, br, sz, dec, cod, (int)n, tiers, HG_QUALITY_MAX);
    if (tn <= 0) {
        logFailureResponse("LADDER", "the video model carried no usable quality tiers", 200, g_page,
                           blen);
        r.error = "the video model carried no usable quality tiers";
        return r;
    }

    out.reserve((size_t)tn);
    for (int i = 0; i < tn; i++) {
        QualityTier t;
        char label[48];
        hg_quality_label(&tiers[i], label, (int)sizeof label);
        t.label = label;
        t.qualityType = tiers[i].quality_type;
        t.width = tiers[i].width;
        t.height = tiers[i].height;
        t.codec = tiers[i].codec;
        t.bitrate = tiers[i].bitrate;
        t.sizeBytes = tiers[i].size_bytes;
        t.sourceIndex = tiers[i].source_index;
        out.push_back(t);
    }

    r.ok = true;
    return r;
}

Result Client::fetchPlayPlanForSource(const std::string& episodeId, int sourceIndex,
                                      HgPlayPlan& out) {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    Result r;

    if (!hg_api_valid_episode_id(episodeId.c_str())) {
        r.error = "episode id is not [0-9]{1,30}";
        return r;
    }
    if (sourceIndex < 0 || sourceIndex >= (int)HG_API_MAX_SOURCES) {
        /* Caught here rather than left to the parser: an out-of-range index would otherwise
         * build a plan from an uninitialised entry -- a plausible-looking plan for the wrong
         * stream, which is a wrong picture and no error at all. */
        r.error = "quality source index out of range";
        return r;
    }

    size_t blen = 0;
    std::string err;
    if (postVideoModel(episodeId, blen, err) != 0) {
        r.error = err;
        return r;
    }

    const char* inner = nullptr;
    size_t innerLen = 0;
    if (hg_play_model_inner(g_page, blen, &inner, &innerLen) != 0) {
        logFailureResponse("PLAN-BY-TIER", "no video_model in the response", 200, g_page, blen);
        r.error = "no video_model in the response (see hg_play.h)";
        return r;
    }

    static HgVideoSource src[HG_API_MAX_SOURCES];
    size_t count = 0;
    int rc = hg_api_parse_video_model(inner, innerLen, src, HG_API_MAX_SOURCES, &count);
    if (rc < 0) {
        char b[128];
        snprintf(b, sizeof b, "parse_video_model rc=%d", rc);
        logFailureResponse("PLAN-BY-TIER", b, 200, g_page, blen);
        r.error = b;
        return r;
    }
    if (sourceIndex >= (int)count) {
        /* The list SHRANK between the ladder fetch and this one. Saying so is the point: the
         * alternative is building a plan from a stale index and playing something else. */
        char b[160];
        snprintf(b, sizeof b, "quality index %d is past the end of the %u sources offered now",
                 sourceIndex, (unsigned)count);
        logFailureResponse("PLAN-BY-TIER", b, 200, g_page, blen);
        r.error = b;
        return r;
    }

    rc = hg_play_plan(src[sourceIndex].url, src[sourceIndex].key_seed, src[sourceIndex].spade_a,
                      src[sourceIndex].codec_type, &out);
    if (rc != 0) {
        char b[128];
        snprintf(b, sizeof b, "play_plan rc=%d (see hg_play.h)", rc);
        logFailureResponse("PLAN-BY-TIER", b, 200, g_page, blen);
        r.error = b;
        return r;
    }
    /* hg_play_plan knows nothing about the ladder's fields, so they are carried across here.
     * Without this the overlay would report the new stream's codec but no resolution, and the
     * quality row could not tell which tier it was on. */
    out.quality_type = src[sourceIndex].has_quality_type ? src[sourceIndex].quality_type : 0;
    out.has_quality_type = src[sourceIndex].has_quality_type;
    if (src[sourceIndex].definition[0]) {
        snprintf(out.definition, sizeof out.definition, "%s", src[sourceIndex].definition);
    }
    if (src[sourceIndex].vwidth) out.vwidth = src[sourceIndex].vwidth;
    if (src[sourceIndex].vheight) out.vheight = src[sourceIndex].vheight;
    if (src[sourceIndex].bitrate) out.bitrate = src[sourceIndex].bitrate;
    if (src[sourceIndex].size_bytes) out.size_bytes = src[sourceIndex].size_bytes;

    r.ok = true;
    return r;
}

/* ---- the network probe ---------------------------------------------------
 *
 * Every stage of one home-page fetch, reported separately. See NetworkProbe in the
 * header for why each field is here; this function is deliberately a straight line
 * through the existing transport rather than a second code path, because a
 * diagnostic that uses different code from the thing it diagnos`es can be green
 * while the real path is broken. */
static std::string dateHeaderValue() {
    const char* h = hg_http_last_headers();
    if (!h) return std::string();
    /* Case-insensitive search for "date:" at the start of a header line. Written by
     * hand rather than with strcasestr, which is a GNU extension this toolchain does
     * expose but which would be one more thing to verify. */
    std::string headers(h);
    size_t lineStart = 0;
    while (lineStart < headers.size()) {
        size_t lineEnd = headers.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = headers.size();
        std::string line = headers.substr(lineStart, lineEnd - lineStart);
        /* Strip a trailing CR, because HTTP header lines end CRLF. */
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 5) {
            std::string key = line.substr(0, 5);
            for (size_t i = 0; i < key.size(); i++) {
                if (key[i] >= 'A' && key[i] <= 'Z') key[i] = (char)(key[i] - 'A' + 'a');
            }
            if (key == "date:") {
                size_t v = line.find(':');
                if (v != std::string::npos) {
                    std::string val = line.substr(v + 1);
                    size_t s = val.find_first_not_of(" \t");
                    if (s != std::string::npos) return val.substr(s);
                }
            }
        }
        lineStart = lineEnd + 1;
    }
    return std::string();
}

/* Days since 1970-01-01 for a civil date. Howard Hinnant's days_from_civil, which is
 * exact for every date in range and needs no table. Used instead of timegm(), which is
 * a GNU extension rather than C99 and would have to be verified present on this
 * toolchain before it could be trusted. */
static long daysFromCivil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097L + (long)doe - 719468L;
}

/* Parse an RFC 1123 Date header ("Wed, 21 Oct 2015 07:28:00 GMT") into a Unix time.
 * Returns 0 when the string is not in that form -- 0 rather than a guess, because a
 * fabricated server time would make the clock-skew line lie. */
static time_t parseHttpDate(const std::string& s) {
    static const char* const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    /* Skip "Www, " -- the weekday is redundant with the date and is not checked, so a
     * server that spells it differently still parses. */
    size_t comma = s.find(',');
    const char* p = (comma != std::string::npos && comma < 10) ? s.c_str() + comma + 1
                                                              : s.c_str();
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[8] = {0};
    if (sscanf(p, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
    if (day < 1 || day > 31 || year < 1970 || hh < 0 || hh > 23 || mm < 0 || mm > 59 ||
        ss < 0 || ss > 60) {
        return 0;
    }
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, MONTHS[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0) return 0;
    long days = daysFromCivil((long)year, (unsigned)month, (unsigned)day);
    return (time_t)(days * 86400L + (long)hh * 3600L + (long)mm * 60L + (long)ss);
}

NetworkProbe Client::networkProbe() {
    std::lock_guard<std::recursive_mutex> lock(g_clientMutex);
    NetworkProbe p;
    std::string err;

    time_t t0 = time(nullptr);

    /* STAGE 1: the transport. A non-zero curl code means nothing was ever spoken to. */
    size_t len = httpGet(HOME_URL, err);
    int curlCode = hg_http_last_curl_code();
    p.transportRc = curlCode;
    if (len == 0) {
        /* httpGet returns 0 for BOTH a transport failure and a non-2xx status, and
         * those have completely different fixes, so the reason is read back from the
         * message it produced rather than re-guessed. */
        if (err.find("HTTP ") == 0) {
            p.httpStatus = strtol(err.c_str() + 5, nullptr, 10);
        } else {
            p.transportError = hg_http_last_error();
            p.transportRc = curlCode != 0 ? curlCode : -2;
        }
        p.error = err.empty() ? "empty response" : err;
        p.msElapsed = (int)((time(nullptr) - t0) * 1000);
        return p;
    }
    p.httpStatus = 200;
    p.bodyBytes = len;
    /* The Date header is read from the SAME response, before anything else issues a
     * request and overwrites hg_http's "last" state. */
    p.serverDate = dateHeaderValue();

    /* STAGE 2: is this the page we expect? Reuses the body already in memory instead
     * of fetching again -- a probe that issued its own second request could be green
     * while the path it is meant to diagnose was the one that failed. */
    int routerLen = hg_router_extract(g_page, len, g_router, sizeof g_router);
    p.routerBytes = routerLen;
    if (routerLen < 0) {
        char b[160];
        snprintf(b, sizeof b, "page loaded (%u bytes) but no _ROUTER_DATA (rc=%d)", (unsigned)len,
                 routerLen);
        p.error = b;
        p.msElapsed = (int)((time(nullptr) - t0) * 1000);
        return p;
    }

    /* STAGE 3: the app's own parser, on the blob just extracted. This is the number
     * the home screen would show. */
    {
        std::vector<Series> cards;
        Result rr = Client::fetchHome(cards);
        if (!rr.ok) {
            p.error = "router ok but parse failed: " + rr.error;
            p.msElapsed = (int)((time(nullptr) - t0) * 1000);
            return p;
        }
        p.cardCount = cards.size();
    }

    /* STAGE 4: the clock, if the server dated its response. Unknown rather than zero
     * when it did not -- a fabricated "0 seconds" reads as health. */
    if (!p.serverDate.empty()) {
        time_t server = parseHttpDate(p.serverDate);
        if (server > 0) {
            p.clockSkewSeconds = (long)(server - time(nullptr));
            p.clockSkewKnown = true;
        }
    }

    p.ok = true;
    p.msElapsed = (int)((time(nullptr) - t0) * 1000);
    return p;
}

} // namespace hg
