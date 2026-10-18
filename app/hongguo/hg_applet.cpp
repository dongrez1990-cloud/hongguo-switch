/*
 * hg_applet implementation. See hg_applet.hpp for why this module exists at all.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_applet.hpp"

#include <borealis.hpp>
#include <switch.h>

#include <atomic>

#include "hg_log.hpp"

namespace hg {

/* ---- WHY THE CALLBACK ONLY SETS FLAGS ------------------------------------
 *
 * libnx dispatches applet hooks from ITS OWN thread, and it can do so at any
 * instant -- including while the main thread is inside mpv, inside nanovg, or
 * inside a view's draw(). borealis is explicitly not thread-safe (thread.hpp:49
 * "Borealis is not thread-safe, and sync() provides a mechanism ..."), and this
 * project has already lost time to exactly this class of mistake: the build167
 * crash was a cover fetch still running when something else tore the process
 * down underneath it.
 *
 * So the callback does the two things that are safe from ANY thread --
 * an atomic store and a std::atomic-guarded log write -- and hands everything
 * else to the main thread through brls::sync, which is a queue rather than a
 * thread: Threading::sync pushes a std::function under a mutex (thread.cpp:67)
 * and Application::mainLoop drains it via performSyncTasks once per frame
 * (application.cpp:204). That is the hop this project already uses for its
 * play-plan and ladder workers -- NAMED RATHER THAN NUMBERED ON PURPOSE: this
 * note used to cite "player_view.cpp:1616, 2229" for them, and by build171 those
 * line numbers pointed at prose. The claim was true and the pointer was stale,
 * which is the worst combination for the next reader. To find them, search for
 * the callers of Client::fetchPlayPlan and Client::fetchQualityLadder.
 *
 * Deliberately NOT done in the callback, and each one is a way to crash:
 *   - touching brls::Application window state or any View (borealis not thread-safe);
 *   - calling mpv (its own locking, but the render context is owned by the view);
 *   - destroying or recreating anything (see the note on renderCtx below).
 */

/* The mode the last hook reported. -2 means "no hook has fired yet", which is a
 * different fact from "handheld" and is why the sentinel is not 0. */
static std::atomic<int> g_lastMode{-2};
static std::atomic<unsigned> g_modeChanges{0};
static std::atomic<bool> g_started{false};

/* Kept in a file-scope static because libnx stores the cookie, not us, and it
 * must outlive the call that registers it. appletHook copies the function
 * pointer and the arg out of it; nothing else reads this afterwards. */
static AppletHookCookie g_hookCookie;

/* ---- THE TWO THINGS THAT HAVE TO HAPPEN ON THE MAIN THREAD ----------------
 *
 * (1) RE-ASSERT "DO NOT SLEEP".  Reported: "the console still goes to sleep in TV mode." The boot line
 *     proves the call worked at startup -- build168's log has
 *     "boot: auto-sleep disabled for playback (rc=00000000)" -- so the failure is not the call, it is the
 *     DURATION of the flag: the operation-mode transition re-initialises the applet's display/tv state, and
 *     the auto-sleep disable is believed to be dropped with it. Re-asserting it here, in the same event that
 *     changes the mode, is the fix that matches that evidence -- and the rc is logged so the NEXT run says
 *     whether the call succeeded rather than leaving it to be assumed.
 *
 * (2) NOTHING ABOUT THE PICTURE. The frame is fixed in player_view.cpp, on the draw path, because that is
 *     where the wrong number actually lives (a frozen mpv FBO size, not a frozen window). Doing it here as
 *     well would be a second, competing writer of the same state -- and the ordering between this queue and
 *     that draw is not defined, so the two would occasionally disagree. One owner, one fix.
 *
 * WHATEVER THIS FUNCTION DOES, IT MUST BE CHEAP AND MUST NOT REALLOCATE. It runs inside the frame loop, in
 * the same frame in which borealis has just torn down and recreated the swapchain for the new resolution. */
static void appletOnMainThread(void) {
    /* The rc is the whole reason this block is worth a log line: without it, a console that still sleeps
     * leaves "did the call fail, or did it succeed and get ignored" unanswerable -- which is precisely the
     * state build168 was in. */
    const ::Result rc = appletSetAutoSleepDisabled(true);
    hg::logPrintf("APPLET: re-asserted auto-sleep disabled after the mode change (rc=%08X) [build170]",
                  (unsigned)rc);
}

