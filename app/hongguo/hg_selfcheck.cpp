/*
 * hg_selfcheck implementation. See hg_selfcheck.hpp for why this exists and why it runs on failure
 * rather than at startup.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_selfcheck.hpp"

#include "hg_client.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>

/* The transport, called directly because that is what the client itself calls (hg_client.cpp's
 * fetchRouter/postVideoModel go through this same function). A probe that used its own HTTP stack could
 * be green while the real path is broken, which is the one thing a self check must not be.
 * hg_tables.h supplies HG_VIDEO_UA: the CDN is served to that user agent, so the Range request below
 * carries it exactly as mpv's own headers do (hg_play.c builds them from the same table). */
#include "hg_http.h"
#include "hg_tables.h"

namespace hg {

namespace {

std::mutex g_mutex;
PlayChainReport g_last;
bool g_haveLast = false;
PlayChainStatus g_status = PLAY_CHAIN_UNKNOWN;
uint64_t g_lastRunMs = 0;

/* One minute. See the note on startPlayChainCheck in the header. */
const uint64_t CHECK_COOLDOWN_MS = 60000ull;

uint64_t nowMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

std::string dec(long long v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    return std::string(b);
}

std::string shorten(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "...";
}

/* The Range request's body, kept in a static because it is up to 4 KiB and this runs on a worker thread
 * with a small stack. Single-threaded use: runPlayChainCheck is serialised by the cooldown and by the
 * client's own mutex. */
char g_streamHead[4096];

} // namespace

/* The stages, in the order the player meets them. Each lambda returns true when the stage worked and
 * leaves its measurement in `detail`. */
static bool stageHome(std::string& detail, std::vector<Series>& series) {
    Result r = Client::fetchHome(series);
    if (!r.ok) {
        detail = r.error;
        return false;
    }
    detail = dec((long long)series.size()) + " cards";
    return !series.empty();
}

static bool stageDetail(const std::vector<Series>& series, std::string& detail,
                        std::string& probeEp) {
    if (series.empty()) {
        detail = "skipped (no series to open)";
        return false;
    }
    std::vector<Episode> eps;
    Result r = Client::fetchEpisodes(series[0].id, eps);
    if (!r.ok) {
        detail = "series " + series[0].id + ": " + r.error;
        return false;
    }
    if (eps.empty()) {
        detail = "the detail page carried no episode id";
        return false;
    }
    probeEp = eps[0].id;
    detail = "series " + series[0].id + " -> " + dec((long long)eps.size()) + " episodes, first " +
             probeEp;
    return true;
}

static bool stageModel(const std::string& ep, std::string& detail, HgPlayPlan& plan) {
    Result r = Client::fetchPlayPlan(ep, plan);
    if (!r.ok) {
        /* THE SERVER'S OWN BYTES, not a paraphrase of them. This is the line that identified the
         * retired endpoint: a 125-byte {"Code":110001} envelope. */
        detail = r.error;
        std::string measured = Client::lastFailureDetail();
        if (!measured.empty()) detail += " | " + measured;
        return false;
    }
    detail = Client::describePlan(plan);
    return true;
}

static bool stageLadder(const std::string& ep, std::string& detail) {
    std::vector<Client::QualityTier> tiers;
    Result r = Client::fetchQualityLadder(ep, tiers);
    if (!r.ok) {
        detail = r.error;
        std::string measured = Client::lastFailureDetail();
        if (!measured.empty()) detail += " | " + measured;
        return false;
    }
    detail = dec((long long)tiers.size()) + " tiers";
    if (!tiers.empty()) detail += ", first '" + tiers[0].label + "'";
    return !tiers.empty();
}

/* THE LAST LINK: does the URL we resolved actually serve a video? Four bytes prove more than a status
 * code here -- an error page also answers 200 -- so the check asks for 64 bytes and looks for "ftyp". */
static bool stageStream(const HgPlayPlan& plan, std::string& detail) {
    if (plan.url[0] == '\0') {
        detail = "no resolved URL to fetch";
        return false;
    }
    const char* headers[] = {"Range: bytes=0-63", "Referer: https://novel.snssdk.com/",
                             "User-Agent: " HG_VIDEO_UA, nullptr};
    size_t len = 0;
    long status = 0;
    int rc = hg_http_request(plan.url, headers, nullptr, 0, g_streamHead, sizeof g_streamHead, &len,
                             &status);
    if (rc != HG_HTTP_OK) {
        detail = "transport rc=" + dec(rc) + " (" + std::string(hg_http_last_error()) + ")";
        return false;
    }
    const bool ftyp = (len >= 8) && (memcmp(g_streamHead + 4, "ftyp", 4) == 0);
    detail = dec(status) + ", " + dec((long long)len) + " bytes";
    detail += ftyp ? ", ftyp present" : ", NO ftyp";
    return ftyp && (status == 200 || status == 206);
}

/* WHAT IT MEANS, in one line the user can act on. Built from which stage broke, because that is the
 * whole point of separating them: a network failure and a changed endpoint look identical to "play does
 * not work" and need different responses. */
static std::string headlineFor(const PlayChainReport& rep) {
    if (rep.ok) {
        return "Play check OK: network, play interface, plan, ladder and stream all answered";
    }
    if (rep.stages.empty()) return "Play check did not run";

    /* Each stage, named with what its failure actually implies. */
    if (rep.stages[0].name == "home page") {
        return "Play check: NETWORK -- the home page did not load, so nothing else was tried: " +
               shorten(rep.stages[0].detail, 90);
    }
    for (size_t i = 0; i < rep.stages.size(); i++) {
        const CheckStage& s = rep.stages[i];
        if (s.ok) continue;
        if (s.name == "detail page") {
            return "Play check: the site's pages load but the episode list does not (" +
                   shorten(s.detail, 80) + ")";
        }
        if (s.name == "video model") {
            /* The case this whole module was written for. */
            if (s.detail.find("HTTP 200") != std::string::npos) {
                return "Play check: THE PLAY INTERFACE ANSWERED 200 WITH NO PLAYABLE SOURCE -- " +
                       shorten(s.detail, 150);
            }
            return "Play check: the play request itself failed (" + shorten(s.detail, 120) + ")";
        }
        if (s.name == "quality ladder") {
            return "Play check: the plan works but the quality list does not (" +
                   shorten(s.detail, 100) + ")";
        }
        if (s.name == "stream") {
            return "Play check: interface fine, STREAM REFUSED (" + shorten(s.detail, 100) + ")";
        }
        return "Play check FAILED at '" + s.name + "': " + shorten(s.detail, 120);
    }
    return "Play check FAILED";
}

PlayChainStatus playChainStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void notePlayChainWorked(const char* what) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = (g_status != PLAY_CHAIN_OK);
        g_status = PLAY_CHAIN_OK;
    }
    /* One line, and only when it changes: this is called once per episode, and a line per episode saying
     * the light is still green is the kind of logging that makes a log unreadable. */
    if (changed) {
        logPrintf("PLAYCHECK: status light -> GREEN (%s) [build158]", what ? what : "playback worked");
    }
}

