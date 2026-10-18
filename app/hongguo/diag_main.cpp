/*
 * diag_main: a staged boot entry point that answers "which stage dies?" in ONE
 * console run.
 *
 * GPL-3.0-only.
 *
 * WHY STAGES. The shipping app crashed before its first frame. The previous
 * session's plan was to bisect that with one console trip per hypothesis
 * (remove the worker thread; if it still crashes, simplify the views). That
 * costs a trip per answer and each answer is only yes/no.
 *
 * This does better with the same single trip, by splitting the startup into
 * stages that are *externally* distinguishable and logging each one:
 *
 *   stage 0  borealis init / createWindow / globalInit   (hg_boot.cpp)
 *   stage A  a bare Label activity, held for 60 frames
 *   stage B  the real HomeActivity, pushed on top of stage A
 *
 * Stage A is the control the shipping app never had. It exercises the whole
 * link graph of the real app -- mpv, ffmpeg, curl, deko3d, borealis, the same
 * object files, the same static initialisers -- and nothing else. So:
 *
 *   dies in stage A  -> the fault is in the link graph or early borealis, NOT
 *                       in HomeActivity. Removing the fetch thread would have
 *                       been wasted work.
 *   survives A, dies in B -> the fault is in HomeActivity or what it builds,
 *                       and stage A's survival rules out everything else.
 *
 * Both cases also print the last completed step, so the answer does not depend
 * on the stages alone.
 *
 * Built with: scripts\build-hongguo.ps1 -Diag
 * Log:        sdmc:/switch/hongguo.log  (pull over FTP; survives a hard crash)
 * Tuning:     sdmc:/switch/hongguo.cfg  containing "fetch=sync" makes
 *             HomeActivity fetch on the main thread instead of a std::thread,
 *             which is the other bisect variable -- changeable by FTP, with no
 *             rebuild and no second NRO.
 *
 * ASCII-only source file.
 */

#include <borealis.hpp>

#include <cstdlib>
#include <cstring>

#include "hg_boot.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"
#include "home_activity.hpp"

/* Frames to hold stage A before pushing the real activity. 60 is one second at
 * borealis' 60 Hz: long enough to cover a slow first layout, short enough that
 * a human watching the screen sees both stages. */
static const int STAGE_A_FRAMES = 60;

int main(int argc, char* argv[]) {
    hg::logOpen("diag");
    hg::installCrashRecorder();
    hg::logPrintf("ENTER main (diag, staged)");

    if (!hg::bootBorealis(argc, argv)) {
        hg::logPrintf("boot failed; exiting without entering the main loop");
        hg::logClose();
        return EXIT_FAILURE;
    }

    /* ---- stage 0: the thread experiment ----------------------------------
     * The crash this build exists to locate was std::thread throwing ENOSYS.
     * That is re-confirmed here, in the same binary, by trying both thread APIs
     * and recording the numeric result of each -- so the diagnosis is measured
     * on whatever console runs it rather than carried over on trust from a
     * previous session. */
    hg::threadSelfTest();

    /* The console screenshot of the previous run showed dark text on a light
     * background, which is the LIGHT theme's palette, even though
     * setThemeVariant(DARK) had been called and the system theme logged as
     * "Dark". Recording what the theme actually reports costs one line and
     * removes the guess. */
    {
        NVGcolor bg = brls::Application::getTheme().getColor("brls/background");
        NVGcolor tx = brls::Application::getTheme().getColor("brls/text");
        hg::logPrintf("theme: light=%d background=%.0f,%.0f,%.0f text=%.0f,%.0f,%.0f",
                      (int)(brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT),
                      bg.r * 255.0, bg.g * 255.0, bg.b * 255.0, tx.r * 255.0, tx.g * 255.0,
                      tx.b * 255.0);
    }

    /* ---- stage A: bare borealis label, held for STAGE_A_FRAMES ------------
     * Built with explicit setWidth/setHeight and no custom cell types, so that
     * if this stage dies the cause cannot be a recycler or a font metric: it is
     * the smallest borealis screen that is still the real app's binary. */
    hg::logPrintf("ENTER stage A: build bare label activity");
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(24.0f);

    auto* t1 = new brls::Label();
    t1->setFontSize(30.0f);
    t1->setText("hongguo DIAG  --  STAGE A");
    t1->setMarginBottom(16.0f);
    box->addView(t1);

    auto* t2 = new brls::Label();
    t2->setFontSize(20.0f);
    t2->setText("If you can read this, this app's WHOLE link graph starts: borealis, deko3d,"
                " mpv, ffmpeg, curl, static initialisers, init, createWindow, globalInit,"
                " pushActivity and mainLoop all work.");
    box->addView(t2);

    auto* t3 = new brls::Label();
    t3->setFontSize(20.0f);
    t3->setMarginTop(14.0f);
    t3->setText("STAGE B (the real home page) starts one second from now.");
    box->addView(t3);

    auto* t4 = new brls::Label();
    t4->setFontSize(18.0f);
    t4->setMarginTop(14.0f);
    t4->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    t4->setText("A log is being written to sdmc:/switch/hongguo.log -- the last line names"
                " the step that did not finish.");
    box->addView(t4);

    hg::logPrintf("ENTER stage A: pushActivity");
    brls::Application::pushActivity(new brls::Activity(box));
    hg::logPrintf("OK    stage A: pushActivity -- waiting %d frames", STAGE_A_FRAMES);

    bool stageBPushed = false;
    int frames = 0;
    hg::logPrintf("ENTER mainLoop");
    while (brls::Application::mainLoop()) {
        frames++;
        /* Dense for the first two seconds, then sparse. Each line is fsync'd,
         * and the question this log exists to answer is "which frame died", so
         * a coarse heartbeat would defeat it: the previous run's log ended at
         * "pushActivity returned" and could not tell frame 61 from frame 500. */
        if (frames <= 120 || frames == 180 || frames == 300 || (frames % 600) == 0) {
            hg::logPrintf("frame %d (stageB=%d)", frames, (int)stageBPushed);
        }

        if (!stageBPushed && frames == STAGE_A_FRAMES) {
            stageBPushed = true;
            hg::logPrintf("OK    stage A survived %d frames", frames);
            hg::logPrintf("ENTER stage B: pushActivity(new HomeActivity)");
            brls::Application::pushActivity(new hg::HomeActivity());
            hg::logPrintf("OK    stage B: pushActivity returned");
        }
    }
    hg::logPrintf("OK    mainLoop ended after %d frames (stageB pushed=%d)", frames, (int)stageBPushed);

    hg::logClose();
    return EXIT_SUCCESS;
}