static void onAppletHook(AppletHookType hook, void* arg) {
    (void)arg;
    if (hook != AppletHookType_OnOperationMode) return;

    /* ASK THE CONSOLE, DO NOT TRUST THE EVENT ARGUMENT. The hook carries no payload, and `appletGetOperationMode`
     * is a plain libnx query -- so the mode that is logged is the one libnx reports at that instant rather than
     * an inference from which event fired. A mis-inferred direction here would be invisible in the log and
     * would send the next investigation after the wrong thing. */
    const AppletOperationMode mode = appletGetOperationMode();
    const int docked = (mode == AppletOperationMode_Console) ? 1 : 0;

    /* The hook can fire for a mode value this code does not model. `docked` is 0 for anything that is not
     * Console, so an unknown value would be logged as "handheld" -- say so explicitly instead. */
    const char* what = (mode == AppletOperationMode_Console)      ? "Console (docked)"
                       : (mode == AppletOperationMode_Handheld) ? "Handheld"
                                                                : "Unknown";

    g_lastMode.store(docked);
    const unsigned seq = g_modeChanges.fetch_add(1) + 1;

    /* THE LOG WRITE IS SAFE FROM THIS THREAD, BUT NOT BECAUSE OF A LOCK -- hg_log has none, and saying it
     * did would be a comment that lies. What makes it safe is its shape (hg_log.cpp:174): every call builds
     * its line in a LOCAL buffer, opens the file with its own handle, writes, flushes and closes, and uses
     * localtime_r rather than localtime precisely so worker threads cannot race on a shared static. There is
     * no shared state between two concurrent calls except the filesystem, and the plan and ladder workers
     * have logged from their own threads since the beginning. */
    hg::logPrintf("APPLET: operation mode -> %s (appletGetOperationMode=%d, change #%u) [build170]", what,
                  (int)mode, seq);

    /* ---- AND NOW THE PART THAT IS NOT SAFE FROM HERE ------------------------------------------------
     *
     * Everything below runs on the main thread. This is where the small-block-in-the-corner fix is armed and
     * where auto-sleep is re-asserted -- see appletOnMainThread for what is and is not done there.
     *
     * `brls::sync` is called UNCONDITIONALLY rather than guarded by a "did the mode actually change" test:
     * libnx may deliver this hook more than once for one physical dock (a resolution change follows the mode
     * change on the same transition), and a duplicate is harmless here -- the action is idempotent, and the
     * log line above already carries the sequence number that makes a duplicate visible. */
    brls::sync([]() { appletOnMainThread(); });
}

void appletWatchStart(void) {
    if (g_started.exchange(true)) {
        /* Not an error, but not nothing either: a second call means the boot sequence was refactored and
         * someone expects this to happen twice. Say so rather than returning silently. */
        hg::logPrintf("APPLET: appletWatchStart() called again -- the hook is already registered, ignoring");
        return;
    }

    /* THE MODE THE APP STARTED IN IS LOGGED TOO, and it is not padding: the interesting failure is a
     * transition, and a log that only has "handheld -> Console" cannot be told apart from a run that started
     * docked and then docked again. This line is the baseline the change lines are read against. */
    {
        const AppletOperationMode mode = appletGetOperationMode();
        const char* what = (mode == AppletOperationMode_Console)      ? "Console (docked)"
                           : (mode == AppletOperationMode_Handheld) ? "Handheld"
                                                                    : "Unknown";
        g_lastMode.store(mode == AppletOperationMode_Console ? 1 : 0);
        hg::logPrintf("APPLET: watching for operation-mode changes -- starting in %s "
                      "(appletGetOperationMode=%d) [build170]",
                      what, (int)mode);
    }

    /* A SECOND COOKIE, INDEPENDENT OF BOREALIS' OWN. borealis registers its hook with its own static cookie
     * in switch_platform.cpp:31 when the SwitchPlatform is constructed, and forwards the same event to
     * SwitchVideoContext::appletCallback to rebuild the swapchain. libnx registers hooks BY COOKIE
     * (applet.h:2752 `void appletHook(AppletHookCookie*, AppletHookFn, void*)`; appletUnhook takes the same
     * cookie back at applet.h:2758), which is the shape of a LIST of registrations rather than a single slot
     * -- that is what makes a second cookie an addition and not a replacement. libnx ships headers only in
     * this toolchain, so this is read off that interface rather than off the implementation.
     *
     * THIS IS VERIFIABLE ON HARDWARE FOR FREE, which is why the risk is acceptable: the log lines on either
     * side of it report the mode themselves, and borealis' own hook still logs through brls::Logger
     * ("AppletHookType_OnOperationMode" is not printed, but a swapchain rebuild shows up as the window-size
     * line at application.cpp:1193). If a future libnx made hooks exclusive, the missing mode lines would
     * say so immediately.
     *
     * THAT MATTERS BECAUSE THE OTHER HALF IS ALREADY CORRECT. The vendored switch_video.cpp:280 resets the
     * framebuffer on this event -- unless the resolution-change EVENT is ready, in which case it defers to
     * that instead (and beginFrame polls it every frame at switch_video.cpp:233). So the swapchain and the
     * framebuffer are handled by borealis and MUST NOT be rebuilt here as well: a second
     * destroy/recreate of the same deko3d objects in the same transition is exactly the kind of
     * double-teardown that has crashed consoles. This module observes; borealis rebuilds. */
    appletHook(&g_hookCookie, onAppletHook, nullptr);
    hg::logPrintf("OK    appletHook(OnOperationMode) registered [build170]");
}