/* The body, under a name of its own so that ONE place publishes the result whatever asked for it --
 * the player's automatic run and the settings screen's manual one both come through the wrapper
 * below. Publishing inside the body would mean five return paths each remembering to do it. */
static PlayChainReport runPlayChainCheckInner(const std::string& trigger,
                                             const std::string& episodeId);

PlayChainReport runPlayChainCheck(const std::string& trigger, const std::string& episodeId) {
    PlayChainReport rep = runPlayChainCheckInner(trigger, episodeId);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = rep.ok ? PLAY_CHAIN_OK : PLAY_CHAIN_FAILED;
        g_last = rep;
        g_haveLast = true;
    }
    logPrintf("PLAYCHECK: status light -> %s", rep.ok ? "GREEN" : "RED");
    return rep;
}

static PlayChainReport runPlayChainCheckInner(const std::string& trigger,
                                             const std::string& episodeId) {
    PlayChainReport rep;
    rep.trigger = trigger;
    rep.episodeId = episodeId;
    const uint64_t t0 = nowMs();

    logPrintf("PLAYCHECK: ==== play chain check, triggered by: %s [build157] ====", trigger.c_str());
    if (!episodeId.empty()) {
        logPrintf("PLAYCHECK: the episode the player was asked for: %s", episodeId.c_str());
    }

    /* SET BY THE STREAM STAGE BELOW WHEN A FRESH ADDRESS FIXED IT (build171). It exists so the final
     * headline can say "repaired" instead of the generic "everything answered" -- see the note there. A
     * local rather than a field on PlayChainReport, because the report's shape is published in
     * hg_selfcheck.hpp and this round's file boundary does not allow that header to change. */
    bool repairedStream = false;

    /* Every stage records a line as it finishes, so the log is useful even if a later stage hangs or the
     * process dies -- the last PLAYCHECK line is then the answer. */
    auto record = [&](const char* name, bool ok, const std::string& detail) {
        CheckStage s;
        s.name = name;
        s.ok = ok;
        s.detail = detail;
        rep.stages.push_back(s);
        logPrintf("PLAYCHECK: %u %-14s %s  %s", (unsigned)rep.stages.size(), name, ok ? "OK  " : "FAIL",
                  detail.c_str());
    };

    std::vector<Series> series;
    {
        std::string detail;
        const bool ok = stageHome(detail, series);
        record("home page", ok, detail);
        if (!ok) {
            /* NOTHING ELSE IS WORTH TRYING: every later stage needs the network this one just failed to
             * use, and a chain of five network failures hides the one fact that matters. */
            rep.msElapsed = (int)(nowMs() - t0);
            rep.headline = headlineFor(rep);
            logPrintf("PLAYCHECK: RESULT FAIL -- %s (%d ms)", rep.headline.c_str(), rep.msElapsed);
            return rep;
        }
    }

    std::string probeEp;
    {
        std::string detail;
        const bool ok = stageDetail(series, detail, probeEp);
        record("detail page", ok, detail);
        if (!ok) {
            rep.msElapsed = (int)(nowMs() - t0);
            rep.headline = headlineFor(rep);
            logPrintf("PLAYCHECK: RESULT FAIL -- %s (%d ms)", rep.headline.c_str(), rep.msElapsed);
            return rep;
        }
    }

    HgPlayPlan plan;
    {
        std::string detail;
        const bool ok = stageModel(probeEp, detail, plan);
        record("video model", ok, detail);
        if (!ok) {
            rep.msElapsed = (int)(nowMs() - t0);
            rep.headline = headlineFor(rep);
            logPrintf("PLAYCHECK: RESULT FAIL -- %s (%d ms)", rep.headline.c_str(), rep.msElapsed);
            return rep;
        }
    }

    {
        std::string detail;
        const bool ok = stageLadder(probeEp, detail);
        record("quality ladder", ok, detail);
        /* The ladder failing does not stop the stream check: an episode with one usable source is
         * playable, and proving the stream works is the more useful half of that answer. */
    }

    {
        std::string detail;
        const bool ok = stageStream(plan, detail);
        record("stream", ok, detail);
        /* THE STAGE'S OWN INDEX, NAMED RATHER THAN ASSUMED. `record` above is the last push, so .back()
         * would work today -- and would silently point at the wrong stage the first time anything is
         * recorded after it. The repair below must correct THIS stage and no other. */
        const size_t streamStage = rep.stages.size() - 1;

        /* ---- AND IF THE STREAM IS THE ONE THAT FAILED, ASK FOR A NEW ADDRESS AND TRY AGAIN (build171) ----
         *
         * THE USER'S REPORT, IN THEIR WORDS: "the source check found the source was abnormal, but it did
         * not repair itself with a new address." This is that repair, on the self-check side.
         *
         * WHY THE STREAM STAGE SPECIFICALLY. The five stages before it are all healthy when this fires --
         * the home page parsed, the episode list parsed, the signed video-model request answered, the plan
         * built and the ladder arrived. So the request path, the signature and the parser are all PROVEN to
         * work by the lines directly above, and the only remaining way for the last link to fail is the URL
         * itself. A URL that is refused is a URL that has expired: measured on this console as
         *
         *     MPVLOG [ffmpeg/warn]  https: HTTP error 403 Forbidden
         *     MPVLOG [stream/error] Failed to open https://v5-bd-daily-reading-video.qznovelvod.com/...
         *
         * and a 403 is the server refusing the request rather than the network failing, so the same bytes
         * will be refused forever. Fetching the plan again is what produces a newly signed address -- there
         * is no separate renew entry point, and Client::fetchPlayPlan is the one the player itself uses.
         *
         * IT IS NOT A SECOND FULL CHAIN RUN. Walking home page and detail page again would re-prove stages
         * that have just been proven, cost three more requests against a service that rate-limits, and bury
         * the one fact that matters. Exactly the two steps that can have changed are repeated: a fresh plan,
         * and a Range request for the first bytes of the address it carries. That is why the retry is
         * written here rather than by re-entering the chain from the top.
         *
         * ONE RETRY, matching the player's own budget for the same event and the spirit of build151's
         * single transparent retry. If a freshly signed address is also refused, the episode is genuinely
         * gone and saying so is more useful than asking a third time. */
        if (!ok) {
            logPrintf("PLAYCHECK: stream stage failed -> re-fetching a fresh address and re-running the "
                      "chain [build171]");

            HgPlayPlan fresh;
            Result rFresh = Client::fetchPlayPlan(probeEp, fresh);
            if (!rFresh.ok) {
                /* NO NEW ADDRESS WAS OBTAINED, so the failure being reported is still the original stream
                 * failure -- the retry never got far enough to have an opinion. The stage detail recorded
                 * above is left untouched, and this line says why no verdict replaced it. */
                std::string why = rFresh.error;
                std::string measured = Client::lastFailureDetail();
                if (!measured.empty()) why += " | " + measured;
                logPrintf("PLAYCHECK: repair -> FAIL (no fresh address could be fetched: %s) [build171]",
                          why.c_str());
            } else {
                std::string detail2;
                const bool ok2 = stageStream(fresh, detail2);
                /* THE VERDICT LINE THE USER ASKED FOR, with the measurement attached: "PASS" or "FAIL",
                 * and what the freshly signed address actually answered. Both halves are needed -- a bare
                 * PASS would not say what the new address returned, and a bare FAIL would not distinguish
                 * "still refused" from "the new plan carried no URL at all". */
                logPrintf("PLAYCHECK: repair -> %s (with a fresh address) [build171]  %s",
                          ok2 ? "PASS" : "FAIL", detail2.c_str());

                if (ok2) {
                    /* THE WHOLE POINT OF THE REPAIR: the chain is green again, and the red dot the user
                     * was looking at was a symptom of a stale URL rather than of anything broken. Both the
                     * stage and the report are corrected, so the settings screen shows the truth and the
                     * status light -- which runPlayChainCheck sets from rep.ok -- turns green.
                     *
                     * The ORIGINAL failure is not erased: the line recorded above still says what the
                     * first address answered, and the repair line says it was replaced. That keeps the
                     * evidence of "the URL expires" in the log while the verdict reflects the recovery. */
                    rep.stages[streamStage].ok = true;
                    rep.stages[streamStage].detail = detail2 + " (repaired with a fresh address)";
                    repairedStream = true;
                    logPrintf("PLAYCHECK: stage 6 stream is now OK on the replacement address [build171]");
                } else {
                    /* STILL BROKEN WITH A NEW SIGNATURE, which is a different and more serious finding
                     * than a stale URL: the address is not the problem. The stage keeps the ORIGINAL
                     * detail (the first failure) and the repair line above carries the second measurement,
                     * so a reader sees both and can tell the two apart. */
                    logPrintf("PLAYCHECK: the freshly signed address was refused as well -- the stream "
                              "itself is the problem, not the signature [build171]");
                }
            }
        }
    }

    rep.ok = true;
    for (size_t i = 0; i < rep.stages.size(); i++) {
        if (!rep.stages[i].ok) rep.ok = false;
    }
    rep.msElapsed = (int)(nowMs() - t0);
    rep.headline = headlineFor(rep);

    /* ---- A REPAIRED RUN DOES NOT GET TO SAY "ALL ANSWERED" (build171) -----------------------------------
     *
     * The user asked for the report to distinguish the two outcomes in as many words: repaired
     * successfully, or still failing after the repair. The generic OK headline above would quietly claim the
     * first address worked, which it did not -- it was refused and replaced. That is a half-truth of exactly
     * the kind this project's logs are supposed to be free of, and the whole point of this round is that the
     * user could SEE what happened.
     *
     * The status light stays GREEN, because it is reporting whether playback is in a working state now, and
     * after a successful repair it is. What changes is the sentence: it names the repair, keeps the original
     * refusal visible, and -- because rep ok reflects the REPAIRED stage -- the FAIL form below still
     * applies when the fresh address was refused too. */
    if (rep.ok && repairedStream) {
        rep.headline = "Play check OK, after self-repair: the stream's address had expired, a freshly "
                       "signed address was fetched and the stream answered";
    }

    logPrintf("PLAYCHECK: RESULT %s -- %s (%d ms)", rep.ok ? "PASS" : "FAIL", rep.headline.c_str(),
              rep.msElapsed);
    if (!rep.ok) {
        /* Where the NEXT round starts, so nobody re-derives it: the stage detail above IS the evidence,
         * and it is the same shape of argument that identified the retired endpoint in build156
         * (docs/新播放接口-第21轮.md). */
        logPrintf("PLAYCHECK: the measured stage detail above is the evidence to work from [build157]");
    }
    return rep;
}

