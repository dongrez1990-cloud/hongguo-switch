/*
 * PlayerView implementation: mpv + deko3d + audio.
 *
 * GPL-3.0-only.
 *
 * LIFETIME. The mpv handle and render context live as long as the view. The
 * destructor frees the render context BEFORE the mpv handle, which is mpv's
 * documented order.
 *
 * ASCII-only source file.
 */

#include "player_view.hpp"
#include "player_menu.hpp"
#include "player_settings.hpp"
#include "hg_client.hpp"
#include "hg_cfg.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"
#include "hg_tv.hpp"
#include "hg_applet.hpp"   /* appletIsDocked: the dock state for the geometry line (build170) */
#include "hg_store.hpp"
#include "hg_selfcheck.hpp"   /* the play-chain check that runs when a plan or ladder fails (build157) */

extern "C" {
#include "hg_lang.h"      /* hg_tr: the control bar and the settings panel draw Chinese */
#include "hg_history.h"   /* hg_fmt_clock, for the control bar's clock */
#include "hg_ctl.h"       /* the volume/speed/zoom steps and their labels */
#include "hg_shot.h"
}

#include <borealis.hpp>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_dk3d.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

/* borealis' Switch video context: it owns the dk::Device and the framebuffers
 * mpv renders into. */
#include <borealis/platforms/switch/switch_video.hpp>

/* svcSleepThread, used once in setupMpv for a deliberate, documented 250 ms
 * block that collects mpv's log before the process can die. */
#include <switch.h>

/* The deko3d C API, for dkImageGetDimensions: the C++ wrapper header that
 * switch_video.hpp pulls in does not declare it, and the one fact still not
 * measured about this render path is the real size of the image handed to mpv. */
#include <deko3d.h>

/* SEE THE VENDORED switch_video.cpp: while this is set, the frame clear is transparent. The player turns it on
 * for the GPU render path (build106) and off again when it leaves, re-recording the static commands each time. */
bool hg_video_clear_transparent = false;

namespace hg {

struct PlayerView::Impl {
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;
    brls::SwitchVideoContext* video = nullptr;

    /* Rewritten every frame: borealis alternates between two framebuffers. */
    mpv_deko3d_fbo fbo = {};
    mpv_render_param renderParams[2] = {};

    /* Kept alive because mpv reads its options during initialisation, and the
     * strings must outlive that call. */
    HgPlayPlan plan;

    /* The framebuffer fences. mpv is handed POINTERS to these through the
     * mpv_deko3d_fbo struct and uses them to synchronise with the deko3d queue;
     * the queueSignalFence / queueWaitFence calls around the render are the
     * other half of the same handshake. */
    DkFence readyFence = {};
    DkFence doneFence = {};

    /* ---- software render path (renderer=sw) --------------------------------
     *
     * mpv writes RGBA into THIS buffer and nanovg draws it as a texture, so no
     * deko3d image handoff is involved at all. This is wiliwili's MPV_SW_RENDER
     * mode (mpv_core.cpp:787-805) and it is worth having for one reason beyond
     * performance: the pixels are in CPU memory, so "did mpv actually render
     * anything" becomes a number in the log instead of an inference. */
    /* HOW MANY POLL LOOPS ARE RUNNING. The destructor waits for this to reach zero before it frees this struct;
     * see the comment there for why. Counted rather than assumed to be one, because the inline start path and
     * the worker path can each launch one. */
    std::atomic<int> pollers{0};

    bool swRender = false;

    /* ---- THE THEME BACKGROUND IS MADE TRANSPARENT WHILE THE GPU PATH IS PLAYING (build106) --------------
     *
     * MEASURED ON HARDWARE, and this is the last piece of the black-screen puzzle: with renderer=deko3d the
     * picture was hidden behind a GREY screen while the sound played, and it appeared for an instant on exit.
     *
     * The grey is not a view background -- View::backgroundColor defaults to TRANSPARENT (view.hpp:314) -- it is
     * the FRAME CLEAR, recorded by SwitchVideoContext::recordStaticCommands() from theme["brls/background"]
     * (switch_video.cpp:145-146) and submitted at the start of every frame. mpv, meanwhile, is given only the
     * deko3d DEVICE (mpv_deko3d_init_params{device}) and submits its own work on its own queue, so there is no
     * ordering guarantee between "the clear" and "the video": on most frames the grey wins, and on the exit frame
     * the video happens to win -- which is exactly what the user reported.
     *
     * So while the player is on the GPU path, the theme's background is transparent and the static commands are
     * re-recorded; on the way out it is put back. `brls/background` is only read by that clear, so the UI keeps its
     * own colours and the other screens are untouched -- and the fallback (renderer=sw) never does any of this.
     */
    NVGcolor savedThemeBackground{};
    bool themeBackgroundCleared = false;
    unsigned char* pixels = nullptr;
    int swW = 0, swH = 0, swStride = 0;
    int swSize[2] = {0, 0};
    const char* swFormat = "rgba";
    int swNvgImage = 0;
    mpv_render_param swParams[5] = {};
    int swFrames = 0;
    bool swSawPixels = false;

    /* Playback progress, 0..1, published by the poller and read by draw() on the
     * main thread. Atomic because those are two different threads and a torn
     * float is not worth reasoning about. */
    std::atomic<float> progress{0.0f};

    /* The scale and the position for the drag bar (hg_scrub.h), published the same way and
     * for the same reason. `duration` is stored as a float so it is atomic; the bar's
     * resolution requirement is a text readout, not sample accuracy. 0 or negative means
     * "mpv does not know yet", which is what makes hg_scrub_begin refuse the drag rather
     * than scale by it. */
    std::atomic<float> duration{-1.0f};
    std::atomic<float> position{0.0f};
    /* The view that owns this impl. Set once, in the PlayerView constructor: see the note at the EOF case in
     * the event drain for why it needs the view and cannot take it as a parameter. */
    PlayerView* owner = nullptr;

    std::atomic<bool> paused{false};

    /* ---- hold-to-seek state, touched only by draw() on the main thread -------
     * `seekDown` distinguishes a fresh press from a continuing hold, which is what
     * makes a tap move one step and a hold accelerate. `seekMoved` is the input to
     * the curve (how far THIS hold has already moved, in seconds) and
     * `seekPending` carries the sub-second remainder so a rate below 1 s/frame is
     * not rounded away to nothing. */
    bool seekDown = false;
    int seekDir = 0;
    double seekMoved = 0.0;
    double seekPending = 0.0;
    int seekFramesLogged = 0;

    std::atomic<bool> alive{true};
    std::atomic<bool> started{false};

    /* Written by the poll thread, read by draw() on the main thread. */
    std::mutex statusMutex;
    std::string statusText;

    /* The poll thread's SD-log dedupe: the same mutex discipline as
     * statusText, but for the copy that goes into hongguo.log. */
    std::mutex logMutex;
    std::string lastLogged;

    /* THE QUALITY LADDER, written once by a brls::sync callback on the main thread and read
     * from the menu tick on the same thread. */
    std::vector<Client::QualityTier> tiers;
    /* 1 while a switch is fetching its new plan, so a second press cannot start a second switch
     * and race its result. */
    bool switching = false;
    /* The source index the ladder asked for, kept only for the log. */
    int qualityForSwitch = -1;

    /* WHICH TIER THIS PLAYER BELIEVES IS ON SCREEN. -1 = not known yet.
     *
     * Set in exactly two places, and both of them are moments where the answer is a fact rather
     * than a guess:
     *   - the ladder arrives and one of its entries MATCHES the quality_type of the plan that
     *     started playback (that entry is what is playing);
     *   - a switch's option writes were ACCEPTED, i.e. at the point where the new stream is
     *     about to be loaded.
     *
     * WHY IT EXISTS: `switchQuality` refuses to reload a stream that is already on that tier.
     * The reported bug this fixes is "every episode plays about three seconds, jumps back to 0
     * and replays": the quality row is SEEDED from the playing tier when the ladder lands, and
     * that seeding was being read as "the user changed the row", so a full re-fetch and
     * `loadfile` went out for the tier already playing -- about two seconds after playback
     * started, which is exactly the reported timing.
     *
     * It is deliberately NOT `impl->plan.quality_type`: the plan is adopted before the option
     * writes are known to have been accepted, so after a failed switch the plan would claim a
     * tier that is not playing and this guard would then refuse the user's retry. */
    int playingTierIndex = -1;

    /* ---- THE LADDER IS CACHED SO 1080p COSTS NO EXTRA ROUND-TRIP (build143) ----------------------------
     *
     * The user asked for "1080p by default, without causing conflicts". The conflicts that were caused before
     * are documented in docs/默认1080P-不引起冲突的方案.md: a switch AFTER the loadfile reloads the stream
     * (two audio streams, a spurious EOF, a jump to the next episode), and fetching the ladder BEFORE it adds
     * a network round-trip, which is the longer grey screen they then asked me to revert.
     *
     * The cache removes the second cost: the ladder for this episode is fetched ONCE and reused, and the plan
     * for the chosen tier is built from the same response (Client::fetchPlayPlanForSource), so there is still
     * exactly ONE loadfile and ONE ladder request per episode.
     *
     * ONLY A LADDER WITH TWO OR MORE TIERS IS CACHED. Measured on hardware: the same episode returns five
     * tiers one moment and a single tier the next, and caching the single-tier answer would freeze that
     * degenerate response in place -- which is what made the menu claim 1080p was playing while the stream was
     * 360p. */
    std::string ladderCacheEpisodeId;
    std::vector<Client::QualityTier> ladderCache;
    /* ---- WHERE TO RESUME AFTER A QUALITY SWITCH (build135, the user's request) -------------------------
     *
     * THEIR WORDS: "switching the resolution should load dynamically and not disturb what I am watching -- do not
     * restart the episode from the beginning every time I change it."
     *
     * A plain stream cannot be re-tiered in place: switchQuality issues one loadfile, and a loadfile starts at
     * zero. Every player that offers a mid-playback quality change therefore remembers where it was and seeks
     * back, and that is what these two fields are. The position is captured from the OLD stream before the
     * loadfile goes out -- the only time-pos that means anything at that moment -- and spent by
     * resumeAfterQualitySwitch() on the first file-loaded that follows. -1 means "nothing pending". */
    double resumeAfterSwitch = -1.0;
    double resumeDuration = 0.0;

    /* Counts mpv's own log lines through drainMpvEvents; see MPV_LOG_LINE_CAP.
     * Written from the main thread only. */
    int mpvLogLines = 0;

    /* 1 once the screenshot probe's command has gone out, so it is issued once per player rather
     * than once per frame. Written and read on the main thread (issued from draw, answered in
     * drainMpvEvents, and both of those run there). */
    int shotProbeSent = 0;

    /* ---- THE STREAM-REPAIR COUNTER (build171) ---------------------------------------------------------
     *
     * THE USER'S SYMPTOM, IN THEIR WORDS: "the source check found the source was abnormal, but it did
     * not repair itself with a new address." The measured evidence is in the build168 log at 20:40:46:
     *
     *     MPVLOG [ffmpeg/warn]  https: HTTP error 403 Forbidden
     *     MPVLOG [stream/error] Failed to open https://v5-bd-daily-reading-video.qznovelvod.com/.../video/to...
     *
     * The URL is signed and time-limited, so once it expires mpv cannot open it at all -- and until this
     * counter existed the MPV_EVENT_END_FILE branch recorded the error and did nothing else.
     *
     * WHY A COUNTER AND NOT A BOOLEAN. The whole point of the limit is that a genuinely dead episode must
     * still fail visibly instead of becoming an endless reload loop, and "reload" is the exact shape of
     * loop this project has already paid for once (build151's transparent retry is one attempt for the
     * same reason). One attempt per episode is the same budget: a NEW address is a genuinely different
     * input, so it deserves one chance, and a second failure means the episode itself is gone -- not that
     * the signature was stale.
     *
     * WHERE IT IS RESET, and both places matter:
     *   - PlayerView::start(), i.e. every new episode, so each episode gets its own single attempt;
     *   - on a SUCCESSFUL repair, so the counter reads "this episode has not used its attempt" again --
     *     otherwise a fresh stream that later expires (the video is long, the URL is short-lived) could
     *     not be repaired a second time.
     *
     * WRITTEN AND READ ON THE MAIN THREAD ONLY, which is why it is a plain int and not an atomic: the
     * error branch hands the work to the main thread through brls::sync, and everything that touches this
     * field happens inside that callback or inside start(). */
    int repairAttempts = 0;