int appletIsDocked(void) { return appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0; }

unsigned appletModeChangeCount(void) { return g_modeChanges.load(); }

int appletLastReportedMode(void) { return g_lastMode.load(); }

/* ---- KEEP AWAKE, AT MOST ONCE A SECOND (build172) ---------------------------------------------------
 *
 * See hg_applet.hpp for the whole argument: why 1 Hz and not per frame, why the read-back is here, and why
 * appletReportUserIsActive is the call that also covers the dimming the user sees before the sleep. */

/* THE GATE, AND IT IS 60 FRAMES RATHER THAN A CLOCK. The main loop of this app is a fixed 60 Hz render loop
 * (main.cpp counts `frames` and logs one line a second with `frames % 60`), and the thing being defended
 * against is measured in MINUTES, so counting frames is exact enough, needs no time source on the hot path,
 * and cannot be disturbed by a clock that steps. `static` is correct here because this function is
 * main-thread only -- said out loud because a static gate read from two threads would be a bug. */
#define HG_KEEP_AWAKE_FRAMES 60

void appletKeepAwakeTick(void) {
    static int framesSinceCheck = 0;
    static bool announced = false;
    static bool reportWasOk = false;

    if (++framesSinceCheck < HG_KEEP_AWAKE_FRAMES) return;
    framesSinceCheck = 0;

    /* READ FIRST, THEN WRITE: the read is the measurement, and it must run before this function's own write
     * makes it always answer "yes". */
    bool flagHeld = false;
    const Result readRc = appletIsAutoSleepDisabled(&flagHeld);
    const Result setRc = appletSetAutoSleepDisabled(true);
    const Result activeRc = appletReportUserIsActive();
    const bool reportOk = R_SUCCEEDED(activeRc);

    if (!announced) {
        announced = true;
        /* ONE LINE THAT SAYS WHAT THIS BUILD CAN AND CANNOT DO on this console: three return codes, and the
         * state the flag was actually in when the app was already running. It is the baseline every later
         * line is read against, exactly like appletWatchStart's "starting in %s" line. */
        hg::logPrintf("APPLET: keep-awake armed -- IsAutoSleepDisabled rc=%08X flag=%d (before we set it), "
                      "set rc=%08X, ReportUserIsActive rc=%08X; re-checked once a second, not every frame "
                      "[build172]",
                      (unsigned)readRc, flagHeld ? 1 : 0, (unsigned)setRc, (unsigned)activeRc);
        reportWasOk = reportOk;
        return;
    }

    /* ---- THE EVIDENCE LINE, AND IT ONLY APPEARS IF THE HYPOTHESIS IS TRUE ---------------------------
     *
     * If the flag has been dropped since the last check, that is the fact the entire diagnosis rested on and
     * nobody had ever seen. It is logged LOUDLY and with the rc of the repair, so the next log answers "did
     * the mode change clear it" without a new build.
     *
     * AND IF IT NEVER APPEARS, THAT IS ALSO AN ANSWER: the flag is holding, so the sleep in TV mode comes
     * from something else and the next step is to look elsewhere rather than to re-assert harder. */
    if (R_SUCCEEDED(readRc) && !flagHeld) {
        hg::logPrintf("APPLET: auto-sleep had been RE-ENABLED underneath us (the flag was cleared since the "
                      "last check) -- re-asserted, set rc=%08X [build172]",
                      (unsigned)setRc);
    }

    /* A FAILING CALL IS WORTH A LINE EVERY TIME IT CHANGES STATE, because "this console rejects the call"
     * would explain the sleep outright and must not be buried in a once-a-second stream. */
    if (reportOk != reportWasOk) {
        reportWasOk = reportOk;
        hg::logPrintf("APPLET: ReportUserIsActive changed state -> rc=%08X (ok=%d) [build172]",
                      (unsigned)activeRc, reportOk ? 1 : 0);
    }
}

} // namespace hg