void startPlayChainCheck(const std::string& trigger, const std::string& episodeId) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint64_t now = nowMs();
        if (g_lastRunMs != 0 && now > g_lastRunMs && (now - g_lastRunMs) < CHECK_COOLDOWN_MS) {
            logPrintf("PLAYCHECK: one ran %u ms ago -- not running another (cooldown %u ms) [build157]",
                      (unsigned)(now - g_lastRunMs), (unsigned)CHECK_COOLDOWN_MS);
            return;
        }
        g_lastRunMs = now;
    }

    const std::string trig = trigger;
    const std::string ep = episodeId;
    if (!runDetached([trig, ep]() {
            /* THE RESULT PUBLISHES ITSELF, AND NOTHING IS PUT ON SCREEN FROM HERE (build158).
             *
             * The report's outcome sets a global status, and the status light reads that status as it
             * draws -- so this thread never touches a view, never needs a brls::sync callback, and
             * cannot write into one that has been freed. It also means the notification that used to
             * appear over the video is gone, at the user's instruction. */
            (void)runPlayChainCheck(trig, ep);
        })) {
        logPrintf("PLAYCHECK: no thread could be created, so the check did not run [build157]");
    }
}

bool lastPlayChainReport(PlayChainReport& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_haveLast) return false;
    out = g_last;
    return true;
}

} // namespace hg