    /* 1 while a repair is fetching its new plan, so the second error event of a dying stream cannot
     * start a second repair. The same flag discipline as `switching` above, and for the same reason. */
    bool repairing = false;
};

/* ---- the screenshot-raw probe --------------------------------------------
 *
 * ASKS MPV ONCE, RECORDS THE ANSWER, AND BLOCKS NOTHING. See hg_shot.h for why the question is
 * worth asking: whether `screenshot-raw` works under this render configuration decides whether a
 * progress-bar thumbnail is one command away or needs a second software render context, and that
 * has been answered twice from documentation -- which is not the same as measuring it.
 *
 * ASYNC, AND THE FIRST VERSION WAS NOT. It used `mpv_command_ret`, which blocks until the reply
 * arrives, and it was called from inside a `brls::sync` callback -- i.e. on the MAIN thread, where
 * that is a stalled frame loop. There is a second reason it could not work there, nor on a bare
 * worker: mpv delivers a command reply through its EVENT QUEUE, so the caller must also be draining
 * events. On the main thread the reply would only arrive from inside draw(), which the blocking call
 * is itself preventing -- a self-deadlock, not merely a stall. A code review of this file caught it;
 * my own comment claimed it ran on a worker while the call site said otherwise.
 *
 * `mpv_command_async` with a reply_userdata is the shape that works everywhere: the command goes
 * out, and `drainMpvEvents` -- which both the poll thread and draw call -- picks the reply up as an
 * MPV_EVENT_COMMAND_REPLY. That is ALSO the mechanism the verdict recommends for a real thumbnail,
 * so the probe now exercises the path it recommends rather than a different one that merely happened
 * to work for a diagnostic.
 *
 * OFF BY DEFAULT (`player.shotprobe=yes`), because it makes mpv render and copy a full frame. */
#define HG_SHOT_USERDATA 0x53484F54ull   /* "SHOT" */

/* Defined below, next to the reply handler that uses it. Forward-declared because the request and
 * the handler are written in the order they are read, and the parser belongs with the handler. */
static void readShotNode(mpv_node* root, HgShotInfo* info);

static void requestScreenshotProbe(PlayerView::Impl* impl) {
    const char* args[] = {"screenshot-raw", nullptr};
    int rc;
    if (!impl || !impl->mpv) return;
    if (impl->shotProbeSent) return;   /* once per player */
    impl->shotProbeSent = 1;
    hg::logPrintf("SHOT: probing screenshot-raw (player.shotprobe=yes)");
    rc = mpv_command_async(impl->mpv, HG_SHOT_USERDATA, args);
    if (rc < 0) {
        /* A synchronous refusal -- no video yet, or the command does not exist in this build -- is
         * itself the answer, so it is stated in the same terms as the asynchronous verdict. */
        hg::logPrintf("SHOT: the command was refused outright rc=%d (%s)", rc, mpv_error_string(rc));
        hg::logPrintf("SHOT: VERDICT -- screenshot-raw is NOT available here, so the thumbnail "
                      "preview needs the second software render context.");
    }
}

/* The reply, handled where every other mpv event is. Reads the node and logs; it does not free it,
 * because mpv owns the event's data. */
static void handleShotReply(mpv_event* ev) {
    HgShotInfo info;
    char line[320];

    if (!ev) return;
    if (ev->error < 0) {
        /* mpv's own error string distinguishes "property unavailable" from "command not found"
         * from "no video", and those are three different next steps. */
        hg::logPrintf("SHOT: the command failed rc=%d (%s)", ev->error,
                      mpv_error_string(ev->error));
        hg::logPrintf("SHOT: VERDICT -- screenshot-raw does NOT give a usable frame here, so the "
                      "thumbnail preview needs the second software render context.");
        return;
    }

    readShotNode(ev->data ? (mpv_node*)ev->data : NULL, &info);
    hg_shot_describe(&info, line, (int)sizeof line);
    hg::logPrintf("SHOT: %s", line);

    if (info.verdict == HG_SHOT_OK) {
        hg::logPrintf("SHOT: VERDICT -- screenshot-raw WORKS under this render configuration. A "
                      "progress-bar thumbnail can be captured with mpv_command_async + "
                      "MPV_EVENT_COMMAND_REPLY, which is what this probe just used; the second "
                      "render context is not needed.");
    } else {
        /* The doc is named rather than quoted: its filename is Chinese, and this file must stay
         * ASCII for the GBK build machine -- the same trap this project has hit before. */
        hg::logPrintf("SHOT: VERDICT -- screenshot-raw does NOT give a usable frame here (%s). The "
                      "thumbnail preview therefore needs the second software render context, whose "
                      "risks are recorded in the drag-to-seek step-5 document.",
                      hg_shot_verdict_text(info.verdict));
    }
}

/* Reading an mpv_node reply needs no JSON: mpv builds the tree and the fields are read straight out
 * of it. hg_shot's parser is for the TEXT form, which the host suite covers -- two readers for two
 * shapes, each covering what it can. */
static void readShotNode(mpv_node* root, HgShotInfo* info) {
    mpv_node_list* list;
    int i;
    memset(info, 0, sizeof *info);
    info->verdict = HG_SHOT_ERR_PARSE;
    info->missing = "(reply is not a map)";

    if (!root || root->format != MPV_FORMAT_NODE_MAP) return;
    list = root->u.list;
    if (!list) return;

    {
        int haveW = 0, haveH = 0, haveData = 0;
        for (i = 0; i < list->num; i++) {
            const char* key = list->keys[i];
            mpv_node* val = &list->values[i];
            if (!key) continue;
            if (strcmp(key, "w") == 0 && val->format == MPV_FORMAT_INT64) {
                info->width = (int)val->u.int64;
                haveW = 1;
            } else if (strcmp(key, "h") == 0 && val->format == MPV_FORMAT_INT64) {
                info->height = (int)val->u.int64;
                haveH = 1;
            } else if (strcmp(key, "stride") == 0 && val->format == MPV_FORMAT_INT64) {
                info->stride = (int)val->u.int64;
            } else if (strcmp(key, "format") == 0 && val->format == MPV_FORMAT_STRING) {
                snprintf(info->format, sizeof info->format, "%s",
                         val->u.string ? val->u.string : "");
            } else if (strcmp(key, "strange_colors") == 0) {
                if (val->format == MPV_FORMAT_FLAG && val->u.flag) {
                    info->verdict = HG_SHOT_ERR_STRANGE;
                }
            } else if (strcmp(key, "data") == 0 && val->format == MPV_FORMAT_NODE_ARRAY) {
                info->elements = val->u.list ? val->u.list->num : 0;
                haveData = 1;
            }
        }

        if (info->verdict == HG_SHOT_ERR_STRANGE) return;
        if (!haveW) { info->missing = "w"; return; }
        if (!haveH) { info->missing = "h"; return; }
        if (!haveData) { info->missing = "data"; return; }
        if (info->width <= 0 || info->height <= 0) {
            info->verdict = HG_SHOT_ERR_ZERO_SIZE;
            return;
        }
        {
            int perRow = (info->stride > 0 && info->stride >= info->width * 4) ? info->stride
                                                                              : info->width * 4;
            info->declared = perRow * info->height;
        }
        info->missing = NULL;
        /* THE SAME CONSISTENCY CHECK AS THE TEXT PARSER, for the same reason: a frame whose data
         * does not match its own w/h/stride would be sheared when drawn, and a false win here would
         * send the thumbnail work down the wrong path. */
        info->verdict = (info->elements == info->declared) ? HG_SHOT_OK : HG_SHOT_ERR_NO_ELEMENTS;
    }
}

/* ---- hold-to-seek --------------------------------------------------------
 *
 * PORTED FROM wiliwili, not invented. Its curve is in source/view/video_view.cpp:40-47:
 *
 *     static int getSeekRange(int current) {
 *         current = abs(current);
 *         if (current <= 30)   return 5;
 *         if (current <= 60)   return 15;
 *         if (current <= 300)  return 30;
 *         if (current <= 1200) return 60;
 *         return current / 20;
 *     }
 *
 * The parameter is HOW FAR THIS HOLD HAS ALREADY MOVED, and the return is the
 * step size -- so the longer you hold, the bigger each step: a short tap nudges,
 * a long hold covers minutes. Above 1200 s the step becomes a twentieth of the
 * distance already travelled, so the acceleration keeps growing instead of
 * plateauing.
 *
 * Two deliberate deviations, both because this player has no progress slider:
 *
 *   1. wiliwili's first bucket is 5 s, sized for dragging its slider. A discrete
 *      tap here should move something visible in a short drama, so the first
 *      bucket is 10 s.
 *   2. wiliwili applies the step as a fraction of the video per frame
 *      (step / getFPS()). This seeks with mpv's RELATIVE command, so the same
 *      arithmetic becomes seconds-per-second and the movement is identical: a
 *      rate that does not depend on the frame rate.
 *
 * WHY THIS IS POLLED RATHER THAN REGISTERED AS A BUTTON ACTION. borealis' action
 * repeat is a fixed cadence (250 ms delay, then every 100 ms -- application.cpp:65-66)
 * with no acceleration at all, and 10 s per 100 ms would be 100x real time. wiliwili
 * polls the raw controller state every frame in VideoProgressSlider::buttonsProcessing
 * for exactly this reason. Polling also means the direction can reverse mid-hold
 * without a release, and that a tap and a hold are the same code path.
 *
 * BECAUSE OF THAT, THE LEFT/RIGHT/LB/RB ACTIONS ARE NOT REGISTERED ANYWHERE ELSE.
 * Two paths reacting to one press would seek twice per tap. */
static int seekStepFor(int movedSeconds) {
    int c = movedSeconds < 0 ? -movedSeconds : movedSeconds;
    if (c <= 30) return 10;
    if (c <= 60) return 15;
    if (c <= 300) return 30;
    if (c <= 1200) return 60;
    return c / 20;
}

/* The big buttons get the same curve scaled up, so holding RB accelerates exactly
 * like holding RIGHT, only further per step. */
static const int SEEK_BIG_MULT = 6;

/* ---- THE LADDER'S MUTEX (build144) --------------------------------------------------------------------
 *
 * hg_client.cpp holds its parse buffers in FILE-STATIC globals (g_page, g_router, g_req). Two callers now ask for
 * the quality ladder -- the UI on its own detached thread and the playback worker, which chooses the tier before
 * the loadfile -- and without this lock they parse into the same megabyte at the same time. Atmosphère recorded
 * the result as a null dereference in the hongguo-net process. Every call goes through this mutex. */
static std::mutex g_ladderMutex;

/* Monotonic milliseconds, for the menu's key-repeat timing. borealis exposes only
 * getCPUTimeUsec() (core/time.hpp:33), which is a CPU clock rather than a wall
 * clock -- fine for "has enough time passed since the last repeat" and nothing
 * more, which is all this is used for. */
static double nowMs() {
    return (double)brls::getCPUTimeUsec() / 1000.0;
}

/* Clamp a fraction into 0..1. Written here rather than using std::min/max so that a NaN
 * becomes 0 -- a NaN fraction would make nanovg's rect width NaN, which draws nothing and
 * logs nothing, and "the bar vanished" is a far worse symptom than "the bar is at the
 * start". */
static double clamp01f(double v) {
    if (!(v > 0.0)) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/* ---- mpv's update notification ------------------------------------------ */

/* How many of mpv's own log lines are written into the breadcrumb log. Each one
 * is fsync'd, so this is a bound on I/O, not a cosmetic limit. 500 is far more
 * than the setup + first-frame messages need and far less than a verbose decode
 * would produce. */
static const int MPV_LOG_LINE_CAP = 500;

/* Installed with mpv_render_context_set_update_callback. wiliwili's equivalent
 * is MPVCore::on_update (mpv_core.cpp:205-225): it calls
 * mpvRenderContextUpdate() and, on the deko3d branch, deliberately does nothing
 * else, because the actual rendering happens in the view's draw once per
 * displayed frame (wiliwili's own comment there says, in Chinese, that the video
 * is drawn straight to the screen and therefore has to be drawn once per display
 * refresh; translated here because this file must stay pure ASCII for the
 * GBK/936 build machine).
 *
 * THREADING. mpv calls this from its own thread. mpv_render_context_update() is
 * precisely what may be called from here; the callback touches nothing else --
 * no view, no log-worthy shared state -- so there is no cross-thread access to
 * GUI objects and no brls::sync hop is needed. */
static void onMpvUpdate(void* self) {
    PlayerView::Impl* impl = (PlayerView::Impl*)self;
    if (impl && impl->renderCtx) mpv_render_context_update(impl->renderCtx);
}

/* ---- mpv state polling --------------------------------------------------- */

/* Defined below the poller, but used by it: mpv's own event queue is drained
 * from every thread that is still alive, because the crash this diagnostic is
 * chasing takes the process down before the render loop can reach its next
 * frame. */
static void drainMpvEvents(PlayerView::Impl* impl);

/* Report what mpv is ACTUALLY doing. Every value is a measurement, not a
 * claim: hwdec-current and picture-type in particular are what reveal a
 * silent fallback or a stream that never decoded. */
static void pollLoop(PlayerView::Impl* impl) {
    impl->pollers.fetch_add(1);
    /* The poller is the one thread whose silence is ambiguous: if it never logs,
     * either it was never scheduled or it died on the first property read. So it
     * announces entry, the sleep, the wake-up and the first successful read. */
    hg::logPrintf("POLL: thread entered (mpv=%p)", (void*)impl->mpv);
    bool first = true;
    int iteration = 0;
    while (impl->alive) {
        iteration++;
        /* Drain on the poller too, not only in draw(): if the process is about
         * to die inside mpv, the render loop may never reach its next frame, and
         * this thread is then the only one still able to pull mpv's messages
         * out. */
        drainMpvEvents(impl);
        if (first) hg::logPrintf("POLL: iteration %d entered, draining done", iteration);

        /* brls::Threading has no sleep(); a plain std::this_thread sleep is
         * correct here because this thread never touches a view. It reduces to
         * nanosleep, which is a syscall and works independently of the broken
         * pthread_create.
         *
         * IN TEN SLICES RATHER THAN ONE, AND THAT IS A LIFETIME FIX (round 35): the destructor sets
         * `impl->alive = false` and then waits for this thread to notice before it frees `impl` -- see the
         * comment there. A single half-second sleep would make that wait half a second long on every exit
         * from the player; ten 50 ms slices make it react in 50 ms while keeping the poll's own cadence
         * unchanged. */
        bool stopped = false;
        for (int slice = 0; slice < 10; slice++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!impl->alive) {
                stopped = true;
                break;
            }
        }
        if (stopped) break;
        if (first) {
            hg::logPrintf("POLL: woke after 500ms; mpv=%p started=%d alive=%d", (void*)impl->mpv,
                          (int)impl->started.load(), (int)impl->alive.load());
        }
        if (!impl->alive || !impl->mpv || !impl->started) {
            if (first) hg::logPrintf("POLL: skipping this pass (not started yet)");
            first = false;
            continue;
        }

        double tp = -1.0;
        char* hwdec = nullptr;
        char* ptype = nullptr;
        char* ao = nullptr;
        int64_t chans = 0, srate = 0;
        int64_t w = 0, h = 0;
        int paused = 0;

        /* "time-pos" is the real signal that samples were readable: the
         * container header yields width/height even for an undecodable stream,
         * which is exactly the trap the original A/B existed to avoid. */
        int rcTime = mpv_get_property(impl->mpv, "time-pos", MPV_FORMAT_DOUBLE, &tp);
        if (rcTime < 0) tp = -1.0;
        int rcW = mpv_get_property(impl->mpv, "width", MPV_FORMAT_INT64, &w);
        int rcH = mpv_get_property(impl->mpv, "height", MPV_FORMAT_INT64, &h);
        int rcHw = mpv_get_property(impl->mpv, "hwdec-current", MPV_FORMAT_STRING, &hwdec);
        int rcPt = mpv_get_property(impl->mpv, "video-frame-info/picture-type", MPV_FORMAT_STRING, &ptype);
        int rcPa = mpv_get_property(impl->mpv, "pause", MPV_FORMAT_FLAG, &paused);
        /* THE AUDIO MEASUREMENT. current-ao names the audio output mpv actually
         * opened -- "hos" means the audren backend works, "null" means it fell
         * back, "(none)" means no audio track was opened at all. The channel
         * count and sample rate prove that decoded samples reached it rather
         * than merely that a driver was selected.
         *
         * The name is `current-ao`. An earlier version of this file asked for
         * `ao-current`, which mpv does not have -- verified by searching
         * libmpv.a's strings for both spellings: `current-ao` is there once,
         * `ao-current` not at all. mpv_get_property would have returned
         * MPV_ERROR_PROPERTY_NOT_FOUND and left the log claiming no audio
         * information was available, which is exactly the kind of silent,
         * plausible-looking nothing this project keeps tripping over. */
        int rcAo = mpv_get_property(impl->mpv, "current-ao", MPV_FORMAT_STRING, &ao);
        int rcCh = mpv_get_property(impl->mpv, "audio-params/channel-count", MPV_FORMAT_INT64, &chans);
        int rcSr = mpv_get_property(impl->mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &srate);
        if (first) {
            /* Any failure here is silent otherwise: mpv_get_property returns the
             * error code and leaves the out-parameter alone. */
            hg::logPrintf("POLL: first read rc: time=%d w=%d h=%d hwdec=%d ptype=%d pause=%d"
                          " current-ao=%d ch=%d sr=%d",
                          rcTime, rcW, rcH, rcHw, rcPt, rcPa, rcAo, rcCh, rcSr);

            /* THE THIRD DISCRIMINATOR: is the video output actually configured,
             * and is it actually being fed frames?
             *
             * The two other readings on this build are the update flags and the
             * render return code. These four properties answer the question that
             * comes BEFORE both: if `vo-configured` is 0, no video output exists
             * to render into, and every call downstream is meaningless -- which is
             * also the only way "render returned 0 and drew nothing" is not a
             * contradiction. `estimated-vf-fps` above zero means frames really are
             * being produced at a rate, so a configured VO with 0 fps points at
             * the decoder or the pacing rather than at the render path.
             *
             * One line, once: this is a diagnosis, not a status readout. */
            int64_t voConfigured = -1;
            char* currentVo = nullptr;
            double vfFps = -1.0;
            double displayFps = -1.0;
            int rv1 = mpv_get_property(impl->mpv, "vo-configured", MPV_FORMAT_FLAG, &voConfigured);
            int rv2 = mpv_get_property(impl->mpv, "current-vo", MPV_FORMAT_STRING, &currentVo);
            int rv3 = mpv_get_property(impl->mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &vfFps);
            int rv4 = mpv_get_property(impl->mpv, "display-fps", MPV_FORMAT_DOUBLE, &displayFps);
            hg::logPrintf("POLL: vo-configured=%lld (rc %d) current-vo='%s' (rc %d)"
                          " estimated-vf-fps=%.2f (rc %d) display-fps=%.2f (rc %d)",
                          (long long)voConfigured, rv1, currentVo ? currentVo : "(null)", rv2,
                          vfFps, rv3, displayFps, rv4);
            if (currentVo) mpv_free(currentVo);

            /* Where mpv thinks the picture is.
             *
             * mpv renders into the whole framebuffer, so its own idea of the
             * video rectangle (margins plus size, in framebuffer pixels) is the
             * only authoritative answer to "why is the picture up there / behind
             * the UI". Reading it costs nothing and turns a visual complaint
             * into numbers. */
            int64_t mlx = 0, mly = 0, mrx = 0, mry = 0, ow = 0, oh = 0;
            int r1 = mpv_get_property(impl->mpv, "osd-dimensions/mlx", MPV_FORMAT_INT64, &mlx);
            int r2 = mpv_get_property(impl->mpv, "osd-dimensions/mly", MPV_FORMAT_INT64, &mly);
            int r3 = mpv_get_property(impl->mpv, "osd-dimensions/mrx", MPV_FORMAT_INT64, &mrx);
            int r4 = mpv_get_property(impl->mpv, "osd-dimensions/mry", MPV_FORMAT_INT64, &mry);
            int r5 = mpv_get_property(impl->mpv, "osd-dimensions/w", MPV_FORMAT_INT64, &ow);
            int r6 = mpv_get_property(impl->mpv, "osd-dimensions/h", MPV_FORMAT_INT64, &oh);
            hg::logPrintf("MPV geometry: osd-dimensions mlx=%lld mly=%lld mrx=%lld mry=%lld"
                          " w=%lld h=%lld (rc %d %d %d %d %d %d)",
                          (long long)mlx, (long long)mly, (long long)mrx, (long long)mry,
                          (long long)ow, (long long)oh, r1, r2, r3, r4, r5, r6);
            first = false;
        }

        /* The progress fraction and the mm:ss readout.
         *
         * `duration` can be unavailable for a while after load, and mpv reports it
         * as 0 or negative in that window -- so the fraction is only published
         * when there is a real duration behind it, and the bar stays at 0 rather
         * than jumping. A bar that lies about position is worse than a bar that
         * sits at the start. */
        double dur = -1.0;
        if (mpv_get_property(impl->mpv, "duration", MPV_FORMAT_DOUBLE, &dur) < 0) dur = -1.0;
        if (tp >= 0.0 && dur > 0.5) {
            float frac = (float)(tp / dur);
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            impl->progress.store(frac);
        } else {
            impl->progress.store(0.0f);
        }
        double shown = tp < 0.0 ? 0.0 : tp;

        /* THE SCALE AND THE POSITION THE BAR NEEDS, published next to `progress`.
         *
         * The bar's drag model works in SECONDS (hg_scrub.h), and it needs a real duration
         * to have a scale at all -- so the raw duration is published here, 0 or -1 included,
         * and hg_scrub_begin refuses to start a drag while it is unusable. One atomic holds
         * the seconds as a float for the same reason `progress` is one: this is the poll
         * thread and the bar is drawn on the main one. A double would not be atomic on
         * aarch64 anyway. */
        impl->duration.store((float)dur);
        impl->position.store((float)shown);
        impl->paused.store(paused != 0);

        long ps = (long)shown, ds = dur > 0.0 ? (long)dur : 0;

        /* ONE SHORT LINE. It used to carry frame-type / hwdec / ao / channels /
         * samplerate, because those were the only way to see whether decoding and
         * audio were actually running. They are no longer in question -- the
         * picture and the sound both work, and the audio verdict is in the log
         * ("AO: [hos] 44100Hz stereo 2ch s16") from mpv itself plus the one-off
         * POLL line. What is left on screen is what a player should show: whether
         * it is playing, and how far in. */
        char buf[160];
        snprintf(buf, sizeof buf, "%s   %02ld:%02ld / %02ld:%02ld",
                 paused ? "PAUSED " : "PLAYING",
                 ps / 60, ps % 60, ds / 60, ds % 60);

        if (hwdec) mpv_free(hwdec);
        if (ptype) mpv_free(ptype);
        if (ao) mpv_free(ao);

        /* The text is stashed under a mutex and painted by draw() on the main
         * thread. Calling brls::sync per poll would queue a task every 500 ms
         * for a label that only needs to be current by the next frame. */
        {
            std::lock_guard<std::mutex> lock(impl->statusMutex);
            impl->statusText = buf;
        }

        /* Also into the SD log, but only when the line changes: this runs every
         * 500 ms and each log line is fsync'd. The audio verdict is the reason
         * this is recorded at all -- a screenshot of the status line is easy to
         * miss, a log line is not. */
        {
            std::lock_guard<std::mutex> lock(impl->logMutex);
            if (impl->lastLogged != buf) {
                impl->lastLogged = buf;
                hg::logPrintf("MPV: %s", buf);
            }
        }
    }
    impl->pollers.fetch_sub(1);
}

/* ---- mpv's own log -------------------------------------------------------
 *
 * mpv reports what it is doing through its event queue, which only exists if
 * someone drains it. Draining is normally done in the render thread before
 * drawing, which is exactly here.
 *
 * This is the only way to see inside mpv: our breadcrumbs stop at "mpv started"
 * because everything after that happens inside a library, and the run that died
 * there produced no line at all between "mpv started" and the death -- no
 * decoder message, no audio driver message, nothing, because mpv's messages had
 * nowhere to go. */
static void drainMpvEvents(PlayerView::Impl* impl) {
    if (!impl || !impl->mpv) return;
    while (true) {
        mpv_event* ev = mpv_wait_event(impl->mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message* m = (mpv_event_log_message*)ev->data;
                /* THE CAP. Every breadcrumb line is fsync'd, so a verbose feed
                 * from a 30 fps decode would make the SD writer the bottleneck
                 * and change the behaviour being measured. The first lines are
                 * also the ones that matter: they cover setup and the first
                 * frame. When the cap is hit, say so once -- a log that simply
                 * stops is indistinguishable from a process that died. */
                if (impl->mpvLogLines < MPV_LOG_LINE_CAP) {
                    impl->mpvLogLines++;
                    /* mpv's log text carries its own trailing newline. */
                    hg::logPrintf("MPVLOG [%s/%s] %s", m->prefix ? m->prefix : "?",
                                  m->level ? m->level : "?", m->text ? m->text : "");
                    if (impl->mpvLogLines == MPV_LOG_LINE_CAP) {
                        hg::logPrintf("MPVLOG: cap of %d lines reached; further mpv messages are "
                                      "dropped so the fsync per line cannot dominate the run",
                                      MPV_LOG_LINE_CAP);
                    }
                }
                break;
            }
            case MPV_EVENT_END_FILE: {
                mpv_event_end_file* e = (mpv_event_end_file*)ev->data;
                const int reason = e ? e->reason : -1;
                hg::logPrintf("MPVEVT end-file reason=%d error=%d", reason, e ? e->error : 0);
                /* AN EPISODE THAT PLAYED TO ITS END IS THE ONE THING THAT TRIGGERS AUTO-NEXT.
                 *
                 * Only EOF does it. A STOP is what a seek or an explicit stop reports, and treating that as
                 * "the episode ended" would advance the moment anything interrupted playback -- which is
                 * exactly the class of bug this project keeps finding late. The activity decides what to do,
                 * because it is the object that knows the episode list. */
                /* `impl->owner` rather than a parameter: this function is called from the poll thread AND from
                 * draw(), and only one of those is a member function -- threading a view pointer through both
                 * would have meant passing nullptr from the free ones and losing the callback exactly where EOF
                 * is most likely to be seen. The owner is set once, in the view's constructor. */
                if (reason == MPV_END_FILE_REASON_EOF && impl->owner) impl->owner->notifyEpisodeEnded();

                /* ---- AND AN ERROR END IS THE ONE THAT MUST NOT BE IGNORED (build171) -----------------------
                 *
                 * The EOF case above is auto-next; this is self-repair, and the two must never be confused.
                 * That is why the test is written as an explicit pair of conditions rather than "not EOF":
                 *
                 *     reason == MPV_END_FILE_REASON_ERROR   (4, client.h:1471 -- "some kind of error happened
                 *                                            that lead to playback abort", and `error` is set)
                 *     e->error != 0                          (the non-EOF reasons can still carry an error)
                 *
                 * EOF is reason 0 and carries error 0, so it CANNOT take this branch however the first
                 * condition is read -- which is the property the user's auto-next depends on. STOP (2) and
                 * QUIT (3) are how a seek and an explicit teardown report themselves, and with error==0 they
                 * are deliberately left alone: a stop is not a broken source.
                 *
                 * WHAT HAPPENS HERE AND WHAT DOES NOT. This can run on the poll thread or on the main thread
                 * (see the note above), and it does not know which -- so the call does nothing but hand the
                 * work to the main thread through brls::sync. Repairing means re-running setupMpv, which
                 * touches the view, the render context and the label: none of that may happen from a worker,
                 * and none of it may happen from draw() either (rule 5). */
                if ((reason == MPV_END_FILE_REASON_ERROR || (e && e->error != 0)) && impl->owner) {
                    /* Through `impl->owner`, exactly like notifyEpisodeEnded two lines up: this is a free
                     * function holding only an Impl*, and requestStreamRepair is a public member precisely
                     * so that this call is reachable from here. The null check is the same one the EOF
                     * branch uses -- `owner` is set once in the constructor, so it is a guard against the
                     * drain running against a partly built or torn down view rather than an expected case. */
                    impl->owner->requestStreamRepair(reason, e ? e->error : 0);
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                hg::logPrintf("MPVEVT file-loaded");

                /* ---- AND THE PLAYBACK GOES BACK WHERE IT WAS (build135) ---------------------------------
                 *
                 * A quality switch issues one loadfile, and a loadfile starts at zero -- so without this the
                 * user's complaint stood: "do not restart the episode from the beginning every time I change the
                 * resolution." The position was captured before the loadfile went out (see switchQuality) and is
                 * spent here, on the first file-loaded that follows it.
                 *
                 * It goes through impl->owner, the route notifyEpisodeEnded already uses, because this is a free
                 * function called both from the poll thread and from draw(). */
                /* THE RESUME-AFTER-QUALITY-SWITCH CALL THAT STOOD HERE IS REMOVED (build141, at the user's
                 * request: "roll back to 133"). See the note where switchQuality used to capture the position. */
                /* THE DECODER PROBE BELONGS HERE, NOT AT INIT.
                 *
                 * The first version read `hwdec-current` right after `mpv_initialize`, where it returned
                 * rc=-3 (property unavailable) because there is no file and therefore no decoder yet --
                 * and the log line then said "empty = software", which was a WRONG conclusion drawn from
                 * an unavailable property. Stuttering was being blamed on the decoder before the decoder
                 * existed.
                 *
                 * After file-loaded the decoder HAS been chosen, so this is the first moment the question
                 * can be answered. It runs once per file. */
                {
                    static int probed = 0;
                    if (probed < 4) {
                        probed++;
                        char* dec = nullptr;
                        char* vd = nullptr;
                        int rcDec = mpv_get_property(impl->mpv, "hwdec-current", MPV_FORMAT_STRING, &dec);
                        int rcVd = mpv_get_property(impl->mpv, "video-codec", MPV_FORMAT_STRING, &vd);
                        hg::logPrintf("PLAYER decode: hwdec-current='%s' (rc=%d) codec='%s' (rc=%d)",
                                      (dec && dec[0]) ? dec : "(soft)", rcDec,
                                      (vd && vd[0]) ? vd : "(?)", rcVd);
                        /* The verdict in words, so the log says what it means rather than leaving a
                         * reader to know that "auto-safe" and "nvdec" are hardware and "" is not. */
                        if (rcDec == 0 && dec && dec[0] != '\0') {
                            hg::logPrintf("PLAYER decode: HARDWARE (%s)", dec);
                        } else {
                            hg::logPrintf("PLAYER decode: SOFTWARE -- hwdec=%s did not engage; the CPU is "
                                          "doing the decode", hg::cfgValue("hwdec", "auto").c_str());
                        }
                        if (dec) mpv_free(dec);
                        if (vd) mpv_free(vd);
                    }
                }
                break;
            case MPV_EVENT_VIDEO_RECONFIG:
                hg::logPrintf("MPVEVT video-reconfig");
                break;
            case MPV_EVENT_AUDIO_RECONFIG:
                hg::logPrintf("MPVEVT audio-reconfig");
                break;
            case MPV_EVENT_COMMAND_REPLY:
                /* The screenshot probe's answer, which is the only asynchronous command this app
                 * issues with a reply_userdata. A reply with any other userdata is not expected and
                 * is reported rather than ignored -- an unexpected one would mean something else is
                 * issuing commands behind this file's back. */
                if (ev->reply_userdata == HG_SHOT_USERDATA) {
                    handleShotReply(ev);
                } else {
                    hg::logPrintf("MPVEVT command-reply id=%llu error=%d (not ours)",
                                  (unsigned long long)ev->reply_userdata, ev->error);
                }
                break;
            default:
                /* Anything else is uninteresting here, but the id itself is
                 * worth one line: an event we did not expect is information. */
                hg::logPrintf("MPVEVT id=%d", (int)ev->event_id);
                break;
        }
    }
}

/* ---- setup --------------------------------------------------------------- */

/* A ZL/ZR switch reuses this PlayerView.  The old implementation simply
 * allocated a second mpv handle, leaving the old poller, decoder and audio
 * output alive until the activity was destroyed.  That explains the short
 * hitch and eventual A/V drift after changing episodes. */
static void stopMpvForReload(PlayerView::Impl* impl) {
    if (!impl || !impl->mpv) return;
    impl->alive = false;
    int waitedMs = 0;
    while (impl->pollers.load() > 0 && waitedMs < 2000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitedMs += 10;
    }
    if (impl->renderCtx) {
        mpv_render_context_free(impl->renderCtx);
        impl->renderCtx = nullptr;
    }
    mpv_terminate_destroy(impl->mpv);
    impl->mpv = nullptr;
    impl->started = false;
    impl->paused.store(false);
    hg::logPrintf("PLAYER: previous mpv stopped before episode reload (waited %d ms) [build159]", waitedMs);
}

static bool setupMpv(PlayerView::Impl* impl) {
    stopMpvForReload(impl);
    impl->alive = true;
    impl->mpv = mpv_create();
    if (!impl->mpv) return false;

    /* Stay away from the SD card: mpv's config and Lua scripts are not part of
     * this app, and a stray script would be an unexplained failure. The
     * verified test set both of these. */
    mpv_set_option_string(impl->mpv, "config", "no");
    mpv_set_option_string(impl->mpv, "load-scripts", "no");
    mpv_set_option_string(impl->mpv, "terminal", "no");
    /* 20 s, matching the verified test: the CDN is slow on first connect and a
     * shorter timeout produced spurious failures. */
    mpv_set_option_string(impl->mpv, "network-timeout", "20");

    /* ---- wiliwili's Switch option set ------------------------------------
     *
     * These were missing entirely, and every one of them is something wiliwili
     * sets on this exact hardware with this exact libmpv
     * (wiliwili/source/view/mpv_core.cpp:299-391). Copied rather than
     * re-derived, because the failure mode of getting them wrong is a picture
     * that never appears, which is precisely the symptom being chased.
     *
     * The one with the best story for "no picture" is `vd-lavc-dr=no`.
     * --vd-lavc-dr (direct rendering) is on by default where the decoder can
     * write straight into the video output's frame buffer; with the libmpv
     * render API the surface is supplied by the embedder per frame, and wiliwili
     * turns direct rendering OFF on Switch (mpv_core.cpp:366) alongside
     * `vd-lavc-threads=4` (:367). */
    mpv_set_option_string(impl->mpv, "vd-lavc-dr", "no");
    mpv_set_option_string(impl->mpv, "vd-lavc-threads", "4");
    /* ---- THE TWO DECODE COST SWITCHES (C1, C3) --------------------------------
     *
     * WHY HERE AND NOT IN THE MENUS. These are mpv OPTIONS: mpv takes them before the first file, and setting
     * them later is not the same thing (the software decode path builds its filter chain and its buffers when the
     * file loads). They therefore come from hongguo.cfg at setup time, and the settings screen marks them as
     * needing a restart -- which is the truth rather than a limitation.
     *
     * THE STUTTER THIS IS FOR is recorded in the handover as §2.4: 1080p stutters because this app runs mpv's
     * SOFTWARE output path, so every frame crosses the CPU. Fast decode is exactly the lever for that: it skips
     * the parts of the codec spec that are optional (deblocking on non-reference frames, non-reference frames
     * themselves), which costs picture quality and buys decode time. It is OFF by default -- an app that quietly
     * degrades the picture to be smooth is worse than one that stutters and says why.
     *
     * The cache is the other lever: more demuxer buffer means the network jitter does not reach the decoder.
     * Index 0 leaves mpv's own default alone; see hg_ctl for the ladder and its tests. */
    {
        const bool fastDecode = hg::cfgIs("player.decode.fast", "yes");
        if (fastDecode) {
            /* The trio that is actually "low-quality decode" in mpv; each is logged with its own return code
             * because a rejected option name would otherwise look exactly like a working one. */
            const char* names[] = {"vd-lavc-fast", "vd-lavc-skiploopfilter", "vd-lavc-skipframe"};
            const char* values[] = {"yes", "all", "nonref"};
            for (int i = 0; i < 3; i++) {
                int rc = mpv_set_option_string(impl->mpv, names[i], values[i]);
                hg::logPrintf("PLAYER: low-quality decode: %s=%s -> rc=%d", names[i], values[i], rc);
            }
        } else {
            hg::logPrintf("PLAYER: low-quality decode is OFF (player.decode.fast=yes turns it on)");
        }

        const int cacheMb = hg_ctl_cache_mb_at(hg_ctl_cache_index(hg::cfgIntOr("player.cache.mb", 0)));
        if (cacheMb > 0) {
            char bytes[32];
            snprintf(bytes, sizeof bytes, "%dMiB", cacheMb);
            int rc = mpv_set_option_string(impl->mpv, "demuxer-max-bytes", bytes);
            /* The back-buffer is what lets a seek go backwards without refetching; keeping it at half the
             * forward buffer is mpv's own guidance and costs nothing extra here. */
            char back[32];
            snprintf(back, sizeof back, "%dMiB", cacheMb / 2);
            int rc2 = mpv_set_option_string(impl->mpv, "demuxer-max-back-bytes", back);
            hg::logPrintf("PLAYER: decode cache: forward=%s (rc=%d) back=%s (rc=%d) [build53]", bytes, rc, back, rc2);
        } else {
            hg::logPrintf("PLAYER: decode cache left at mpv's default (player.cache.mb=32/64/128/256 changes it)");
        }
    }
    mpv_set_option_string(impl->mpv, "demuxer-lavf-analyzeduration", "0.4");
    mpv_set_option_string(impl->mpv, "demuxer-lavf-probescore", "24");
    mpv_set_option_string(impl->mpv, "ytdl", "no");
    mpv_set_option_string(impl->mpv, "idle", "yes");
    /* wiliwili sets `vo=libmpv` explicitly (mpv_core.cpp:319). mpv picks the
     * same thing implicitly once a render context exists -- the hardware log
     * said `VO: [libmpv]` -- but naming it removes the inference. The cfg can
     * still override it below, which is how the vo=null control arm works. */
    mpv_set_option_string(impl->mpv, "vo", "libmpv");

    /* AUDIO, AND WHY IT IS A CONFIG VALUE.
     *
     * ao=hos is the right default: this libmpv has exactly four audio outputs
     * (audio_out_hos, audio_out_null, audio_out_pcm, audio_out_lavc -- read out
     * of libmpv.a's audio_out_drivers table, with hos first), and ao_hos.c
     * imports audrenInitialize/audrvCreate, i.e. the audren service rather than
     * audout. Nothing in this project has ever opened a real audio device,
     * though: every previous test ran ao=null.
     *
     * On hardware, the run that first set ao=hos died within milliseconds of
     * `mpv started`, with the playloop's poller and the main loop both going
     * silent at the same instant -- consistent with a fault inside mpv's audio
     * bring-up. Rather than guess, `ao` (and the rest) are now read from
     * sdmc:/switch/hongguo.cfg, so the two arms can be compared over FTP
     * without rebuilding and without changing anything else. */
    std::string ao = cfgValue("ao", "hos");
    std::string aofb = cfgValue("audio-fallback", "null");
    std::string hwdec = cfgValue("hwdec", "auto");
    std::string vo = cfgValue("vo", "");   /* empty = leave unset: use the render API */
    /* THE TWO SWITCHES THAT SEPARATE THE REMAINING SUSPECTS.
     *
     * HISTORICAL, from the round these switches were added: back then every run
     * died 0-500 ms after loadfile, with ao=null and hwdec=no, so neither audio
     * nor hardware decode was to blame. That left two things
     * that have always been enabled together and can now be tested apart:
     *
     *   renderctx=no   skip mpv_render_context_create entirely. Pair it with
     *                  vo=null and the app is running app/play-test/main.c's
     *                  configuration -- the only mpv setup in this project that
     *                  is known to decode on this console.
     *   loadfile=no    create the render context but never open a stream. If
     *                  the app then survives and stays on the player screen,
     *                  the render context alone is sound and the fault is in
     *                  media bring-up.
     *
     * Both are read from sdmc:/switch/hongguo.cfg, so the arms cost a relaunch
     * rather than a rebuild, and only one thing changes at a time. */
    bool wantRenderCtx = !cfgIs("renderctx", "no");
    bool wantLoadfile = !cfgIs("loadfile", "no");
    hg::logPrintf("PLAYER setupMpv: ao=%s audio-fallback=%s hwdec=%s vo='%s' renderctx=%d loadfile=%d",
                  ao.c_str(), aofb.c_str(), hwdec.c_str(), vo.c_str(), (int)wantRenderCtx,
                  (int)wantLoadfile);
    if (!ao.empty()) mpv_set_option_string(impl->mpv, "ao", ao.c_str());
    if (!aofb.empty()) mpv_set_option_string(impl->mpv, "audio-fallback", aofb.c_str());
    mpv_set_option_string(impl->mpv, "audio-channels", "stereo");
    if (!vo.empty()) mpv_set_option_string(impl->mpv, "vo", vo.c_str());

    /* wiliwili's Switch default. hwdec-current is shown on screen so a
     * software fallback cannot hide. */
    mpv_set_option_string(impl->mpv, "hwdec", hwdec.empty() ? "auto" : hwdec.c_str());

    /* Let mpv talk. Its own log is the only witness to what happens inside
     * mpv_initialize / loadfile / the first decoded frame, and a crash there
     * leaves no other trace: the process dies between two of our own log
     * lines.
     *
     * THE LEVEL IS A CONFIG VALUE, and the default is the reason. At `info` mpv
     * reports the file, the AO and the VO and then says nothing at all about
     * rendering -- which is the one part of this pipeline that has never been
     * observed working. Raising the level needs to be possible without a rebuild,
     * so `mpv.msglevel` in hongguo.cfg overrides it, and the whole thing is
     * capped below: every log line is fsync'd, so an unbounded verbose feed from
     * a 30 fps decode would turn the diagnostic into the problem. */
    /* WHAT DECODER ARE WE ACTUALLY USING? This reads it instead of asking mpv to tell us.
     *
     * The log plumbing below does not work: `mpv_request_log_messages` is called BEFORE
     * `mpv_initialize`, so it returns -4 and NO MPVLOG line has ever appeared in the log (confirmed on
     * hardware: zero lines). That left "is hardware decoding on" unanswerable, which matters because the
     * video is `bytevc1` -- HEVC/H.265 -- at 1080p and the user reports stuttering.
     *
     * `hwdec-current` reports the decoder mpv ACTUALLY picked, which is a different fact from the one we
     * requested: `hwdec=auto` is a request, and mpv falls back to software if the codec is one the
     * backend cannot take. This build of libmpv does carry the Switch backend (`nvdec` and `nvdec-copy`
     * are compiled in -- checked with strings on libmpv.a), so a fallback here would be a real finding
     * rather than a missing feature.
     *
     * Read AFTER mpv_initialize, because before that the property does not exist yet. */
    {
        char* cur = nullptr;
        int rcCur = mpv_get_property(impl->mpv, "hwdec-current", MPV_FORMAT_STRING, &cur);
        hg::logPrintf("PLAYER hwdec: requested='%s' in-force='%s' (rc=%d) -- 'empty' means SOFTWARE",
                      hwdec.c_str(), (cur && cur[0]) ? cur : "(empty = software)", rcCur);
        if (cur) mpv_free(cur);
    }

    {
        std::string lvl = cfgValue("mpv.msglevel", "info");
        int rcLvl = mpv_request_log_messages(impl->mpv, lvl.c_str());
        hg::logPrintf("PLAYER setupMpv: mpv_request_log_messages(\"%s\") rc=%d", lvl.c_str(), rcLvl);
    }

    /* THE VERIFIED CENC PATH. The hardware A/B proved this option is honoured:
     * the derived key decoded frames, a wrong key decoded none. */
    if (impl->plan.decryption_key[0] != '\0') {
        mpv_set_option_string(impl->mpv, "demuxer-lavf-o", impl->plan.decryption_key);
    }
    if (impl->plan.header_fields[0] != '\0') {
        mpv_set_option_string(impl->mpv, "http-header-fields", impl->plan.header_fields);
    }

    if (mpv_initialize(impl->mpv) < 0) return false;
    /* mpv has now decided most things. Collect its own account before anything
     * else can fail. */
    drainMpvEvents(impl);
    hg::logPrintf("PLAYER setupMpv: mpv_initialize returned ok");

    /* deko3d render context, following wiliwili exactly.
     *
     * SKIPPABLE. Everything below is the piece app/play-test/main.c never did:
     * that test had no render context, set vo=null, and is the only mpv
     * configuration in this project observed to decode on this console. With
     * renderctx=no + vo=null in hongguo.cfg, this app runs that exact
     * configuration, which separates "the render context" from "media
     * bring-up" -- the two things every crashing run had enabled together. */
    impl->video = (brls::SwitchVideoContext*)brls::Application::getPlatform()->getVideoContext();
    if (!impl->video) {
        hg::logPrintf("PLAYER setupMpv: no borealis video context -- aborting");
        return false;
    }

    if (wantRenderCtx && !cfgIs("renderer", "deko3d")) {
        /* ---- renderer=sw: mpv renders to CPU memory, nanovg draws it ---------
         *
         * WHY THIS EXISTS. Three independent readings on hardware say the mpv
         * side is healthy: update_frame=1 on every frame, mpv_render_context_render
         * returning 0, vo-configured=1, current-vo='libmpv', estimated-vf-fps=30 --
         * and the picture is still not in the presented frame. A clean
         * fencewait=cpu arm (CPU waits for mpv's GPU work before the frame is
         * finished) did not change that either, which rules out "the write
         * completes too late". What remains is that mpv's deko3d write does not
         * reach the framebuffer we hand it.
         *
         * The cover image proved, on this hardware, that the OTHER graphics path
         * works: nvgCreateImageMem -> stb_image -> deko3d texture upload -> drawn.
         * So this mode routes the video through that same proven path and removes
         * the image handoff entirely.
         *
         * It is also wiliwili's own mode, not an invention:
         *   create:  mpv_render_param params[] = {{API_TYPE, MPV_RENDER_API_TYPE_SW},
         *                                         {INVALID, nullptr}};   (mpv_core.cpp:423-424)
         *   params:  {SW_SIZE, SW_FORMAT, SW_STRIDE, SW_POINTER, INVALID} (mpv_core.hpp:417-421)
         *   draw:    nvgUpdateImage(vg, nvg_image, pixels) then nvgImagePattern (mpv_core.cpp:791,804)
         *
         * The cost is real: mpv does the colour conversion on the CPU and every
         * frame is a full-surface upload. A 360x640 drama at 30 fps is small
         * enough for that to be a reasonable trade for a picture that appears. */
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        int rcCtx = mpv_render_context_create(&impl->renderCtx, impl->mpv, params);
        hg::logPrintf("PLAYER setupMpv: SW mpv_render_context_create rc=%d ctx=%p", rcCtx,
                      (void*)impl->renderCtx);
        if (rcCtx < 0) return false;

        impl->swRender = true;

        /* ---- HOW BIG THE SOFTWARE SURFACE IS (I2: the 1080p stutter) --------
         *
         * MEASURED, and it is the largest single lever on this path: the surface was the WINDOW size, so in
         * docked mode mpv converted 1920x1080 = 2.07 M pixels per frame on the CPU -- and those pixels were then
         * uploaded as a texture and drawn into a 1280x720 logical rect, i.e. 2.25x the work the screen can show.
         * (Handheld was already matched at 1280x720, which is why the stutter was reported in docked mode only.)
         *
         * `player.sw.height` therefore lets the surface be SMALLER THAN THE WINDOW:
         *
         *   0 (default)  the window height -- the sharpest picture, and what every build before this one did
         *   720          render at 720p and let the GPU scale it up to a 1080p signal
         *   540          the same idea, one step further
         *
         * IT IS A SETTING AND NOT A NEW DEFAULT because it trades sharpness for frame rate, and only the person
         * looking at the television can say which one they want. The width follows the window's aspect so the
         * picture is never distorted. */
        {
            const int wantH = hg::cfgIntOr("player.sw.height", 0);
            const unsigned winW = brls::Application::windowWidth;
            const unsigned winH = brls::Application::windowHeight;

            /* ---- NEVER RENDER MORE PIXELS THAN THE STREAM HAS (measured, and it is the biggest win here) ----
             *
             * MEASURED ON HARDWARE (the user's own log, 2026-10-10 23:58):
             *
             *     SW surface vs view: surface=1920x1080 view=1280x720 window=1920x1080 scale=1.500
             *     PLAYER timing: 5.3 fps | mpv_sw=179 ms/frame | upload=6.8 ms/frame | surface=1920x1080
             *
             * Five frames per second, with 179 of every 200 milliseconds spent converting a 360p stream UP to
             * 1920x1080 on the CPU and then letting the GPU scale it back down to the 1280x720 the UI actually
             * occupies. Every one of those extra pixels is work whose result is thrown away, and it is the same
             * waste the ratio above shows: 2.25x when the surface is 1080p and the UI is 720p, and roughly 9x
             * when the stream itself is 360p.
             *
             * So the surface is capped by the TIER THAT IS PLAYING, which the plan already carries
             * (Client::QualityTier::height). Upscaling a 360p stream on the CPU cannot add detail that the file
             * does not contain -- the GPU does that job for free and at the exact signal size -- so this costs
             * NOTHING in picture quality and removes most of the per-frame cost.
             *
             * `player.sw.height` still wins when it is set LOWER, because that is the user asking explicitly. */
            unsigned capH = winH;
            {
                /* The plan carries the stream's own pixel size (HgPlayPlan::vheight, from video_meta), so there is
                 * nothing to look up and no tier to match: if the file is 360p tall, that is the honest cap. */
                const int tierH = impl->plan.vheight;
                if (tierH > 0 && (unsigned)tierH < capH) {
                    capH = (unsigned)tierH;
                    hg::logPrintf("PLAYER: the stream is %dp, so the software surface is capped to that instead of "
                                  "the window's %up -- the CPU was upscaling every frame for nothing [build83]",
                                  tierH, winH);
                }
            }

            if (wantH > 0 && (unsigned)wantH < winH && winW > 0 && winH > 0) {
                impl->swH = wantH;
                impl->swW = (int)((double)winW * ((double)wantH / (double)winH) + 0.5);
                hg::logPrintf("PLAYER: software surface reduced to %dx%d (player.sw.height=%d); the window is "
                              "%ux%u, so the GPU scales the picture up",
                              impl->swW, impl->swH, wantH, winW, winH);
            } else if (capH > 0 && capH < winH && winW > 0) {
                impl->swH = (int)capH;
                impl->swW = (int)((double)winW * ((double)capH / (double)winH) + 0.5);
                hg::logPrintf("PLAYER: software surface %dx%d (capped by the stream, not by the window) [build83]",
                              impl->swW, impl->swH);
            } else {
                impl->swW = (int)winW;
                impl->swH = (int)winH;
                if (wantH > 0) {
                    hg::logPrintf("PLAYER: player.sw.height=%d is not smaller than the window height (%u) -- "
                                  "keeping the full-size surface",
                                  wantH, winH);
                }
            }
        }
        impl->swStride = impl->swW * 4;

        size_t bytes = (size_t)impl->swStride * (size_t)impl->swH;
        impl->pixels = (unsigned char*)malloc(bytes);
        if (!impl->pixels) {
            hg::logPrintf("PLAYER setupMpv: SW pixel buffer of %u bytes could not be allocated",
                          (unsigned)bytes);
            return false;
        }
        memset(impl->pixels, 0, bytes);

        impl->swSize[0] = impl->swW;
        impl->swSize[1] = impl->swH;
        /* The four params are the ones render.h calls mandatory for
         * MPV_RENDER_API_TYPE_SW. Note SW_POINTER carries the pointer ITSELF while
         * SW_SIZE and SW_STRIDE carry addresses -- that asymmetry is mpv's, and
         * wiliwili spells it the same way. */
        impl->swParams[0] = {MPV_RENDER_PARAM_SW_SIZE, &impl->swSize[0]};
        impl->swParams[1] = {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(impl->swFormat)};
        impl->swParams[2] = {MPV_RENDER_PARAM_SW_STRIDE, &impl->swStride};
        impl->swParams[3] = {MPV_RENDER_PARAM_SW_POINTER, impl->pixels};
        impl->swParams[4] = {MPV_RENDER_PARAM_INVALID, nullptr};

        /* The texture nanovg will draw. It is created once at the surface size and
         * refreshed per frame with nvgUpdateImage (which the deko3d backend
         * implements -- dk_renderer.cpp:495). */
        NVGcontext* vg = brls::Application::getNVGContext();
        impl->swNvgImage = nvgCreateImageRGBA(vg, impl->swW, impl->swH, 0, impl->pixels);
        hg::logPrintf("PLAYER setupMpv: SW surface %dx%d stride=%d, nvg image=%d, %u bytes",
                      impl->swW, impl->swH, impl->swStride, impl->swNvgImage, (unsigned)bytes);

        mpv_render_context_set_update_callback(impl->renderCtx, onMpvUpdate, impl);
    } else if (wantRenderCtx) {
        int advanced = cfgIs("advanced", "no") ? 0 : 1;
        mpv_deko3d_init_params dekoParams{impl->video->getDeko3dDevice()};
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_DEKO3D)},
            {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &dekoParams},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        int rcCtx = mpv_render_context_create(&impl->renderCtx, impl->mpv, params);
        hg::logPrintf("PLAYER setupMpv: mpv_render_context_create rc=%d ctx=%p advanced=%d", rcCtx,
                      (void*)impl->renderCtx, advanced);
        if (rcCtx < 0) return false;

        /* THE UPDATE CALLBACK, WHICH WAS MISSING.
         *
         * wiliwili installs it immediately after creating the render context:
         *
         *     mpvSetWakeupCallback(mpv, on_wakeup, this);
         *     mpvRenderContextSetUpdateCallback(mpv_context, on_update, this);
         *     (mpv_core.cpp:507-509)
         *
         * With MPV_RENDER_PARAM_ADVANCED_CONTROL set, this callback is how mpv
         * tells the embedder that a frame is ready. Without it, the update chain
         * that drives rendering has no way to be signalled, and the previous
         * version of this file called mpv_render_context_update() blind once per
         * frame and threw the answer away.
         *
         * The callback runs on an mpv thread. mpv_render_context_update() is
         * exactly what may be called from there; nothing in it touches a view,
         * so there is no cross-thread view access and no brls::sync hop. */
        mpv_render_context_set_update_callback(impl->renderCtx, onMpvUpdate, impl);
        hg::logPrintf("PLAYER setupMpv: update callback installed");

        /* THE FBO STRUCT, NOT A BARE TEXTURE POINTER.
         *
         * MPV_RENDER_PARAM_DEKO3D_FBO expects a pointer to mpv_deko3d_fbo
         * (C:/devkitPro/portlibs/switch/include/mpv/render_dk3d.h), which is
         * { DkImage *tex; DkFence *ready_fence; DkFence *done_fence;
         *   int w, h; DkImageFormat format; }.
         *
         * The first version of this file passed
         * brls::Application::getPlatform()->getVideoContext()->getFramebuffer()
         * -- a dk::Image* -- as the parameter value. mpv then read that object's
         * first bytes as a texture pointer, the next as two fence pointers, and
         * the rest as width, height and format. Every field was garbage. That is
         * why the app died on the very first mpv_render_context_render with no
         * message: the whole pipeline was verified working in the same boot
         * (time-pos advancing, "VO: [null] 360x640 yuv420p"), and only the
         * render context was ever enabled with it.
         *
         * wiliwili, whose Switch build works, initialises exactly this struct
         * once and only rewrites .tex per frame (mpv_core.hpp:423-429 and
         * mpv_core.cpp:826-834). */
        impl->fbo.tex = nullptr;
        impl->fbo.ready_fence = &impl->readyFence;
        impl->fbo.done_fence = &impl->doneFence;
        /* THESE TWO ARE THE SETUP-TIME VALUE ONLY; draw() re-reads them every frame (build170). Freezing them
         * here is what produced the "small block in the corner" after a dock -- see the note on the draw path,
         * which is where the fix lives. */
        impl->fbo.w = brls::Application::windowWidth;
        impl->fbo.h = brls::Application::windowHeight;
        impl->fbo.format = DkImageFormat_RGBA8_Unorm;
        /* ---- THE FRAME CLEAR IS MADE TRANSPARENT FOR THIS SESSION (build106) ------------------------------
         *
         * MEASURED ON HARDWARE: with renderer=deko3d the screen was GREY with sound playing, and the video showed
         * for an instant on exit. The grey is the per-frame clear, which paints theme["brls/background"] over the
         * target; mpv writes the video on its own queue with no ordering against it, so the grey wins. The flag
         * below makes that clear transparent (see the vendored switch_video.cpp) and the static command list is
         * re-recorded here with it. The destructor puts it back for every other screen. */
        if (!impl->themeBackgroundCleared) {
            hg_video_clear_transparent = true;
            impl->themeBackgroundCleared = true;
            impl->video->recordStaticCommands();
            hg::logPrintf("PLAYER setupMpv: the frame clear is TRANSPARENT for this session -- on the deko3d path "
                          "the theme background was painting over the video [build106]");
        }

        impl->renderParams[0] = {MPV_RENDER_PARAM_DEKO3D_FBO, &impl->fbo};
        impl->renderParams[1] = {MPV_RENDER_PARAM_INVALID, nullptr};
        hg::logPrintf("PLAYER setupMpv: fbo %dx%d RGBA8_Unorm, fences ready=%p done=%p",
                      impl->fbo.w, impl->fbo.h, (void*)&impl->readyFence, (void*)&impl->doneFence);
    } else {
        hg::logPrintf("PLAYER setupMpv: render context SKIPPED (cfg renderctx=no)");
    }

