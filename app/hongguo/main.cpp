/*
 * hongguo: a Nintendo Switch client for 红果短剧.
 *
 * GPL-3.0-only.
 *
 * WHAT THIS APP IS. The data and playback layer underneath it is already
 * verified on real hardware (see docs/让程序可用-步骤.md, "全链路已在真机跑通"):
 * the home page yields 36 cards, the detail page yields episode ids, the
 * signed video-model POST returns a playable URL with a CENC key, and mpv
 * decodes frames with that key. This file adds only the shell: three
 * activities (home, detail, player) and the gamepad mapping.
 *
 * STARTUP. The sequence itself lives in hg_boot.cpp, because the diagnostic
 * entry point (diag_main.cpp, built with -Diag) must run the SAME sequence --
 * a diagnostic build that starts differently tests a different program.
 *
 * BUTTON MAPPING:
 *   A  - open detail (on home) / play the episode (on detail)
 *   B  - go back (borealis' built-in activity-stack pop)
 *   +  - open the settings screen, ON THE HOME SCREEN ONLY (build167)
 * A and B are borealis defaults. + used to mean "exit", which CRASHED the console -- see the note on
 * setGlobalQuit(false) in hg_boot.cpp for the two crash reports that name the collision. There is no
 * in-app exit any more, at the user's instruction: the console's own Home menu closes the title.
 *
 * DIAGNOSTICS. Every step writes a line to sdmc:/switch/hongguo.log and to
 * stdout, flushed immediately, so a crash leaves the name of the step that
 * killed it rather than an error dialog and nothing else. See hg_log.hpp.
 *
 * ASCII-only source file.
 */

#include <borealis.hpp>
#include <switch.h>

#include <cstdlib>

#include "hg_boot.hpp"
#include "hg_applet.hpp"   /* appletKeepAwakeTick: the once-a-second keep-awake (build172) */
#include "hg_log.hpp"
#include "hg_thread.hpp"   /* waitForHttpIdle: the drain before this process exits (build168) */
#include "home_activity.hpp"

int main(int argc, char* argv[]) {
    hg::logOpen("app");
    hg::installCrashRecorder();
    hg::logPrintf("ENTER main (shipping)");

    if (!hg::bootBorealis(argc, argv)) {
        hg::logPrintf("boot failed; exiting without entering the main loop");
        hg::logClose();
        return EXIT_FAILURE;
    }

    /* The activity is pushed BEFORE the loop starts, exactly as before. The
     * breadcrumbs on either side separate "pushActivity threw" from "the first
     * frame threw", which are different bugs with the same symptom. */
    hg::logPrintf("ENTER pushActivity(new HomeActivity)");
    brls::Application::pushActivity(new hg::HomeActivity());
    hg::logPrintf("OK    pushActivity(new HomeActivity)");

    int frames = 0;
    hg::logPrintf("ENTER mainLoop");
    while (brls::Application::mainLoop()) {
        /* ---- THE CONSOLE MUST NOT SLEEP WHILE THIS APP IS OPEN (build172) ---------------------------
         *
         * The user: "主机模式下还是有睡眠状态". build159 disabled auto-sleep once at boot, build170
         * re-asserts it when the operation mode changes, and the console still slept in TV mode -- so
         * build172 adds a tick. See hg_applet.hpp for why it does real work once a SECOND rather than once
         * a frame (per frame would be one IPC request per frame to the applet service that appletMainLoop
         * polls), and why it also calls appletReportUserIsActive: that call's own documentation covers the
         * DIMMING which comes before the sleep, and "dims, then sleeps a few minutes later" is exactly the
         * sequence the user described. */
        hg::appletKeepAwakeTick();
        frames++;
        /* One line a second, plus the first few frames. The previous version
         * logged every frame for two seconds, which made a two-minute run 627
         * lines of mostly "frame N"; one per second still pins down when a run
         * stopped, which is the only thing the heartbeat is for.
         *
         * ---- MY SWITCH AROUND THIS LINE IS REMOVED (build153, the user's instruction) --------------------
         *
         * build149 put a cfg gate here (`log.playback`) so the heartbeat could be silenced; build152 made it
         * default-on again. The user asked directly: "you added this to main.cpp, where it was not before -- is
         * that the problem?" It should not be (a cfg read cannot make the server answer without a play URL), but
         * their method is right -- one change at a time -- and this one has no value that is worth arguing about:
         * the line is back to exactly what it was, and the cfg key is gone from this file.
         *
         * If silencing the log is worth revisiting, it belongs next to the logger (hg_log.cpp), not here, and it
         * should silence the WRITES rather than each call site. */
        if (frames <= 10 || (frames % 60) == 0) {
            hg::logPrintf("frame %d", frames);
        }
    }
    appletSetAutoSleepDisabled(false);
    hg::logPrintf("OK    mainLoop ended after %d frames", frames);

    /* ---- NOTHING MAY BE IN FLIGHT WHEN THIS PROCESS TEARS libnx DOWN (build168) ---------------------
     *
     * THE CRASH THIS EXISTS FOR. Pressing + used to exit, and it crashed the console every time. Two
     * Atmosphere reports from 2026-10-14 (Data Abort / 2168-0002, fault address 0) have the same stack,
     * and it names both halves of the collision:
     *
     *     exit -> __syscall_exit -> serviceClose -> _bsdDispatchImpl            (libnx bsd.c:133)
     *     ...on the other thread:
     *     curl_easy_perform -> Curl_readwrite -> Curl_poll -> poll()
     *                       -> _socketParseBsdResult -> _socketGetFd            (libnx socket.c:295)
     *
     * A cover fetch was still running when the socket service went away, so poll() used a socket that no
     * longer existed. build167 took the exit BUTTON away, which is what made it easy to reach; the race
     * itself is HERE, at the top of every exit path -- and the console's own HOME menu goes through the
     * same line, which is why the user still hit it after build167.
     *
     * SO THE PROCESS WAITS. waitForHttpIdle polls the transport's own in-flight count and returns once
     * it is zero, or after 3 seconds (see hg_thread.hpp for why it must be bounded: a fetch stuck on a
     * dead CDN must not turn "close the app" into "the app never closes"). It logs what it waited, so
     * the next log says whether this was ever needed, and how long.
     *
     * WHAT THIS DOES NOT COVER YET: the history store's background writer has the same shape -- it is
     * still running when main returns -- and it is the other half of the handover's item 2. One worker
     * at a time: this one is the one the crash reports name. */
    hg::waitForHttpIdle(3000);

    /* What happens after this line is no longer under this program's control in
     * any obvious way: logClose() hands borealis' logger back to stdout, this
     * function returns, libnx runs static destructors, userAppExit() frees the
     * services, and the loader unloads the NRO. A complaint of the form
     * "+ exits and then the console shows an error" is answered by knowing how
     * far this side got. */
    hg::logPrintf("ENTER logClose() and return from main; libnx userAppExit follows");
    hg::logClose();
    hg::logPrintf("ENTER return EXIT_SUCCESS");

    return EXIT_SUCCESS;
}
