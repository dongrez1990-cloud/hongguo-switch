/*
 * hg_image implementation. See hg_image.hpp.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_image.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

#include "hg_cfg.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"

extern "C" {
#include "hg_http.h"
/* The concurrency budget is `image.threads` from the same settings the HTTP client uses, so its
 * default lives in one place -- hg_httpcfg.h. */
#include "hg_httpcfg.h"
}

namespace hg {

/* THE CAP IS MEASURED, NOT GUESSED.
 *
 * The first version of this file allowed 512 KiB, on the assumption that covers
 * are 15-40 KB. Probing the live CDN (scripts/cover-probe.mjs) showed:
 *
 *   homeSections[].video_list[].series_cover   JPEG, 76,430 bytes on a sampled
 *                                              card -- the URL already carries
 *                                              "~tplv-shrink:640:0.image", so
 *                                              the CDN is doing the resizing
 *   bannerList[].background_cover_pc           PNG, 1,304,744 bytes
 *
 * and hg_api_parse_home uses background_cover as the cover FALLBACK for
 * mBannerList cards. So a banner-derived card would have hit the old cap, and
 * hg_http_request answers HG_HTTP_ERR_TOO_BIG rather than overflowing -- which
 * means the cover would have failed looking exactly like a network problem.
 *
 * 2 MiB covers every cover observed, with room for a bigger one. It is a heap
 * buffer per in-flight load: fine for the detail page, which loads one. Anything
 * that loads a screenful at once should revisit this -- 13 concurrent 2 MiB
 * buffers is 26 MiB, which a fixed console heap notices.
 *
 * NOTE ON SIZE SUFFIXES: wiliwili appends "@672w_378h" to cover URLs
 * (utils/image_helper.hpp). Doing that here returns HTTP 400 -- measured, see
 * cover-probe.mjs -- so image.suffix must stay empty, and the CDN's own
 * "~tplv-shrink:640:0.image" is what keeps these files small. */
static const size_t MAX_COVER_BYTES = 2 * 1024 * 1024;

/* The same browser UA the page fetches use: the CDN is served by a web stack that
 * varies what it hands out by UA, so this is part of the measured configuration
 * rather than a cosmetic string. Kept in step with hg_client.cpp's copy. */
static const char* IMAGE_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

/* HOW MANY COVER FETCHES MAY BE IN FLIGHT AT ONCE.
 *
 * A grid page binds a dozen or more cells as it lays out, and every one of them asks for a cover
 * immediately. Each fetch is an HTTP request on its own worker thread (hg_thread) with a buffer of
 * up to HG_IMAGE_RETRY_MAX_BYTES (1.5 MiB) -- so an unbounded page turns "scroll fast" into
 * "allocate a lot of megabytes at once" on a console with a fixed memory budget.
 *
 * The ceiling is `image.threads` from hongguo.cfg, defaulting to 6 and bounded by hg_httpcfg
 * (1..16). The cfg parse and its bounds are covered by hg_httpcfg_test on the host; what is here
 * is only the counter.
 *
 * WHY A COUNTER AND NOT A QUEUE. The cell that is asking does not care whether its cover arrives
 * now or 300 ms later -- it shows a placeholder either way, and the request token (ImageSlot)
 * makes a late arrival safe. So a request that finds the limit reached simply does not start, and
 * the log says so. A queue would hold the view locked (ptrLock) for longer and add a lifetime
 * question this project has already paid for once. */
/* THE BUDGET IS BACK, AND THIS TIME IT IS FIVE LINES.
 *
 * HISTORY, because the shape of this code is a direct response to getting it wrong twice.
 *
 * The first version capped concurrency but released the slot through a REFCOUNTED TOKEN shared by
 * every exit callback, so that a missed decrement could not shrink the budget for ever. It failed the
 * other way: on hardware the count jammed at its limit after nine covers and every later request was
 * refused for the whole session (measured: 402 refusals against 9 installs), and the retry heartbeat
 * turned that into 400 log lines which buried the real problem.
 *
 * So the cap was REMOVED ENTIRELY. That was overkill, and it crashed the app ON STARTUP: the home
 * layout binds all 36 cells in one pass, every cell asks immediately, and 36 concurrent fetch threads
 * with their buffers is not something this heap tolerates. The log shows it exactly -- seven
 * "IMG: fetching" lines and then nothing, with no install ever completing.
 *
 * What is here now is the boring version: ONE atomic counter, checked BEFORE the thread is created,
 * decremented in exactly one named place (`releaseFetch`) that every exit path already calls. What
 * went wrong the first time was never the idea of a cap; it was the bookkeeping around it.
 *
 * The old complaint -- "only the first six covers load" -- is answered by the OTHER half of this:
 * HomeGridView::tickCoverRetries re-asks for cells that are still missing, on a 400 ms heartbeat, and
 * the three-state `settled` stops it re-asking for anything that definitively failed. So a refused
 * fetch means "not yet" rather than "never", and covers arrive in waves instead of all at once. */
static std::atomic<int> g_imageInFlight{0};

/* WHEN THE LAST COVER WAS UPLOADED, in ms since boot. See the throttle in loadImageAsync:
 *
 * ---- IT IS 200 ms, AND IT WAS 60 ms FOR ONE BUILD. THAT BUILD CRASHED ON LAUNCH TWICE (build94) -----------
 *
 * In build93 I lowered this to 60 ms as an "optimisation" the user had asked for. The next launch died on the
 * way into the home screen, twice, with no crash report at all -- which is what being killed for resources looks
 * like rather than a fault. The log beside it shows the shape: "HOME apply: ENTER attach 36 rows" followed by
 * seven cover fetches inside the same second, and then the log itself torn mid-line.
 *
 * build92 had 16 concurrent fetches and a 120 ms gap and the user had confirmed it was fine. So the variable that
 * changed between a working build and a non-working one was this number, and it goes back -- and then some margin.
 * The lesson is the one this file has already learned twice: this console has a hard resource ceiling and the
 * uploads are what touch it, so the cadence stays conservative and the speed comes from not drawing off-screen
 * cells instead (see the cull in home_activity.cpp).
 *
 * MEASURED, from the console's own crash report: the deko3d command-memory exhaustion (`Result 0x767`) has
 * BlitCopyEngine in its stack, reached from nvgEndFrame. BlitCopyEngine is an IMAGE UPLOAD, so the frame that
 * overflows its command buffer is a frame in which several cover textures were uploaded at once -- and covers
 * arrive in waves because the fetch budget is several fetches deep.
 *
 * So uploads are spaced instead of batched: one every HG_IMAGE_INSTALL_GAP_MS. The fetch scheduler refuses to
 * START a fetch while an upload is that fresh, which keeps two uploads out of the same frame without touching any
 * object's lifetime (a deferred-install design would have to hold a lock across frames, and a lock held across a
 * frame is how a view gets freed underneath it). */
#define HG_IMAGE_INSTALL_GAP_MS 0.0
static double g_lastInstallMs = -1000.0;

/* WHEN THE LAST COVER FETCH WAS STARTED, in ms since boot.
 *
 * ---- THIS IS THE FIX FOR "SEVEN FETCHES IN ONE SECOND, THEN THE CONSOLE DIES" (build95) ----------------
 *
 * MEASURED, from the log of the failing launch:
 *
 *     HOME apply: ENTER attach 36 rows
 *     IMG: fetching ... (seven lines, ALL IN THE SAME SECOND)
 *     <the log is torn mid-line; not one cover was ever installed>
 *
 * No crash report and nothing installed: the process was killed while the downloads were in flight. Each fetch
 * holds up to HG_IMAGE_RETRY_MAX_BYTES while it grows, so a dozen simultaneous starts is a dozen buffers at once
 * on a console with a fixed memory budget.
 *
 * WHY THE INSTALL THROTTLE BELOW DID NOT PREVENT IT: that one refuses a start only when an INSTALL happened
 * recently -- and at boot no install has happened yet, so it was wide open exactly when the burst arrives (the
 * home grid attaches 36 rows and asks for every cover at once).
 *
 * So the starts are paced too: at most one every HG_IMAGE_START_GAP_MS. The in-flight cap (image.threads) still
 * exists, but with this pace the real concurrency settles at whatever the network supports -- a few at a time --
 * instead of sixteen at once. It also makes the fill orderly, since covers are now requested in cell order rather
 * than all together. */
#define HG_IMAGE_START_GAP_MS 0.0
static double g_lastStartMs = -1000.0;

/* How many fetches may run at once, from `image.threads` (hg_httpcfg parses and bounds it to 1..16).
 * Falls back to the default when nothing configured it, so this is never zero. */
static int imageThreadBudget() {
    const HgHttpCfg* cfg = hg_http_get_options();
    int n = cfg ? cfg->image_threads : HG_HTTPCFG_IMG_THREADS_DEFAULT;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

/* THE ONE PLACE A FETCH ENDS, called from every exit path -- six of them.
 *
 * This replaces a refcounted "budget slot" whose whole purpose was to make a missed decrement
 * impossible. It did the opposite: on hardware the count jammed at its limit after nine successful
 * covers and every later request was refused for the rest of the session (measured: 402 refusals
 * against 9 installs). One named function that does both halves of the teardown -- unlock the views,
 * drop the count -- is what a reviewer can actually check, unlike a shared pointer threaded through
 * six closures. */
static void releaseFetch(brls::Image* target, brls::View* owner) {
    if (target) target->ptrUnlock();
    if (owner) owner->ptrUnlock();
    g_imageInFlight.fetch_sub(1);
}

bool loadImageAsync(brls::Image* target, const std::string& url, ImageSlot* slot, size_t maxBytes,
                    std::atomic<int>* settled, brls::View* owner) {
    /* ZEROED FIRST, so the caller's flag describes THIS attempt: 0 = not installed, 1 = installed.
     * See the header -- the return value below only says "a fetch started", and a caller that treats
     * that as "I have my picture" stops retrying a card that never got one. */
    if (settled) settled->store(HG_IMG_PENDING);

    /* ---- THE UPLOAD THROTTLE (see g_lastInstallMs) -----------------------------------------------------
     *
     * Refusing to START a fetch while an upload is fresh is what keeps two cover textures out of the same
     * frame, which is the frame whose command buffer the console ran out of (the crash report's stack goes
     * BlitCopyEngine -> nvgEndFrame).
     *
     * Refusing is safe and self-correcting: the caller sees "not started" and its retry tick asks again later
     * (the retries back off now rather than stopping, so nothing is lost). It returns false rather than blocking,
     * because this runs on the main thread and blocking here is the whole UI. */
    {
        const double nowMs = brls::getCPUTimeUsec() / 1000.0;
        if (nowMs - g_lastInstallMs < HG_IMAGE_INSTALL_GAP_MS) {
            static bool said = false;
            if (!said) {
                said = true;
                hg::logPrintf("IMG: an upload happened %.0f ms ago, so this fetch is held back (%.0f ms gap) -- "
                              "one cover texture per %0.f ms keeps the command buffer from overflowing [build89]",
                              nowMs - g_lastInstallMs, (double)HG_IMAGE_INSTALL_GAP_MS,
                              (double)HG_IMAGE_INSTALL_GAP_MS);
            }
            return false;
        }
    }
    if (hg::cfgIs("image.enable", "no")) {
        static bool said = false;
        if (!said) {
            said = true;
            hg::logPrintf("IMG: disabled by cfg (image.enable=no); no covers will be fetched");
        }
        return false;
    }
    if (!target) {
        hg::logPrintf("IMG: null target for '%s'", url.c_str());
        return false;
    }
    if (url.empty()) {
        hg::logPrintf("IMG: empty URL; nothing to load");
        return false;
    }

    /* THE GATE, BEFORE ANY ALLOCATION AND BEFORE THE THREAD.
     *
     * Deliberately before `full` is built and before the worker exists: the expensive things are the
     * request and the thread, not the counter. A refused fetch leaves `settled` at PENDING, which is
     * what tells the caller's retry heartbeat that this cell may ask again -- so a refusal delays a
     * cover instead of cancelling it.
     *
     * NOT LOGGED PER CALL. At 60 fps of retry passes this would flood an fsync'd log; the caller logs
     * its own throttled progress line, and the aggregate number is what matters there. */
    {
        const int budget = imageThreadBudget();
        const int have = g_imageInFlight.load();
        if (have >= budget) {
            return false;
        }
    }

    std::string full = url;
    std::string suffix = cfgValue("image.suffix", "");
    if (!suffix.empty()) full += suffix;

    /* THE LOCK, TAKEN BEFORE THE WORKER EXISTS.
     *
     * ptrLock increments a plain int on the View; borealis' deletion pool refuses
     * to delete a locked view, so the object stays valid for as long as the fetch
     * can take. The unlock happens in the brls::sync callback, which runs on the
     * main loop thread -- the only thread allowed to touch this counter.
     *
     * AND THE SAME LOCK ON THE OWNER, WHICH IS THE PART THAT WAS MISSING. Locking only the Image
     * protects the Image and NOTHING ELSE -- and the callback below also dereferences `slot` and
     * `settled`, which are members of the view that OWNS the image (the home grid's card). Measured on
     * hardware: pressing ??? rebuilds the grid, box.cpp:240-256 hands every old card to the free
     * queue, Box::~Box (box.cpp:759-772) deletes the card -- the locked Image survives, but the card
     * does not, so a fetch that was still in flight wrote its result into FREED memory
     * (`settled->store`) and read a freed std::string (`slot->stillWants`). The console showed the
     * consequence as an intermittent crash: an Atmosph?re report with Instruction Abort inside
     * `brls::Threading::performSyncTasks()`, i.e. a queued main-thread task jumping to a wild address
     * after the heap had been corrupted. Locking the owner keeps the whole card -- slot, settled and
     * the Image inside it -- alive until the callback has run. */
    target->ptrLock();
    if (owner) owner->ptrLock();
    /* The in-flight count is kept FOR THE LOG ONLY. It is not a gate any more -- see the note above
     * `g_imageInFlight`. It is decremented in exactly one place, the same place the view is unlocked,
     * which is the point: the old scheme spread that bookkeeping across a refcounted token and got it
     * wrong in a way that permanently jammed the feature. */
    /* THE START GAP IS ARMED HERE, where the fetch really begins (not at the gate above): the point of
     * the gate is to keep the NEXT start away for HG_IMAGE_START_GAP_MS, and that only means anything
     * once this one has actually been allowed through. */
    g_lastStartMs = brls::getCPUTimeUsec() / 1000.0;
    /* A FETCH IS RUNNING FOR THIS VIEW NOW. See HG_IMG_IN_FLIGHT in hg_image.hpp: the retry tick must not start a
     * second one, because two arrivals mean two LOADS, and the second load deletes a texture the first may still
     * be holding. This is the race the user described as "the load is removed and then the load comes again". */
    if (settled) settled->store(HG_IMG_IN_FLIGHT);
    g_imageInFlight.fetch_add(1);

    bool started = hg::runDetached([target, full, slot, maxBytes, settled, owner]() {
        /* WORKER THREAD. It touches no view and no GUI state: only the HTTP
         * buffer and the thread-safe breadcrumb log. Pixel work is deliberately
         * NOT here -- see the sync callback below. */
        hg::logPrintf("IMG: fetching %s", full.c_str());

        /* hg_http_request wants "Name: value" strings. The UA needs its name
         * prefixed; getting that wrong would send a header named after the whole
         * UA string, which the CDN would ignore -- and the result would look like
         * a CDN problem rather than a formatting one. */
        char ua[320];
        std::snprintf(ua, sizeof ua, "User-Agent: %s", IMAGE_UA);

        const char* headers[3];
        headers[0] = "Accept: image/avif,image/webp,image/apng,image/*,*/*;q=0.8";
        headers[1] = ua;
        headers[2] = nullptr;

        /* THE CALLER'S CAP IS A FIRST GUESS, NOT A LIMIT.
         *
         * Measured over 14 live covers: thirteen are 71-168 KiB and ONE is
         * 1,117,229 bytes (the CDN's own "~tplv-shrink:640:0.image" suffix did not
         * shrink that one). The first version of this file took ONE 76 KiB sample
         * as typical and capped the list at 320 KiB -- so that card showed no cover
         * at all, and the log said "larger than the cap ... this is NOT a network
         * failure", which is the only reason the mistake stayed visible instead of
         * looking like a CDN problem.
         *
         * Simply raising the cap is the wrong fix: the buffer is allocated at the
         * cap up front, so a dozen visible cells at a 1.5 MiB cap is ~19 MiB of
         * mostly-empty buffers. Instead the first attempt uses the caller's small
         * cap and, ONLY on TOO_BIG, retries once at the measured worst case. The
         * common case allocates the small one. */
        size_t attemptCap = maxBytes;
        std::vector<unsigned char> body(attemptCap + 1, 0);

        size_t len = 0;
        long status = 0;
        int rc = hg_http_request(full.c_str(), headers, nullptr, 0,
                                 (char*)body.data(), body.size(), &len, &status);

        if (rc == HG_HTTP_ERR_TOO_BIG && attemptCap < HG_IMAGE_RETRY_MAX_BYTES) {
            attemptCap = HG_IMAGE_RETRY_MAX_BYTES;
            hg::logPrintf("IMG: %s did not fit in %u bytes; retrying with %u",
                          full.c_str(), (unsigned)maxBytes, (unsigned)attemptCap);
            body.assign(attemptCap + 1, 0);
            len = 0;
            status = 0;
            rc = hg_http_request(full.c_str(), headers, nullptr, 0,
                                 (char*)body.data(), body.size(), &len, &status);
        }

        if (rc != HG_HTTP_OK) {
            /* TOO_BIG gets its own line: "the cover exceeds even the retry cap" and
             * "the network failed" call for completely different fixes, and a
             * generic rc=%d would let the first look like the second. */
            if (rc == HG_HTTP_ERR_TOO_BIG) {
                hg::logPrintf("IMG: %s exceeds even the %u-byte retry cap (this is NOT a network "
                              "failure)",
                              full.c_str(), (unsigned)attemptCap);
            } else {
                hg::logPrintf("IMG: transport failed rc=%d (%s) for %s", rc,
                              hg_http_last_error(), full.c_str());
            }
            brls::sync([target, full, settled, owner]() {
                /* DEFINITIVE: the retry inside hg_http already happened, so asking again would get the
                 * same answer. Marking it FAILED is what stops a permanently-broken URL from being
                 * re-requested on every retry pass for the life of the screen. */
                if (settled) settled->store(HG_IMG_FAILED);
                hg::logPrintf("IMG: gave up on %s (transport)", full.c_str());
                releaseFetch(target, owner);
            });
            return;
        }
        if (status < 200 || status >= 300) {
            hg::logPrintf("IMG: HTTP %ld for %s", status, full.c_str());
            brls::sync([target, full, settled, owner]() {
                if (settled) settled->store(HG_IMG_FAILED);
                hg::logPrintf("IMG: gave up on %s (HTTP)", full.c_str());
                releaseFetch(target, owner);
            });
            return;
        }
        if (len == 0) {
            hg::logPrintf("IMG: HTTP %ld with 0 bytes for %s", status, full.c_str());
            brls::sync([target, full, settled, owner]() {
                if (settled) settled->store(HG_IMG_FAILED);
                hg::logPrintf("IMG: gave up on %s (empty body)", full.c_str());
                releaseFetch(target, owner);
            });
            return;
        }

        /* COPY THE BYTES OUT BEFORE LEAVING THE WORKER. `body` dies with this
         * lambda, and the main thread needs the pixels later. This is a copy of a
         * 15-40 KB buffer, which is the correct trade for not sharing a buffer
         * across threads. */
        std::string bytes((const char*)body.data(), len);

        brls::sync([target, bytes, full, slot, settled, owner]() {
            /* MAIN THREAD. Two things are checked here, in this order.
             *
             * FIRST, THE SLOT. A RecyclerCell reuses both the view and the slot,
             * so bytes fetched for the item that used to be in this cell must NOT
             * be installed now that a different item is. Without this check the
             * screen looks perfectly normal and shows the wrong cover -- see
             * hg_image.hpp. */
            if (slot && !slot->stillWants(full)) {
                /* READ THE OWNER'S STATE BEFORE RELEASING THE OWNER. The unlock below is what makes the
                 * card deletable again, and `slot` lives inside that card -- logging its current URL
                 * afterwards would be reading memory that is no longer ours to read. (The deletion pool
                 * only drains later in the frame, so in practice this never crashed; the ordering costs
                 * nothing and removes the dependency on that fact.) */
                const std::string nowWanted = slot->currentUrl();
                releaseFetch(target, owner);
                /* ---- AND THE STATE GOES BACK TO "ASK ME AGAIN" (build98) ------------------------------------
                 *
                 * A dropped response must not leave the view marked IN_FLIGHT: the loader set that when the fetch
                 * started, and if this path is the last one to run, nothing would ever clear it -- the cell would
                 * be skipped by the retry tick for the rest of the session. PENDING is the honest state: no fetch
                 * is running for this view any more, and the next retry pass may ask again. */
                if (settled) settled->store(HG_IMG_PENDING);
                hg::logPrintf("IMG: DROPPED %u bytes for %s -- the view now wants '%s'",
                              (unsigned)bytes.size(), full.c_str(), nowWanted.c_str());
                return;
            }
            /* SECOND, the install. setImageFromMem hands the encoded bytes to
             * nanovg, which decodes them and uploads a deko3d texture. All of that
             * must happen here: the nanovg deko3d backend is not thread-safe and
             * calling into it off the main thread is the classic crash in exactly
             * this kind of change. */
            target->setImageFromMem((const unsigned char*)bytes.data(), (int)bytes.size());
            g_lastInstallMs = brls::getCPUTimeUsec() / 1000.0;
            /* THE ONE PLACE "THIS CELL HAS ITS PICTURE" BECOMES TRUE. See the header: the return
             * value of loadImageAsync only means a fetch started, and a caller that conflates the
             * two stops retrying a card that failed.
             *
             * WRITTEN BEFORE THE UNLOCK, for the same reason as the drop path above: `settled` is a
             * member of the owner, and the unlock is what lets the owner be deleted. */
            if (settled) settled->store(HG_IMG_INSTALLED);
            releaseFetch(target, owner);
            hg::logPrintf("IMG: installed %u bytes into the image view for %s",
                          (unsigned)bytes.size(), full.c_str());
            /* A decode failure is NOT reported by setImageFromMem -- it returns
             * void. nanovg logs it through borealis' logger, which hg_log points
             * at hongguo-brls.log, so a genuinely undecodable body is still
             * visible there rather than silent. */
        });
    });

    if (!started) {
        /* No worker. The counter and the lock are released here, explicitly -- this is one of the six
         * exit paths `releaseFetch` exists to keep identical. The state goes back to PENDING for the same reason
         * as the dropped path above: nothing is in flight any more, so the retry tick may ask again (build98). */
        hg::logPrintf("IMG: no worker thread available; unlocking and skipping %s", full.c_str());
        if (settled) settled->store(HG_IMG_PENDING);
        releaseFetch(target, owner);
        return false;
    }
    return true;
}

} // namespace hg