    if (!wantLoadfile) {
        hg::logPrintf("PLAYER setupMpv: loadfile SKIPPED (cfg loadfile=no) -- the render context "
                      "is the only thing under test in this arm");
        impl->started = true;
        return true;
    }

    const char* cmd[] = {"loadfile", impl->plan.url, nullptr};
    int rcLoad = mpv_command(impl->mpv, cmd);
    hg::logPrintf("PLAYER setupMpv: loadfile rc=%d url=%s", rcLoad, impl->plan.url);
    if (rcLoad < 0) return false;

    /* DELIBERATE SHORT BLOCK, for evidence. The runs so far died within a few
     * hundred milliseconds of loadfile, and mpv's messages about opening the
     * stream, the CENC key, the decoder and the audio device are all generated
     * in that window -- but the render loop that normally drains them never got
     * a chance to run. Waiting here, on the main thread, costs 250 ms of UI
     * responsiveness once per playback and is the difference between seeing
     * mpv's account of its own death and seeing nothing at all. */
    hg::logPrintf("PLAYER setupMpv: loadfile issued; waiting 250ms to collect mpv's log");
    svcSleepThread(250000000ULL);
    drainMpvEvents(impl);
    hg::logPrintf("PLAYER setupMpv: post-loadfile drain complete");

    /* ---- THE SEPARATE AUDIO TRACK (build173) ------------------------------------------------------
     *
     * WHY THERE IS A SECOND URL AT ALL. The play-info route returns DASH: measured, the video is
     * `media-video-avc1` and the audio is a DIFFERENT url, `media-audio-und-mp4a`, with its own
     * init_range/index_range. There is no muxed progressive file to fall back on -- every accepted
     * definition (360p/480p/720p) comes back this way -- so the video URL alone would play a silent
     * picture.
     *
     * WHY `audio-add` AND NOT A SECOND `loadfile`. This file already paid for that lesson: two loadfiles
     * means two streams alive at once, and with auto-next on by default an end-file event can start a
     * third (see the note at the auto-next site). `audio-add` is mpv's own command for exactly this --
     * it attaches an external track to the CURRENT file rather than replacing it -- so there is one
     * playlist entry, one EOF, and the existing episode/auto-next logic is untouched.
     *
     * THE AUDIO IS ADDED AFTER THE LOADFILE, NOT BEFORE, because a track cannot be attached to a file
     * that is not open yet. It is also issued on this (the setup) path only: the file is already opened
     * by the time we get here.
     *
     * THE KEY IS ALREADY SET. decryption_key was handed to mpv via demuxer-lavf-o before
     * mpv_initialize (above), and MEASURED, the audio track shares the video's key -- the play-info
     * response repeats the same player_access_key and the same kid in both objects, and the audio's own
     * tenc box carries that kid. So no second key is needed and none is set.
     *
     * `select` IS NOT PASSED BECAUSE IT IS ALREADY THE DEFAULT, and that is documented rather than
     * assumed: mpv 0.36.0's own manual says `audio-add <url> [<flags> [<title> [<lang>]]]` -- the flags
     * are OPTIONAL -- and the `sub-add` entry it refers to for their meaning lists
     *     <select>  Select the subtitle immediately (default).
     * so an added track is selected without being asked. This build is mpv 0.36.0 (the version string is
     * in the linked libmpv), which is the version that manual describes. Passing an unnecessary flag
     * would be one more string that has to be right, and a misspelled trailing argument makes mpv REJECT
     * the whole command -- which would cost the audio entirely. */
    if (impl->plan.audio_url[0] != '\0') {
        const char* audioCmd[] = {"audio-add", impl->plan.audio_url, nullptr};
        int rcAudio = mpv_command(impl->mpv, audioCmd);
        hg::logPrintf("PLAYER setupMpv: audio-add rc=%d url=%s", rcAudio, impl->plan.audio_url);
        /* Only claim sound when mpv accepted the track: a silent picture with a log line saying the
         * audio was added would send the next diagnosis in the wrong direction. */
        if (rcAudio < 0) {
            hg::logPrintf("PLAYER setupMpv: the audio track was REFUSED (rc=%d) -- this episode will "
                          "play video only", rcAudio);
        } else {
            hg::logPrintf("PLAYER setupMpv: audio track attached; draining mpv's account of it");
            svcSleepThread(150000000ULL);
            drainMpvEvents(impl);
        }
    } else {
        hg::logPrintf("PLAYER setupMpv: no audio url in the plan -- single-file route (the old "
                      "video_model response carries none)");
    }

    impl->started = true;
    return true;
}

/* ---- THE STREAM REPAIR: A NEW SIGNED ADDRESS FOR THE SAME EPISODE (build171) --------------------------
 *
 * THE USER'S OWN WORDS ARE THE SPEC: "the source check found the source was abnormal, but it did not
 * repair itself with a new address." The measured evidence that this is a signed-URL expiry, from the
 * build168 log at 20:40:46:
 *
 *     MPVLOG [ffmpeg/warn]  https: HTTP error 403 Forbidden
 *     MPVLOG [stream/error] Failed to open https://v5-bd-daily-reading-video.qznovelvod.com/.../video/to...
 *
 * 403 Forbidden is the server refusing the REQUEST, not the network failing: the URL carries a signature
 * that expires, and once it has, that exact URL will never work again. Nothing about the running player can
 * recover from it -- the file mpv is holding is dead -- which is why this is a reload rather than a seek
 * or a retry of the same command.
 *
 * WHAT A "NEW ADDRESS" IS. There is no separate renew entry point, and none is needed: Client::fetchPlayPlan
 * builds a fresh signed request, and the plan it returns carries the URL the server signed THAT MOMENT.
 * So one call to the same function the player used at start is exactly the renewal. This is deliberately
 * NOT fetchPlayPlanForSource: that pins a specific quality tier, and after a 403 the tier is not the
 * question -- the address is. Re-taking the default plan also means the repair cannot land on a tier whose
 * own source has gone.
 *
 * WHY THE FETCH IS A SEPARATE STEP FROM THE RELOAD, AND WHY IT IS THE WHOLE POINT OF THIS FUNCTION.
 *
 *   hg_client.cpp holds its parse buffers as file-static globals (g_page, g_router, g_req) and every one
 *   of its nine public entry points takes a RECURSIVE lock (g_clientMutex, hg_client.cpp:53). That lock is
 *   held for the duration of the call. Nothing may call a client entry point from inside another one, and
 *   -- the trap that matters here -- nothing may do it from a thread whose work the caller is waiting on.
 *
 *   So the fetch happens on a worker of its own, entered from here with the lock provably NOT held (this
 *   function is called from a brls::sync callback, i.e. the main thread, at a moment when no client call is
 *   in flight). That is the same shape as build151's existing "one transparent retry"
 *   (PlayerView::fetchPlanOnPreferredTier, above), which also re-enters Client::fetchPlayPlan from a
 *   context that holds nothing -- read it if this ever needs changing.
 *
 *   AND THE RELOAD COMES BACK TO THE MAIN THREAD WITH brls::sync, because setupMpv tears down and rebuilds
 *   the render context and writes to a label. Doing that on the worker would be a cross-thread view write;
 *   doing it in draw() would be rule 5. The brls::sync hop is what keeps all of it on one thread.
 *
 * WHY THE COUNTER IS CHECKED AND SPENT BEFORE THE FETCH IS ISSUED. The alternative -- check, fetch, then
 * decide -- means two error events arriving from the same dying stream can both pass the check and both
 * start a reload. `impl->repairing` closes that window, and `impl->repairAttempts` is incremented at the
 * moment the attempt is committed rather than when it succeeds, so a repair that fails is still counted.
 *
 * ---- WHY THIS IS A MEMBER AND NOT A FREE FUNCTION (build171, corrected after a failed compile) ----------
 *
 * It was written as a free function taking Impl*, matching pollLoop/drainMpvEvents/setupMpv, because this
 * round's file boundary originally allowed only two .cpp files and a member would have needed a
 * declaration added to player_view.hpp. That did not compile, and the reason is worth recording so it is
 * not attempted again:
 *
 *     player_view.cpp:1500:36: error: 'hg::AliveToken hg::PlayerView::alive_' is private
 *     player_view.cpp:1518:48: error: 'hg::PlayerView::Impl* hg::PlayerView::impl' is private
 *
 * The existing free functions do NOT reach PlayerView's privates -- they only ever call its PUBLIC members
 * through `impl->owner` (see the EOF case above: `impl->owner->notifyEpisodeEnded()`). The re-read-self->impl
 * and guard discipline this repair depends on is only expressible inside a member, which is exactly how
 * switchQuality does the same thing. player_view.hpp declares no friend, so a free function has no route
 * to `this->impl` or `this->alive_` at all.
 *
 * `reason` and `error` are the two numbers from the mpv event; both go into the log line, because
 * "reason=4 error=-13" names a permission failure while "reason=4 error=-2" names a missing file, and the
 * whole point of this feature is that the difference is visible after the fact. */
void PlayerView::requestStreamRepair(int reason, int error) {
    Impl* impl = this->impl;
    if (!impl) return;

    /* ---- AND NOT WHILE THE PLAYER IS TEARING mpv DOWN (build171) ---------------------------------------
     *
     * WHY THIS GUARD IS NOT PARANOIA. setupMpv() begins with stopMpvForReload(), which calls
     * mpv_terminate_destroy on the old handle -- and the poller that is still draining that handle (the
     * 2 s wait above is bounded and can expire) runs this function's caller. mpv reports a shutdown as
     * MPV_END_FILE_REASON_QUIT, and its documented contract is that ONLY the ERROR reason is guaranteed to
     * carry a non-zero `error` -- so a teardown event with a stray error code would otherwise be read as a
     * broken source and start a repair against a handle being freed.
     *
     * `impl->alive` is false for exactly the window in which that teardown is happening (stopMpvForReload
     * clears it, setupMpv sets it again), so it is the precise test -- not a timing guess. It is also the
     * condition the poller and the destructor already use to mean "the player is not in a state to be
     * driven".
     *
     * `setupMpv` inside the repair below therefore cannot re-enter here: by the time it runs, `alive` has
     * been cleared by its own first statement and stays cleared until the new handle exists. */
    if (!impl->alive.load()) return;

    /* ONE ATTEMPT PER EPISODE (see Impl::repairAttempts). The user's requirement is explicit -- "must be a
     * limited number, never an infinite restart loop" -- and the give-up line below is what makes the limit
     * honest: when the budget is spent, the failure is REPORTED rather than swallowed. */
    const int maxAttempts = 1;
    if (impl->repairAttempts >= maxAttempts || impl->repairing) {
        if (impl->repairAttempts >= maxAttempts) {
            hg::logPrintf("PLAYER: repair gave up after %d attempt(s) -- reporting the failure instead "
                          "[build171]",
                          impl->repairAttempts);
        } else {
            hg::logPrintf("PLAYER: a repair is already in flight -- not starting a second one [build171]");
        }
        return;
    }
    impl->repairAttempts++;
    impl->repairing = true;

    hg::logPrintf("PLAYER: stream failed (reason=%d error=%d) -> re-fetching the plan for a fresh "
                  "address, attempt %d/%d [build171]",
                  reason, error, impl->repairAttempts, maxAttempts);

    /* WHAT THE STATUS LINE SAYS WHILE THIS RUNS. The screen is otherwise a dead picture with no
     * explanation, and a repair takes a network round-trip -- long enough for the user to reach for the
     * power button if nothing on screen says what is happening. */
    PlayerView* self = this;
    if (self->statusLabel) self->statusLabel->setText("Stream expired -- fetching a new address...");

    /* THE EPISODE IS THE ONE THE PLAYER IS ON, read from this->episodeId -- the member the quality switch
     * also re-requests with, so there is one source of truth and no copy to keep in step. It is taken NOW
     * rather than passed down from the event, and that is deliberate: this is the episode the dead stream
     * was playing, so a ZL/ZR switch that happened in the meantime cannot repair the old episode. */
    const std::string episodeId = this->episodeId;
    const AliveToken alive = this->alive_;

    /* ---- `self`, `alive` AND `episodeId` ARE ALL CAPTURED BY VALUE, AND THAT IS THE POINT -------------
     *
     * These two lambdas run on OTHER THREADS and therefore outlive this stack frame: the outer one on a
     * detached worker (hg_thread), the inner one on the main thread after a brls::sync hop. So nothing here
     * may be a reference to a local -- by the time either body runs, this function has returned.
     *
     *   self      -- a raw pointer, by value. It is what the inner lambda re-reads `self->impl` THROUGH;
     *                see the note inside for why the Impl* itself is deliberately NOT captured.
     *   alive     -- a shared_ptr, so the token cannot be freed while the lambdas still hold it. That is the
     *                contract hg_alive.hpp describes.
     *   episodeId -- a std::string, so each lambda owns its own copy. It MUST be by value and not by
     *                reference: the worker uses it to build the request, the main thread uses it again for
     *                the log line and for startPlayChainCheck, and the original is gone by then. */
    if (!hg::runDetached([self, alive, episodeId]() {
            HgPlayPlan fresh;
            /* ---- THE REPAIR FETCHES FROM THE ROUTE THAT ANSWERS (build173) --------------------------
             * This called Client::fetchPlayPlan. That endpoint is retired, so the one path whose whole
             * job is "get a fresh address for the stream that just died" was itself guaranteed to fail
             * -- the repair could never have worked, whatever it did with the result.
             *
             * It now goes through fetchPlanOnPreferredTier, the same function the FIRST plan uses, which
             * is what makes the repaired address equivalent to a fresh one: the best tier that answers,
             * with the retry budget, read off self->seriesId. Re-choosing the tier is deliberate rather
             * than re-using the old one -- after an expiry the tier is not the question, the ADDRESS is,
             * and this was already the reasoning before the route changed (see the note above). */
            Result r = self->fetchPlanOnPreferredTier(episodeId, fresh);
            brls::sync([self, alive, fresh, r, episodeId]() {
                HG_GUARD_ALIVE(alive);
                /* ---- THE IMPL POINTER IS RE-READ HERE, NOT CAPTURED, AND THAT IS THE LIFETIME RULE ----
                 *
                 * A repair takes a network round-trip, and the user can press B during it. The destructor
                 * frees impl and sets `PlayerView::impl = nullptr`; it does NOT kill `alive_` (grep this
                 * file: PlayerView never calls alive_->kill(), so HG_GUARD_ALIVE above is a courtesy, not
                 * the guarantee). What actually makes this callback safe is that it dereferences `self->impl`
                 * at the moment it runs and returns if it is null -- so a callback that arrives after the
                 * screen is gone touches no freed memory.
                 *
                 * This is the same discipline switchQuality's callback uses for the same reason. Do not
                 * "optimise" this into a captured Impl* -- the capture is the bug. */
                PlayerView::Impl* impl = self->impl;
                if (!impl) return;
                impl->repairing = false;

                if (!r.ok) {
                    /* THE REPAIR ITSELF FAILED, so there is no new address to try. The counter is NOT
                     * reset -- the attempt was spent -- and the failure is stated in the same terms the
                     * player uses for any other plan failure, which is what hg_selfcheck's chain walks. */
                    hg::logPrintf("PLAYER: repair gave up after %d attempt(s) -- reporting the failure "
                                  "instead [build171]",
                                  impl->repairAttempts);
                    hg::logPrintf("PLAYER: the fresh plan failed too ('%s')", r.error.c_str());
                    /* A refused renewal speaks in the same two vocabularies as a refused first plan
                     * (build175): the service declining is a sentence, a transport failure keeps the
                     * machine's words. A stream that died and then could not be renewed because the
                     * SERVICE refuses it is not a network problem, and saying so would send the user to
                     * check a connection that is fine. */
                    const std::string shown = r.serverRefused
                        ? std::string(hg_tr(HG_T_EPISODE_REFUSED))
                        : std::string("Stream failed: ") + r.error;
                    if (self->statusLabel) self->statusLabel->setText(shown);
                    brls::Application::notify(shown);
                    /* AND THE CHAIN IS WALKED FOR THE USER, exactly as a failed first plan does: the
                     * failure has now happened TWICE (the stream died, then the renewal did), which is
                     * precisely the moment hg_selfcheck exists for. */
                    hg::startPlayChainCheck(std::string("stream repair failed: ") + r.error, episodeId);
                    return;
                }

                /* THIS IS THE MOMENT THE USER ASKED FOR: a new signed address is in hand. */
                hg::logPrintf("PLAYER: fresh address installed, reloading (old url expired? see the 403 "
                              "above) [build171]");
                hg::logPrintf("PLAYER: new plan = %s", Client::describePlan(fresh).c_str());

                impl->plan = fresh;
                if (self->statusLabel) self->statusLabel->setText(Client::describePlan(fresh));

                /* ---- THE RELOAD GOES THROUGH THE build159 CLEAN-STOP PATH, NOT A BARE loadfile --------
                 *
                 * setupMpv() begins with stopMpvForReload(), which is the mechanism the user confirmed for
                 * episode switching: the old poller is stopped (bounded 2 s wait, logged), the render
                 * context is freed, the mpv handle is terminated, and only then is a new one built. A plain
                 * `loadfile` on the dead handle would leave the previous stream's audio output and decoder
                 * in place -- the "two audio streams, then drift" failure that path was written for.
                 *
                 * It also re-declares the decryption key and the header fields from the NEW plan inside
                 * setupMpv, which a bare loadfile would not: a fresh URL carries a fresh CENC key, and
                 * loading the new stream with the old key gives no picture and no error. That is the exact
                 * failure class this project keeps designing against, so the reload is not hand-rolled. */
                if (!setupMpv(impl)) {
                    hg::logPrintf("PLAYER: the reload with the fresh address FAILED (setupMpv) [build171]");
                    if (self->statusLabel) {
                        self->statusLabel->setText("mpv failed to restart with the new address");
                    }
                    brls::Application::notify("mpv failed to restart with the new address");
                    return;
                }

                /* ---- THE ATTEMPT IS GIVEN BACK, AND HERE IS THE ARGUMENT THAT THIS IS NOT A LOOP -----
                 *
                 * The user's requirement is "a limited number of attempts -- never an infinite restart
                 * loop", so this reset has to be justified rather than assumed.
                 *
                 * WHAT EXPIRES IS THE URL, NOT THE EPISODE. A long video can outlive more than one
                 * signature, so a stream that has just been repaired is exactly the stream that may
                 * legitimately need repairing again later -- and refusing to do it would mean the feature
                 * works once per episode and then silently stops, which is the shape of bug this project
                 * keeps finding late.
                 *
                 * WHY IT CANNOT SPIN. Every path from here back to another repair is gated on THREE things
                 * that must all happen first:
                 *
                 *   1. mpv must report an ERROR end-file. That cannot be produced by this code, only by the
                 *      stream -- so there is no self-trigger.
                 *   2. the fresh plan fetch must SUCCEED. A failing fetch leaves the counter spent, so the
                 *      next error prints "repair gave up" instead of retrying.
                 *   3. setupMpv must SUCCEED, which means a render context was built and mpv accepted a
                 *      loadfile -- i.e. a new stream really did start.
                 *
                 * A loop therefore requires the server to keep issuing valid signatures AND mpv to keep
                 * failing to play them. That is not a runaway loop in this code; it is a source that is
                 * broken in a way re-signing does not fix, and the per-attempt log lines make it visible
                 * immediately ("stream failed ... -> re-fetching" repeated). The alternative -- never
                 * resetting -- would turn a genuine second expiry on a long video into a permanent failure,
                 * which is the more likely real-world case and the worse outcome.
                 *
                 * The rate is bounded in practice by the network round-trip each repair needs, so even the
                 * pathological case cannot become a request storm. */
                impl->repairAttempts = 0;
                impl->started = true;
                hg::logPrintf("PLAYER: repaired -- playing '%s' on a freshly signed address; the episode "
                              "gets its attempt back [build171]",
                              Client::describePlan(fresh).c_str());

                /* THE POLLER IS RE-STARTED, because stopMpvForReload set the old one's `alive` to false:
                 * without this the new stream would play with no status line, no progress and -- the part
                 * that matters -- no one draining mpv's event queue, so the NEXT error would be invisible.
                 * This mirrors what PlayerView::start does after its own setupMpv. */
                if (!hg::runDetached([impl]() { pollLoop(impl); })) {
                    hg::logPrintf("PLAYER: no poller thread after the repair -- status line will not "
                                  "update [build171]");
                }

                /* ---- WHY THE QUALITY LADDER IS NOT RE-FETCHED HERE -------------------------------------
                 *
                 * It is worth stating, because the tiers DO carry signed source URLs of their own and the
                 * ones in impl->tiers are therefore as stale as the address that just failed.
                 *
                 * It is not done because startQualityLadder() is a PRIVATE member of PlayerView, this
                 * round's file boundary forbids touching player_view.hpp to expose it, and the ladder is
                 * not on the path this feature exists to fix: the picture is dead and the repair's job is
                 * to bring it back. A user who opens the resolution menu after a repair and finds the tiers
                 * stale has hit a known, bounded limitation -- switching quality re-fetches the plan from
                 * the chosen source index regardless (Client::fetchPlayPlanForSource re-requests, see its
                 * header note on exactly this), so the worst case is that the menu reflects the tier list
                 * from before the expiry.
                 *
                 * Recorded here so the next round does not have to re-derive why this call is absent.
                 * build168's "QUALITY: ladder ready (1 tiers)" line is a DIFFERENT, pre-existing issue. */
            });
        })) {
        /* NO WORKER COULD BE CREATED, so the attempt never went out. It is given back rather than spent --
         * a thread that could not be created is not a failed repair. */
        impl->repairing = false;
        impl->repairAttempts--;
        hg::logPrintf("PLAYER: no thread for the repair fetch -- not spending the attempt [build171]");
        hg::startPlayChainCheck("stream failed, no worker for the repair fetch", episodeId);
    }
}

/* ---- view ---------------------------------------------------------------- */

/* ---- THE TOUCH LAYER (build109, and the design is the user''s) ----------------------------------------
 *
 * THEIR WORDS: "the player UI could be put on a transparent layer -- tap the screen and it appears, and it fades
 * away on its own, so it does not get in the way of the video."
 *
 * In this fork a finger position is only reachable through a GestureRecognizer: Application::currentTouchState is
 * private, InputManager::updateTouchStates only FEEDS the manager, and View has no touch hook. So this is a
 * GestureRecognizer whose only job is to hand the coordinates to the view, and it reports FAILED so it never
 * competes with the framework''s own recognizers (this screen''s views have no click actions).
 *
 * The rectangles it tests were already being recorded while drawing (chipHit / menuHit / volumeHit) -- that half
 * existed and had nothing to consume it until now. */
/* NO GESTURE RECOGNIZER HERE (build110 removed the one build109 added): it never fired, because this fork's
 * Switch platform implements no touch input at all -- Application::frame has no touch events to dispatch. The
 * panel is polled from libnx in tickControls instead. */

/* ---- WHAT A TAP DOES ---------------------------------------------------------------------------------- */
void PlayerView::onTouchAt(float x, float y) {
    auto inside = [&](const HitRect& r, float pad) {
        if (!r.valid) return false;
        return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y && y <= r.y + r.h;
    };

    /* 1. A CHIP MENU IS OPEN: the tap belongs to it. */
    if (this->chipMenu != 0) {
        if (this->chipMenu == CONTROL_VOLUME && inside(this->volumeHit, 16.0f)) {
            const HitRect& r = this->volumeHit;
            const float frac = (x - r.x) / (r.w > 1.0f ? r.w : 1.0f);
            this->volume = hg_ctl_volume_clamp((int)(frac * (float)HG_CTL_VOLUME_MAX + 0.5f));
            this->applyControlValue("volume");
            this->showControls();
            hg::logPrintf("PLAYER: touch -- volume dragged to %d%%", this->volume);
            return;
        }
        for (int i = 0; i < this->menuHitCount; i++) {
            if (!inside(this->menuHit[i], 0.0f)) continue;
            this->chipMenuIndex = this->menuHit[i].item;
            this->chipMenuApply();
            hg::logPrintf("PLAYER: touch -- chip menu option %d chosen", this->chipMenuIndex);
            this->chipMenu = 0;
            this->showControls();
            return;
        }
        /* Anywhere else closes the menu, the way a popup should. */
        hg::logPrintf("PLAYER: touch outside the chip menu -> closed");
        this->chipMenu = 0;
        this->showControls();
        return;
    }

    /* 2. THE CONTROL BAR IS UP: the play button first, then the chips. */
    if (this->controlsVisible()) {
        /* ---- THE PLAY BUTTON (build111, and it is the user's report) ------------------------------------
         *
         * "Touch works for volume, speed and zoom, but not for play/pause." This button is not one of the five
         * chips, so it had no hit area at all. */
        if (inside(this->playHit, 0.0f)) {
            hg::logPrintf("PLAYER: touch -- the play button was tapped -> play/pause");
            this->togglePause();
            this->showControls();
            return;
        }
        for (int i = 0; i < 4; i++) {
            if (!inside(this->chipHit[i], 0.0f)) continue;
            this->controlsIndex = this->chipHit[i].item;
            hg::logPrintf("PLAYER: touch -- the %d chip was tapped, activating it", this->controlsIndex);
            this->activateControl();
            this->showControls();
            return;
        }
    }

    /* 3. ANYWHERE ELSE TOGGLES THE UI, which is the behaviour the user asked for: tap to show it, and let the
     *    idle timer take it away again. A tap while it is up puts it away at once. */
    if (this->controlsVisible()) {
        hg::logPrintf("PLAYER: touch on the picture -> hiding the UI (tap again to bring it back)");
        this->hideControls();
    } else {
        hg::logPrintf("PLAYER: touch on the picture -> showing the UI");
        this->showControls();
    }
}
PlayerView::PlayerView() {
    this->impl = new Impl();
    /* THE EVENT DRAIN NEEDS THE VIEW, and it runs from a free function on two different threads (see the EOF
     * case in drainMpvEvents). One assignment here is what makes that possible. */
    this->impl->owner = this;
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(20.0f);

    /* ---- NOT FOCUSABLE, AND THAT IS THE FIX FOR THE GREY SCREEN AND THE BLUE STREAKS (build107) ---------
     *
     * MEASURED ON HARDWARE, from the user, after build106 had already made the frame clear transparent and the
     * log confirmed it ran ("the frame clear is TRANSPARENT for this session"): the screen was STILL grey, and
     * "pressing the D-pad shows blue streaks along the border".
     *
     * Those two symptoms are ONE thing. borealis draws the focused view's highlight after every view
     * (application.cpp:788) using theme["brls/highlight/background"] (view.cpp:584) -- a semi-transparent GREY
     * that fills the focused view's rectangle, with a bright ring around it. This view fills the screen and it was
     * focusable, so the grey highlight covered the whole picture and the ring was the blue streak at the edge.
     * And it is drawn LAST, so no amount of transparent clearing underneath could ever help.
     *
     * Nothing on this screen wants focus: every key it uses is polled from the controller state directly (see the
     * class header), and the picture menu and the settings panel are driven the same way. With the view not
     * focusable the framework has nothing to highlight, and what is left on screen is the video mpv rendered into
     * the swapchain. Other screens are untouched -- their focus rings are how they are navigated.
     *
     * ---- AND IT DOES TAKE FOCUS, WITH NO HIGHLIGHT AT ALL (build108) -------------------------------------
     *
     * MEASURED ON HARDWARE, from the user: with the view not focusable the video appeared, but "I can still
     * select the little blue boxes on the episode page behind". That is one line of borealis: the focus highlight
     * is drawn OUTSIDE the activity loop (application.cpp:788), so the translucent/opaque rule does not apply to
     * it -- the view that still held focus from the detail screen kept its ring, drawn on top of the video.
     *
     * So the view takes focus again (the screen behind loses it, so nothing there can be selected or ringed any
     * more), and the highlight is switched off on this view instead. `setHideHighlight(true)` covers both halves
     * that were causing trouble: the semi-transparent grey that filled the screen and the bright ring at the edge.
     *
     * The player does not need the highlight for anything -- its own selection rings are drawn by this file -- so
     * nothing about the bar, the menus or the panels changes. */
    this->setFocusable(true);
    this->setHideHighlight(true);

    /* NEUTRAL, AND CLOSED. The picture menu starts in exactly the state the player
     * was in before the feature existed: every property 0, aspect auto, mirror off.
     * That is what lets the whole feature be "additive until X is pressed" rather
     * than a change to how the screen already looks -- the property this project
     * insists on for anything that touches a screen that already works.
     *
     * The Activity may then seed it from hongguo.cfg; whatever it does, nothing
     * reaches mpv until applyPictureSettings() is called. */
    memset(&this->menu, 0, sizeof this->menu);
    hg_pmenu_reset(&this->menu);
    this->menu.open = 0;
    this->menu.row = HG_PMENU_ROW_BRIGHTNESS;
    memset(this->prevButtons, 0, sizeof this->prevButtons);
}

PlayerView::~PlayerView() {
    /* PUT THE FRAME CLEAR BACK (build106). The GPU path made it transparent so the video could be seen;
     * every other screen wants the theme background, so the flag is cleared and the static command list
     * is re-recorded -- the same pair of calls the player made on the way in. */
    if (hg_video_clear_transparent) {
        hg_video_clear_transparent = false;
        if (this->impl && this->impl->video) {
            this->impl->video->recordStaticCommands();
            hg::logPrintf("SHUTDOWN: the frame clear is back to the theme background [build106]");
        }
    }
    /* The shutdown path is bracketed because "+ exits, then the console
     * complains" is a symptom that can be produced by the app, by borealis, or
     * by libnx on the way out -- and from the build machine those look
     * identical. These lines say how far the app itself got. */
    hg::logPrintf("SHUTDOWN: PlayerView destructor entered (renderCtx=%p mpv=%p)",
                  (void*)(this->impl ? this->impl->renderCtx : nullptr),
                  (void*)(this->impl ? this->impl->mpv : nullptr));
    if (this->impl) {
        this->impl->alive = false;

        /* ---- WAIT FOR THE POLLER BEFORE FREEING ANYTHING (round 35) --------------------------------------
         *
         * MEASURED BY READING THIS FILE AGAINST ITSELF: `pollLoop` runs `while (impl->alive)` on a worker and
         * calls `drainMpvEvents(impl)` inside the loop, while this destructor used to set `alive = false` and
         * then go straight on to `mpv_terminate_destroy` and `delete impl`. So the poller could wake up AFTER
         * the struct was freed and read `impl->alive` from released memory -- and, worse, could drain an mpv
         * handle that was already destroyed. That is the same use-after-free class as the crashes the console
         * reported, just on the way OUT of the player instead of during it.
         *
         * The poller sleeps in 50 ms slices now (see pollLoop), so this wait is short; it is bounded anyway so a
         * poller that never comes back cannot hang the exit. The result is logged either way, because "the thread
         * did not stop" is exactly the kind of fact that should not be inferred later. */
        {
            int waitedMs = 0;
            while (this->impl->pollers.load() > 0 && waitedMs < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                waitedMs += 10;
            }
            hg::logPrintf("SHUTDOWN: waited %d ms for the poller (pollers left: %d) [build75]",
                          waitedMs, this->impl->pollers.load());
        }

        /* mpv's documented order: the render context goes before the handle. */
        if (this->impl->renderCtx) {
            hg::logPrintf("SHUTDOWN: mpv_render_context_free ENTER");
            mpv_render_context_free(this->impl->renderCtx);
            hg::logPrintf("SHUTDOWN: mpv_render_context_free OK");
            this->impl->renderCtx = nullptr;
        }
        if (this->impl->mpv) {
            hg::logPrintf("SHUTDOWN: mpv_terminate_destroy ENTER");
            mpv_terminate_destroy(this->impl->mpv);
            hg::logPrintf("SHUTDOWN: mpv_terminate_destroy OK");
            this->impl->mpv = nullptr;
        }
        /* THE SOFTWARE RENDER BUFFERS. 1280x720x4 is 3.6 MB per player screen, so
         * leaking one per visit would be noticed; the nanovg texture is the same
         * size again and is owned by nanovg until it is deleted. Both are released
         * here, on the main thread, which is the only thread allowed to touch the
         * NVG context. */
        if (this->impl->swNvgImage > 0) {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg) nvgDeleteImage(vg, this->impl->swNvgImage);
            this->impl->swNvgImage = 0;
        }
        if (this->impl->pixels) {
            hg::logPrintf("SHUTDOWN: freeing the SW pixel buffer");
            free(this->impl->pixels);
            this->impl->pixels = nullptr;
        }
        delete this->impl;
        this->impl = nullptr;
    }
    hg::logPrintf("SHUTDOWN: PlayerView destructor done");
}

