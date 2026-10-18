/*
 * Minimal borealis smoke test for Nintendo Switch.
 *
 * This exists to answer ONE question before any UI is written: does a borealis
 * application LINK and RUN when built with the hand-rolled build driver in
 * scripts/build-borealis.ps1? Everything else about the app is downstream of
 * that. If the link fails on an undefined symbol, the fix belongs here where
 * the surface is small, not inside a three-activity UI.
 *
 * It creates a window with one label. No network, no mpv.
 *
 * ASCII-only source file (system ANSI codepage is GBK/936).
 */

#include <borealis.hpp>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!brls::Application::init()) {
        brls::Logger::error("borealis init failed");
        return 1;
    }

    brls::Application::createWindow("hongguo smoke test");

    /* One label naming what this build is, so a successful boot is
     * distinguishable from a black screen at a glance. */
    brls::Label* label = new brls::Label();
    label->setText("borealis + deko3d: LINK AND BOOT OK");
    label->setFontSize(28.0f);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setMarginTop(120.0f);

    /* borealis pushes ACTIVITIES (a View subclass), not raw views:
     * brls::Application::pushActivity, not pushView. */
    brls::Application::pushActivity(new brls::Activity(label));
    brls::Application::setGlobalQuit(true);

    while (brls::Application::mainLoop())
        ;

    return 0;
}
