/*
 * hg_boot implementation. See hg_boot.hpp.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_boot.hpp"

#include <borealis.hpp>
#include <switch.h>   /* setGetSystemLanguage, SetLanguage_* */

#include <cstring>

#include "hg_client.hpp"
#include "hg_cfg.hpp"
#include "hg_log.hpp"
#include "hg_applet.hpp"   /* the operation-mode hook (build170); see hg_applet.hpp for why it is needed */

extern "C" {
#include "hg_lang.h"
}

namespace hg {

bool bootBorealis(int argc, char** argv) {
    /* Video playback must keep the applet awake.  Without this, the console's
     * inactivity timer can suspend the process while mpv is still decoding;
     * resume then leaves a black framebuffer and audio/video state behind. */
    ::Result sleepRc = appletSetAutoSleepDisabled(true);
    logPrintf("boot: auto-sleep disabled for playback (rc=%08X)", sleepRc);
    /* -d enables borealis' debug logging. hg_log already forces DEBUG into the
     * log file, so this only matters for the stdout mirror. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }
    logPrintf("boot: argc=%d, heap in use %ld KiB at startup [build84]", argc, hgMemoryUsedKB());

    /* State which arm of every configurable experiment this run is on, once,
     * before anything else can go wrong. A log that does not say which values
     * were in force cannot be compared against another log. */
    hg::cfgLogOnce();

    /* ---- UI SCALE: wiliwili's "resolution" setting ---------------------------
     *
     * wiliwili has a setting called APP_UI_SCALE whose options are
     * 544p / 720p / 900p / 1080p, and it does exactly one thing
     * (source/utils/config_helper.cpp:538-554):
     *
     *     if (UIScale == "544p") { ORIGINAL_WINDOW_WIDTH = 960;  ORIGINAL_WINDOW_HEIGHT = 544; }
     *     else if ("720p")  { ... = 1280; ... = 720; }
     *     else if ("900p")  { ... = 1600; ... = 900; }
     *     else if ("1080p") { ... = 1920; ... = 1080; }
     *
     * Those two statics are the LOGICAL coordinate space the UI lays out in. The
     * framebuffer keeps whatever size the console gives us and borealis scales the
     * result, so this is a UI-scale switch rather than a video-resolution one.
     *
     * IT MUST BE SET HERE, BEFORE Application::init(). borealis reads them while
     * sizing the window, so assigning them later changes nothing at all -- and
     * nothing would report that. wiliwili sets it inside ProgramConfig::init(),
     * which for the same reason runs before Application::init(), and its settings
     * screen restarts the app afterwards (DialogHelper::quitApp()).
     *
     * The log line states what was in force, including when nothing was, because
     * a scale change that silently did nothing looks exactly like a scale change
     * that was not read. */
    /* THE INTERFACE LANGUAGE, READ HERE BECAUSE EVERY SCREEN NEEDS IT BEFORE IT IS BUILT.
     *
     * Unlike ui.scale this could in principle be read late, but reading it in the same place keeps
     * one answer to "what did this run configure" in the log -- and the log line states the language
     * in force, which is the thing a "the labels are in the wrong language" report needs.
     *
     * The default is Chinese; `language=en` in hongguo.cfg switches it. A typo is not an error --
     * hg_lang_parse falls back to the default, so a mistyped code leaves a readable interface rather
     * than a silent one, and the line below says which language was actually chosen. */
    {
        std::string lang = cfgValue("language", "");
        int code = hg_lang_parse(lang.c_str());
        if (code == HG_LANG_AUTO) {
            /* THE CONSOLE'S OWN ANSWER, and the fallback is stated rather than implied: this app ships two
             * translations, so anything that is not Chinese resolves to English. A silent fallback would make
             * "why is this in English" unanswerable from the log. */
            /* libnx' entry point is setGetSystemLanguage(u64*), not an int: the value is a SetLanguage_*
             * constant (set.h:40+, and the Chinese ones are SetLanguage_ZHCN/ZHTW). The RAW value is logged,
             * because "the console says 13" is the fact a wrong-language report needs. */
            u64 sysLang = 0;
            /* `::Result` because this file also sees hg::Result (the client's own out-parameter struct). */
            const ::Result rc = setGetSystemLanguage(&sysLang);
            code = (R_SUCCEEDED(rc) && (sysLang == SetLanguage_ZHCN || sysLang == SetLanguage_ZHTW))
                       ? HG_LANG_ZH
                       : HG_LANG_EN;
            logPrintf("boot: language='auto' -> the console reports %d, so %s (this app ships zh/en only)",
                      sysLang, code == HG_LANG_EN ? "English" : "Chinese");
        } else {
            logPrintf("boot: language='%s' -> %s", lang.empty() ? "(default)" : lang.c_str(),
                      code == HG_LANG_EN ? "English" : "Chinese");
        }
        hg_lang_set(code);
    }

    {
        std::string scale = cfgValue("ui.scale", "");
        unsigned w = 0, h = 0;
        if (scale == "544p")       { w = 960;  h = 544;  }
        else if (scale == "720p")  { w = 1280; h = 720;  }
        else if (scale == "900p")  { w = 1600; h = 900;  }
        else if (scale == "1080p") { w = 1920; h = 1080; }
        if (w != 0) {
            brls::Application::ORIGINAL_WINDOW_WIDTH  = w;
            brls::Application::ORIGINAL_WINDOW_HEIGHT = h;
            logPrintf("boot: ui.scale='%s' -> ORIGINAL_WINDOW %ux%u", scale.c_str(), w, h);
        } else {
            if (!scale.empty()) {
                logPrintf("boot: ui.scale='%s' is not one of 544p/720p/900p/1080p -- "
                          "keeping borealis' built-in 1280x720", scale.c_str());
            } else {
                logPrintf("boot: ui.scale unset -- borealis' built-in 1280x720");
            }
        }
    }

    /* ---- THE THREE DISPLAY/INPUT SWITCHES (C8, C12, C13) ---------------------
     *
     * All three are borealis' own calls; this app only decides which cfg key maps to which. They are read here,
     * before init(), because the frame limit has to be in place before the first frame and the input swap before
     * the first key.
     *
     * `app.fps.limit` accepts 0 (no limit), 30, 60 or 120 -- anything else is refused with a line rather than
     * clamped silently, because a frame cap that is not the one asked for is a stutter report waiting to be
     * filed. `app.fps.show` turns on borealis' own FPS readout; `app.swap.ab` swaps A/B and X/Y for players used
     * to the other layout. */
    {
        const int limit = cfgIntOr("app.fps.limit", 60);
        if (limit == 0 || limit == 30 || limit == 60 || limit == 120) {
            brls::Application::setLimitedFPS((size_t)limit);
            logPrintf("boot: app.fps.limit=%d -> borealis limit set (0 = no limit) [build52]", limit);
        } else {
            logPrintf("boot: app.fps.limit=%d is not 0/30/60/120 -- leaving borealis' default in place", limit);
        }
        const bool showFps = cfgIs("app.fps.show", "yes");
        brls::Application::setFPSStatus(showFps);
        logPrintf("boot: app.fps.show=%s -> the FPS readout is %s", showFps ? "yes" : "no",
                  showFps ? "on" : "off");
        const bool swap = cfgIs("app.swap.ab", "yes");
        brls::Application::setSwapInputKeys(swap);
        logPrintf("boot: app.swap.ab=%s -> A/B and X/Y are %s", swap ? "yes" : "no",
                  swap ? "swapped" : "as labelled");
    }
    logPrintf("ENTER brls::Application::init()");
    if (!brls::Application::init()) {
        logPrintf("FAIL brls::Application::init() returned false");
        brls::Logger::error("borealis init failed");
        return false;
    }
    logPrintf("OK    brls::Application::init()");

    /* THE UI'S OWN NUMBERS, AND THEY ARE THE FIRST THING A "EVERYTHING IS OFF THE EDGE OF THE SCREEN"
     * REPORT NEEDS. TV / docked mode does not change the LOGICAL layout: the console reports a different
     * resolution, borealis turns it into a window SCALE and keeps content at ORIGINAL_WINDOW
     * (application.cpp:1167-1181). So these five numbers -- window, content, scale, and the logical
     * size the app asked for -- say exactly which of them is wrong when something is drawn beyond the
     * screen. The player adds the same comparison for its own frame ("SW surface vs view"). [build40] */
    logPrintf("UI scale: window=%ux%u content=%.0fx%.0f scale=%.3f logical=%dx%d",
              brls::Application::windowWidth, brls::Application::windowHeight,
              (double)brls::Application::contentWidth, (double)brls::Application::contentHeight,
              (double)brls::Application::windowScale,
              brls::Application::ORIGINAL_WINDOW_WIDTH, brls::Application::ORIGINAL_WINDOW_HEIGHT);

    /* THE THEME (C9) -- RECORDED HERE, APPLIED AFTER createWindow. AND THAT SPLIT IS THE BUG FIX.
     *
     * MEASURED ON HARDWARE (the console's own crash reports, 2026-10-10 evening): with `theme=dark` in the cfg,
     * the app died ON EVERY LAUNCH, before the home page, and the report said
     *
     *     Result 0x4A8 / Data Abort, Fault Address 0x0
     *     Stack: brls::SwitchVideoContext::recordStaticCommands()  <- switch_video.cpp:140
     *
     * because borealis' `SwitchPlatform::setThemeVariant()` calls `this->videoContext->recordStaticCommands()`
     * UNCONDITIONALLY (switch_platform.cpp:293-299) -- and before createWindow there IS no video context, so
     * that is a null dereference. The cfg value only had to be set once (the settings screen writes it) for the
     * app to become unstartable; that is why it "started crashing after a few tries".
     *
     * So the value is only READ here, and applied below where the context exists. Note also what CHANGED for
     * "auto": the app used to force DARK after createWindow. It no longer does -- "auto" now means what the
     * setting says it means, "whatever the console is set to", which borealis' backend has already read
     * (switch_platform.cpp:73-83). */
    const std::string themeWanted = cfgValue("theme", "auto");
    logPrintf("boot: theme='%s' (applied after createWindow -- see the note in this file) [build70]",
              themeWanted.c_str());
    /* RETURN TO THE HOME MENU ON EXIT, NOT TO AN ERROR DIALOG.
     *
     * This is wiliwili's line, in wiliwili's position -- straight after
     * Application::init() and before createWindow (wiliwili/source/main.cpp:45-46):
     *
     *     // Return directly to the desktop when closing the application (only for NX)
     *     brls::Application::getPlatform()->exitToHomeMode(true);
     *
     * On Switch it is `__nx_applet_exit_mode = value`
     * (borealis .../platforms/switch/switch_platform.cpp:223-226), and libnx uses
     * that flag to decide what the console does when this homebrew returns. Left
     * unset, quitting from hbmenu produced the console's "the software was
     * closed because an error occurred" dialog -- from the home screen as well
     * as from the player, and with no error code -- even though this program's
     * own log reached "ENTER return EXIT_SUCCESS" with no exception, no abort and
     * a clean borealis teardown (SHUTDOWN breadcrumbs in player_view.cpp,
     * "Exiting..." in hongguo-brls.log). That is what a missing exit mode looks
     * like: the app is fine, the console has simply not been told how to leave
     * it. */
    logPrintf("ENTER exitToHomeMode(true)");
    brls::Application::getPlatform()->exitToHomeMode(true);
    logPrintf("OK    exitToHomeMode(true)");

    logPrintf("ENTER createWindow");
    brls::Application::createWindow("hongguo");
    logPrintf("OK    createWindow");

    /* ---- THE APPLET HOOK GOES HERE, AND THE POSITION IS THE POINT (build170) ----------------------
     *
     * WHAT IT FIXES. Reported twice: "playing video, insert the dock -- the picture becomes a small block in
     * the top-left corner, and only closing and reopening the app restores the full-screen TV picture", and
     * "the console still goes to sleep in TV mode". build168's log has ZERO lines about docking or the
     * operation mode, so the program was never told; see hg_applet.hpp for the full evidence chain.
     *
     * WHY AFTER createWindow AND NOT WITH THE OTHER BOOT CALLS ABOVE. borealis registers its OWN hook in the
     * SwitchPlatform constructor (switch_platform.cpp:111) and that hook does
     * `((SwitchVideoContext*)platform->getVideoContext())->appletCallback(hook)` with NO null check
     * (switch_platform.cpp:57-60). The video context is created by createWindow. So this app has to be past
     * createWindow before ANY hook can safely run -- ours included, since ours is registered in the same list
     * and libnx dispatches the list from its own thread the moment the console says so. Registering earlier
     * would not by itself crash (ours does not touch the context), but it would widen the window in which
     * borealis' OWN callback can fire with a null context, and there is no cost to registering here: the
     * lines immediately above and below are already the frame in which the app becomes able to react to
     * anything at all.
     *
     * WHY NOT IN main.cpp: this function is the ONE startup sequence, shared by the shipping main and the
     * diagnostic main (see hg_boot.hpp), so a hook registered here is present in every build that runs --
     * including the ones used to diagnose this very bug. */
    logPrintf("ENTER hg::appletWatchStart()");
    hg::appletWatchStart();
    logPrintf("OK    hg::appletWatchStart()");

    /* THE THEME, HERE AND ONLY HERE -- because this is the first point at which the video context exists.
     *
     * `setThemeVariant` re-records deko3d's static command list, which needs that context; calling it earlier is
     * the null dereference the crash reports show. This used to force DARK unconditionally; it now applies the
     * cfg's choice and leaves the platform alone for "auto" (see the note above). The result is read back and
     * logged, because a theme that did not take is invisible in a log that only records the request.
     *
     * AND THE CONTEXT IS CHECKED ANYWAY -- the ordering above is the fix, this is the belt to its braces. The
     * crash was a null dereference inside a framework call that cannot fail gracefully, so relying on "the call
     * site is now correct" means the next person who moves a line brings the unstartable app back. Asking the
     * platform for its video context (platform.hpp:215) turns that failure into a log line and a theme that did
     * not change -- recoverable, and visible. */
    {
        brls::Platform* platform = brls::Application::getPlatform();
        const bool wanted = (themeWanted == "dark" || themeWanted == "light");
        if (platform && wanted && platform->getVideoContext() == nullptr) {
            logPrintf("boot: theme='%s' NOT applied [build77] -- the video context does not exist yet, and borealis' "
                      "setThemeVariant() dereferences it unconditionally (this is the crash from build69). The "
                      "console's own theme is used for this session.",
                      themeWanted.c_str());
        } else if (platform && wanted) {
            platform->setThemeVariant(themeWanted == "dark" ? brls::ThemeVariant::DARK
                                                            : brls::ThemeVariant::LIGHT);
        }
        logPrintf("OK    theme applied: cfg='%s' -> the platform reports %s%s", themeWanted.c_str(),
                  (platform && platform->getThemeVariant() == brls::ThemeVariant::DARK) ? "dark" : "light",
                  (themeWanted == "auto") ? " (auto = the console's own setting)" : "");
    }

    /* ---- THE APP NO LONGER EXITS ITSELF (build167, the user's instruction) -------------------------
     *
     * THIS LINE USED TO BE `setGlobalQuit(true)`, WHICH PUT AN EXIT ACTION ON THE + BUTTON OF EVERY
     * SCREEN (borealis' Activity::registerExitAction defaults to BUTTON_START). Pressing + ran that
     * action -> Application::quit() -> exit() -> libnx tearing its own services down -- and THAT
     * CRASHED THE CONSOLE, reproducibly, which is what the user reported as "按加号会闪退".
     *
     * THE CRASH REPORTS SAY EXACTLY WHAT COLLIDED. Two Atmosphere reports from 2026-10-14 (10:26 and
     * 11:00; the 11:00 one carries the clearer stack), both Data Abort / 2168-0002, address 0:
     *
     *     exit -> __syscall_exit -> serviceClose -> _bsdDispatchImpl        (libnx bsd.c:133)
     *     ...while another thread was still inside:
     *     curl_easy_perform -> Curl_readwrite -> Curl_poll -> poll()
     *                       -> _socketParseBsdResult -> _socketGetFd         (libnx socket.c:295)
     *
     * i.e. a cover fetch was still in flight when the socket service was torn down underneath it, and
     * poll() walked into a null pointer. The exit action is what made that reachable from a button.
     *
     * SO THERE IS NO IN-APP EXIT ANY MORE. That is also how the user asked for it: "退出用 Switch 自带的
     * Home 键按 X，然后再确定，我们软件不做退出". The console's own Home menu closes the title without
     * this program running its own teardown. + on the home screen opens the settings screen instead --
     * see the registration in HomeActivity::createContentView.
     *
     * KEEPING THE CALL WITH `false` RATHER THAN DELETING IT is deliberate: the default is already false
     * (application.hpp:402), so a line that says what is NOT done would otherwise be invisible to
     * whoever reads this file next -- and this is exactly the line they would flip back, hitting the
     * crash again.
     *
     * NOTE THIS IS NOT THE WHOLE FIX. The race itself -- a fetch in flight while ANYTHING tears the
     * process down -- is still there for the Home-menu path. Draining in-flight HTTP before main()
     * returns is the next change; this one removes the button that made it easy to hit. */
    logPrintf("ENTER setGlobalQuit(false) -- the in-app exit is REMOVED (see the note in this file) [build167]");
    brls::Application::setGlobalQuit(false);
    logPrintf("OK    setGlobalQuit(false)");

    /* The C layer's curl init. Must happen before the first fetch; safe to
     * call more than once. */
    logPrintf("ENTER hg::Client::globalInit()");
    hg::Client::globalInit();
    logPrintf("OK    hg::Client::globalInit()");

    return true;
}

} // namespace hg