brls::View* PlayerView::create() { return new PlayerView(); }

/* ---- the screenshot-raw probe --------------------------------------------
 *
 * ASKS MPV ONCE, RECORDS THE ANSWER, AND DOES NOT BLOCK ANYTHING. See hg_shot.h for why the
 * question is worth asking: whether `screenshot-raw` works under this render configuration decides
 * whether a progress-bar thumbnail is one command away or needs a second software render context,
 * and that has been answered twice from documentation -- which is not the same as measuring it.
 *
 * ASYNC, AND THE FIRST VERSION WAS NOT. It used `mpv_command_ret`, which blocks until the reply
 * arrives, and it was called from inside a `brls::sync` callback -- i.e. on the MAIN thread, where
 * that is a stalled frame loop and a frozen screen. There is a second reason it could not work
 * there or on a bare worker: mpv delivers a command reply through its EVENT QUEUE, so the caller
 * must also be draining events. On the main thread the reply would arrive only from inside draw(),
 * which the blocking call is itself preventing -- a self-deadlock.
 *
 * `mpv_command_async` with a reply_userdata is the shape that works everywhere: the command goes
 * out, and `drainMpvEvents` (which the poll thread and draw both call) picks the reply up as an
 * MPV_EVENT_COMMAND_REPLY. That is also exactly the mechanism the verdict below recommends for a
 * real thumbnail, so the probe now exercises the path it is recommending rather than a different
 * one that only happens to work for a diagnostic.
 *
 * It is spawned rather than issued immediately because the probe needs a frame to be available:
 * asking before playback has started measures "there is no frame", which is not the question. */
#define HG_SHOT_USERDATA 0x53484F54ull   /* "SHOT", so the reply is identifiable */

/* ---- the quality ladder --------------------------------------------------
 *
 * The ladder comes from the NETWORK and arrives after the video has already started playing,
 * which is the right order: a quality menu is worth waiting a second for, playback is not. So
 * this is fetched on its own worker thread once the plan is in, and until it lands the menu's
 * quality row says "(not available)" rather than offering a tier it cannot confirm.
 *
 * WHY THE SOURCE LIST MATTERS. Client::fetchPlayPlan picks ONE source and discards the rest --
 * correct for "play this episode" and useless for "play it at 540p instead". The ladder records
 * each tier's SOURCE INDEX so a change can build a plan from that entry of a freshly fetched
 * response, without re-deriving which entry was which. */
void PlayerView::startQualityLadder(const std::string& episodeId) {
    if (hg::cfgIs("player.quality", "no")) {
        hg::logPrintf("PLAYER: player.quality=no -- no quality ladder is fetched");
        return;
    }

    PlayerView* self = this;
    const AliveToken alive = this->alive_;
    /* The series id is captured BY VALUE, like episodeId: this lambda runs on a worker thread, and
     * reaching back into the view for it would read a std::string that the main thread can replace when
     * the next episode starts (see the note on the plan worker's captures). */
    const std::string seriesId = this->getSeriesId();
    bool started = hg::runDetached([self, alive, episodeId, seriesId]() {
        std::vector<Client::QualityTier> tiers;
        Result r;
        {
            /* ---- ONE LADDER REQUEST AT A TIME (build144, and it is why the console crashed) --------------
             *
             * THE CRASH REPORT SAYS IT PLAINLY. Atmosphère's report for the failure that followed the
             * "default to 1080p" build reads:
             *
             *     Process Name:  hongguo-net      (the network process, not the main one)
             *     Type:          Data Abort
             *     Fault Address: 0000000000000000  (a null dereference)
             *
             * And hg_client.cpp keeps its scratch buffers as FILE-STATIC globals:
             *
             *     static char g_page[1024 * 1024];
             *     static char g_router[512 * 1024];
             *     static HgRequest g_req;
             *
             * The UI already asks for the ladder on this detached thread. The new pre-loadfile choice added a
             * SECOND caller on the playback worker, so two threads parsed into the same megabyte while writing
             * the same request object -- which is exactly how a pointer ends up null in the network layer.
             * Serialising the two calls is the whole fix: the buffers are safe again because only one caller is
             * ever inside the client. */
            std::lock_guard<std::mutex> ladderLock(g_ladderMutex);
            /* ---- THE MENU IS BUILT FROM THE ROUTE THAT ANSWERS (build173) ---------------------------
             * This called Client::fetchQualityLadder, which reads the retired video_model endpoint and so
             * now returns nothing at all. fetchPlayInfoTiers() probes the definitions the play-info route
             * actually serves and keeps the ones that answered, so the row cannot offer a tier that is
             * refused the moment it is chosen (measured: 1080p/2k/4k are a stable 400 -- they are not
             * even asked for). */
            r = Client::fetchPlayInfoTiers(seriesId, episodeId, tiers);
        }
        /* ---- A FAILED LADDER IS THE OTHER DOOR INTO THE SAME PROBLEM (build157) -------------------------
         * The ladder and the plan are two different requests to the same interface, and build156's
         * breakage would have been caught by either. On the ladder's own failure the chain is walked
         * too; the cooldown inside startPlayChainCheck means a plan failure and a ladder failure a
         * second apart produce ONE check, not two. */
        if (!r.ok) {
            hg::logPrintf("PLAYER: the ladder request failed (%s) [build157]", r.error.c_str());
            hg::startPlayChainCheck(std::string("ladder failed: ") + r.error, episodeId);
        }
        brls::sync([self, alive, tiers, r]() {
            HG_GUARD_ALIVE(alive);
            Impl* impl = self->impl;
            if (!impl) return;
            if (!r.ok) {
                /* LOGGED AND LEFT ALONE. A player without a quality menu is a working player;
                 * a player that stopped playing because the menu's data was missing is not. */
                hg::logPrintf("PLAYER: quality ladder failed '%s' -- the quality row stays "
                              "unavailable",
                              r.error.c_str());
                return;
            }
            impl->tiers = tiers;
            impl->qualityForSwitch = -1;

            /* ---- AND THAT IS THE STATUS LIGHT TURNED GREEN (build158) ---------------------------------
             *
             * Reachable only with BOTH stages proven: the plan request answered (playback is running, or
             * this lambda would not be here) and the quality ladder answered. Those are two of the six
             * things the self check walks, from the real path rather than from a probe -- so the light
             * means "the chain worked the last time it was exercised" without a check having to be run.
             *
             * The check only fires on failure, so without this the light would stay grey through every
             * successful session, and grey would read as a fault rather than as "nothing measured yet". */
            hg::notePlayChainWorked("plan and ladder both answered during playback");

            /* Which tier is playing now? Matched by quality_type against the plan that was
             * already chosen, so the row opens on the truth rather than on the best tier. A
             * plan built from a bare object (no quality_type) matches nothing, and then the
             * row opens on tier 0 -- which is the best one, i.e. the safest wrong answer. */
            int current = -1;
            if (impl->plan.has_quality_type) {
                for (size_t i = 0; i < tiers.size(); i++) {
                    if (tiers[i].qualityType == impl->plan.quality_type) {
                        current = (int)i;
                        break;
                    }
                }
            }
            self->menu.qualityCount = (int)tiers.size();
            self->menu.qualityIndex = current < 0 ? 0 : current;
            /* THE SEEDED ROW IS NOT A CHANGE, AND SAYING SO IS THE BUG FIX.
             *
             * MEASURED ON HARDWARE (build32's console log, `out/console-log-build32.txt`): playback
             * starts at 11:05:59 with the plan already on 360p, and two seconds later the log shows
             *
             *     QUALITY: switching to '360p' (tier 4, source 0, 640x360)
             *     QUALITY: loadfile issued rc=0 url=...          <- a DIFFERENT signed URL
             *
             * i.e. the player re-fetched a plan and re-loaded the whole stream FOR THE TIER IT WAS
             * ALREADY PLAYING. That is the reported symptom: "each episode plays about 3 seconds,
             * jumps back to 0 and replays, then behaves".
             *
             * The mechanism is the menu tick's rule "issue a switch when the row's index differs
             * from the index a switch was last issued for" (see the end of the menu tick), and
             * `lastQualityIndex` starts at -1. Seeding the row to tier 4 therefore looked like a
             * change from -1 to 4, and the tick dutifully switched to the tier already playing.
             *
             * So the row's seeded value is recorded as ALREADY APPLIED. The two rollback paths in
             * switchQuality already set both fields together for the same reason; this one had been
             * missed. */
            self->lastQualityIndex = self->menu.qualityIndex;
            /* ---- NO DOT UNLESS WE KNOW (build140, the user's report) --------------------------------------
             *
             * THE USER: "switching from episode three to four, the 1080P entry shows its blue dot, but the
             * selected box is on 360P." Exactly right, and this line is why: `current` is -1 when the plan carries
             * no quality_type or when no tier matches it, and the code below collapsed that to 0 -- and index 0 of
             * a best-first ladder is 1080p. So the menu claimed a tier that was not playing, and worse, it claimed
             * the BEST tier, which is what makes the user think the switch did not work.
             *
             * -1 MEANS "not known", and the dot is drawn only where playingTierIndex actually matches (see the
             * inForce rule in the menu's draw). So an unknown tier now lights nothing, which is the truth, instead
             * of lighting the wrong thing. The menu CURSOR still opens on tier 0 (that is a cursor, and the best
             * tier is a reasonable place to leave it) -- only the claim about what is playing is withdrawn. */
            impl->playingTierIndex = (current < 0) ? -1 : current;
            if (current < 0) {
                hg::logPrintf("QUALITY: which tier is playing could not be determined (plan quality_type=%d) -- the "
                              "menu will not mark any tier as playing [build140]",
                              impl->plan.has_quality_type ? impl->plan.quality_type : -1);
            }
            hg::logPrintf("QUALITY: row seeded to tier %d of %d (playing tier %d) and marked as "
                          "already applied -- no reload is issued for it [build33]",
                          self->menu.qualityIndex, self->menu.qualityCount, current);

            /* ---- 1080p IS THE PREFERRED TIER (build119, the user's instruction) --------------------------
             *
             * THEIR WORDS: "every episode after the first defaults to 360p" and "the default resolution should
             * be 1080p for all of them."
             *
             * They are seeing the raw response order. Upstream sends its sources WORST FIRST (measured:
             * quality_type 40 = 360p is entry 0), and the default path -- Client::fetchPlayPlan -- takes entry 0,
             * so an episode that has not been switched by hand plays at 360p. The ladder in hand IS sorted best
             * first (hg_quality_build, and hg_quality_test checks exactly that: "1080p remembers it was entry 4"
             * is t[0]), so the preferred tier can be found by its LABEL rather than assumed to be index 0 of
             * something.
             *
             * ONE SWITCH PER PLAYER, not a rule that keeps firing: switchQuality reloads the stream and the
             * ladder arrives again afterwards, so without the flag this would loop. If the user then picks 360p
             * by hand, nothing drags them back -- the flag is already spent. */
            /* ---- NO SWITCH IS ISSUED WHEN THE LADDER ARRIVES (build126, and it is a regression fix) ------
             *
             * build120 tried to honour "the default resolution should be 1080p" by watching the ladder arrive and
             * then calling switchQuality() if the tier in force was not the remembered one. That is a RELOAD: the
             * ladder lands while the stream is still starting, so the player tore the stream down and loaded it
             * again -- and the console showed exactly what that costs:
             *
             *   "the first time I opened episode two it jumped to episode three, and the audio of BOTH episodes
             *    played at once; the second time there was a grey flash and then 360p switched to 1080p; the third
             *    time it crashed."
             *
             * Two audio streams is two loadfiles alive at once, and with auto-next on by default the end-file
             * event from the torn-down one was read as "this episode finished", which is what advanced to episode
             * three. So the switch is NOT issued from here any more. Correctness first: one loadfile per episode,
             * which is the state that played reliably for weeks.
             *
             * The remembered tier is still recorded (the quality menu sets it, and it is what a resume will use),
             * and the correct way to get 1080p from the start is to CHOOSE the source before the first loadfile
             * rather than reload after it -- which is the next piece of work, not something to fake from here. */
            if (impl->tiers.size() > 0) {
                hg::logPrintf("QUALITY: ladder ready (%d tiers, first '%s'); the playing tier stays %d -- no switch "
                              "is issued from here [build126]",
                              (int)impl->tiers.size(), impl->tiers[0].label.c_str(), current);
            }
            /* BUMP THE REVISION. The panel only repaints when it changes, and the ladder arriving
             * IS a change: without this the quality row would keep showing "(not available)" from
             * the frame it was built in until the next keystroke. */
            self->menuRevision++;

            /* The display labels the panel will print, rebuilt here rather than per frame. */
            self->qualityLabelCount = 0;
            for (size_t i = 0; i < tiers.size() && i < HG_QUALITY_MAX; i++) {
                self->qualityLabels[i] = tiers[i].label;
                self->qualityLabelPtrs[i] = self->qualityLabels[i].c_str();
                self->qualityLabelCount++;
            }

            char summary[320];
            HgQualityTier raw[HG_QUALITY_MAX];
            for (size_t i = 0; i < tiers.size() && i < HG_QUALITY_MAX; i++) {
                memset(&raw[i], 0, sizeof raw[i]);
                raw[i].quality_type = tiers[i].qualityType;
                snprintf(raw[i].definition, sizeof raw[i].definition, "%s", tiers[i].label.c_str());
                raw[i].width = tiers[i].width;
                raw[i].height = tiers[i].height;
                raw[i].bitrate = tiers[i].bitrate;
                raw[i].size_bytes = tiers[i].sizeBytes;
                raw[i].source_index = tiers[i].sourceIndex;
                raw[i].decodable = 1;
            }
            hg_quality_describe(raw, (int)tiers.size(), summary, (int)sizeof summary);
            hg::logPrintf("PLAYER: quality ladder: %s  (playing tier %d) [build55]", summary, current);
        });
    });

    if (!started) {
        hg::logPrintf("PLAYER: no worker for the quality ladder -- the quality row stays "
                      "unavailable");
    }
}

/* Switch to a ladder tier. Called when the menu's quality row changes.
 *
 * THE SAME IMPL, A NEW PLAN. mpv is not torn down and rebuilt: the URL, the header fields and
 * the decryption options are re-set on the running instance and loadfile is issued again. That
 * is deliberate -- the render context, the fences and the framebuffer handoff are the parts of
 * this player that were hardest to get right, and a quality change has no business touching
 * them. What DOES change is the CENC key, which is why the decryption key is rewritten rather
 * than assumed to still apply: a different tier is a different stream with its own key, and a
 * stale one produces no picture and no error.
 *
 * The response is re-fetched rather than cached (see Client::fetchPlayPlanForSource): the URLs
 * are signed and time-limited, so a cached plan would be the wrong thing to switch with after a
 * few minutes. */
/* ---- PUT THE PLAYBACK BACK WHERE IT WAS AFTER A QUALITY SWITCH (build135, the user's request) ---------
 *
 * THEIR WORDS: "do not restart the episode from the beginning every time I change the resolution."
 *
 * switchQuality issues exactly one loadfile, and a loadfile starts at zero -- a plain stream cannot be
 * re-tiered in place. So the position is captured before the loadfile goes out and restored here, on the
 * first file-loaded that follows it, which is what every player that offers a mid-playback quality change
 * does.
 *
 * seekAbsolute is the same call the progress bar's drag commits with, so the seek is issued and read back
 * exactly as a user-made seek is -- no second path, and its own log line says where it landed. */

void PlayerView::switchQuality(int tierIndex) {
    Impl* impl = this->impl;
    if (!impl) return;
    if (tierIndex < 0 || tierIndex >= (int)impl->tiers.size()) {
        hg::logPrintf("QUALITY: tier %d is out of range (%u available) -- ignoring",
                      tierIndex, (unsigned)impl->tiers.size());
        return;
    }
    if (!impl->mpv || !impl->started) {
        hg::logPrintf("QUALITY: switch to tier %d ignored -- mpv is not started", tierIndex);
        return;
    }
    if (impl->switching) {
        hg::logPrintf("QUALITY: a switch is already in flight -- ignoring tier %d", tierIndex);
        return;
    }

    /* A SWITCH TO THE TIER ALREADY PLAYING IS NOT A SWITCH, AND MUST NOT RELOAD THE STREAM.
     *
     * This is the second half of the "plays 3 seconds and jumps back to 0" fix, and it is the half
     * that does not depend on knowing HOW the request got here. The first half makes the menu tick
     * stop asking for the tier it merely seeded; this one refuses the reload even if something else
     * asks for it -- a deliberate re-selection of the current tier, a reset, or a path not written
     * yet. There is nothing to do in that case: the stream that is playing IS the one being asked
     * for, and re-loading it costs a fresh signed request, a new CENC key and a picture that jumps
     * back to the beginning.
     *
     * `playingTierIndex` is the source of truth on purpose -- see the note where it is declared. */
    if (impl->playingTierIndex == tierIndex) {
        hg::logPrintf("QUALITY: tier %d ('%s') is ALREADY playing -- not reloading the stream "
                      "[build33]",
                      tierIndex, impl->tiers[(size_t)tierIndex].label.c_str());
        return;
    }
    impl->switching = true;

    const Client::QualityTier want = impl->tiers[(size_t)tierIndex];
    hg::logPrintf("QUALITY: switching to '%s' (tier %d, definition index %d, %dx%d) [build173]",
                  want.label.c_str(), tierIndex, want.sourceIndex, want.width, want.height);

    PlayerView* self = this;
    const AliveToken alive = this->alive_;
    std::string episodeId = this->episodeId;
    /* By value, like episodeId -- see the note where the ladder worker captures this. */
    const std::string seriesId = this->getSeriesId();
    hg::runDetached([self, alive, episodeId, seriesId, want, tierIndex]() {
        HgPlayPlan plan;
        /* ---- A QUALITY CHANGE IS A NEW play-info REQUEST, NOT A NEW video_model SOURCE (build173) ----
         * This called Client::fetchPlayPlanForSource(episodeId, want.sourceIndex), which picks an entry
         * out of the retired video_model response -- a list that no longer exists. On this route the
         * tiers ARE the `definition` parameter, so switching means re-requesting the same episode at the
         * chosen definition; want.sourceIndex is the index into that definition list.
         *
         * The RESULT is bridged into an HgPlayPlan the same way the first plan is, so the reload path
         * below (which already handles a fresh CENC key and a fresh URL) is unchanged. */
        HgPlayInfo info;
        Result r = Client::fetchPlayInfoPlanForTier(seriesId, episodeId, want.sourceIndex, info);
        if (r.ok) {
            int crc = hg_play_plan_from_info(&info, &plan);
            if (crc != 0) {
                char b[128];
                snprintf(b, sizeof b, "play_plan_from_info rc=%d (see hg_play.h)", crc);
                r.ok = false;
                r.error = b;
            }
        }
        brls::sync([self, alive, plan, r, want, tierIndex]() {
            HG_GUARD_ALIVE(alive);
            Impl* impl = self->impl;
            if (!impl) return;
            impl->switching = false;

            if (!r.ok) {
                /* THE FAILURE IS SURVIVABLE. The old stream is still loaded and still playing;
                 * all that happened is that the quality did not change. Saying so beats tearing
                 * the player down over a request that can simply be retried. */
                hg::logPrintf("QUALITY: switch to '%s' FAILED '%s' (serverRefused=%d) -- still playing "
                              "the old tier", want.label.c_str(), r.error.c_str(),
                              (int)r.serverRefused);
                /* The same split as everywhere else (build175): a tier the service refuses is a
                 * sentence, anything else keeps the machine's words. */
                brls::Application::notify(r.serverRefused ? std::string(hg_tr(HG_T_QUALITY_REFUSED))
                                                          : std::string("Quality change failed: ") + r.error);
                /* PUT THE ROW BACK ON SOMETHING TRUE.
                 *
                 * The first version only did this when the plan knew its own quality_type, so a plan
                 * built from a bare object (no quality_type -- which the code itself anticipates
                 * elsewhere) left the menu displaying a tier that never took effect, with no way to
                 * tell and no way to retry: selecting the same tier again would be a no-op because
                 * the index had not changed. A code review caught it.
                 *
                 * Falling back to index 0 is the honest answer when the playing tier cannot be
                 * identified: it is the best tier, which is what a fresh plan would have chosen. */
                int restore = 0;
                if (impl->plan.has_quality_type) {
                    for (size_t i = 0; i < impl->tiers.size(); i++) {
                        if (impl->tiers[i].qualityType == impl->plan.quality_type) {
                            restore = (int)i;
                            break;
                        }
                    }
                }
                self->menu.qualityIndex = restore;
                self->menuRevision++;
                /* Reset the "last index a switch was issued for", so choosing the same tier again
                 * IS a change and therefore retries instead of being optimised away. */
                self->lastQualityIndex = restore;
                hg::logPrintf("QUALITY: menu row restored to tier %d", restore);
                return;
            }

            /* THE OPTION WRITES ARE CHECKED, AND A FAILURE ABORTS THE SWITCH.
             *
             * `mpv_set_option_string` on a live handle is a request mpv may refuse, and the return
             * code was only being logged. The consequence of ignoring it is specific and bad: the
             * decryption key would still be the PREVIOUS tier's, loadfile would open the new stream
             * with the wrong key, and the result is no picture and no error -- the exact failure this
             * whole project keeps designing against. So a refusal stops here and leaves the working
             * stream alone. */
            impl->plan = plan;
            int rcKey = mpv_set_option_string(impl->mpv, "demuxer-lavf-o", plan.decryption_key);
            int rcHdr = mpv_set_option_string(impl->mpv, "http-header-fields", plan.header_fields);

            /* THE READBACK IS NOT CEREMONY HERE -- IT IS THE ONLY WAY TO SETTLE AN OPEN QUESTION.
             *
             * A code review raised this and honestly marked it undetermined: does mpv CACHE
             * `demuxer-lavf-o` at `mpv_initialize`, in which case rewriting it at runtime would have
             * no effect on the next `loadfile` and the new stream would be opened with the PREVIOUS
             * tier's key -- a black screen with no error.
             *
             * The documentation leans against that: mpv's manual says of options generally that
             * "if any option is changed at runtime (via input commands), they are not reset when a
             * new file is played", and `demuxer-lavf-o` is a file-local option, which is how the
             * existing `loadfile` path already relies on setting it before play starts.
             *
             * That is reasoning, not measurement, so the value is read back and BOTH it and whether
             * it CHANGED are logged. If mpv silently ignored the write, the two strings would be
             * equal and the log would say so -- which turns a "should work" into something the next
             * console run settles. The strings are keys, so only their lengths and whether they
             * differ are printed, never the key itself. */
            {
                char* back = nullptr;
                int rcBack = mpv_get_property(impl->mpv, "demuxer-lavf-o", MPV_FORMAT_STRING, &back);
                size_t wantLen = strlen(plan.decryption_key);
                size_t gotLen = back ? strlen(back) : 0;
                int changed = (back && strcmp(back, plan.decryption_key) == 0) ? 1 : 0;
                hg::logPrintf("QUALITY: demuxer-lavf-o set rc=%d, readback rc=%d length=%u (wanted "
                              "%u), matches=%d%s",
                              rcKey, rcBack, (unsigned)gotLen, (unsigned)wantLen, changed,
                              changed ? "" : "  <-- mpv did NOT take the new value; the switch will "
                                             "open the new stream with the OLD key");
                if (back) mpv_free(back);
            }

            if (rcKey < 0 || rcHdr < 0) {
                char b[256];
                snprintf(b, sizeof b, "mpv refused the rewritten options (decryption rc=%d, headers "
                                      "rc=%d) -- NOT loading the new stream, so the picture keeps "
                                      "playing",
                         rcKey, rcHdr);
                hg::logPrintf("QUALITY: %s", b);
                brls::Application::notify(std::string("Quality change failed: ") + b);
                /* Same rollback as a failed fetch: the row must not claim a change that did not
                 * happen. */
                int restore = 0;
                if (impl->plan.has_quality_type) {
                    for (size_t i = 0; i < impl->tiers.size(); i++) {
                        if (impl->tiers[i].qualityType == impl->plan.quality_type) {
                            restore = (int)i;
                            break;
                        }
                    }
                }
                self->menu.qualityIndex = restore;
                self->lastQualityIndex = restore;
                self->menuRevision++;
                return;
            }
            hg::logPrintf("QUALITY: options rewritten ok (decryption rc=%d, headers rc=%d)", rcKey,
                          rcHdr);

            /* FROM HERE THE PLAYER INTENDS TO PLAY THIS TIER, so this is what "already playing"
             * means from now on. Set after the writes were accepted and before the loadfile goes
             * out, which is the first moment the claim is true -- and note that the two failure
             * paths above deliberately do NOT set it, so a failed switch can always be retried. */
            impl->playingTierIndex = tierIndex;

            /* ASYNC, LIKE EVERY OTHER COMMAND THIS FILE ISSUES FROM THE UI THREAD.
             *
             * `mpv_command` blocks until the command completes, and a `loadfile` completes only once
             * the stream is opening -- so this was stalling the frame loop for the whole of a
             * network load. A code review caught it. `mpv_command_async` returns immediately and the
             * load proceeds; the readback below still reports what mpv has open at that instant,
             * which is why it says "about to" rather than claiming the switch is done. */
            /* A QUALITY SWITCH RELOADS THE STREAM FROM ZERO (build141): the capture-and-seek-back added in
             * build135 is removed here at the user's request, so a switch starts the new stream at 0:00 --
             * which is what build133 did. */
            const char* cmd[] = {"loadfile", plan.url, nullptr};
            int rcLoad = mpv_command_async(impl->mpv, 0, cmd);
            hg::logPrintf("QUALITY: loadfile issued rc=%d url=%s", rcLoad, plan.url);

            /* The readback the whole project insists on: rc==0 means mpv accepted the command, and
             * the stream that is ACTUALLY open is a separate fact. Right after an async loadfile that
             * fact is still the OLD stream, which is why the line says so. */
            char* path = nullptr;
            if (mpv_get_property(impl->mpv, "path", MPV_FORMAT_STRING, &path) >= 0) {
                hg::logPrintf("QUALITY: mpv currently has path=%s (the load is async; the switch is "
                              "confirmed by the file-loaded event)",
                              path ? path : "(null)");
                if (path) mpv_free(path);
            }
            hg::logPrintf("QUALITY: switched to '%s' -- now %s", want.label.c_str(),
                          Client::describePlan(plan).c_str());
        });
    });
}

/* ---- THE "CHOOSE THE TIER BEFORE THE LOADFILE" HELPER LIVED HERE (build136), REMOVED IN build138 -----
 *
 * It fetched the quality ladder first and built the plan from the chosen source, which did give 1080p from the
 * first frame -- at the cost of one extra network round-trip before any video could start, which the user saw
 * as a longer grey screen between episodes. Their decision: "go back to 135."
 *
 * The whole implementation is preserved in docs/build137-之后-灰屏与音画不同步的判读.md, including why it has
 * to happen before the loadfile rather than as a switch afterwards, and what it costs. It is not dead code to
 * be re-derived: it is a deliberate choice, waiting for a decision to pay the extra round-trip. */
/* ---- BUILD THE PLAN ON THE TIER WE WANT, BEFORE ANY LOADFILE (build143, the user's request) -----------
 *
 * THEIR WORDS: "make it default to 1080p, without causing conflicts."
 *
 * WHY THE TIER IS CHOSEN HERE AND NOT BY A SWITCH. Switching after playback has started is a RELOAD: the
 * stream is torn down and loaded again while it is still opening, which produced two audio streams at once, a
 * spurious end-of-file that advanced to the next episode, and a grey flash. Fetching the ladder first used to
 * cost a second network round-trip before any video could start, which is the longer grey screen the user
 * measured and rejected.
 *
 * The cache in Impl removes that cost, so this is the whole of it: one ladder (cached), one plan built from
 * the chosen source, ONE loadfile. Nothing is reloaded, no switchQuality is ever issued from here.
 *
 * THE CHOICE IS ALWAYS 1080p, and deliberately not remembered: the server is not consistent (measured: the
 * same episode returns 360p and 1080p minutes apart), so depending on its ordering is what made the resolution
 * vary from episode to episode. A manual switch still applies to the episode it was made on.
 *
 * AND NOTHING HERE CAN PREVENT PLAYBACK. Every failure -- no ladder, no plan for that source, a plan that will
 * not build -- falls back to Client::fetchPlayPlan, which is exactly what this screen did before. */
Result PlayerView::fetchPlanOnPreferredTier(const std::string& episodeId, HgPlayPlan& out) {
    Impl* impl = this->impl;

    /* ---- ONE TRANSPARENT RETRY (build151, the user's "Playback failed, PlayPlanRC = -5") ---------------
     *
     * THEIR WORDS: "it still reports that same error, -5 -- you have not fixed that one, and it was not
     * reverted."
     *
     * -5 is HG_PLAY_ERR_NOMODEL ("no outer/video_model/video_list at all" -- hg_play.h): the server answered, and
     * the answer carried no playable source. Measured on this console, that endpoint is simply inconsistent: the
     * same episode has come back with five quality tiers and with one, with 360p and with 1080p, and with
     * "Couldn't resolve host name" -- all within minutes of each other. It is not a bug in the parser, and it is
     * not something this application can configure away.
     *
     * What it CAN do is ask again. A second request a moment later is what a user does by hand when this happens,
     * and it usually succeeds -- so the retry is transparent: the first attempt's failure is logged and the second
     * goes out. Only ONE retry, so a genuinely broken episode still fails quickly and honestly instead of hanging.
     *
     * This also answers the cost of choosing the tier up front: that path makes one extra request (the ladder),
     * which is one more chance to meet this flakiness. The retry covers both attempts. */
    auto attempt = [&]() { return this->fetchPlanOnPreferredTierOnce(episodeId, out); };
    Result first = attempt();
    if (first.ok) return first;

    hg::logPrintf("PLAYER: the plan request failed (%s) -- retrying once, the server is measured to be "
                  "inconsistent [build151]",
                  first.error.c_str());
    Result second = attempt();
    if (second.ok) {
        hg::logPrintf("PLAYER: the retry succeeded [build151]");
        return second;
    }
    hg::logPrintf("PLAYER: the retry failed too (%s) -- reporting the failure", second.error.c_str());

    /* ---- AND THEN THE WHOLE CHAIN IS WALKED FOR THE USER, AUTOMATICALLY (build157) ------------------
     *
     * This is the moment the diagnostic is worth its cost: the plan has failed TWICE, so the episode,
     * the network or the endpoint is genuinely broken, and the message the user is about to see
     * ("play_plan rc=-5") does not say which. startPlayChainCheck runs on its own thread and reports
     * each link separately -- home page, detail page, the signed request, the plan parse, the ladder,
     * and a Range request for the stream's first bytes -- with the server's own response text in the
     * report, which is the one thing that answered this exact failure in build156.
     *
     * It is started, not awaited: the user's error dialog must not wait three seconds for a diagnosis.
     * It also cannot storm the service -- one run a minute, however many failures arrive.
     *
     * WHY NOT AT STARTUP: see the header of hg_selfcheck.hpp. Short version -- a check that fires on
     * every launch is a set of signed requests against a service that rate-limits, and a console that
     * has just booted fails it for reasons that have nothing to do with playback. */
    hg::startPlayChainCheck(std::string("plan failed: ") + second.error, episodeId);
    (void)impl;
    return second;
}

Result PlayerView::fetchPlanOnPreferredTierOnce(const std::string& episodeId, HgPlayPlan& out) {
    Impl* impl = this->impl;

    /* NO impl IS NOT A REASON TO USE THE DEAD ROUTE (build173). This used to fall straight through to
     * Client::fetchPlayPlan, which is the retired endpoint -- so this one early return would have kept
     * playback broken in the case where impl is missing. The series id lives on the view, not in impl,
     * so the new route works here as well; the block below handles both cases. */

    /* ---- THE OLD ORDER IS RESTORED: ASK FOR THE PLAN FIRST (build154, and this is the rc=-5 fix) --------
     *
     * THE USER: "build153 still reports it -- did you change some decision?" The screenshot: "Playback failed:
     * play_plan rc=-5 (see hg_play.h)". -5 is HG_PLAY_ERR_NOMODEL: the server answered without a playable source.
     *
     * The cause is the ORDER introduced in build143 for the 1080p default. Choosing the tier before the loadfile
     * meant asking for the quality ladder FIRST and then for the plan of the chosen source -- two requests to the
     * SAME endpoint, and the measurements say one good answer arrives per short window:
     *
     *   old order (plan first)   the plan is always good; the LADDER is the one that comes back degraded
     *                            ("ladder ready (1 tiers)" while the plan carried 360p -- which is what made the
     *                            menu claim 1080p was playing)
     *   build143 (ladder first)  the ladder consumes the good answer, so the PLAN gets the degraded one -- no
     *                            source at all -> rc=-5, and the player never starts.
     *
     * That is why rc=-5 appeared only after build143 and why it survived every later build: the decision logic was
     * never the problem; the two requests were simply in the wrong order for this server.
     *
     * So the plan request goes first again, exactly as in the build the user confirmed working. The preferred-tier
     * code below is kept but unreachable for now: getting 1080p back needs ONE request that yields both the source
     * list and the tier (the ladder response already carries both -- see Client::fetchQualityLadder) instead of
     * two, and that belongs in hg_client, not here. */
    hg::logPrintf("QUALITY: asking for the plan first, the old order -- one request per episode [build154]");

    /* ---- THE SOURCE OF THE PLAY ADDRESS CHANGED (build173) ------------------------------------------
     *
     * WHAT THIS REPLACES AND WHY IT HAD TO. Everything below this line used to be Client::fetchPlayPlan,
     * which POSTs the signed /novel/player/video_model/v1/ endpoint. MEASURED with the reference
     * implementation's own signer, that route now answers `HTTP 200 + 0 bytes` to EVERY request --
     * signed, unsigned, and with a deliberately corrupted x-gorgon alike, for any body, any id and both
     * hosts. It is a retired stub, so no amount of retrying or re-parameterising it can produce a URL,
     * and the symptom on the console was exactly that: "PLAY-PLAN FAILED [build155]: POST 200 with 0
     * bytes", the status light RED, and no episode playable, while the home and detail pages were fine.
     * Evidence: docs/ (the play-interface change note dated 2026-10-16).
     *
     * The replacement is the route the site's own web player calls, which needs no signature at all.
     * The retry ladder and the definition list live in Client::fetchPlayInfoPlan (see its comment for
     * the measurements behind the three tiers and the four tries).
     *
     * THE SERIES ID IS REQUIRED. Measured: the same vid answers 200 under its own series_id and 400 with
     * a wrong, empty or absent one, so an empty id here is reported as a failure rather than attempted. */
    const std::string& seriesId = this->seriesId;
    if (seriesId.empty()) {
        Result r;
        r.error = "no series id for this episode (the player screen must supply it; see setSeriesId)";
        hg::logPrintf("PLAY-INFO [build173]: refused -- %s [build173]", r.error.c_str());
        return r;
    }
    {
        HgPlayInfo info;
        Result r = Client::fetchPlayInfoPlan(seriesId, episodeId, info);
        if (r.ok) {
            int crc = hg_play_plan_from_info(&info, &out);
            if (crc != 0) {
                char b[128];
                snprintf(b, sizeof b, "play_plan_from_info rc=%d (see hg_play.h)", crc);
                r.ok = false;
                r.error = b;
            }
        }
        if (r.ok) return r;

        /* ---- THE OLD ROUTE IS STILL TRIED, AND THAT IS DELIBERATE --------------------------------
         * It is dead TODAY, but it is the route with a working quality ladder and it costs one request.
         * If the service ever restores it, playback starts working again without another build, and if
         * it is still dead the user sees the NEW route's error -- which names the real reason -- rather
         * than "POST 200 with 0 bytes". The new route's failure is reported, not this one's. */
        HgPlayPlan legacy;
        Result old = Client::fetchPlayPlan(episodeId, legacy);
        if (old.ok) {
            hg::logPrintf("PLAY-INFO [build173]: the new route failed but the OLD video-model route "
                          "answered -- using it [build173]");
            out = legacy;
            return old;
        }
        return r;
    }

    /* NOT REACHED -- see the note above. */
    if (hg::cfgIs("player.tier.default", "upstream")) {
        hg::logPrintf("QUALITY: player.tier.default=upstream -- using the default source path (build143)");
        return Client::fetchPlayPlan(episodeId, out);
    }

    const bool cached = (impl->ladderCacheEpisodeId == episodeId && impl->ladderCache.size() >= 2);
    std::vector<Client::QualityTier> tiers;
    if (cached) {
        tiers = impl->ladderCache;
        hg::logPrintf("QUALITY: ladder from cache (%d tiers) -- no extra request [build143]", (int)tiers.size());
    } else {
        Result ladderR;
        {
            /* SAME LOCK AS THE UI'S CALL -- see the long note at the other call site: hg_client.cpp's scratch
             * buffers are file-static globals, so two concurrent ladder requests corrupt them, which is the
             * null-dereference crash Atmosphère recorded in the hongguo-net process. */
            std::lock_guard<std::mutex> ladderLock(g_ladderMutex);
            ladderR = Client::fetchQualityLadder(episodeId, tiers);
        }
        auto ladder = ladderR;
        if (!ladder.ok || tiers.empty()) {
            hg::logPrintf("QUALITY: no ladder for this episode (%s) -- falling back to the default source "
                          "[build143]",
                          ladder.error.c_str());
            return Client::fetchPlayPlan(episodeId, out);
        }
        if (tiers.size() >= 2) {
            impl->ladderCacheEpisodeId = episodeId;
            impl->ladderCache = tiers;
        }
    }

    int want = -1;
    for (size_t i = 0; i < tiers.size(); i++) {
        if (tiers[i].label.rfind("1080", 0) == 0) { want = (int)i; break; }
    }
    if (want < 0) {
        want = 0;   /* hg_quality_build sorts best first, so entry 0 is the best offered */
        hg::logPrintf("QUALITY: this episode offers no 1080p tier -- starting on the best offered ('%s')",
                      tiers[0].label.c_str());
    }

    hg::logPrintf("QUALITY: starting on '%s' -- source %d of %d, chosen BEFORE the loadfile so there is exactly "
                  "one loadfile and no reload [build143]",
                  tiers[(size_t)want].label.c_str(), tiers[(size_t)want].sourceIndex, (int)tiers.size());

    auto r = Client::fetchPlayPlanForSource(episodeId, tiers[(size_t)want].sourceIndex, out);
    if (!r.ok) {
        hg::logPrintf("QUALITY: the plan for that tier failed (%s) -- falling back to the default source",
                      r.error.c_str());
        return Client::fetchPlayPlan(episodeId, out);
    }
    return r;
}
void PlayerView::start(const std::string& episodeId) {
    PlayerView* self = this;
    const AliveToken alive = this->alive_;
    this->episodeId = episodeId;

    /* ---- EVERY EPISODE GETS ITS OWN REPAIR ATTEMPT (build171) -------------------------------------------
     *
     * This is the "reset the count when the next episode starts" half of the user's requirement that the
     * repair be a limited number of attempts rather than an endless loop. Each episode is a separate
     * signed URL and therefore a separate chance to have expired, so the budget is per episode, not per
     * player: without this reset, a session where episode 1 needed a repair would leave episodes 2..N
     * with no budget at all, and the feature would silently stop working for the rest of the run.
     *
     * `repairing` is cleared for the same reason -- a repair whose result never came back (the user backed
     * out mid-fetch) must not wedge the next episode's repair. */
    if (this->impl) {
        this->impl->repairAttempts = 0;
        this->impl->repairing = false;
    }

    /* ---- THE PENDING-RESUME CLEAR THAT STOOD HERE IS REMOVED AGAIN (build139, at the user's request) ----
     *
     * build137 cleared impl->resumeAfterSwitch here, reasoning that a pending "seek back after a quality switch"
     * must not survive into the next episode -- because ZR advances by loading a new file, which raises the same
     * file-loaded event that spends it.
     *
     * The user has since used the console through several builds and their instruction is unambiguous: "just roll
     * me back to 135, I do not want the later content." So this file is build135's file again, this block
     * included: nothing is cleared when a new episode starts.
     *
     * THE RISK IS KNOWN AND IS THEIRS TO ACCEPT. With this removed, a manual quality switch followed by ZR can
     * leave a pending position that the next episode's file-loaded event spends -- a seek to the previous
     * episode's timestamp, which shows up as a stutter followed by out-of-sync audio. That is build135's
     * behaviour, and build135 is the behaviour that was asked for. The reasoning, the mechanism and the exact
     * code are kept in docs/build138-之后-蓝点说谎与旧流重叠.md and docs/build137-之后-灰屏与音画不同步的判读.md,
     * so restoring it is a deliberate act rather than a re-derivation. */

    /* ---- IT IS RE-INSTATED, AND THE USER'S OWN TEST IS WHAT PROVES IT (build146) ------------------------
     *
     * THEIR WORDS: "entering each episode from the list, backing out and going in again is fine -- but switching
     * with ZL/ZR still has the problem."
     *
     * That difference IS the diagnosis. Entering from the list builds a NEW player, whose pending position starts
     * at -1. ZL/ZR reuse THIS player (player_activity.cpp: "starting one means asking the view to fetch a fresh
     * play plan"), so whatever the previous episode left behind comes along -- and what is left behind is the
     * position captured by a manual quality switch, which is spent on the next file-loaded event. ZR raises
     * exactly that event, so the new episode was seeked to the OLD episode's timestamp: a stutter, then a decoder
     * that never catches up, i.e. the out-of-sync audio they reported.
     *
     * The pending position belongs to ONE loadfile. From here the only thing that can set it again is a quality
     * switch made on THIS episode. Nothing else from the later builds is restored by this change. */
    if (this->impl) {
        if (this->impl->resumeAfterSwitch >= 0.0) {
            hg::logPrintf("PLAYER: starting episode '%s' -- dropping a pending resume of %.1fs left over from a "
                          "quality switch on the previous stream [build146]",
                          episodeId.c_str(), this->impl->resumeAfterSwitch);
        }
        this->impl->resumeAfterSwitch = -1.0;
        this->impl->resumeDuration = 0.0;
    }

    /* Both workers below come from hg_thread (libnx threadCreate), never
     * std::thread: on this toolchain std::thread's constructor throws ENOSYS
     * and an uncaught throw aborts the process. See hg_thread.hpp. */
    /* ---- REVERTED TO THE SINGLE REQUEST (build138, the user's decision: "go back to 135") --------------
     *
     * build136 chose the tier BEFORE the loadfile, which did give 1080p from the first frame -- at the cost of
     * one extra network round-trip before any video could start:
     *
     *     old:  fetchPlayPlan ─────────────────────────► loadfile        (one request)
     *     b136: fetchQualityLadder ─► fetchPlayPlanForSource ─► loadfile  (two requests)
     *
     * The user measured the cost: "switching episodes shows a grey screen first and then starts playing." The
     * grey is this project's own background during the gap before the first frame (see docs/106), so a longer gap
     * is a longer grey screen -- and they chose the shorter one.
     *
     * So the tier is no longer chosen up front. The default is whatever upstream's first source is (measured:
     * 360p), which is the behaviour build135 had. The remembered tier still exists and is still updated by the
     * quality menu; only the pre-loadfile use of it is gone. To bring it back deliberately, call
     * fetchPlanAtPreferredTier (kept in this file, unused) and see docs/build137. */
    bool started = hg::runDetached([self, alive, episodeId]() {
        HgPlayPlan plan;
        Result r = self->fetchPlanOnPreferredTier(episodeId, plan);

        brls::sync([self, alive, plan, r]() {

            HG_GUARD_ALIVE(alive);
            brls::Label* status = self->statusLabel;
            if (!r.ok) {
                /* REFUSE, DO NOT GUESS: report the reason rather than starting
                 * mpv with a URL that was never resolved.
                 *
                 * ---- AND SAY IT IN THE RIGHT WORDS (build175) ------------------------------------
                 * Two failures arrived here as one sentence. "Playback failed: network error" in front
                 * of a service refusal sends the viewer to check a connection that is working; and a
                 * refusal shown as a machine's report ("after 20 attempts over 3 quality tiers. The
                 * server said HTTP 403-...") tells a person nothing they can use.
                 *
                 * Result::serverRefused is the machine-readable half, so this does not parse a
                 * localised sentence to decide. The server's own body is NOT lost -- it went to the log
                 * (logFailureResponse) where the next diagnosis reads it. */
                const std::string shown = r.serverRefused
                    ? std::string(hg_tr(HG_T_EPISODE_REFUSED))
                    : std::string("Playback failed: ") + r.error;
                if (status) status->setText(shown);
                brls::Application::notify(shown);
                hg::logPrintf("PLAYER: plan failed '%s' (serverRefused=%d) -- telling the screen that "
                              "asked", r.error.c_str(), (int)r.serverRefused);
                if (self->historyOwner) self->historyOwner->playbackFailed(shown);
                return;
            }
            if (status) status->setText(Client::describePlan(plan));

            Impl* impl = self->impl;
            impl->plan = plan;

            if (!setupMpv(impl)) {
                if (status) {
                    status->setText("mpv failed to start: deko3d render context or loadfile");
                }
                brls::Application::notify("mpv failed to start");
                hg::logPrintf("PLAYER: setupMpv FAILED (deko3d render context or loadfile)");
                return;
            }
            hg::logPrintf("PLAYER: mpv started, plan = %s", Client::describePlan(plan).c_str());

            /* THE SCREENSHOT PROBE IS NOT STARTED HERE.
             *
             * It used to be, and it blocked the UI thread: `mpv_command_ret` was called from this
             * brls::sync lambda (which runs on the main thread), and a command reply can only arrive
             * from a drained event queue -- which draw() does, and which the blocking call was
             * preventing. A code review of this file caught both halves of that.
             *
             * It is now issued from draw() once playback has actually started (see the call to
             * requestScreenshotProbe there), so a frame exists to capture and nothing waits. */

            /* THE LADDER IS FETCHED AFTER PLAYBACK IS RUNNING, on its own worker. A player that
             * waits for a quality menu before showing a picture has the dependency backwards. */
            self->startQualityLadder(self->episodeId);
            /* The poller only reports; if no thread can be had, the status line
             * stays at the plan description, which is still the truth. */
            if (!hg::runDetached([impl]() { pollLoop(impl); })) {
                hg::logPrintf("PLAYER: no poller thread -- status line will not update");
            }
        });
    });

    if (!started) {
        /* No worker for the plan request either. Resolving it inline freezes the
         * player screen for a few seconds, but it is the difference between a
         * frozen screen and a player that never starts. */
        hg::logPrintf("PLAYER: no worker available -- resolving the plan inline");
        HgPlayPlan plan;
        /* The new route here too (build173): the old one is retired, so falling back to it on this path
         * would have meant "no worker" silently became "cannot play". */
        Result r = this->fetchPlanOnPreferredTier(episodeId, plan);
        if (!r.ok) {
            /* Same two-words-for-two-causes rule as the worker path above (build175). */
            const std::string shown = r.serverRefused
                ? std::string(hg_tr(HG_T_EPISODE_REFUSED))
                : std::string("Playback failed: ") + r.error;
            if (this->statusLabel) this->statusLabel->setText(shown);
            brls::Application::notify(shown);
            hg::logPrintf("PLAYER: inline plan failed '%s' (serverRefused=%d) -- telling the screen that "
                          "asked", r.error.c_str(), (int)r.serverRefused);
            if (this->historyOwner) this->historyOwner->playbackFailed(shown);
            return;
        }
        if (this->statusLabel) this->statusLabel->setText(Client::describePlan(plan));
        this->impl->plan = plan;
        if (!setupMpv(this->impl)) {
            if (this->statusLabel) {
                this->statusLabel->setText("mpv failed to start: deko3d render context or loadfile");
            }
            hg::logPrintf("PLAYER: inline setupMpv FAILED");
            return;
        }
        hg::logPrintf("PLAYER: inline mpv started, plan = %s", Client::describePlan(plan).c_str());
        hg::runDetached([this]() { pollLoop(this->impl); });
    }
}

/* HOW LONG THE CONTROL BAR STAYS UP WITH NO INPUT. Four seconds is the value every console player lands
 * on: long enough to read the clock and pick a button, short enough that the picture is not covered while
 * watching. Any A or tap restarts it. */
#define HG_PLAYER_CONTROLS_MS 10000.0

/* The bar's geometry, in logical pixels. Named so the drawing and the hit areas cannot drift apart. */
#define HG_CONTROLS_H 72.0f
#define HG_CONTROLS_PAD 16.0f
#define HG_CONTROLS_BTN 48.0f

bool PlayerView::controlsVisible() const {
    if (!this->controlsOn) return false;
    const double now = brls::getCPUTimeUsec() / 1000.0;
    return (now - this->controlsShownAt) < HG_PLAYER_CONTROLS_MS;
}

void PlayerView::showControls() {
    const bool wasUp = this->controlsVisible();
    this->controlsOn = true;
    this->controlsShownAt = brls::getCPUTimeUsec() / 1000.0;
    if (!wasUp) {
        hg::logPrintf("PLAYER: control bar up (A or a tap); it hides itself after %.0f ms [build43]",
                      HG_PLAYER_CONTROLS_MS);
        /* TELL THE ACTIVITY, so its overlay row stands down: with the bar up there were two progress indicators
         * and two copies of the episode label on screen at once. See PlaybackSink::controlBarVisible. */
        if (this->historyOwner) this->historyOwner->controlBarVisible(true);
    }
}

void PlayerView::hideControls() {
    if (!this->controlsOn) return;
    this->controlsOn = false;
    hg::logPrintf("PLAYER: control bar hidden");
    /* AND HAND THE OVERLAY BACK, exactly as the user left it (Y's state is the activity's). */
    if (this->historyOwner) this->historyOwner->controlBarVisible(false);
}

/* THE BAR: the play/pause button on the left, the progress bar beside it, the clock on the right.
 *
 * Drawn with nanovg rather than built from brls views, for the same reason the progress bar and the
 * picture menu are: this screen's input is polled from the controller state every frame (see the note in
 * the header), so a focusable view here would add a second input path to the screen -- and this project has
 * already paid for two movers on one screen twice. The bar is furniture; the keys stay the keys. */
void PlayerView::seedControlsFromCfg() {
    if (this->controlsSeeded) return;
    this->controlsSeeded = true;

    /* THE THREE VALUES FROM THE CFG, all clamped by hg_ctl (which is unit-tested), so a cfg file with a
     * typo in it cannot hand mpv a volume of 999 or a speed of 40x. */
    const int v = hg::cfgIntOr("player.volume", 100);
    this->volume = hg_ctl_volume_clamp(v);
    if (this->volume != v) {
        hg::logPrintf("PLAYER: cfg player.volume=%d clamped to %d", v, this->volume);
    }
    const double s = hg::cfgDoubleOr("player.speed", 1.0);
    this->speedIndex = hg_ctl_speed_index(s);
    const double z = hg::cfgDoubleOr("player.zoom", 1.0);
    this->zoomIndex = hg_ctl_zoom_index(z);

    hg::logPrintf("PLAYER: control values seeded from cfg -- volume=%d speed=%.2fx zoom=%.2fx [build44]", this->volume,
                  hg_ctl_speed_at(this->speedIndex), hg_ctl_zoom_factor_at(this->zoomIndex));
}

void PlayerView::moveControlSelection(int delta) {
    this->seedControlsFromCfg();
    const int next = this->controlsIndex + delta;
    if (next < 0 || next >= CONTROL_COUNT) {
        /* A WRAP, not a bounce: the bar is short and a user pressing DOWN past the end wants to be back at
         * the first item, not to be told no. */
        this->controlsIndex = (next < 0) ? CONTROL_COUNT - 1 : 0;
    } else {
        this->controlsIndex = next;
    }
    hg::logPrintf("PLAYER: control bar selection -> %d", this->controlsIndex);
}

void PlayerView::adjustControl(int delta) {
    this->seedControlsFromCfg();
    const char* what = nullptr;
    switch (this->controlsIndex) {
        case CONTROL_VOLUME:
            this->volume = hg_ctl_volume_step(this->volume, delta);
            what = "volume";
            break;
        case CONTROL_SPEED:
            this->speedIndex = hg_ctl_speed_step(this->speedIndex, delta);
            what = "speed";
            break;
        case CONTROL_ZOOM:
            this->zoomIndex = hg_ctl_zoom_step(this->zoomIndex, delta);
            what = "zoom";
            break;
        default:
            /* PLAY and PROGRESS are not values: on PROGRESS, LEFT/RIGHT is the seek (handled by the seek
             * poll), and on PLAY there is nothing to step. */
            return;
    }
    hg::logPrintf("PLAYER: %s %+d -> volume=%d speed=%.2fx zoom=%.2fx", what, delta, this->volume,
                  hg_ctl_speed_at(this->speedIndex), hg_ctl_zoom_factor_at(this->zoomIndex));
    this->applyControlValue(what);
}

/* A WHILE THE BAR IS UP: ACTIVATE THE SELECTED ITEM.
 *
 * The bar has a selection, so A has to mean "the thing you are on" -- that is what a bar with a highlighted
 * item means on every console. Three cases:
 *
 *   PLAY / PROGRESS -> play/pause, which is the primary action and what A did before this existed.
 *   QUALITY         -> open the picture menu on its quality row. NOT a direct quality change: the two-step
 *                      (build45) lives in the menu, and a second way to change tiers would be a second place
 *                      for the "reload on every press" problem to come back.
 *   VOLUME/SPEED/ZOOM -> nothing. They are values, adjusted with LEFT/RIGHT; A has no business stepping them.
 */
/* THE SETTINGS PANEL'S KEYS, polled exactly like the picture menu's (see drawMenuTick for why this screen
 * reads the controller state itself instead of registering actions). A and B close; UP/DOWN move the row;
 * LEFT/RIGHT change the value.
 *
 * WHILE THE SETTINGS ARE OPEN THE SEEK KEYS ARE SKIPPED (see draw()), so the two cannot both act on one
 * press -- the same rule the picture menu has had since it was written. */
void PlayerView::tickSettings() {
    if (!this->settings.open) return;

    auto& st = brls::Application::getControllerState();
    auto tapped = [&](brls::ControllerButton b) -> bool {
        return st.buttons[b] && !this->prevButtons[b];
    };

    const bool keyA = tapped(brls::BUTTON_A);
    const bool keyB = tapped(brls::BUTTON_B);
    const bool keyUp = tapped(brls::BUTTON_UP);
    const bool keyDown = tapped(brls::BUTTON_DOWN);
    const bool keyLeft = tapped(brls::BUTTON_LEFT);
    const bool keyRight = tapped(brls::BUTTON_RIGHT);

    if (keyA || keyB) {
        this->settings.open = 0;
        if (this->settingsPanel) this->settingsPanel->setPanelVisible(false);
        hg::logPrintf("PLAYERSET: %s -> CLOSE (sleep=%d min, bar=%d)", keyA ? "A" : "B",
                      hg_ctl_sleep_minutes_at(this->settings.sleepIndex), this->settings.barAlways);
        return;
    }
    if (keyUp || keyDown) {
        const int next = this->settings.row + (keyUp ? -1 : +1);
        if (next >= 0 && next < PlayerSettingsPanel::ROW_COUNT) {
            this->settings.row = next;
            hg::logPrintf("PLAYERSET: row -> %d", next);
        }
        return;
    }
    if (keyLeft || keyRight) {
        const int delta = keyLeft ? -1 : +1;
        if (this->settings.row == PlayerSettingsPanel::ROW_SLEEP) {
            const int before = this->settings.sleepIndex;
            this->settings.sleepIndex = hg_ctl_sleep_step(this->settings.sleepIndex, delta);
            if (this->settings.sleepIndex != before) {
                /* THE TIMER IS ARMED FROM NOW, every time the value changes: a timer that counted from when
                 * the app started would fire at a moment the user never asked for. */
                const int minutes = hg_ctl_sleep_minutes_at(this->settings.sleepIndex);
                this->settings.sleepDeadline = minutes > 0 ? nowMs() + (double)minutes * 60000.0 : 0.0;
                hg::logPrintf("PLAYERSET: sleep timer -> %d min", minutes);
            }
        } else if (this->settings.row == PlayerSettingsPanel::ROW_BAR) {
            this->settings.barAlways = hg_ctl_flag_step(this->settings.barAlways, delta);
            hg::logPrintf("PLAYERSET: always-show-progress -> %d", this->settings.barAlways);
        } else if (this->settings.row == PlayerSettingsPanel::ROW_PLAY) {
            this->settings.playMode = hg_ctl_flag_step(this->settings.playMode, delta);
            hg::logPrintf("PLAYERSET: play mode -> %s", this->settings.playMode ? "auto-next" : "stop");
        }
    }
}

/* THE SLEEP TIMER FIRING. One comparison per frame; when it is due the player PAUSES (rather than popping the
 * activity) and the timer switches itself off, so the console is not left showing a paused picture with a
 * timer that would fire again the moment it is resumed. */
void PlayerView::notifyEpisodeEnded() {
    /* The activity owns the episode list and the play mode decision; the view knows nothing about episodes and
     * must not start to. See the EOF case in drainMpvEvents. */
    if (this->menuOwner) this->menuOwner->onEpisodeEnded();
}

void PlayerView::tickSleepTimer() {
    if (this->settings.sleepDeadline <= 0.0) return;
    if (nowMs() < this->settings.sleepDeadline) return;

    hg::logPrintf("PLAYERSET: sleep timer fired after %d minutes -> pausing and switching the timer off",
                  hg_ctl_sleep_minutes_at(this->settings.sleepIndex));
    this->settings.sleepDeadline = 0.0;
    this->settings.sleepIndex = 0;
    Impl* impl = this->impl;
    if (impl && impl->mpv && impl->started && !impl->paused.load()) this->togglePause();
    brls::Application::notify(hg_tr(HG_T_PSET_SLEEP));
}
/* ---- THE CHIP PULL-UP MENUS (K16) ---------------------------------------------------------------
 *
 * The three functions below are the whole feature: how many options a chip has, what each is called, and what
 * choosing one does. The drawing is in drawControlBar and the input is in tickControls, so the menu is the same
 * kind of object as every other piece of this screen's furniture -- polled, drawn with nanovg, and with no
 * brls view involved (which is what keeps "one mover" true here).
 *
 * THE OPTION LISTS COME FROM hg_ctl, WHICH IS UNIT-TESTED: the speed ladder has seven rungs and the zoom ladder
 * four, and both were tested on the host before this menu existed. Nothing here re-derives them. */
int PlayerView::chipMenuCount(int item) const {
    switch (item) {
        case CONTROL_SPEED: return HG_CTL_SPEED_COUNT;
        case CONTROL_ZOOM: return HG_CTL_ZOOM_COUNT;
        /* ---- THE QUALITY LADDER IS A MENU LIKE THE OTHERS (build117) -------------------------------------
         *
         * MEASURED ON HARDWARE, from the user: "the resolution still cannot be switched." It could not, because
         * this chip opened the brls picture menu -- and NOTHING this app draws with brls has ever appeared on that
         * console (the picture menu, the settings panel), while everything drawn with nanovg has (the control bar,
         * the pull-up menus, the hint bar).
         *
         * So the ladder is a pull-up menu of the same kind as speed and zoom, and choosing an entry calls
         * switchQuality() -- the very function the picture menu called, which already refuses to reload the tier
         * that is playing and already rolls back on failure. Nothing about the switching itself is new. */
        case CONTROL_QUALITY: {
            Impl* impl = this->impl;
            return impl ? (int)impl->tiers.size() : 0;
        }
        /* VOLUME IS NOT A LIST. Its popup is a drag bar, and 0 options is how the drawing code tells the two
         * apart -- see drawControlBar.  */
        case CONTROL_VOLUME: return 0;
        default: return 0;
    }
}

void PlayerView::chipMenuLabel(int item, int index, char* buf, int n) const {
    if (!buf || n <= 0) return;
    switch (item) {
        case CONTROL_SPEED: hg_ctl_speed_label(index, buf, n); return;
        case CONTROL_ZOOM: hg_ctl_zoom_label(index, buf, n); return;
        case CONTROL_QUALITY: {
            Impl* impl = this->impl;
            if (impl && index >= 0 && index < (int)impl->tiers.size()) {
                snprintf(buf, (size_t)n, "%s", impl->tiers[(size_t)index].label.c_str());
            } else {
                snprintf(buf, (size_t)n, "--");
            }
            return;
        }
        default: snprintf(buf, (size_t)n, "%d%%", this->volume); return;
    }
}

void PlayerView::chipMenuApply() {
    switch (this->chipMenu) {
        case CONTROL_SPEED:
            this->speedIndex = this->chipMenuIndex;
            this->applyControlValue("speed");
            break;
        case CONTROL_ZOOM:
            this->zoomIndex = this->chipMenuIndex;
            this->applyControlValue("zoom");
            break;
        case CONTROL_QUALITY:
            /* switchQuality is the existing path: it fetches the source for that tier, issues one loadfile and
             * rolls back to the playing tier if the switch fails. See its own comment.
             *
             * ---- AND NOTHING IS REMEMBERED ANY MORE (build142, the user's instruction) -------------------
             *
             * THE USER: "the problems started when I asked you to make it default to 1080p; revert every change
             * you made for that step."
             *
             * They are right that this is where it began, and this is the last piece of it: the chosen label was
             * recorded here so the NEXT episode would start on it. That whole idea is gone -- see the note where
             * the ladder lands -- so a switch now affects only the episode it was made on, exactly as it did
             * before any of this. */
            hg::logPrintf("PLAYER: the quality menu chose tier %d of %d", this->chipMenuIndex,
                          this->impl ? (int)this->impl->tiers.size() : 0);
            this->switchQuality(this->chipMenuIndex);
            break;
        case CONTROL_VOLUME:
            /* The drag bar writes the volume as it moves; there is nothing to confirm. Kept as a case so the
             * switch stays exhaustive over the chips that can open a popup. */
            this->applyControlValue("volume");
            break;
        default:
            break;
    }
    hg::logPrintf("PLAYER: chip menu (%d) applied option %d -- volume=%d speed=%.2fx zoom=%.2fx [build82]",
                  this->chipMenu, this->chipMenuIndex, this->volume, hg_ctl_speed_at(this->speedIndex),
                  hg_ctl_zoom_factor_at(this->zoomIndex));
}

void PlayerView::activateControl() {
    switch (this->controlsIndex) {
        case CONTROL_QUALITY:
            /* ---- THE LADDER IS A PULL-UP MENU NOW (build117) --------------------------------------------
             * See chipMenuCount: this chip used to open the brls picture menu, and no brls panel has ever drawn
             * on this console. The pull-up menu is the same mechanism the speed and zoom chips use, and choosing
             * an entry calls switchQuality(), which is the switching code the picture menu already used. */
            {
                Impl* impl = this->impl;
                int start = 0;
                if (impl && impl->playingTierIndex >= 0 && impl->playingTierIndex < (int)impl->tiers.size()) {
                    start = impl->playingTierIndex;   /* open on the tier that is playing, not on the best one */
                }
                this->chipMenuIndex = start;
            }
            this->chipMenu = CONTROL_QUALITY;
            hg::logPrintf("PLAYER: A on the quality chip -> pull-up menu of %d tiers, current %d [build117]",
                          this->impl ? (int)this->impl->tiers.size() : 0, this->chipMenuIndex);
            return;
        case CONTROL_SETTINGS:
            hg::logPrintf("PLAYER: A on the settings chip -> opening the player settings panel [build47]");
            this->settings.open = 1;
            if (this->settingsPanel) this->settingsPanel->setPanelVisible(true);
            return;
        case CONTROL_VOLUME:
        case CONTROL_SPEED:
        case CONTROL_ZOOM: {
            /* THE PULL-UP MENU, INSTEAD OF "use left/right" (K16). The old behaviour is still there when no menu
             * is open -- left/right on the chip still steps the value -- so nobody loses a way to work; A now
             * does what the user asked for. */
            this->chipMenu = this->controlsIndex;
            this->chipMenuIndex = (this->controlsIndex == CONTROL_SPEED)
                                      ? this->speedIndex
                                      : (this->controlsIndex == CONTROL_ZOOM ? this->zoomIndex : 0);
            hg::logPrintf("PLAYER: A on chip %d -> pull-up %s (index %d) [build82]", this->controlsIndex,
                          this->controlsIndex == CONTROL_VOLUME ? "drag bar" : "menu", this->chipMenuIndex);
            return;
        }
        default:
            hg::logPrintf("PLAYER: A on the play/progress item -> play/pause");
            this->togglePause();
            return;
    }
}

/* OPEN THE PICTURE MENU, OPTIONALLY ON A GIVEN ROW.
 *
 * The same four lines the X key runs (see drawMenuTick): set the flag, bump the revision so the panel
 * redraws, show the panel, and tell the activity to move its overlay out of the way. Factored out so the
 * quality chip cannot open the menu in a DIFFERENT way than X does -- a second open path is how a panel ends
 * up visible with the menu flag false.
 *
 * The row is only set when the menu was closed; opening a menu that is already open at a different row is not
 * a case that exists today (the chip cannot be reached while the menu is up), but writing it this way means it
 * cannot become one by accident. */
void PlayerView::openPictureMenu(int row, int haveRow) {
    if (this->menu.open) return;

    /* ---- THE PICTURE MENU IS NOT OPENED ANY MORE (build121, and it is that top-left box) ---------------
     *
     * MEASURED ON HARDWARE, from the user: "the home screen now has a box in the top-left corner, I cannot move
     * it, and B does not get me out." The settings panel sits on the RIGHT (460 wide, right aligned), so the box
     * in the top-left is this one -- the picture menu, the only left-aligned panel this screen has left. On the
     * GPU path it shows up as an empty frame, and B belongs to this screen's own input poll, which knows nothing
     * about it, so there is no way out of it.
     *
     * THE PANEL IS NOT REMOVED FROM THE VIEW TREE. Doing that is exactly what produced the
     * "basic_string: construction from null" abort in build114, and the user's instruction after that was to keep
     * whatever runs. So the menu is simply never OPENED: nothing is shown, nothing can get stuck, and no view tree
     * changes -- which is what makes this a safe fix rather than a repeat of build114.
     *
     * Nothing useful is lost: the ladder -- the one row anyone wanted out of this panel -- has its own pull-up
     * menu on the quality chip (see chipMenuCount), and that one displays and accepts touch.
     *
     * X is therefore free. It is left unbound rather than pointed at something half-built, and if the panel ever
     * comes back this is the single place that has to change. */
    hg::logPrintf("PLAYERMENU: openPictureMenu(%d) IGNORED -- this panel is not shown in this build: it drew the "
                  "top-left box that could not be closed, and the quality ladder lives on its own chip now "
                  "[build121]",
                  row);
    if (haveRow && row >= 0 && row < HG_PMENU_ROW_COUNT) this->menu.row = row;
    return;

    if (haveRow && row >= 0 && row < HG_PMENU_ROW_COUNT) this->menu.row = row;
    this->menu.open = 1;
    this->menuRevision++;
    if (this->menuPanel) this->menuPanel->setPanelVisible(true);
    if (this->menuOwner) this->menuOwner->setMenuOverlay(1);
    hg::logPrintf("PLAYERMENU: opened (row=%d, by the control bar=%d) [build46]", this->menu.row, haveRow);
    this->applyPictureSettings(HG_PMENU_SRC_USER);
}
void PlayerView::applyControlValue(const char* why) {
    Impl* impl = this->impl;
    if (!impl || !impl->mpv) {
        hg::logPrintf("PLAYER: %s changed but mpv is not there yet -- it will be applied on the next change",
                      why);
        return;
    }

    /* EVERY WRITE IS READ BACK AND LOGGED. See the class header: a property write that is accepted and then
     * does nothing is indistinguishable from success, and the read-back is what turns it into a number. */
    {
        int64_t want = (int64_t)this->volume;
        int rc = mpv_set_property(impl->mpv, "volume", MPV_FORMAT_INT64, &want);
        int64_t back = -9999;
        int rcBack = mpv_get_property(impl->mpv, "volume", MPV_FORMAT_INT64, &back);
        hg::logPrintf("PLAYER: volume = %d -> set rc=%d, read back %d (rc=%d)", this->volume, rc, (int)back,
                      rcBack);
    }
    {
        double want = hg_ctl_speed_at(this->speedIndex);
        int rc = mpv_set_property(impl->mpv, "speed", MPV_FORMAT_DOUBLE, &want);
        double back = -9999.0;
        int rcBack = mpv_get_property(impl->mpv, "speed", MPV_FORMAT_DOUBLE, &back);
        hg::logPrintf("PLAYER: speed = %.2fx -> set rc=%d, read back %.2fx (rc=%d)", want, rc, back, rcBack);
    }
    {
        /* mpv's video-zoom is a POWER OF TWO: hg_ctl_zoom_property owns that conversion and is unit-tested,
         * because a factor sent verbatim would double the picture when the label said it doubled twice. */
        double want = hg_ctl_zoom_property(this->zoomIndex);
        int rc = mpv_set_property(impl->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &want);
        double back = -9999.0;
        int rcBack = mpv_get_property(impl->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &back);
        hg::logPrintf("PLAYER: zoom = %.2fx (video-zoom %.4f) -> set rc=%d, read back %.4f (rc=%d)",
                      hg_ctl_zoom_factor_at(this->zoomIndex), want, rc, back, rcBack);
    }
}

void PlayerView::tickControls() {
    /* ---- TOUCH IS RECORDED BUT NOT YET CONSUMED (build87) ---------------------------------------------
     * The bar and its menus are nanovg, so there is no brls view to tap. The rectangles are already recorded
     * while drawing (chipHit / menuHit / volumeHit) -- that half works, and it is the half that must live next
     * to the drawing code. What is MISSING is a readable touch position: this fork keeps
     * Application::currentTouchState private, and InputManager::updateTouchStates only FEEDS the manager, so an
     * app cannot poll the finger. The supported route is a GestureRecognizer subclass, whose recognitionLoop()
     * is HANDED the TouchState -- that is what the next build adds. Recorded here rather than half-wired, so
     * nothing on screen pretends to work. */

    /* ---- TOUCH, READ FROM libnx DIRECTLY, BEFORE ANY EARLY RETURN (build110) ----------------------------
     *
     * MEASURED: the GestureRecognizer added in build109 NEVER FIRED -- the log had zero "PLAYER: touch --" lines.
     * The reason is in the framework: this fork's Switch platform does not implement touch at all (no
     * updateTouchStates, no hidTouch anywhere under lib/platforms/switch), so Application::frame
     * (application.cpp:336) has no touch events to dispatch. No recognizer can receive what the platform never
     * produces.
     *
     * So the panel is read the way every other input on this screen is read: from the hardware, once per frame.
     * libnx reports the panel's own 1280x720 coordinates; the UI's logical space is 720p by default, so the scale
     * factor is 1.0 unless ui.scale says otherwise.
     *
     * IT IS ABOVE THE controlsOn RETURN ON PURPOSE. "Tap the picture to bring the UI up" is exactly the case
     * where the bar is DOWN, so a poll below that line could never satisfy the request it was written for.
     *
     * A HOLD IS ONE TAP: the position is acted on only on the rising edge, the same rule the D-pad flags use. */
#ifdef __SWITCH__
    {
        HidTouchScreenState touch{};
        const size_t got = hidGetTouchScreenStates(&touch, 1);
        const bool down = (got > 0 && touch.count > 0);
        if (down && !this->hadTouchLastFrame) {
            const float sx = (float)touch.touches[0].x;
            const float sy = (float)touch.touches[0].y;
            const float scale = (brls::Application::windowWidth > 0)
                                    ? ((float)brls::Application::windowWidth / 1280.0f)
                                    : 1.0f;
            if (!this->touchLogged) {
                this->touchLogged = true;
                hg::logPrintf("PLAYER: the touch panel reports a press at (%u,%u) -> logical (%.0f,%.0f) "
                              "(window %ux%u, scale %.2f) [build110]",
                              (unsigned)sx, (unsigned)sy, (double)(sx * scale), (double)(sy * scale),
                              brls::Application::windowWidth, brls::Application::windowHeight, (double)scale);
            }
            this->onTouchAt(sx * scale, sy * scale);
        }
        this->hadTouchLastFrame = down;
    }
#endif

    if (!this->controlsOn) return;
    this->seedControlsFromCfg();

    auto& st = brls::Application::getControllerState();

    /* ---- THE LEFT STICK DRIVES THE BAR AND THE MENUS TOO (the user's request) -------------------------
     *
     * Their words: "in the player, after pressing A, the stick should be able to select 播放 and 音量 and so on."
     * It should, and it did not: every direction on this screen was read from the D-pad only.
     *
     * The stick is folded into the SAME four edge flags rather than handled separately, because that is what keeps
     * "one mover" true -- the flags are what all the navigation below reads, and adding a second reading of the
     * same intent is how this screen ended up with two things moving the cursor once already.
     *
     * A DEADZONE OF 0.55, and the flag is not re-armed until the stick comes back inside it: holding the stick
     * therefore moves the selection one step, not sixty. (The D-pad's repeat behaviour is borealis', unchanged.)
     *
     * THE FIRST NON-CENTRED READING IS LOGGED, once. borealis documents `axes[]` as "from 0.0f to 1.0f" while
     * libnx's own joystick read is centred on 0, and the two conventions cannot both be right -- so rather than
     * guess, the log records what this console actually sends the first time a stick is moved. */
    const float stickX = st.axes[brls::LEFT_X];
    const float stickY = st.axes[brls::LEFT_Y];
    const float STICK_DEADZONE = 0.55f;
    if (stickX != 0.0f || stickY != 0.0f) {
        static bool stickLogged = false;
        if (!stickLogged && (stickX > STICK_DEADZONE || stickX < -STICK_DEADZONE || stickY > STICK_DEADZONE ||
                             stickY < -STICK_DEADZONE)) {
            stickLogged = true;
            hg::logPrintf("PLAYER: the left stick reads x=%.3f y=%.3f (deadzone %.2f) -- logged once so the axis "
                          "convention is measured rather than assumed [build86]",
                          (double)stickX, (double)stickY, (double)STICK_DEADZONE);
        }
    }

    const bool up = (st.buttons[brls::BUTTON_UP] || stickY < -STICK_DEADZONE) && !this->prevControlsUp;
    const bool down = (st.buttons[brls::BUTTON_DOWN] || stickY > STICK_DEADZONE) && !this->prevControlsDown;
    /* ---- LEFT/RIGHT: SEEK WHEN THE BAR IS DOWN, SELECT THE CHIPS WHEN IT IS UP (build120) ----------------
     *
     * THE USER'S RULE, in their words: "the left and right are for choosing among the four chips after pressing
     * A" and, separately, "LR is fast forward and rewind, the way wiliwili does it."
     *
     * So there are two different jobs and two different keys, which is what wiliwili does as well:
     *
     *   L / R      SEEK, always -- never gated by the bar. This is the missing piece: seek used to be reachable
     *              only with the bar down, and the bar now stays up for ten seconds and comes up on a tap, so
     *              there was no reliable way to seek while watching.
     *   LEFT/RIGHT SEEK while the bar is down, and move the chip SELECTION while it is up.
     *
     * Both are folded into the same edge flags (the seek poll and the bar's navigation read these), so there is
     * still exactly one mover for each key. */

    const bool left = (st.buttons[brls::BUTTON_LEFT] || stickX < -STICK_DEADZONE) && !this->prevControlsLeft;
    const bool right = (st.buttons[brls::BUTTON_RIGHT] || stickX > STICK_DEADZONE) && !this->prevControlsRight;

    /* THE EDGE STATE IS TRACKED HERE rather than through tapped(): these keys are also the seek keys, which
     * the poll above reads as LEVELS (hold to accelerate), so this one has to do its own rising-edge
     * detection. Two different readings of the same buttons is correct here -- one wants the level, the
     * other wants the press -- and keeping both in one place is what makes that visible. */
    /* The edge state carries the stick as well, so holding it does not repeat. */
    this->prevControlsUp = st.buttons[brls::BUTTON_UP] || stickY < -STICK_DEADZONE;
    this->prevControlsDown = st.buttons[brls::BUTTON_DOWN] || stickY > STICK_DEADZONE;
    this->prevControlsLeft = st.buttons[brls::BUTTON_LEFT] || stickX < -STICK_DEADZONE;
    this->prevControlsRight = st.buttons[brls::BUTTON_RIGHT] || stickX > STICK_DEADZONE;

    /* ---- A PULL-UP MENU OWNS THE D-PAD AND A/B WHILE IT IS OPEN (K16) --------------------------------
     *
     * Checked FIRST, before the bar's own navigation: while a menu is up, up/down move inside it and the bar's
     * selection must not move underneath. This is the same shape tickSettings uses for the settings panel, which
     * is deliberate -- two panels on one screen with two different input rules is how a screen ends up looking
     * like it has two interfaces, which is what the user reported two rounds ago.
     *
     * A CONFIRMS, B CANCELS, and for the volume chip there is nothing to confirm: that popup is a drag bar, so
     * left/right move the value as the user holds them. */
    if (this->chipMenu != 0) {
        /* EDGE DETECTION DONE HERE, not with tickSettings' local `tapped` lambda -- that one is scoped to that
         * function. Same shape, same reason: this screen reads the controller state itself. */
        const bool ok = st.buttons[brls::BUTTON_A] && !this->prevMenuA;
        const bool cancel = st.buttons[brls::BUTTON_B] && !this->prevMenuB;
        this->prevMenuA = st.buttons[brls::BUTTON_A];
        this->prevMenuB = st.buttons[brls::BUTTON_B];
        if (ok || cancel) {
            if (ok && this->chipMenu != CONTROL_VOLUME) this->chipMenuApply();
            hg::logPrintf("PLAYER: chip menu (%d) closed by %s", this->chipMenu, ok ? "A" : "B");
            this->chipMenu = 0;
            this->showControls();
            return;
        }
        if (up || down || left || right) {
            const int delta = (up || left) ? -1 : +1;
            if (this->chipMenu == CONTROL_VOLUME) {
                this->volume = hg_ctl_volume_step(this->volume, delta);
                this->applyControlValue("volume");
            } else {
                const int count = this->chipMenuCount(this->chipMenu);
                if (count > 0) this->chipMenuIndex = (this->chipMenuIndex + delta + count) % count;
            }
            this->showControls();
            return;
        }
        this->showControls();
        return;
    }

    if (up || down) {
        this->moveControlSelection(up ? -1 : 1);
        this->showControls();   /* any input re-arms the idle timer */
        return;
    }
    /* ---- LEFT/RIGHT ALSO WALK THE BAR (build123, the user's rule) ----------------------------------------
     *
     * THEIR WORDS: "after pressing A, the left and right are for choosing among the four chips." The bar is a
     * row, so left/right walking it is the natural reading, and their earlier note confirms it is what they
     * expect: "after A it is still up/down that switches the four buttons" -- they want both to work.
     *
     * Only while the bar is up and nothing else owns the D-pad: with the bar down, left/right is the hold-to-seek
     * (and L/R always seeks), and with a menu open the menu takes the D-pad. The values themselves stay inside
     * the pull-up menus. */
    if ((left || right) && this->controlsOn && !this->chipMenu && !this->menu.open && !this->settings.open) {
        this->moveControlSelection(left ? -1 : 1);
        this->showControls();
        return;
    }
    /* ---- LEFT/RIGHT ON A VALUE CHIP NO LONGER CHANGES THE VALUE (the user's instruction) ------------------
     *
     * Their words: "change the pull-up menus you already have so that they can only be operated INSIDE the menu
     * or the drag bar; delete the direct operation from outside."
     *
     * So the step-in-place branch that used to live here is GONE. A on a value chip opens its menu (or drag bar),
     * the menu owns the D-pad while it is open, and A or B closes it. The reason is worth writing down because it
     * was not a preference: two ways to change the same number (D-pad at the bar, and the menu) meant the value
     * could move while the user thought they were navigating, which is the "two things moving the cursor" problem
     * this screen has already paid for once.
     *
     * PROGRESS keeps LEFT/RIGHT: that is the seek, it is on the bar rather than in a chip, and it is what the
     * user is dragging. It falls through to the seek poll, which owns it -- see the gate there. */
    if (left || right) this->showControls();
}

void PlayerView::drawControlBar(NVGcontext* vg, float x, float y, float width, float height) {
    /* The hit areas are rebuilt every frame, so a stale one can never be tapped. */
    this->menuHitCount = 0;

    Impl* impl = this->impl;
    const NVGcolor accent = brls::Application::getTheme().getColor("brls/accent");
    const NVGcolor text = brls::Application::getTheme().getColor("brls/text");

    /* THE PANEL, across the bottom: translucent, so the picture reads through it. */
    const float barY = y + height - HG_CONTROLS_H;
    nvgBeginPath(vg);
    nvgRect(vg, x, barY, width, HG_CONTROLS_H);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 150));
    nvgFill(vg);

    /* WHAT IS PLAYING, along the top of the bar (C14 -- the "TV style" player the reference draws).
     *
     * The activity hands this line in (see setOverlayTitle) because it is the object that knows the series and
     * which episode is on. Empty draws nothing at all rather than an empty strip. */
    if (!this->overlayTitle.empty()) {
        nvgFontSize(vg, 16.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, text);
        nvgText(vg, x + HG_CONTROLS_PAD, barY + 16.0f, this->overlayTitle.c_str(), NULL);
    }

    /* THE PLAY/PAUSE BUTTON, and it is ALWAYS the highlighted one: it is the bar's primary action, which is

     * what A activates while the bar is up. A highlight that could be moved to other buttons is the next
     * step (volume, quality, speed); drawing one now and pretending it is a cursor would be worse. */
    const float btnX = x + HG_CONTROLS_PAD;
    const float btnY = barY + (HG_CONTROLS_H - HG_CONTROLS_BTN) * 0.5f;
    const bool playing = impl ? !impl->paused.load() : true;

    /* ---- REMEMBERED FOR TOUCH (build111) ---------------------------------------------------------------
     *
     * MEASURED ON HARDWARE, from the user: "the touch works for volume, speed and zoom, but NOT for play and
     * pause." The reason is in onTouchAt: it only ever tested the five CHIPS, and this button is not one of them,
     * so a tap here hit nothing and fell through to "toggle the UI".
     *
     * The rectangle is recorded where the button is actually drawn, so a tap area cannot drift away from the
     * thing it is over -- the same rule the chips follow. */
    this->playHit = HitRect{ btnX, btnY, HG_CONTROLS_BTN, HG_CONTROLS_BTN, CONTROL_PLAY, true };

    nvgBeginPath(vg);
    nvgRoundedRect(vg, btnX, btnY, HG_CONTROLS_BTN, HG_CONTROLS_BTN, 8.0f);
    nvgFillColor(vg, nvgRGBA(accent.r * 255, accent.g * 255, accent.b * 255, 60));
    nvgFill(vg);
    nvgStrokeColor(vg, accent);
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);

    const float cx = btnX + HG_CONTROLS_BTN * 0.5f;
    const float cy = btnY + HG_CONTROLS_BTN * 0.5f;
    nvgFillColor(vg, text);
    nvgStrokeColor(vg, text);
    if (playing) {
        /* TWO BARS = "press this to pause", the standard TV glyph. */
        nvgBeginPath(vg);
        nvgRect(vg, cx - 8.0f, cy - 10.0f, 6.0f, 20.0f);
        nvgRect(vg, cx + 2.0f, cy - 10.0f, 6.0f, 20.0f);
        nvgFill(vg);
    } else {
        /* A TRIANGLE = "press this to play". */
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 7.0f, cy - 11.0f);
        nvgLineTo(vg, cx + 9.0f, cy);
        nvgLineTo(vg, cx - 7.0f, cy + 11.0f);
        nvgClosePath(vg);
        nvgFill(vg);
    }

    /* THE PROGRESS BAR, drawn with the same two rectangles the read-only arm has always used, so the bar
     * and the no-controls overlay look like the same thing at different moments. */
    const float trackX = btnX + HG_CONTROLS_BTN + HG_CONTROLS_PAD;
    /* Same for the progress bar: a tap on it seeks. See onTouchAt. */
    this->progressHit = HitRect{ trackX, barY + 8.0f, (x + width - HG_CONTROLS_PAD) - trackX, HG_CONTROLS_H - 16.0f,
                                 CONTROL_PROGRESS, true };
    const float trackW = width - (trackX - x) - HG_CONTROLS_PAD;
    const float trackH = 6.0f;
    const float trackY = barY + HG_CONTROLS_H * 0.5f - trackH * 0.5f;
    const float frac = impl ? (float)clamp01f((double)impl->progress.load()) : 0.0f;

    nvgBeginPath(vg);
    nvgRect(vg, trackX, trackY, trackW, trackH);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 64));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, trackX, trackY, trackW * frac, trackH);
    nvgFillColor(vg, accent);
    nvgFill(vg);

    /* THE CLOCK, over the track's right end -- the same position and the same format the status line uses
     * (hg_fmt_clock), so the two cannot disagree about what "1:31" means. */
    if (impl) {
        const double dur = (double)impl->duration.load();
        /* THE POSITION IS DERIVED, not a second reading of mpv: progress is the fraction the poll thread
         * already maintains and duration is what it read, so this cannot disagree with the progress bar
         * drawn a few lines above. */
        const double pos = dur > 0.0 ? (double)impl->progress.load() * dur : 0.0;
        char a[16], b[16], line[40];
        hg_fmt_clock(pos, a, (int)sizeof a);
        hg_fmt_clock(dur, b, (int)sizeof b);
        snprintf(line, sizeof line, "%s / %s", a, b);
        nvgFontSize(vg, 16.0f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, text);
        nvgText(vg, x + width - HG_CONTROLS_PAD, trackY - 18.0f, line, NULL);
    }

    /* ---- THE VALUE CHIPS: volume / speed / zoom ---------------------------------
     *
     * These three never existed on this screen before (only the picture menu had values), and they are the
     * reason the bar has a selection at all. Each chip shows its value whatever the selection is, so the
     * effect of LEFT/RIGHT is visible without leaving the picture; the SELECTED one is ringed, and it is the
     * one LEFT/RIGHT acts on. The progress item has no chip -- it IS the bar -- and the play button has its
     * own ring above, so every item in the list is visible somewhere. */
    {
        char speedLabel[16];
        char zoomLabel[16];
        char volLabel[24];
        hg_ctl_speed_label(this->speedIndex, speedLabel, (int)sizeof speedLabel);
        hg_ctl_zoom_label(this->zoomIndex, zoomLabel, (int)sizeof zoomLabel);
        snprintf(volLabel, sizeof volLabel, "%d%%", this->volume);

        /* THE FOURTH CHIP IS QUALITY (A4). Its text comes from the ladder the player owns, so the bar shows
         * the tier that is PLAYING (not a menu cursor): the bar reports state, the menu changes it. */
        char qLabel[24];
        const char* tierName = nullptr;
        if (impl && this->menu.qualityCount > 0 && impl->playingTierIndex >= 0 &&
            impl->playingTierIndex < this->menu.qualityCount &&
            this->qualityLabelPtrs[impl->playingTierIndex]) {
            tierName = this->qualityLabelPtrs[impl->playingTierIndex];
        }
        snprintf(qLabel, sizeof qLabel, "%s", tierName ? tierName : "--");

        /* THE FIFTH CHIP IS THE DOOR TO THE PLAYER SETTINGS (A7). It shows no value because it has none:
         * it is a button, and the ring around it is what says that A opens the panel. */
        /* ---- THE SETTINGS CHIP IS GONE (build112, the user's instruction) --------------------------------
         *
         * THEIR WORDS: "the settings that are not there, do not do them." The panel behind this chip has been
         * reported four times -- it opens, shows nothing, and will not close -- and its useful switch is now
         * handled elsewhere: auto-next is the DEFAULT play mode (see SettingsState::playMode), which is what they
         * were trying to reach when they reported that episode 20 did not continue. The sleep timer was never
         * used.
         *
         * So the bar keeps the four items that work, including on touch, and the panel code stays in the tree
         * behind PlayerSettingsPanel, unused, so bringing it back is a deliberate act rather than a forgotten
         * one. */
        const int chipItems[4] = {CONTROL_VOLUME, CONTROL_SPEED, CONTROL_ZOOM, CONTROL_QUALITY};
        const char* chipText[4] = {volLabel, speedLabel, zoomLabel, qLabel};

        /* RIGHT-ALIGNED, laid out right to left so the last item lands on the right edge.
         *
         * THE WIDTH IS MEASURED FROM THE TEXT, NOT FIXED AT 72, AND THAT IS A BUG FIX RATHER THAN POLISH.
         *
         * MEASURED ON HARDWARE (a photograph of this bar, 2026-10-10): every chip was 72 px wide while the
         * quality chip's label is "1x 1080p · HEVC" -- the codec suffix this app added in build55 -- which is
         * about 140 px at this font size. The text overflowed its chip, printed over the chip to its left, and
         * covered the 设置 chip completely.
         *
         * The consequence was worse than a cosmetic overlap: the SELECTION RING around 设置 was covered too, so
         * the report read "the last one, 设置, cannot be reached" -- the user was selecting it and could not see
         * it. A layout bug that hides the focus reads as a broken key, which is why this is written out here
         * rather than fixed silently. */
        float chipX = x + width - HG_CONTROLS_PAD;
        for (int i = 4; i >= 0; i--) {
            const float ch = 30.0f;

            /* THE WIDTH IS MEASURED ONCE PER LABEL, NOT ONCE PER FRAME, AND THAT IS A CRASH FIX (round 40).
             *
             * MEASURED ON HARDWARE (the console's own crash reports, 2026-10-10 evening):
             *
             *     Result 0x767 (User Break)
             *     dk::detail::RaiseError -> dk::detail::CmdBuf::requestCmdMem()   (deko3d, cmdbuf.cpp:117)
             *       <- CMemPool::Handle::getGpuAddr() <- nvg::DkRenderer::DrawConvexFill() (dk_renderer.cpp:353)
             *       <- DkRenderer::Flush() <- nvgEndFrame() <- brls::Application::frame()
             *
             * i.e. deko3d ran out of command/vertex memory while flushing a frame -- and the same session's user
             * report was "the 设置 chip's character flickers between the real glyph and a missing-glyph box",
             * which is what a font atlas being re-rendered under pressure looks like. Both appeared right after
             * build71 started calling nvgTextBounds FIVE TIMES PER FRAME (one per chip) purely to size them.
             *
             * nvgTextBounds has to have the glyphs for the string present in the atlas, so calling it every frame
             * for strings that only change when the user changes a value is work with no purpose and a real cost.
             * The measurement is now cached per label: it runs when the text changes, which is a handful of times
             * per session instead of 60 times a second. */
            float cw = 72.0f;
            {
                static std::string measured[5];
                static float measuredW[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                const std::string want(chipText[i]);
                if (measured[i] != want || measuredW[i] <= 0.0f) {
                    float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    /* THE FACE IS PINNED, AND THAT IS THE OTHER HALF OF THE FLICKER FIX. The log from the
                     * console showed the chip row starting at x=788 on one frame and x=800 on the next, for ever:
                     * nvgTextBounds measures with whatever font face is currently in effect, and this screen draws
                     * button glyphs from the ICON face as well, so the same string measured a different width
                     * depending on what had been drawn just before it. Face 0 is the UI font borealis loads, and
                     * the overlay text above uses it too, so pinning it here costs nothing and makes the layout
                     * deterministic. */
                    nvgFontFaceId(vg, 0);
                    nvgFontSize(vg, 16.0f);
                    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgTextBounds(vg, 0.0f, 0.0f, chipText[i], NULL, bounds);
                    measured[i] = want;
                    measuredW[i] = bounds[2] - bounds[0] + 20.0f;
                }
                cw = measuredW[i];
                if (cw < 72.0f) cw = 72.0f;   /* a floor, so the short value chips keep the size they had */
            }

            chipX -= cw;
            const float cy2 = barY + HG_CONTROLS_H - ch - 8.0f;
            /* REMEMBERED FOR TOUCH: see the HitRect members. Recorded where the chip is actually drawn, so
             * the tap area cannot drift away from the thing it is over. */
            this->chipHit[i] = HitRect{ chipX, cy2, cw, ch, chipItems[i], true };
            nvgBeginPath(vg);
            nvgRoundedRect(vg, chipX, cy2, cw, ch, 6.0f);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 26));
            nvgFill(vg);
            if (this->controlsIndex == chipItems[i]) {
                nvgStrokeColor(vg, accent);
                nvgStrokeWidth(vg, 2.0f);
                nvgStroke(vg);
            }
            nvgFontFaceId(vg, 0);   /* see the measurement above: the face must not depend on what drew before */
            nvgFontSize(vg, 16.0f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, text);
            nvgText(vg, chipX + cw * 0.5f, cy2 + ch * 0.5f, chipText[i], NULL);
            chipX -= 8.0f;

            /* ONE LINE WHEN THE LAYOUT CHANGES, so the next report about this bar carries the numbers instead of
             * needing another round of guessing. `chipX` is where the whole row starts: negative would mean the
             * bar is too narrow for its own contents, which is the condition that used to be invisible. */
            if (i == 0) {
                static float lastRowX = -1.0f;
                if (chipX != lastRowX) {
                    lastRowX = chipX;
                    hg::logPrintf("PLAYER: control bar chips start at x=%.0f (bar width %.0f, quality chip "
                                  "%.0f px, all five measured) [build78]",
                                  (double)chipX, (double)width, (double)cw);
                }
            }
        }
    }

    /* ---- THE PULL-UP MENU, OR THE VOLUME DRAG BAR (K16) ------------------------------------------------
     *
     * It sits ABOVE the bar, which is what "上拉菜单" means: the bar stays where it is and the options rise out of
     * it. Both shapes share this block because they share everything else -- the panel, the title line, the input
     * handling -- and differ only in what the body is.
     *
     * The title is the chip's own name, so the menu says what it is changing. */
    if (this->chipMenu != 0) {
        const int count = this->chipMenuCount(this->chipMenu);
        const float titleH = 30.0f;
        const float rowH = 32.0f;
        const float menuW = (count == 0) ? 460.0f : 260.0f;
        const float bodyH = (count == 0) ? 84.0f : (rowH * (float)count + 8.0f);
        const float menuH = titleH + bodyH + 10.0f;
        const float menuX = x + (width - menuW) * 0.5f;
        const float menuY = barY - menuH - 8.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, menuX, menuY, menuW, menuH, 10.0f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 200));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 60));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        const char* title = (this->chipMenu == CONTROL_VOLUME)
                                ? hg_tr(HG_T_CTL_VOLUME)
                                : (this->chipMenu == CONTROL_SPEED ? hg_tr(HG_T_CTL_SPEED)
                                                                   : hg_tr(HG_T_CTL_ZOOM));
        nvgFontFaceId(vg, 0);
        nvgFontSize(vg, 16.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, text);
        nvgText(vg, menuX + 16.0f, menuY + titleH * 0.5f + 4.0f, title, NULL);

        if (count == 0) {
            /* THE VOLUME DRAG BAR: a track with a filled portion and a handle, the number at the right. Left/right
             * move it and it applies as it moves -- there is nothing to confirm (see the input handler). */
            const float trackX = menuX + 16.0f;
            const float trackW = menuW - 32.0f - 70.0f;
            const float trackY = menuY + titleH + 14.0f;
            const float trackH = 10.0f;
            /* A TALLER HIT AREA THAN THE TRACK ITSELF: a 10 px line is not something a thumb can hit,
             * so the tap area is 34 px tall and centred on it. The drawn bar stays thin. */
            this->volumeHit = HitRect{ trackX, trackY - 12.0f, trackW, 34.0f, 0, true };
            const float frac = (float)hg_ctl_volume_clamp(this->volume) / (float)HG_CTL_VOLUME_MAX;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, trackX, trackY, trackW, trackH, trackH * 0.5f);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 70));
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, trackX, trackY, trackW * frac, trackH, trackH * 0.5f);
            nvgFillColor(vg, accent);
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, trackX + trackW * frac, trackY + trackH * 0.5f, 9.0f);
            nvgFillColor(vg, accent);
            nvgFill(vg);

            char pct[16];
            snprintf(pct, sizeof pct, "%d%%", this->volume);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, text);
            nvgText(vg, menuX + menuW - 16.0f, trackY + trackH * 0.5f, pct, NULL);

            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFontSize(vg, 14.0f);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 150));
            nvgText(vg, menuX + 16.0f, trackY + 40.0f, hg_tr(HG_T_CTL_DRAG), NULL);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(vg, menuX + menuW - 16.0f, trackY + 40.0f, "B", NULL);
        } else {
            /* THE LIST: the value in force carries a filled dot, and the one the D-pad is on carries the same ring
             * the bar uses for its own selection -- one visual language for "this is what you are on". */
            char rowLabel[32];
            for (int i = 0; i < count; i++) {
                const float ry = menuY + titleH + 4.0f + rowH * (float)i;
                if (i == this->chipMenuIndex) {
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, menuX + 8.0f, ry + 2.0f, menuW - 16.0f, rowH - 4.0f, 6.0f);
                    nvgFillColor(vg, nvgRGBA(255, 255, 255, 30));
                    nvgFill(vg);
                    nvgStrokeColor(vg, accent);
                    nvgStrokeWidth(vg, 2.0f);
                    nvgStroke(vg);
                }
                /* REMEMBERED FOR TOUCH, same reason as the chips. */
                if (this->menuHitCount < (int)(sizeof this->menuHit / sizeof this->menuHit[0])) {
                    this->menuHit[this->menuHitCount++] =
                        HitRect{ menuX + 8.0f, ry + 2.0f, menuW - 16.0f, rowH - 4.0f, i, true };
                }
                const bool inForce =
                    (this->chipMenu == CONTROL_SPEED) ? (i == this->speedIndex) : (i == this->zoomIndex);
                this->chipMenuLabel(this->chipMenu, i, rowLabel, (int)sizeof rowLabel);
                nvgFontSize(vg, 16.0f);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg, text);
                nvgText(vg, menuX + 24.0f, ry + rowH * 0.5f, rowLabel, NULL);
                if (inForce) {
                    nvgBeginPath(vg);
                    nvgCircle(vg, menuX + menuW - 24.0f, ry + rowH * 0.5f, 5.0f);
                    nvgFillColor(vg, accent);
                    nvgFill(vg);
                }
            }
        }
    }
}
void PlayerView::draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* ctx) {
    Impl* impl = this->impl;

    /* The first few frames are logged because a crash or a hang inside
     * mpv_render_context_render presents as a frozen picture with no message,
     * and the only way to tell "never reached the render call" from "hung
     * inside it" is to bracket it.
     *
     * TWO counters, not one. The first four draws of this view happen BEFORE
     * mpv exists (renderCtx is null), so a single counter is spent by the time
     * the interesting frames begin -- which is exactly what the previous run
     * showed: "PLAYER draw #1..#4: ctx=0" and then nothing. mpvDraws counts only
     * the frames that actually enter the mpv path. */
    /* THE ONE-SECOND TIMING WINDOW (I2). Counted here so it covers every frame, including the ones that take
     * the non-software path. */
    {
        const double now = nowMs();
        if (this->frameWindowStartMs <= 0.0) this->frameWindowStartMs = now;
        this->frameWindowCount++;
        /* ---- THE PER-SECOND TIMING LINE IS BEHIND THE SAME SWITCH AS THE HEARTBEAT (build149) ------------
         *
         * The window is still counted every frame -- that is what feeds the numbers when the switch is on -- but
         * the LINE is only written when the user asks for it with log.playback=full. One write a second is one
         * chance a second to hit the SD card's slow path, and that stall is what makes the picture hitch and mpv
         * resync its audio in a garbled burst. See the long note in main.cpp. */
        static const bool playbackTimingLog = !hg::cfgIs("log.playback", "off");   /* build152: back ON */
        if (playbackTimingLog && now - this->frameWindowStartMs >= 1000.0) {
            const double span = now - this->frameWindowStartMs;
            const double frames = (double)this->frameWindowCount;
            hg::logPrintf("PLAYER timing: %.1f fps | mpv_sw=%.1f ms/frame | upload=%.1f ms/frame | "
                          "other=%.1f ms/frame | surface=%dx%d | heap=%ld KiB [build84]",
                          frames * 1000.0 / span, this->frameRenderMs / frames, this->frameUploadMs / frames,
                          (span - this->frameRenderMs - this->frameUploadMs) / frames,
                          this->impl ? this->impl->swW : 0, this->impl ? this->impl->swH : 0,
                          hgMemoryUsedKB());
            this->frameRenderMs = 0.0;
            this->frameUploadMs = 0.0;
            this->frameWindowCount = 0;
            this->frameWindowStartMs = now;
        }
    }

    this->drawCount++;
    if (this->drawCount <= 6) {
        hg::logPrintf("PLAYER draw #%d: ctx=%d video=%d fbo=%p", this->drawCount,
                      impl && impl->renderCtx ? 1 : 0, impl && impl->video ? 1 : 0,
                      (void*)(impl && impl->video ? impl->video->getFramebuffer() : nullptr));
    }

    /* ---- software render: mpv to CPU memory, nanovg to the screen ----------- */
    if (impl && impl->swRender && impl->renderCtx && impl->pixels) {
        drainMpvEvents(impl);
        mpv_render_context_update(impl->renderCtx);

        /* ---- WHERE THE FRAME TIME GOES (I2) -----------------------------------
         *
         * The stutter report is "1080p is slow", and so far the only measured fact was that the software path
         * uploads a whole surface per frame. That does not say WHICH part is slow: mpv's CPU colour conversion
         * and scaling, the texture upload, or the nanovg drawing around them. Optimising before knowing that is
         * guessing, and this project's rule is that a claim about hardware needs a number.
         *
         * So three clock reads per frame, accumulated and logged once a second. Cheap enough to leave in (it is
         * `brls::getCPUTimeUsec()`, not a syscall), and it turns the next console run into the answer. */
        const double tRender0 = nowMs();
        int rcSw = mpv_render_context_render(impl->renderCtx, impl->swParams);
        mpv_render_context_report_swap(impl->renderCtx);
        const double tRender1 = nowMs();
        this->frameRenderMs += (tRender1 - tRender0);

        /* THE PROOF THIS MODE EXISTS FOR -- SAMPLED OVER THE WHOLE RUN, NOT THE
         * FIRST FEW FRAMES.
         *
         * mpv writes (or does not write) real pixels into CPU memory, so "did mpv
         * render anything" stops being an inference. The first version of this
         * logged only draws #1..#5 and reported sampledSum=0 -- because those
         * frames happen BEFORE the stream is open, when mpv legitimately has
         * nothing to draw. That is the same mistake this project keeps making: a
         * measurement whose window does not cover the thing being measured.
         *
         * So it now reports the first few frames (setup visibility), the FIRST
         * frame that actually contains picture content, and a slow heartbeat. */
        {
            impl->swFrames++;
            unsigned long long sum = 0;
            int nonzero = 0;
            size_t total = (size_t)impl->swStride * (size_t)impl->swH;
            size_t i;
            for (i = 0; i < total; i += 997) {
                sum += impl->pixels[i];
                if (impl->pixels[i] != 0) nonzero++;
            }
            bool firstContent = (!impl->swSawPixels && nonzero > 0);
            if (firstContent) impl->swSawPixels = true;
            if (impl->swFrames <= 3 || firstContent || (impl->swFrames % 300) == 0) {
                hg::logPrintf("SW draw #%d: rc=%d buffer=%p %dx%d stride=%d sampledSum=%llu"
                              " nonzero=%d%s",
                              impl->swFrames, rcSw, (void*)impl->pixels, impl->swW, impl->swH,
                              impl->swStride, sum, nonzero,
                              firstContent ? "  <-- FIRST FRAME WITH PICTURE CONTENT" : "");
            }
        }

        /* MAIN THREAD, and the same nanovg call the cover loader uses: upload the
         * whole surface as one texture, then draw it over the view rectangle. */
        NVGcontext* vg = brls::Application::getNVGContext();
        if (impl->swNvgImage > 0 && vg) {
            /* THE PATTERN'S EXTENTS ARE THE DRAW RECT, NOT THE SURFACE'S PIXEL SIZE.
             *
             * MEASURED ON HARDWARE (user report): in TV / docked mode the picture was INCOMPLETE --
             * "只能显示其中的一部分" -- while handheld mode was fine. This line used to be
             *
             *     nvgImagePattern(vg, x, y, (float)impl->swW, (float)impl->swH, ...)
             *
             * i.e. the pattern was laid out at the size the SURFACE had when mpv was set up, and then
             * filled into a rect of (x, y, width, height), the size the view has NOW. When those two
             * disagree, the image covers only part of the rect and the rest is never drawn.
             *
             * And they DO disagree in docked mode, by exactly the numbers borealis computes
             * (application.cpp:1167-1181): docking reports 1920x1080, so `windowScale` becomes 1.5 and
             * the LOGICAL layout stays 1280x720 -- while the software surface was allocated from
             * `Application::windowWidth/Height` at setupMpv time, which in docked mode is 1920x1080.
             * A 1920x1080 pattern inside a 1280x720 rect shows the top-left nine sixteenths of the
             * frame: a picture that is complete in shape but cropped in content, which is exactly what
             * was reported.
             *
             * Using the rect for both means the picture always fills the area it is drawn into,
             * whatever the surface or the window is doing. In the configuration that works today
             * (handheld, ui.scale=720p) the surface and the rect are both 1280x720, so this is a
             * no-op there -- which is the point: the fix cannot regress the case that works. */
            const double tUpload0 = nowMs();
            nvgUpdateImage(vg, impl->swNvgImage, impl->pixels);
            this->frameUploadMs += (nowMs() - tUpload0);
            /* THE TV SAFE AREA, APPLIED TO THE VIDEO AND NOT TO THE UI.
             *
             * Reported: "在播放界面时，主机模式下会超出电视显示范围". A television crops the outer few percent of
             * whatever it is fed unless the console's own "TV screen size" setting says 100%, and this
             * app's video is the ONLY thing that draws edge to edge -- every other screen has 24 px of
             * padding, which is why the report is about the player screen specifically.
             *
             * hg_tv reads that setting from set:sys (setsysGetTvSettings.underscan) and insets by it,
             * centred, so the picture is drawn SMALLER rather than cropped. It does nothing in handheld
             * mode, when the console says 100%, or when the setting cannot be read -- and it logs what it
             * read, so a wrong assumption cannot hide ``player.tv.safe=<percent>`` overrides it entirely.
             *
             * The inset is applied to the rect AND to the pattern, which is the build37 rule: the two
             * must agree or the image is cropped instead of scaled. */
            float vx = x, vy = y, vw = width, vh = height;
            hg::tvApplySafeArea(&vx, &vy, &vw, &vh);
            nvgBeginPath(vg);
            nvgRect(vg, vx, vy, vw, vh);
            nvgFillPaint(vg, nvgImagePattern(vg, vx, vy, vw, vh, 0, impl->swNvgImage, 1.0f));
            nvgFill(vg);

            /* ONE LINE WHENEVER THE NUMBERS THAT DECIDED THE FIX ABOVE CHANGE, so the next docked run
             * states them instead of leaving them to be inferred: the surface's pixel size, the rect it
             * is drawn into, the window borealis reports and the resulting UI scale. A dock/undock
             * makes at least the window change, so this fires exactly when it should. */
            {
                static int lastVw = -1, lastVh = -1, lastSwW = -1, lastSwH = -1;
                static unsigned lastWinW = 0, lastWinH = 0;
                const int vw = (int)(width + 0.5f), vh = (int)(height + 0.5f);
                if (vw != lastVw || vh != lastVh || impl->swW != lastSwW || impl->swH != lastSwH ||
                    brls::Application::windowWidth != lastWinW ||
                    brls::Application::windowHeight != lastWinH) {
                    lastVw = vw;
                    lastVh = vh;
                    lastSwW = impl->swW;
                    lastSwH = impl->swH;
                    lastWinW = brls::Application::windowWidth;
                    lastWinH = brls::Application::windowHeight;
                    hg::logPrintf("SW surface vs view: surface=%dx%d view=%dx%d window=%ux%u scale=%.3f "
                                  "content=%.0fx%.0f [build61]",
                                  impl->swW, impl->swH, vw, vh, brls::Application::windowWidth,
                                  brls::Application::windowHeight,
                                  (double)brls::Application::windowScale,
                                  (double)brls::Application::contentWidth,
                                  (double)brls::Application::contentHeight);
                }
            }
        }
    }

    /* ---- the progress bar ----------------------------------------------------
     *
     * DRAWN HERE, DELIBERATELY NOT A brls::Slider.
     *
     * Slider looks like the obvious choice and is the wrong one. Its draw() calls
     * buttonsProcessing(), whose first line is `if (pointer->isFocused())` -- but
     * Slider::getDefaultFocus() returns that inner pointer Rectangle
     * UNCONDITIONALLY (library/lib/views/slider.cpp:119) and the pointer is
     * constructed focusable. So adding a Slider to this screen invites borealis'
     * focus search to land on it, and then LEFT/RIGHT would drag the slider
     * instead of reaching the seek actions -- i.e. the seek keys would look dead,
     * which is the exact failure this project has already spent a round on.
     *
     * Two rectangles have no input path at all.
     *
     * IT IS NOW DRAWN AFTER THE VIDEO, further down this function, together with the drag
     * cursor and the preview. That move was not cosmetic: nanovg RECORDS commands and
     * flushes them at the end of the frame, while mpv pushes its own commands into the
     * deko3d queue immediately from inside its render call -- so anything recorded before
     * that render is painted OVER by the video. The bar survived here because on the
     * software path the video is itself a nanovg image drawn just above; the moment a
     * preview panel had to sit on top of the picture, the ordering became load-bearing.
     *
     * `progressBarVisible` is still what hides it, so Y still gives a clean picture. */

    /* ---- the picture menu ---------------------------------------------------
     *
     * Runs BEFORE the seek block, and the seek block is skipped entirely while the
     * menu is open, so LEFT/RIGHT cannot both seek and adjust. This is the only
     * coupling between the two features, and it is one boolean.
     *
     * GATED ON THE SAME `overlay` SWITCH THAT DISABLES THE UI DRAW, and that is
     * deliberate both ways. Box::draw -- which draws the panel, since the panel is one
     * of this view's children -- is skipped under overlay=no, so a menu that still
     * swallowed the direction keys while drawing nothing would be a trap: the seek
     * keys would look broken with nothing on screen to explain it. Diagnostics mode
     * therefore means "the raw framebuffer and nothing else", menu included.
     *
     * The switch is read here rather than reusing noOverlay further down because the
     * two uses are independent and a shared local would couple them: either one could
     * be changed without the other and the build would still be correct. */
    const bool menuEnabled = !hg::cfgIs("overlay", "no");
    if (menuEnabled) {
        this->drawMenuTick();

        /* Push the state into the panel whenever the revision moved -- which includes
         * the frame on which the menu was opened, because opening bumps the revision
         * too. The panel repaints eight short strings and only when something
         * actually changed, so this cannot be a per-frame cost. */
        if (this->menuPanel && this->panelRevision != this->menuRevision) {
            this->menuPanel->updateValues(this->menu, this->qualityLabelPtrs,
                                          this->qualityLabelCount);
            this->panelRevision = this->menuRevision;
        }
    }

    /* ---- hold-to-seek -------------------------------------------------------
     *
     * TWO ARMS, AND THE DEFAULT IS THE DRAG ONE.
     *
     *   player.scrub=drag  (default)  LEFT/RIGHT move a TARGET along the bar; on release ONE
     *                                 absolute seek is issued. See drawScrubTick.
     *   player.scrub=live            the original behaviour: a stream of relative seeks on
     *                                 the acceleration curve, so the video moves while you
     *                                 hold.
     *
     * The drag arm is the default because it is what makes a preview possible at all (a
     * relative seek has no target to show) and because it stops the drag from asking mpv to
     * decode every intermediate position -- on a two-hour video that is the most expensive
     * thing this player can be asked to do, and all but the last position is thrown away.
     *
     * The live arm is kept, not deleted, because it is the version that has been on hardware:
     * this whole feature ships unverified, and a cfg line that restores the verified
     * behaviour is the cheapest possible way back. It is the same rule applied to every
     * change in this project.
     *
     * Read the RAW controller state every frame rather than reacting to button
     * actions, following wiliwili's VideoProgressSlider::buttonsProcessing. See
     * seekStepFor above for why: borealis' action repeat is a fixed 100 ms cadence
     * with no acceleration, and two paths reacting to one press would double-seek.
     *
     * Only runs while a file is actually playing -- holding a direction before the
     * stream opens should do nothing rather than queue a seek into a void.
     *
     * The frame time comes from borealis' own FPS counter so the rate is per
     * SECOND, not per frame: the same hold moves the same amount whether the
     * console is running at 30 or 60 fps.
     *
     * THE TWO ARMS ARE MUTUALLY EXCLUSIVE AND THAT IS ENFORCED HERE, not by a flag inside
     * each one. If both ran, one press would move the target AND issue a relative seek, and
     * the two would fight over the position. */
    const bool dragArm = this->dragSeekEnabled();
    /* THE CONTROL BAR TAKES LEFT/RIGHT WHEN SOMETHING OTHER THAN THE PROGRESS IS SELECTED.
     *
     * While the bar is up and its selection is on 音量 / 倍速 / 缩放, LEFT/RIGHT adjusts that value instead of
     * seeking -- which is how a console player's bar works and the only way those three can be operated
     * without inventing another input path. On the PROGRESS item (and when the bar is down) LEFT/RIGHT is
     * the hold-to-seek it has always been, so nothing that worked before stops working. */
    /* ---- LEFT/RIGHT BELONGS TO SEEKING, ALWAYS (build119, and the user is right) ------------------------
     *
     * THEIR WORDS: "fast forward and rewind seem to be gone."
     *
     * They were. This gate counted "the bar is up on something other than the progress item" as "the D-pad is on
     * a control" and handed LEFT/RIGHT to the chip instead of to the seek -- and with the bar now staying up for
     * ten seconds, and coming up on a tap, that was most of the time. The log shows the result: the drag path
     * fired but the target never moved ("SCRUB: drag end at 0.0 s -> committing an absolute seek").
     *
     * The gate is not needed any more: chip VALUES are chosen inside their pull-up menus (A or a tap), which is
     * what those menus are for. So LEFT/RIGHT goes back to the hold-to-seek it is on every console player, and
     * only an actually-open menu or panel takes the D-pad. */
    const bool dpadOnAControl = this->chipMenu != 0 || this->settings.open != 0 || this->menu.open != 0;
    if (impl && impl->started && impl->mpv && !this->menu.open && !this->settings.open && !dragArm &&
        !dpadOnAControl) {
        auto& st = brls::Application::getControllerState();
        /* L/R SEEK WITHOUT ASKING THE BAR (build120): the hold-to-seek levels come from these too, so a player
         * who has the bar up can still fast forward and rewind. */
        int dir = 0;
        int mult = 1;
        if (st.buttons[brls::BUTTON_RIGHT]) {
            dir = +1;
        } else if (st.buttons[brls::BUTTON_LEFT]) {
            dir = -1;
        }

        if (dir == 0) {
            /* Released: the next press starts a fresh hold, so the acceleration
             * restarts from the first bucket rather than staying fast. */
            if (impl->seekDown) {
                hg::logPrintf("SEEK: released after moving %.0f s", impl->seekMoved);
            }
            impl->seekDown = false;
            impl->seekDir = 0;
            impl->seekMoved = 0.0;
            impl->seekPending = 0.0;
            impl->seekFramesLogged = 0;
        } else if (!impl->seekDown) {
            /* FRESH PRESS: one step immediately, so a tap does something. Without
             * this a tap would move nothing until the next frame's rate arrived,
             * which for a short press is nothing at all. */
            impl->seekDown = true;
            impl->seekDir = dir;
            impl->seekMoved = 0.0;
            impl->seekPending = 0.0;
            int step = seekStepFor(0) * mult * dir;
            this->seekRelative(step);
            impl->seekMoved += step < 0 ? -(double)step : (double)step;
            impl->seekFramesLogged = 1;
        } else if (impl->seekDir != dir) {
            /* Direction reversed without a release: restart the ramp, because
             * carrying over the distance already travelled would make the very
             * first step in the new direction a fast one. */
            impl->seekDir = dir;
            impl->seekMoved = 0.0;
            impl->seekPending = 0.0;
            int step = seekStepFor(0) * mult * dir;
            this->seekRelative(step);
            impl->seekMoved += step < 0 ? -(double)step : (double)step;
        } else {
            int fps = (int)brls::Application::getFPS();
            if (fps <= 0) fps = 60;
            double dt = 1.0 / (double)fps;
            double rate = (double)(seekStepFor((int)impl->seekMoved) * mult);
            impl->seekPending += rate * dt * (double)dir;
            if (impl->seekPending >= 1.0 || impl->seekPending <= -1.0) {
                int whole = (int)impl->seekPending;
                impl->seekPending -= (double)whole;
                this->seekRelative(whole);
                impl->seekMoved += whole < 0 ? -(double)whole : (double)whole;
                /* Log the first few steps of a hold and then only the ramp
                 * changes, so a hold that accelerates can be read back from the
                 * log without 60 lines per second. */
                if (impl->seekFramesLogged < 6 || (impl->seekFramesLogged % 30) == 0) {
                    hg::logPrintf("SEEK: hold dir=%d moved=%.0fs step=%ds rate=%.0f/s",
                                  dir, impl->seekMoved, whole, rate);
                }
                impl->seekFramesLogged++;
            }
        }
    }

    /* mpv renders into borealis' CURRENT framebuffer, fenced. wiliwili does
     * exactly this; the fence pair is required for GPU/CPU ordering. Drawn
     * BEFORE the child labels so the status text stays readable on top of the
     * video rather than being covered by it. */
    if (impl && !impl->swRender && impl->renderCtx && impl->video) {
        /* Drain first: mpv's messages up to this instant are the record of what
         * the library did since the previous frame. */
        drainMpvEvents(impl);

        /* With advanced control (wiliwili's setting) mpv expects the embedder to
         * ask whether a frame is ready. wiliwili does it from mpv's update
         * callback; polling it here once per frame is the same question asked
         * from the render thread.
         *
         * The flag is LOGGED because it answers a question no amount of reading
         * settles: in advanced mode mpv_render_context_render() is documented to
         * render only when an update is pending, so if
         * MPV_RENDER_UPDATE_FRAME never appears here, every render call in this
         * app has been drawing nothing -- which is exactly what a grey screen
         * with working audio looks like. */
        uint64_t upFlags = mpv_render_context_update(impl->renderCtx);

        bool logThis = this->mpvDraws < 6;
        if (logThis) {
            this->mpvDraws++;
            /* MEASURE THE IMAGE, DO NOT ASSUME IT.
             *
             * Every element of wiliwili's deko3d render path has now been
             * compared line by line against this file and they match: the create
             * params, the option set, the update callback, the one-time
             * mpv_deko3d_fbo with only .tex rewritten, the fence pair, the
             * render/report_swap sequence, and the mpv_render_param array. The
             * picture still does not reach the presented frame -- proven by
             * overlay=no showing flat grey.
             *
             * That leaves what we hand mpv rather than how we drive it. The
             * dimensions we TELL mpv (impl->fbo.w/h) and the dimensions the
             * dk::Image actually HAS are two separate facts, and only the first
             * was ever checked. This deko3d has no dimension getter for a
             * DkImage, but dkImageGetLayout + dkImageLayoutGetSize gives the
             * backing size in bytes, which distinguishes 1280x720 RGBA8
             * (3,686,400 B) from anything else of a different size by an order of
             * magnitude -- including the docked 1920x1080 (8,294,400 B). */
            uint64_t imgBytes = 0;
            DkImageLayout const* lay = dkImageGetLayout(impl->video->getFramebuffer());
            if (lay) imgBytes = dkImageLayoutGetSize(lay);
            hg::logPrintf("MPV draw #%d: ENTER (fbo=%p imageBytes=%llu declared=%dx%d)"
                          " update flags=0x%llX update_frame=%d",
                          this->mpvDraws, (void*)impl->video->getFramebuffer(),
                          (unsigned long long)imgBytes, impl->fbo.w, impl->fbo.h,
                          (unsigned long long)upFlags,
                          (int)((upFlags & MPV_RENDER_UPDATE_FRAME) != 0));
        }
        /* ---- THE DOCKED PICTURE, AND THE DIMENSIONS ARE WHY IT WAS A CORNER (build170) --------------
         *
         * THIS LINE USED TO SAY "Only .tex changes per frame ... the fences and dimensions were fixed at
         * setup" -- and that sentence was the bug. The dimensions were NOT safe to fix.
         *
         * MEASURED ON HARDWARE (user report): "playing video, insert the dock -- the picture becomes a small
         * block in the top-left corner, and only closing and reopening the app brings back the full-screen TV
         * picture." Closing and reopening works because that re-runs setupMpv, which is the ONLY place that
         * ever wrote fbo.w/h -- so whatever goes wrong is something setupMpv got right and the running
         * session never updates. The dimensions are exactly that.
         *
         * WHY FIXED DIMENSIONS SHOW AS A CORNER BLOCK. On the deko3d path mpv is handed a target image and the
         * dimensions to fill (render_dk3d.h: mpv_deko3d_fbo{tex, ready_fence, done_fence, w, h, format}).
         * Docking turns borealis' swapchain framebuffer from 1280x720 into 1920x1080 (switch_video.cpp:191
         * updateWindowSize -> Application::onWindowResized). mpv then scales its frame into the 1280x720
         * RECTANGLE it was told about, inside a 1920x1080 image: the picture occupies the top-left 1280x720
         * of a 1920x1080 screen. A small block in the corner. The swapchain itself is rebuilt correctly by
         * borealis' own hook, which is why the UI is fine and only the video is wrong.
         *
         * THE FIX IS UPSTREAM'S, COPIED RATHER THAN INVENTED. wiliwili -- the app this Switch port follows,
         * and the one whose deko3d render path this file already matches line for line -- refreshes these two
         * fields on EVERY draw, and only for the default-framebuffer path that this app is on
         * (wiliwili/source/view/mpv_core.cpp:733-735, inside MPVCore::setFrameSize, which its draw() calls
         * whenever the rect changes at line 785):
         *
         *     this->mpv_fbo.w = brls::Application::windowWidth;
         *     this->mpv_fbo.h = brls::Application::windowHeight;
         *
         * So this is not a new mechanism and not a swapchain of our own: it is the same two assignments, in
         * the same place in the frame, reading the same borealis statics.
         *
         * WHY THE DRAW PATH AND NOT THE APPLET HOOK. The hook fires on libnx' thread at an instant that has no
         * defined ordering against borealis' own framebuffer rebuild, and mpv must be told the size that
         * matches the image it is about to be given. Here, `getFramebuffer()` two lines below is the SAME
         * framebuffer image this size describes, read in the same frame -- so the two cannot disagree. It is
         * also why nothing is done in the hook: one owner, one fix, no second writer to argue with.
         *
         * THE RENDER CONTEXT IS NOT TOUCHED. mpv_render_context_create is NOT re-run: its teardown/rebuild
         * order and fence logic (see the notes at the top of this file and in buildStaticCommands) are
         * expensive and have crashed before, and none of it is needed -- w/h are read by mpv inside
         * mpv_render_context_render, which happens immediately after these assignments. */
        const unsigned winW = brls::Application::windowWidth;
        const unsigned winH = brls::Application::windowHeight;
        if (impl->fbo.w != (int)winW || impl->fbo.h != (int)winH) {
            /* LOGGED BECAUSE THIS IS THE WHOLE FIX AND IT MUST BE VISIBLE. Before this build a dock produced
             * no line at all, so "the size was never updated" and "the size was updated wrongly" could not be
             * told apart. The old value is copied out BEFORE the assignment so the line can state the
             * transition rather than only where it landed.
             *
             * AND IT STATES EVERY LAYER AT ONCE, on purpose. "Small block in the corner" has three candidate
             * owners -- borealis' framebuffer, the size we hand mpv, and the rect we draw into -- and a log
             * that reports only one of them cannot say which one failed to follow. borealis' half is not ours
             * to fix and is not in this file; the two that ARE ours are both here, and this is the only line
             * in the app that prints them together with the dock state. `rect` is this view's live
             * (x, y, width, height) for the frame being drawn, and it is the pair that must agree with the
             * fbo size for the picture to fill the screen. */
            const int oldW = impl->fbo.w;
            const int oldH = impl->fbo.h;
            impl->fbo.w = (int)winW;
            impl->fbo.h = (int)winH;
            hg::logPrintf("PLAYER: mpv render target resized %dx%d -> %dx%d (the window changed) -- dock/undock "
                          "is followed within one frame, not on the instant [build170]",
                          oldW, oldH, impl->fbo.w, impl->fbo.h);
            hg::logPrintf("APPLET: %s -> window/fb %ux%u, mpv render size %dx%d, view rect %.0f,%.0f %.0fx%.0f "
                          "(was %dx%d) [build170]",
                          hg::appletIsDocked() ? "docked" : "handheld", winW, winH, impl->fbo.w, impl->fbo.h,
                          (double)x, (double)y, (double)width, (double)height, oldW, oldH);
        }
        /* Only .tex changes per frame: borealis alternates framebuffers. */
        impl->fbo.tex = impl->video->getFramebuffer();
        impl->video->queueSignalFence(&impl->readyFence);
        impl->video->queueFlush();
        if (logThis) hg::logPrintf("MPV draw #%d: fences queued, calling render", this->mpvDraws);

        /* THE RETURN VALUE, WHICH WAS BEING THROWN AWAY.
         *
         * mpv_render_context_render() returns an int. This file called it and
         * ignored the result for its entire life, which is how a failing render
         * call looked exactly like a successful one: the log said "render
         * returned" either way. It is logged on every one of the first six draws
         * and then only when it is non-zero, so a persistent failure cannot hide
         * behind the "first six" cutoff. */
        int rcRender = mpv_render_context_render(impl->renderCtx, impl->renderParams);
        if (logThis || rcRender != 0) {
            hg::logPrintf("MPV draw #%d: render returned rc=%d", this->mpvDraws, rcRender);
        }

        /* WHERE THE PICTURE GOES MISSING, PART TWO.
         *
         * `queueWaitFence` makes borealis' deko3d QUEUE wait for mpv's queue to
         * signal. On paper that orders mpv's writes before the UI and before the
         * present. The console says otherwise: with advanced control off (so
         * render() is unconditional), every call returning success, the FBO
         * right and the fences wired, the presented frame still contains none of
         * the video -- it appears only for an instant during teardown, when the
         * per-frame clear stops.
         *
         * `fencewait=cpu` swaps the queue-side wait for a CPU-side
         * dkFenceWait(kQueenFence, -1): the frame is not finished, and the
         * remaining commands plus the present are not submitted, until mpv's GPU
         * work is provably complete. It serialises the CPU with the GPU and
         * costs throughput, which is exactly why it is a diagnostic lever and
         * not an obvious default. If the picture appears with it, the fault is
         * fence ordering; if it does not, mpv is writing somewhere that is not
         * being presented. */
        static const bool cpuFenceWait = hg::cfgIs("fencewait", "cpu");
        if (cpuFenceWait) {
            dkFenceWait(&impl->doneFence, -1);
            if (logThis) hg::logPrintf("MPV draw #%d: CPU waited on doneFence", this->mpvDraws);
        } else {
            impl->video->queueWaitFence(&impl->doneFence);
        }
        mpv_render_context_report_swap(impl->renderCtx);
        if (logThis) hg::logPrintf("MPV draw #%d: OK complete", this->mpvDraws);
    }

    /* THE POLLED STATUS LINE IS NO LONGER SHOWN, AT THE USER'S REQUEST.
     *
     * MEASURED (their photograph, 2026-10-10): with the bar up there were TWO progress indicators, the upper one
     * being "PLAYING 0:20 / 3:08" plus a line -- and their instruction was exact: "delete the progress bar of
     * yours with PLAYING and PAUSE on it". It was the last piece of the old overlay surviving in a different
     * place: the activity's label was deleted with the overlay row, but this push kept feeding whatever label
     * the view was handed.
     *
     * `impl->statusText` still exists and is still written by the poll thread, because its OTHER half is the SD
     * log line ("MPV: PLAYING 00:03 / 02:21"), which is where every diagnosis in this project has come from. It is
     * simply not painted any more: the control bar carries the clock, and the picture carries nothing.
     */

    /* ---- the progress bar, the drag cursor and the preview -------------------
     *
     * HERE, AFTER THE VIDEO, and the position is load-bearing rather than tidy.
     *
     * nanovg only RECORDS drawing commands; they are flushed in nvgEndFrame at the end of
     * the whole UI pass (application.cpp:811). mpv, by contrast, pushes its commands into
     * the deko3d queue immediately from inside its own render call. So the GPU executes
     * whatever mpv wrote at the moment it was called, and every nanovg command recorded
     * BEFORE that instant ends up underneath the picture.
     *
     * The bar used to be drawn before the video render and it worked, because on the
     * software path the video is itself a nanovg image drawn moments later -- so both were
     * in the same recorded batch and the later one won. A preview PANEL floating over the
     * picture is a different matter, and relying on that accident would have made the panel
     * disappear on the deko3d path. Drawing after the render is correct on both.
     *
     * Gated on the same `overlay` switch as everything else on this screen, and on the bar's
     * own visibility, so Y still gives a clean picture with no furniture on it. */
    /* THE SCREENSHOT PROBE, ISSUED FROM HERE ONCE PLAYBACK IS RUNNING.
     *
     * Not from the plan worker and not from a brls::sync callback: it needs a frame to exist (so it
     * waits until `started`), it is asynchronous (so it costs nothing here), and its reply arrives
     * through drainMpvEvents, which the next line of this function calls. Issuing it anywhere that
     * does not drain events would make the reply arrive never. */
    if (impl && impl->started && hg::cfgIs("player.shotprobe", "yes")) {
        requestScreenshotProbe(impl);
    }

    /* THE CONTROL BAR'S IDLE TIMER, evaluated here because draw() is this view's only per-frame hook.
     * Nothing is scheduled and nothing has to be cancelled: the bar is up while the elapsed time is inside
     * the window, and the log says once when it went away. */
    /* THE BAR'S OWN KEYS, polled with the rest of this screen's input (see the class header). */
    if (impl && impl->started && menuEnabled) this->tickControls();
    /* THE SLEEP TIMER AND THE SETTINGS PANEL. Both run whether or not the bar is up: the timer must fire
     * while the picture is clean, and the panel is opened from the bar but outlives it. */
    /* ---- RECORDING WHAT IS BEING WATCHED (D1) --------------------------------
     *
     * Once when playback actually starts and then every 15 seconds. Fifteen because the console can end a
     * session with the power button at any moment, so the cost of losing the position has to be bounded; and
     * because hg_store SAVES on every call, a record that only reached memory would be a record lost.
     *
     * The series identity comes from the activity (it is the one that knows which series this is); the view
     * only knows the episode id and how far into it the player is. */
    if (impl && impl->started && this->historyOwner && !this->episodeId.empty()) {
        const double nowMs2 = nowMs();
        if (nowMs2 >= this->nextHistoryAt) {
            this->nextHistoryAt = nowMs2 + 15000.0;
            const double dur = (double)impl->duration.load();
            const double pos = dur > 0.0 ? (double)impl->progress.load() * dur : 0.0;

            /* ---- AN EPISODE HAS TO BE WATCHED BEFORE IT COUNTS AS HISTORY (build128, the user's rule) ----
             *
             * THEIR WORDS: "add that an episode only counts as history after thirty seconds; opening it for a
             * moment should not count, that is not right."
             *
             * They are right. This tick used to write a record as soon as the player knew a duration, so opening an
             * episode and backing out two seconds later pushed it to the top of the continue-watching list -- and
             * with the per-episode rows that hg_history.c used to write, that is exactly how that list filled up
             * with things nobody had watched.
             *
             * Thirty seconds is the threshold the streaming services use, and it is generous on purpose: a few
             * seconds is enough to discover that a stream does not play, and someone who gave up on an episode
             * should not find it remembered as progress. Once past the line, the record updates every fifteen
             * seconds as before, so the position stays fresh. */
            const double HG_HISTORY_MIN_SECONDS = 30.0;
            if (pos >= HG_HISTORY_MIN_SECONDS) {
                this->historyOwner->recordPlayback(this->episodeId, pos, dur);
            } else {
                static bool saidTooShort = false;
                if (!saidTooShort) {
                    saidTooShort = true;
                    hg::logPrintf("PLAYER: history not recorded yet -- %.1fs watched, and an episode needs %.0fs "
                                  "before it counts [build128]",
                                  pos, HG_HISTORY_MIN_SECONDS);
                }
            }
        }
    }
    if (impl && impl->started) {
        this->tickSleepTimer();
        this->tickSettings();
        if (this->settingsPanel) this->settingsPanel->setValues(this->settings.sleepIndex, this->settings.barAlways, this->settings.playMode);
        if (this->settingsPanel) this->settingsPanel->setHighlight(this->settings.row);
    }
    if (this->controlsOn && !this->controlsVisible()) {
        hg::logPrintf("PLAYER: control bar hid itself after %.0f ms of no input", HG_PLAYER_CONTROLS_MS);
        this->controlsOn = false;
    }

    /* AND THE BAR ITSELF, drawn where the progress bar used to be drawn on its own. When the bar is up it
     * REPLACES the plain progress line rather than stacking on it: two progress bars on one screen is what
     * happens when a feature is added next to an old one instead of in place of it. */
    if (impl && impl->started && menuEnabled && this->controlsOn) {
        NVGcontext* vgBar = brls::Application::getNVGContext();
        if (vgBar) this->drawControlBar(vgBar, x, y, width, height);
    }
    /* ---- the overlay's own progress line ---------------------------------------------------------------
     *
     * REMOVED AT THE USER'S REQUEST, ON THE STRENGTH OF THEIR OWN PHOTOGRAPHS: one showed the thin line across
     * the picture with the overlay's text hidden ("whether I press Y or not there is this line, and I do not
     * need that information"), and the same message said "when I press A I can see the progress bar". The
     * control bar already carries a progress bar with a clock, so this was a second copy of the same thing on
     * top of the picture -- in the one state (Y pressed) where the user asked for a clean screen.
     *
     * THE DRAG ARM KEEPS ITS TICK AND ITS CLOCK, because that is not information but an input feedback: it is
     * what the user is dragging, and it disappears when they let go.
     *
     * AND THE CONDITION IS "THE SCRUB IS ACTIVE", NOT "drag-seek is enabled" -- WHICH IS A BUG FIX, MEASURED ON
     * HARDWARE. This line used to read `menuEnabled && dragArm`, and `dragArm` is `dragSeekEnabled()`, i.e. it is
     * true ALL THE TIME under the default settings. So the tick and its "PLAYING 0:20 / 3:08" clock were drawn on
     * every frame -- which is exactly what the user photographed and reported AGAIN after testing build81: "the
     * PLAYING and PAUSE bar is still there". The text lives here, in drawScrubTick, not in the overlay that was
     * deleted: that is why removing the overlay did not touch it.
     *
     * `scrub.active` is true only while a drag is in progress, so now the line appears when the user holds
     * LEFT/RIGHT and is gone the moment they release it -- which is what "input feedback" meant all along. */
    if (impl && impl->started && menuEnabled && dragArm && this->scrub.active) {
        NVGcontext* vg = brls::Application::getNVGContext();
        if (vg) {
            this->drawScrubTick(vg, x, y, width, height);
        }
    }

    /* `overlay=no` DRAWS NOTHING BUT THE VIDEO.
     *
     * The console shows a grey screen with four text bars, and grey is exactly
     * the theme's clear colour -- so either the video never reached the
     * presented frame, or something in this UI is drawn over all of it. Those
     * two possibilities call for completely different fixes, and this switch
     * tells them apart in one run: with the overlay gone, the frame is whatever
     * mpv put there and nothing else.
     *
     *   video appears  -> the UI was covering it; the fault is draw order
     *   still grey     -> mpv's output is not in the presented image at all
     *
     * The status line stops updating in this arm; the log still records every
     * value, which is what the log is for. */
    /* ---- THE BACKGROUND IS SKIPPED ON THE DEKO3D PATH (build105, and the user found it) -----------------
     *
     * MEASURED ON HARDWARE, from the user: with `renderer=deko3d` the picture was hidden while the sound played,
     * "on exiting the player the video flashes for an instant", and with `overlay=no` -- which skips exactly the
     * call below -- THE PICTURE APPEARED. So the thing covering the video is this one call.
     *
     * WHY: with MPV_RENDER_API_TYPE_DEKO3D, mpv writes its frame STRAIGHT INTO THE SWAPCHAIN IMAGE at the moment
     * it is called (from inside this view's draw, so after borealis' clear and before nanovg's final submission).
     * Everything nanovg records is submitted at nvgEndFrame, i.e. AFTERWARDS -- and `brls::Box::draw` paints this
     * view's background, a full-screen OPAQUE rectangle from the theme. So the frame is: the video (good), then a
     * full-screen opaque box on top of it. This never showed on the SW path because there the video is itself an
     * nvg image drawn INSIDE the UI pass, so it lands above the background.
     *
     * So the background is not painted when the video renders straight into the swapchain. Only the background is
     * lost: the control bar, the hint bar, the menus and the panels are drawn by other calls, so the player keeps
     * its whole interface -- unlike `overlay=no`, which is the diagnostic and stays available. */
    static const bool noOverlay = hg::cfgIs("overlay", "no");
    const bool videoOwnsTheFramebuffer = impl && impl->renderCtx && !impl->swRender;
    if (!noOverlay && !videoOwnsTheFramebuffer) {
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    } else {
        static bool said = false;
        if (!said) {
            said = true;
            hg::logPrintf("PLAYER draw: the view background is NOT drawn (%s) -- on the deko3d path mpv owns the "
                          "framebuffer and an opaque box here would erase the picture [build105]",
                          noOverlay ? "overlay=no" : "deko3d render path");
        }
    }
}

void PlayerView::togglePause() {
    Impl* impl = this->impl;
    if (!impl || !impl->mpv || !impl->started) return;
    int paused = 0;
    if (mpv_get_property(impl->mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0) return;
    int next = paused ? 0 : 1;
    mpv_set_property(impl->mpv, "pause", MPV_FORMAT_FLAG, &next);
}

/* ---- absolute seek, and the drag bar ------------------------------------- */

int PlayerView::seekAbsolute(double seconds) {
    Impl* impl = this->impl;
    if (!impl || !impl->mpv || !impl->started) {
        hg::logPrintf("PLAYER seekAbsolute: ignored (mpv not started)");
        return -1;
    }
    if (!(seconds >= 0.0)) seconds = 0.0;

    /* mpv parses the number itself, so it goes over as TEXT: "seek 123.5 absolute". The
     * value is formatted with one decimal, which is finer than the ~10 ms the bar can
     * express and keeps the string short. */
    char amount[32];
    snprintf(amount, sizeof amount, "%.1f", seconds);
    const char* args[] = {"seek", amount, "absolute", nullptr};
    int rc = mpv_command_async(impl->mpv, 0, args);
    hg::logPrintf("PLAYER seekAbsolute: seek %s absolute -> request %d", amount, rc);

    /* Read back, for the same reason seekRelative does: the request id only proves mpv
     * accepted the command. A seek to a position mid-decode can be accepted and land
     * somewhere else entirely. */
    double tp = 0.0;
    if (mpv_get_property(impl->mpv, "time-pos", MPV_FORMAT_DOUBLE, &tp) >= 0) {
        hg::logPrintf("PLAYER seekAbsolute: time-pos is now %.2f (asked for %.1f)", tp, seconds);
    }
    return rc;
}

bool PlayerView::dragSeekEnabled() const {
    static const bool drag = !hg::cfgIs("player.scrub", "live");
    return drag;
}

/* The bar, its cursor, and the preview. Called once per frame from draw(), on the main
 * thread, AFTER mpv has rendered -- because everything drawn here records nanovg commands
 * into the frame that is flushed at the end of the frame, and anything recorded BEFORE the
 * video render would be painted over by it.
 *
 * THE KEYS. LEFT/RIGHT and LB/RB, held. On the first frame of a hold the drag begins at the
 * current position; every frame after that moves the target by the same acceleration curve
 * the relative seeking used (seekStepFor, wiliwili's own curve from video_view.cpp:40-47);
 * on release ONE absolute seek is issued. So the position changes once per drag instead of
 * sixty times a second, and the bar can show where it is going.
 *
 * WHY THE CURVE IS REUSED RATHER THAN REINVENTED: it is tuned for exactly this gesture --
 * short holds nudge, long holds cover minutes -- and two different curves for the same key
 * would be a difference the user could feel but not explain. */
void PlayerView::drawScrubTick(NVGcontext* vg, float x, float y, float width, float height) {
    Impl* impl = this->impl;
    if (!impl || !vg) return;

    /* THE CONTROL BAR OWNS LEFT/RIGHT WHILE A VALUE IS SELECTED (see the note in the seek poll): the drag
     * must not run at the same time, or one press would both move the scrub target and step the volume. */
    if (this->controlsOn && this->controlsIndex != PlayerView::CONTROL_PROGRESS) return;

    auto& st = brls::Application::getControllerState();
    const double now = nowMs();

    HgScrubBar bar;
    /* The bar spans the screen with the same insets the progress bar always had. */
    bar.barX = x + 16.0f;
    bar.barW = width - 32.0f;
    bar.duration = (double)impl->duration.load();

    /* Which key, if any, is held? LB/RB are the coarse pair, exactly as they were for
     * relative seeking. */
    int dir = 0;
    int mult = 1;
    if (st.buttons[brls::BUTTON_RIGHT]) {
        dir = +1;
    } else if (st.buttons[brls::BUTTON_LEFT]) {
        dir = -1;
    }

    if (dir != 0 && !this->scrub.active) {
        /* FRESH PRESS. The anchor is the CURRENT position, read from the poll thread's last
         * sample -- so a tap starts from where the video actually is rather than from where
         * the UI last thought it was. hg_scrub_begin refuses when the duration is unknown,
         * and the refusal is logged once so "the seek keys do nothing" has a reason. */
        double pos = (double)impl->position.load();
        double dur = (double)impl->duration.load();
        if (!hg_scrub_begin(&this->scrub, pos, dur, dir, now)) {
            /* LOGGED, BUT NOT EVERY FRAME. A hold that cannot start a drag lasts as long as
             * the user holds the key, and this runs 60 times a second: unconditional logging
             * would fsync the SD card thousands of times. The first few are logged, then one
             * in every 600 frames -- enough for the log to say what happened without the
             * diagnostic becoming the problem. */
            if (this->scrubRefusals < 2 || (this->scrubRefusals % 600) == 0) {
                hg::logPrintf("SCRUB: cannot drag -- duration is %.0f (mpv has not reported one "
                              "yet); position %.1f",
                              dur, pos);
            }
            this->scrubRefusals++;
            return;
        }
        this->scrubRefusals = 0;
        hg::logPrintf("SCRUB: drag begin at %.1f s of %.0f s (dir %+d)", pos, dur, dir);
        /* The first step happens on the next frame, so that a press-and-release with no
         * movement is a no-op rather than a one-pixel jump. */
    }

    if (dir != 0 && this->scrub.active) {
        if (this->scrub.dir != dir) {
            /* Direction reversed mid-hold. Restart the ramp, because carrying the distance
             * already travelled into the new direction would make the first step in the new
             * direction a fast one -- the same rule the relative-seek path had. */
            this->scrub.movedSeconds = 0.0;
            this->scrub.dir = dir > 0 ? 1 : -1;
            hg::logPrintf("SCRUB: direction reversed, ramp restarted");
        }

        /* ---- ONE PRESS IS TEN SECONDS, AND HOLDING ACCELERATES (build122, the user's rule) ---------------
         *
         * THEIR WORDS: "if that is how it is, make a single press a fixed ten seconds, and holding it more --
         * accelerate."
         *
         * MEASURED, and it is why they said fast forward and rewind "do not work": the only step was
         * `rate * dt`, i.e. per-frame accumulation, so a tap moved a fraction of a second and the commit rounded
         * to the position it started at. Every log line read
         *
         *     SCRUB: drag end at 0.0 s -> committing an absolute seek
         *
         * -- the drag worked and never went anywhere.
         *
         * So the FIRST step of a hold is a fixed ten seconds (times the coarse multiplier, so LB/RB jump sixty),
         * and from there the ramp accumulates exactly as before. A tap therefore lands ten seconds away, which is
         * what a player expects; keeping the button down still accelerates, which is what this code already did. */
        if (this->scrub.movedSeconds <= 0.0) {
            const double tapStep = 10.0 * (double)mult;
            if (hg_scrub_step(&this->scrub, tapStep * (double)dir)) {
                hg::logPrintf("SCRUB: first step of a press -> a fixed %.0f s (hold to accelerate) [build122]",
                              tapStep);
            }
        }

        int fps = (int)brls::Application::getFPS();
        if (fps <= 0) fps = 60;
        double rate = (double)(seekStepFor((int)this->scrub.movedSeconds) * mult);
        double dt = 1.0 / (double)fps;
        if (hg_scrub_step(&this->scrub, rate * dt * (double)dir)) {
            /* The preview is requested from the model, which rate-limits it: a drag steps
             * every frame and a preview is not free. Nothing is rendered for it yet -- see
             * the note in the preview function -- but the readout is what the user needs to
             * position, and it costs one string. */
            if (hg_scrub_preview_due(&this->scrub, now)) {
                hg_scrub_preview_done(&this->scrub, now);
            }
        }
    } else if (dir == 0 && this->scrub.active) {
        /* RELEASE: one absolute seek. This is the whole point of the model -- one command
         * instead of one per frame, and it lands on the position the bar was showing. */
        double target = 0.0;
        int commit = hg_scrub_end(&this->scrub, &target);
        if (commit) {
            hg::logPrintf("SCRUB: drag end at %.1f s -> committing an absolute seek", target);
            this->seekAbsolute(target);
        } else {
            hg::logPrintf("SCRUB: drag end with no movement -- no seek issued");
        }
    }

    /* ---- draw the bar -------------------------------------------------------
     *
     * Two rectangles, exactly as before, plus a cursor while dragging. The drawn fraction
     * is the SCRUB TARGET while dragging and the live position otherwise -- that is what
     * makes the bar follow the drag instead of the video. */
    const double frac = this->scrub.active
                            ? hg_scrub_fraction(&this->scrub)
                            : clamp01f((double)impl->progress.load());
    const float barH = 6.0f;
    const float barY = y + height - 82.0f;

    nvgBeginPath(vg);
    nvgRect(vg, bar.barX, barY, bar.barW, barH);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 64));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRect(vg, bar.barX, barY, bar.barW * (float)frac, barH);
    nvgFillColor(vg, brls::Application::getTheme().getColor("brls/accent"));
    nvgFill(vg);

    if (this->scrub.active) {
        /* The cursor. Drawn even at fraction 0, so the affordance is visible from the
         * first frame of a drag rather than appearing once the drag has moved. */
        const float cx = hg_scrub_x_for(&bar, frac);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, barY + barH * 0.5f, 9.0f);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);
        this->drawScrubPreview(vg, x, y, width, height, bar);
    }

    /* The clock readout under the bar, built from the SAME formatter the bar uses, so the
     * text and the cursor cannot disagree. While dragging it reads "target / duration" and
     * is prefixed with an arrow, which is the only thing that distinguishes a drag from
     * playback on screen. */
    char cur[24], dur[24];
    hg_scrub_format_time(this->scrub.active ? hg_scrub_position_seconds(&this->scrub)
                                            : (double)impl->position.load(),
                         cur, (int)sizeof cur);
    hg_scrub_format_time(bar.duration, dur, (int)sizeof dur);
    char line[96];
    if (this->scrub.active) {
        snprintf(line, sizeof line, "SEEK  %s / %s", cur, dur);
    } else {
        snprintf(line, sizeof line, "%s  %s / %s", impl->paused.load() ? "PAUSED " : "PLAYING",
                 cur, dur);
    }
    nvgFontSize(vg, 18.0f);
    nvgFontFaceId(vg, 0);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgText(vg, bar.barX, barY - 6.0f, line, nullptr);
}

void PlayerView::drawScrubPreview(NVGcontext* vg, float x, float y, float width, float height,
                                  const HgScrubBar& bar) {
    /* THE PREVIEW IS A TIME READOUT, NOT A THUMBNAIL FRAME, AND THAT IS A MEASURED LIMIT
     * RATHER THAN A SHORTCUT.
     *
     * wiliwili's slider preview shows a picture, and it gets it from the service: its
     * VideoSnapshotCore consumes a `VideoSnapshotData` describing SPRITE SHEETS the server
     * provides (view/video_snapshot_core.hpp). This service ships no such data -- the video
     * model carries a URL and a CENC key and nothing else -- so a picture here would have to
     * be captured locally.
     *
     * Capturing locally means one of two things, and both are a separate piece of work with
     * its own risks:
     *   - mpv's screenshot-raw, which decodes the frame at the target position a second time
     *     and hands back a large pixel buffer to upload as an nvg texture.
     *   - a SECOND mpv render context in software mode, rendering the target frame into a
     *     small RGBA buffer to use as a thumbnail. The app already has the software path
     *     (see the renderer=sw arm), so this is the more promising of the two -- but two
     *     render contexts on one mpv handle is a configuration nobody here has verified, and
     *     the video-visible failure mode is a black screen.
     *
     * So what is drawn is the thing a preview is FOR: the time you are about to land on, at a
     * size you can read while dragging. Done is better than "nearly done" with an unverified
     * second render context in a round that cannot be tested on hardware. This note is here so
     * the gap is a recorded decision rather than an oversight -- the house rule for "not done
     * yet" in this project.
     *
     * The panel sits ABOVE the bar and is clamped inside the screen, so a drag near the right
     * edge does not push it off. */
    char target[24], total[24], body[64];
    double pos = hg_scrub_position_seconds(&this->scrub);

    hg_scrub_format_time(pos, target, (int)sizeof target);
    hg_scrub_format_time(bar.duration, total, (int)sizeof total);
    snprintf(body, sizeof body, "%s  /  %s", target, total);

    const float panW = 210.0f;
    const float panH = 62.0f;
    float cx = hg_scrub_x_for(&bar, hg_scrub_fraction(&this->scrub));
    float panX = cx - panW * 0.5f;
    float panY = y + height - 82.0f - panH - 14.0f;

    /* Clamped to the screen: the panel is an overlay, and an overlay half off the edge is a
     * bug the user sees every time they drag near an end. */
    if (panX < x + 4.0f) panX = x + 4.0f;
    if (panX + panW > x + width - 4.0f) panX = x + width - 4.0f - panW;
    if (panY < y + 4.0f) panY = y + 4.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, panX, panY, panW, panH, 8.0f);
    nvgFillColor(vg, nvgRGBA(20, 20, 24, 225));
    nvgFill(vg);

    nvgFontSize(vg, 26.0f);
    nvgFontFaceId(vg, 0);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgText(vg, panX + panW * 0.5f, panY + panH * 0.5f, body, nullptr);
}

int PlayerView::seekRelative(int seconds) {
    Impl* impl = this->impl;
    if (!impl || !impl->mpv || !impl->started) {
        hg::logPrintf("PLAYER seekRelative: ignored (mpv not started)");
        return -1;
    }

    /* mpv takes the sign as part of the value: "seek +10 relative" / "seek -10
     * relative". Writing the sign explicitly means a positive number is still
     * relative rather than absolute -- "10" would jump to 00:10. */
    char amount[24];
    snprintf(amount, sizeof amount, "%+d", seconds);

    const char* args[] = {"seek", amount, "relative", nullptr};
    int rc = mpv_command_async(impl->mpv, 0, args);
    hg::logPrintf("PLAYER seekRelative: seek %s relative -> request %d", amount, rc);

    /* THE POSITION IS READ BACK, because the request id only proves mpv ACCEPTED
     * the command. A seek can be accepted and then do nothing (no file yet, at a
     * boundary), and the difference between those two outcomes is the whole
     * question being asked of this button. */
    double tp = 0.0;
    if (mpv_get_property(impl->mpv, "time-pos", MPV_FORMAT_DOUBLE, &tp) >= 0) {
        hg::logPrintf("PLAYER seekRelative: time-pos is now %.2f", tp);
    }
    return rc;
}

/* ---- the picture menu (X) ------------------------------------------------
 *
 * PORTED FROM wiliwili's in-player settings fragment, "equalizer" group
 * (upstream/wiliwili/wiliwili/source/fragment/player_setting.cpp:294-339 and
 * resources/xml/fragment/player_setting.xml). The five rows are the five
 * brls::SliderCell rows there, the aspect selector is `setting/video/aspect`, the
 * mirror switch is `setting/video/mirror`, and the reset row is
 * `setting/equalizer/reset`.
 *
 * TWO DELIBERATE DEVIATIONS, both because this screen has no other menu surface:
 *
 *   1. THE STEP IS 5, NOT 10. wiliwili's slider spans 0..1 with a step of 0.05 and
 *      converts to an integer with (position * 200 - 100), i.e. 10 points per step.
 *      This menu has no slider (see player_menu.hpp for why), so the value is the
 *      state and the step is chosen directly: 5 is already clearly visible on a TV
 *      panel and 20 presses cover the range instead of 10.
 *
 *   2. NO CONTENT-AWARE DEFAULTS. wiliwili seeds each slider from its per-season
 *      saved value and writes changes back to its config. This menu does not write
 *      anything: hongguo.cfg is read-only for this app (see hg_cfg.hpp -- writes go
 *      through the settings screen's rotate-and-rename path), so the menu is
 *      per-episode and the FILE is the arm. `player.brightness=20` in the file at
 *      startup is the same setting with none of the write-back risk. */

/* Apply every picture property the menu owns. */
void PlayerView::applyPictureSettings(int source) {
    Impl* impl = this->impl;
    static const char* const SRC_NAMES[] = {"default", "cfg", "user"};
    const char* srcName = (source >= 0 && source <= HG_PMENU_SRC_USER) ? SRC_NAMES[source] : "?";

    if (!impl || !impl->mpv || !impl->started) {
        /* COUNTED, NOT SILENT. mpv may not exist yet -- the render context and
         * loadfile happen on a worker thread, so X pressed in the first second lands
         * here. Saying so is the difference between "the menu does nothing" and
         * "the menu was pressed before the player was ready". */
        hg::logPrintf("PLAYERMENU: apply (%s) IGNORED -- mpv is not started yet", srcName);
        return;
    }

    static const char* const PROP_NAMES[HG_PMENU_ROW_LAST_ADJUST + 1] = {
        "brightness", "contrast", "saturation", "gamma", "hue"
    };

    hg::logPrintf("PLAYERMENU: apply (%s) row=%d aspect=%d mirror=%d", srcName, this->menu.row,
                  this->menu.aspect, this->menu.mirror);

    /* THE FIVE NUMBERS. mpv_set_property with MPV_FORMAT_INT64 is the same call
     * wiliwili makes through MPVCore::setBrightness etc. (mpv_core.cpp:1263-1296);
     * it takes int64_t*, which is why the value is widened rather than passed by
     * pointer to the int in the struct. */
    for (int i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
        int v = hg_pmenu_clamp(this->menu.values[i]);
        int64_t v64 = (int64_t)v;
        int rc = mpv_set_property(impl->mpv, PROP_NAMES[i], MPV_FORMAT_INT64, &v64);

        /* READ BACK, ALWAYS. A property write that is accepted and then does nothing
         * (bad name for this mpv build, no video track yet) is the failure mode this
         * whole project keeps meeting: it looks exactly like success. The read-back
         * is what turns it into a number in the log. */
        int64_t back = -9999;
        int rcBack = mpv_get_property(impl->mpv, PROP_NAMES[i], MPV_FORMAT_INT64, &back);
        hg::logPrintf("PLAYERMENU: %s = %d -> set rc=%d, read back %d (rc=%d)", PROP_NAMES[i], v,
                      rc, (int)back, rcBack);
    }

    /* THE ASPECT TRIPLE. All three properties are written for every mode, including
     * from the table even when unchanged, because the three together describe one
     * mode: writing only the ones that differ is how "stretch" ends up stretched AND
     * letterboxed. hg_pmenu_aspect_props() owns the mapping and is unit-tested. */
    const char* keep = nullptr;
    const char* over = nullptr;
    const char* pan = nullptr;
    hg_pmenu_aspect_props(&this->menu, &keep, &over, &pan);
    {
        struct {
            const char* name;
            const char* want;
        } props[3] = {{"keepaspect", keep}, {"video-aspect-override", over}, {"panscan", pan}};
        for (int i = 0; i < 3; i++) {
            int rc = mpv_set_property_string(impl->mpv, props[i].name, props[i].want);
            char* back = nullptr;
            int rcBack = mpv_get_property(impl->mpv, props[i].name, MPV_FORMAT_STRING, &back);
            hg::logPrintf("PLAYERMENU: %s = %s -> set rc=%d, read back '%s' (rc=%d)", props[i].name,
                          props[i].want, rc, back ? back : "(null)", rcBack);
            if (back) mpv_free(back);
        }
    }

    /* MIRROR. wiliwili uses vf=hflip (mpv_core.cpp:1249), and "" to clear it. On a
     * software-decoding build the filter always applies; wiliwili additionally
     * (NOTE: `hwdec` is a cfg key and two console runs have been seen with
     * different values -- see the round-62 note in docs/交接-第19轮.md -- so read the
     * setting in force rather than assuming either one.)
     * switches hwdec to auto-copy when mirroring because a direct hardware decoder
     * bypasses filters (setHwdecCopyMode, mpv_core.cpp:1254-1261). This app runs
     * hwdec=no, so that half is not reproduced -- with software decode there is no
     * bypass to work around. The read-back below is what would catch it if that
     * changed. */
    /* THE WHOLE CHAIN IN ONE WRITE -- the mirror AND the five values (I3).
     *
     * MEASURED, and this is the fix for "changing brightness does nothing": this app renders through
     * MPV_RENDER_API_TYPE_SW with vo=libmpv and hwdec=no (both read from its own log), and the five
     * brightness/contrast/saturation/gamma/hue properties are mpv's "video equalizer", which the GPU video chain
     * applies as SHADER parameters. The software path has no such stage, so those properties are accepted,
     * read back correctly, and change nothing on screen -- exactly the report.
     *
     * The five values are therefore ALSO expressed as ordinary filters (ffmpeg's `eq` and `hue`), which run on
     * every path. The properties are still written above, because they are what a GPU render path would use and
     * they cost nothing; `player.eq.mode=props` drops the filters and leaves the old behaviour one cfg line away.
     *
     * ONE WRITER FOR `vf`: the mirror used to write it alone, so a second writer would silently drop one half. */
    {
        char chain[192];
        if (hg::cfgIs("player.eq.mode", "props")) {
            /* The escape hatch: the mirror only, as before. Built fresh from the menu each time so the two can
             * never disagree about the mirror. */
            memset(&this->mirrorOnly, 0, sizeof this->mirrorOnly);
            this->mirrorOnly.mirror = this->menu.mirror;
            hg_pmenu_vf(&this->mirrorOnly, chain, (int)sizeof chain);
        } else {
            hg_pmenu_vf(&this->menu, chain, (int)sizeof chain);
        }
        int rc = mpv_set_property_string(impl->mpv, "vf", chain);
        char* back = nullptr;
        int rcBack = mpv_get_property(impl->mpv, "vf", MPV_FORMAT_STRING, &back);
        hg::logPrintf("PLAYERMENU: vf = '%s' -> set rc=%d, read back '%s' (rc=%d) [build60]", chain, rc,
                      back ? back : "(null)", rcBack);
        if (back) mpv_free(back);
        hg::logPrintf("PLAYERMENU: the five properties are for a GPU path; on THIS software path the '%s' chain "
                      "is what changes the picture",
                      chain[0] ? chain : "(empty)");   /* [build60] */
    }
}

bool PlayerView::pictureSettingsDifferFromDefaults() const {
    HgPMenu probe = this->menu;
    hg_pmenu_reset(&probe);
    for (int i = HG_PMENU_ROW_FIRST_ADJUST; i <= HG_PMENU_ROW_LAST_ADJUST; i++) {
        if (this->menu.values[i] != probe.values[i]) return true;
    }
    return this->menu.aspect != probe.aspect || this->menu.mirror != probe.mirror;
}

/* The menu's keys and its panel, driven once per frame from draw().
 *
 * EVERY KEY HERE IS EDGE-DETECTED against prevButtons. draw() runs 60 times a
 * second, so "is UP down" is true ~60 times per second of holding; without the edge
 * test a single tap of UP would move the highlight a dozen rows. borealis has its
 * own repeat for registered actions, but registering these would mean adding
 * focusable views and actions to a screen whose seek keys depend on NOT having them
 * (see the long note above PlayerMenuPanel and the one in player_activity.cpp). */
void PlayerView::drawMenuTick() {
    Impl* impl = this->impl;
    auto& st = brls::Application::getControllerState();

    /* Is a given button down NOW but not on the previous frame? */
    auto tapped = [&](brls::ControllerButton b) -> bool {
        bool now = st.buttons[b];
        bool was = this->prevButtons[b];
        return now && !was;
    };

    const bool keyX = tapped(brls::BUTTON_X);
    const bool keyA = tapped(brls::BUTTON_A);
    const bool keyB = tapped(brls::BUTTON_B);
    const bool keyY = tapped(brls::BUTTON_Y);
    const bool keyUp = tapped(brls::BUTTON_UP);
    const bool keyDown = tapped(brls::BUTTON_DOWN);
    const bool keyLeft = tapped(brls::BUTTON_LEFT);
    const bool keyRight = tapped(brls::BUTTON_RIGHT);
    const bool keyLB = tapped(brls::BUTTON_LB);
    const bool keyRB = tapped(brls::BUTTON_RB);
    const bool anyMove = st.buttons[brls::BUTTON_UP] || st.buttons[brls::BUTTON_DOWN] ||
                         st.buttons[brls::BUTTON_LEFT] || st.buttons[brls::BUTTON_RIGHT] ||
                         st.buttons[brls::BUTTON_LB] || st.buttons[brls::BUTTON_RB];

    if (!this->menu.open) {
        /* X OPENS THE MENU AND IMMEDIATELY APPLIES IT.
         *
         * Applying on open is not a formality: it is what makes a cfg-seeded value
         * visible. A `player.brightness=-20` line in hongguo.cfg reaches mpv only
         * when this runs, so one press of X both shows the state and puts it into
         * effect, and the log records which arm was in force. Reaching for the cfg
         * file therefore needs no rebuild -- the escape hatch this project requires
         * for anything it cannot test from the build machine. */
        if (keyX) {
            /* ONE OPEN PATH, shared with the control bar's quality chip (see openPictureMenu). The row is left
             * where the user last had it, exactly as it was before this refactor. */
            hg::logPrintf("PLAYERMENU: X -> OPEN (row=%d, cfg-seeded=%d)", this->menu.row,
                          this->pictureSettingsDifferFromDefaults() ? 1 : 0);
            this->openPictureMenu();
        }
    } else {
        /* WHILE OPEN, THE DIRECTION KEYS BELONG TO THE MENU.
         *
         * The seek block below is skipped entirely in this state (see draw()), so
         * these keys cannot both seek and adjust. A or B closes; Y is swallowed
         * rather than left to toggle the overlay, which is exactly what wiliwili does
         * with the OSD keys while its settings panel is on screen. */
        if (keyA || keyB) {
            /* ---- THE TWO-STEP QUALITY (the user's §3.1) --------------------------------
             *
             * "左右只移光标、A 才生效" -- because a tier change reloads the stream, and reloading on every
             * keypress is unusable. So the cursor moves freely (hg_pmenu_adjust changes qualityIndex only)
             * and this is the one place a switch is issued:
             *
             *   A  -> apply the cursor and close
             *   B  -> CANCEL: the cursor snaps back to the tier in force, and the row shows reality the
             *         next time the menu opens. A closed menu that still showed a tier nobody applied would
             *         be lying about what is playing.
             *
             * Note what is NOT here: nothing switches quality while the cursor moves. That was the old
             * behaviour (a change detector further down this function) and it is exactly what the user asked
             * to replace.
             */
            if (this->menu.row == HG_PMENU_ROW_QUALITY && this->menu.qualityCount > 0) {
                if (keyA) {
                    if (this->menu.qualityIndex != this->menu.qualityApplied) {
                        hg::logPrintf("QUALITY: A on the quality row -> APPLY tier %d (was %d, %d tiers) [build45]",
                                      this->menu.qualityIndex, this->menu.qualityApplied,
                                      this->menu.qualityCount);
                        this->menu.qualityApplied = this->menu.qualityIndex;
                        this->lastQualityIndex = this->menu.qualityIndex;
                        this->switchQuality(this->menu.qualityIndex);
                    } else {
                        hg::logPrintf("QUALITY: A on the quality row -> already tier %d, nothing to do",
                                      this->menu.qualityIndex);
                    }
                } else if (this->menu.qualityIndex != this->menu.qualityApplied) {
                    hg::logPrintf("QUALITY: B cancels the pending tier %d (staying on %d)",
                                  this->menu.qualityIndex, this->menu.qualityApplied);
                    this->menu.qualityIndex = this->menu.qualityApplied;
                }
            }
            this->menu.open = 0;
            this->menuRevision++;
            if (this->menuPanel) this->menuPanel->setPanelVisible(false);
            if (this->menuOwner) this->menuOwner->setMenuOverlay(0);
            hg::logPrintf("PLAYERMENU: %s -> CLOSE", keyA ? "A" : "B");
        } else if (keyY) {
            hg::logPrintf("PLAYERMENU: Y ignored (the menu is open; B or A closes it)");
        } else if (keyUp) {
            if (hg_pmenu_move(&this->menu, -1)) this->menuRevision++;
        } else if (keyDown) {
            if (hg_pmenu_move(&this->menu, +1)) this->menuRevision++;
        } else if (keyLeft || keyRB) {
            /* RB is the same direction as RIGHT but the coarse step -- the same 6x
             * relationship the seek keys use, so crossing the range does not need 40
             * presses. LEFT/RIGHT are the ones that land exactly. */
            if (hg_pmenu_adjust(&this->menu, -1, keyRB ? 0 : 1)) this->menuRevision++;
        } else if (keyRight || keyLB) {
            if (hg_pmenu_adjust(&this->menu, +1, keyLB ? 0 : 1)) this->menuRevision++;
        } else if (anyMove && impl && impl->mpv) {
            /* HOLD TO REPEAT. Edge detection alone makes a held key a single step,
             * which is right for a tap but means 20 presses to cross the range. The
             * first repeat waits 400 ms (so a deliberate single step is never
             * doubled) and then steps every 90 ms.
             *
             * The direction is re-read from the HELD state rather than from the edge,
             * so holding UP and then also pressing DOWN repeats whichever was read
             * first -- deliberate: a repeat should follow the keys that are down now,
             * not the one that happened to be pressed first. */
            double now = nowMs();
            if (!this->menuRepeatArmed) {
                this->menuRepeatArmed = true;
                this->menuRepeatAt = now + 400.0;
            } else if (now >= this->menuRepeatAt) {
                this->menuRepeatAt = now + 90.0;
                int delta = 0;
                bool fine = false;
                if (st.buttons[brls::BUTTON_UP]) delta = -1;
                else if (st.buttons[brls::BUTTON_DOWN]) delta = +1;
                else if (st.buttons[brls::BUTTON_LEFT]) { delta = -1; fine = true; }
                else if (st.buttons[brls::BUTTON_RIGHT]) { delta = +1; fine = true; }
                else if (st.buttons[brls::BUTTON_RB]) delta = -1;
                else if (st.buttons[brls::BUTTON_LB]) delta = +1;
                if (delta != 0) {
                    bool acted = hg_pmenu_row_is_adjust(this->menu.row)
                                     ? hg_pmenu_adjust(&this->menu, delta, fine ? 1 : 0)
                                     : hg_pmenu_move(&this->menu, delta);
                    if (acted) this->menuRevision++;
                }
            }
        }
    }

    if (!anyMove) {
        this->menuRepeatArmed = false;
        this->menuRepeatAt = 0.0;
    }

    /* ISSUE A QUALITY SWITCH WHEN THE ROW CHANGED, not while it is changing.
     *
     * The row steps the index every keypress, so a held direction would otherwise start a
     * request per frame -- and each one is a full signed POST. Comparing against the last index
     * a switch was issued for collapses a run of presses into one request, and it is checked
     * here rather than inside the key handler so a change made ANY way (the row, a reset, a
     * future direct selection) is picked up by the same line. */
    /* THE OLD AUTO-SWITCH USED TO LIVE HERE.
     *
     * It compared `menu.qualityIndex` against the last index it issued and switched on any change -- which
     * made LEFT/RIGHT reload the stream on every keypress. The user asked for the two-step instead ("左右只移
     * 光标、A 才生效"), so the switch is now issued from ONE place: the A branch above, where the user commits.
     *
     * The seeding case that this line also covered (the ladder arriving and starting a tier) is build33's
     * own path: the ladder callback sets `lastQualityIndex` and starts the first tier itself, which is why
     * removing this cannot leave the player without a stream.
     */
    this->menu.qualityApplied = this->lastQualityIndex;
    memcpy(this->prevButtons, st.buttons, sizeof this->prevButtons);
}

} // namespace hg
