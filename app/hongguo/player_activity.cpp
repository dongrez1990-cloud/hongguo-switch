/*
 * PlayerActivity: container for PlayerView, plus this page's input mapping.
 *
 * GPL-3.0-only.
 *
 * B goes back: borealis registers that on every activity by default, so it is
 * not re-implemented here. A toggles pause on this page (A means "play the
 * episode" on the detail list, so on the player it is the natural
 * play/pause key). + does NOT exit any more (build167): there is no in-app exit at all, because the
 * exit action's teardown collided with in-flight cover fetches and crashed the console -- see the note
 * in hg_boot.cpp. Use the console's own Home menu to close the title.
 *
 * ASCII-only source file.
 */

#include "player_activity.hpp"
#include "player_view.hpp"
#include "hg_store.hpp"
#include "hg_lang.h"
#include "player_menu.hpp"
#include "player_settings.hpp"
#include "hg_cfg.hpp"
#include "hg_log.hpp"

#include <borealis.hpp>

namespace hg {

namespace {

/* Read an integer setting out of hongguo.cfg for the picture menu, clamped.
 *
 * The keys are `player.<property>` -- player.brightness, player.contrast,
 * player.saturation, player.gamma, player.hue, player.aspect, player.mirror -- so
 * that every arm of every experiment this app can run is one FTP write to one file
 * (hg_cfg.hpp). The menu itself never writes: it applies these values when X is
 * pressed and the file is what makes a value persist, which keeps a single store
 * for settings rather than two that can disagree.
 *
 * A malformed value falls back to the default rather than being treated as an
 * error: the file is hand-edited over FTP and `player.brightness=twenty` should
 * leave a working player, not a silent zero. */
int cfgIntClamped(const char* key, int fallback, int lo, int hi) {
    std::string raw = hg::cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) {
        hg::logPrintf("PLAYERMENU: cfg %s='%s' is not a number -- using %d", key, raw.c_str(),
                      fallback);
        return fallback;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

/* Parse a text value against a list of the names hg_pmenu.h defines for aspect
 * modes. Returns -1 when the text is not one of them. */
int parseAspect(const std::string& text) {
    if (text.empty()) return -1;
    if (text == "auto") return HG_PMENU_ASPECT_AUTO;
    if (text == "crop") return HG_PMENU_ASPECT_CROP;
    if (text == "stretch") return HG_PMENU_ASPECT_STRETCH;
    return -1;
}

} // namespace

PlayerActivity::PlayerActivity(Series s, std::vector<Episode> eps, int i)
    : series(std::move(s)), episodes(std::move(eps)), episodeIndex(i < 1 ? 1 : i) {
    hg::logPrintf("PLAYER: opened for '%s' (id=%s) episode %d of %u [build49]", this->series.title.c_str(),
                  this->series.id.c_str(), this->episodeIndex, (unsigned)this->episodes.size());
}

PlayerActivity::~PlayerActivity() {
    /* Sever the menu's back-pointer before anything else. PlayerView::draw runs on
     * the main thread and cannot run after the activity starts being destroyed, but
     * "cannot" is the kind of claim this project has been wrong about before, and the
     * cost of being sure is one line. */
    if (this->playerView) this->playerView->clearMenuOverlayCallback();
    if (this->playerView) this->playerView->clearHistorySink();
}
/* THE EPISODE LIST, AND THE THREE THINGS THAT NEED IT.
 *
 * This is where F1 lives (ZL/ZR = previous/next episode) together with "what happens when an episode ends",
 * because they are the same question asked at different moments: one by the user, one by mpv. Both go through
 * startEpisode(), so exactly one place knows how to begin an episode -- including the fact that starting one
 * means asking the view to fetch a fresh play plan.
 *
 * WHY THE ENDS REFUSE INSTEAD OF WRAPPING. An episode list is not a ring. Jumping from the last episode back
 * to the first is what a wrap looks like, and a user who pressed ZR once too often would be shown episode 1
 * again with no way to tell that the list had restarted. The refusal is logged, with the reason. */
std::string PlayerActivity::currentEpisodeId() const {
    const size_t i = (size_t)(this->episodeIndex - 1);
    if (i >= this->episodes.size()) return std::string();
    return this->episodes[i].id;
}

std::string PlayerActivity::episodeLabel() const {
    char b[32];
    snprintf(b, sizeof b, "  \xE7\xAC\xAC %d \xE9\x9B\x86", this->episodeIndex);   /* "  第 N 集" */
    return this->series.title + b;
}

/* PlaybackSink. The view has the episode id and the position; THIS class has the series, which is what a
 * history record is made of. The label is the episode's 1-based number, which is how this app labels episodes
 * everywhere (there are no per-episode titles upstream). */
void PlayerActivity::recordPlayback(const std::string& episodeId, double position, double duration) {
    if (this->series.id.empty()) {
        hg::logPrintf("PLAYER: not recording history -- the series has no id");
        return;
    }
    char label[16];
    snprintf(label, sizeof label, "%d", this->episodeIndex);
    Store::recordPlayback(this->series, episodeId, label, position, duration);
}

void PlayerActivity::playbackFailed(const std::string& reason) {
    if (!this->lastStartWasAJump) return;   /* the first episode has nothing to go back to */
    if (this->previousIndex < 1 || (size_t)this->previousIndex > this->episodes.size()) return;

    hg::logPrintf("PLAYER: episode %d could not start ('%s') [build66] -- going back to episode %d, because the "
                  "one playing is still it",
                  this->episodeIndex, reason.c_str(), this->previousIndex);
    this->episodeIndex = this->previousIndex;
    if (this->playerView) this->playerView->setOverlayTitle(this->episodeLabel());
    brls::Application::notify(std::string("Could not start the next episode: ") + reason);
}
void PlayerActivity::startEpisode(int oneBasedIndex, const char* why) {
    const auto now = std::chrono::steady_clock::now();
    if (this->lastEpisodeSwitch.time_since_epoch().count() != 0 &&
        now - this->lastEpisodeSwitch < std::chrono::milliseconds(700)) {
        hg::logPrintf("PLAYER: %s ignored while the previous episode switch is settling [build159]", why);
        return;
    }
    if (oneBasedIndex < 1 || (size_t)oneBasedIndex > this->episodes.size()) {
        hg::logPrintf("PLAYER: %s asked for episode %d, which does not exist (%u episodes) -- ignored", why,
                      oneBasedIndex, (unsigned)this->episodes.size());
        brls::Application::notify(std::string(why) + ": no such episode");
        return;
    }
    if (oneBasedIndex == this->episodeIndex) {
        hg::logPrintf("PLAYER: %s asked for episode %d, which is already playing -- ignored", why,
                      oneBasedIndex);
        return;
    }
    /* REMEMBER WHERE WE WERE, so a failed start can be undone: see playbackFailed. */
    this->previousIndex = this->episodeIndex;
    this->lastStartWasAJump = true;
    this->lastEpisodeSwitch = now;
    this->episodeIndex = oneBasedIndex;
    const std::string id = this->currentEpisodeId();
    hg::logPrintf("PLAYER: %s -> episode %d (id=%s)", why, this->episodeIndex, id.c_str());
    if (this->playerView) {
        /* The view owns mpv and the play plan; asking it to start is what makes the new episode play, and it is
         * the same call the constructor makes for the first one. */
        this->playerView->start(id);
    }
    /* The overlay carries the episode number: without it, an auto-advance looks exactly like a glitch. */
    /* AND THE CONTROL BAR'S OWN LINE, which is the TV-style title (C14). */
    if (this->playerView) this->playerView->setOverlayTitle(this->episodeLabel());
}

void PlayerActivity::switchEpisode(int delta) {
    if (this->episodes.empty()) {
        hg::logPrintf("PLAYER: no episode list (the caller passed an empty one) -- cannot switch");
        brls::Application::notify("No episode list");
        return;
    }
    this->startEpisode(this->episodeIndex + delta, delta > 0 ? "next episode" : "previous episode");
}

void PlayerActivity::onEpisodeEnded() {
    /* THE PLAY MODE DECIDES. It lives in the view (the settings panel owns it) and defaults to "stop here",
     * which is what the player did before this feature existed -- a new behaviour is opt-in. */
    const bool advance = this->playerView && this->playerView->playModeAdvances();
    hg::logPrintf("PLAYER: the episode ended; play mode says %s (episode %d of %u)",
                  advance ? "advance to the next one" : "stop here", this->episodeIndex,
                  (unsigned)this->episodes.size());
    if (!advance) return;
    if ((size_t)this->episodeIndex >= this->episodes.size()) {
        hg::logPrintf("PLAYER: the last episode ended -- there is nothing to advance to");
        return;
    }
    this->startEpisode(this->episodeIndex + 1, "auto-next");
}
brls::View* PlayerActivity::createContentView() {
    hg::logPrintf("PLAYER: ENTER createContentView (episode %s, index %d)", episodeId.c_str(),
                  index);

    /* THE PLAYER VIEW IS THE WHOLE SCREEN, AND THE TEXT SITS ON TOP OF IT.
     *
     * mpv does not draw into a view rectangle: with the deko3d render API it
     * draws into the WHOLE framebuffer that borealis is currently presenting,
     * letterboxed to the video's aspect. The first version of this file put the
     * PlayerView inside a column under a title label, so the video filled the
     * screen from the top while the layout believed it occupied the lower part
     * -- which is exactly what the console showed: a picture riding up above the
     * player UI and partly hidden behind it.
     *
     * wiliwili does the same thing for the same reason: full-screen video, with
     * the UI drawn over it (mpv_core.cpp:823-841 comments that the video is only
     * drawn when the overlay is fully opaque). So the view that owns mpv is the
     * root, and the labels are its children, which borealis draws after the
     * video because PlayerView::draw calls mpv first and Box::draw afterwards. */
    auto* root = new PlayerView();
    root->setAxis(brls::Axis::COLUMN);
    this->playerView = root;

    /* THE OVERLAY IS TWO SMALL LINES AT THE BOTTOM, NOT FOUR BANDS OVER THE
     * PICTURE.
     *
     * The first version stacked four full-width labels at the top, each with a
     * 150-alpha black band: the series title, the mpv status, the raw episode id
     * and the control hint. On the console that is what the user described first
     * -- "four rows of text" across the drama. A player's overlay should be as
     * close to invisible as it can be while still saying what is playing and what
     * the buttons do.
     *
     * So everything now sits at the BOTTOM in two lines at 16pt with a much
     * lighter background, and the episode id is gone: it existed to prove the data
     * path was populated, which the breadcrumb log does without costing screen
     * space. `overlay.labels=no` hides the lot. */
    /* ---- THE OVERLAY ROW IS GONE, AT THE USER'S INSTRUCTION ------------------------------------------
     *
     * WHAT THEY SAW AND SAID (2026-10-10, with photographs): "it feels like you built two interfaces -- one
     * with the PAUSED/PLAYING progress bar and the series/episode name at the top, plus the picture settings on
     * X; move that and the episode name into the interface I asked for, and delete the one you made."
     *
     * They are right, and this is the second interface. The control bar carries the same three things already:
     * the series name and episode number on its top line, a progress bar with a clock, and (from the 清晰度 chip)
     * the picture settings. So the overlay is DELETED rather than hidden: the picture is clean by default, the
     * bar is the one place that shows state, and there is no way for the two to disagree -- which they could,
     * because this row was a brls view tree and the bar is nanovg, so the two were drawn by different code on
     * different schedules.
     *
     * WHAT STILL WORKS WITHOUT IT: `Y` (retargeted below to show the bar, since the thing it used to show no
     * longer exists), the picture menu on X and on the 清晰度 chip, and every loading/error message -- those go
     * through brls::Application::notify(), which is a toast and never depended on this row.
     *
     * The poll thread still fills `impl->statusText`; it was always the SD log's copy of that line, and the log
     * is what the diagnosis work reads. */

    root->setHistorySink(this);
    /* THE SERIES ID, WHICH ONLY THIS CLASS HAS (build173). The play-info route requires it together with
     * the episode id: measured, the same vid answers 200 under its own series and 400 with a wrong or
     * empty one. setSeriesId must be called BEFORE start(), because start() is what kicks off the first
     * play-info request on a worker thread. */
    root->setSeriesId(this->series.id);
    /* C14: the control bar shows what is playing. Set here and again on every episode switch (see
     * startEpisode), because the line contains the episode number. */
    root->setOverlayTitle(this->episodeLabel());
    root->start(this->currentEpisodeId());

    /* ---- THE PICTURE MENU (X) ------------------------------------------------
     *
     * The panel is built once, here, and added LAST so that Box::draw() reaches it
     * after the overlay row: borealis draws children in insertion order
     * (box.cpp:121-159), so last means on top. It starts GONE, which is what keeps
     * this whole feature invisible until X is actually pressed -- "additive until
     * invoked" is the rule for anything that touches a screen that already works.
     *
     * Note what is NOT here: no brls::Slider, no cell, no scrolling frame, nothing
     * focusable. player_menu.hpp explains why at length; the short version is that a
     * Slider's getDefaultFocus() returns its focusable inner pointer unconditionally
     * (slider.cpp:119) and would take LEFT/RIGHT away from seeking. */
    /* ---- THE PANELS ARE BACK IN THE TREE, TEMPORARILY (build115) ----------------------------------------
     *
     * build114 removed both panels from the view tree, and the player then CRASHED on the first A press --
     * "FATAL exception what() = basic_string: construction from null is not valid", immediately after
     * "control values seeded from cfg" (the console's own log). Removing the panels changed the view tree the
     * framework walks, and something on that walk constructs a std::string from a null pointer. That bug is real
     * and worth fixing, but the user is testing: a build that runs with an annoying empty frame beats a build
     * that aborts when the controls come up.
     *
     * So this is a REVERT to the state that ran (build112), kept deliberately and marked as such. The plan to
     * take the panels out again goes with the wider player-UI change (M4/M5), where the whole layout is redone
     * with nanovg and the hint-bar/focus path can be checked in the same pass -- see docs/待做清单.md.
     *
     * The settings chip is still gone from the control bar (build112), so the panel below is inert: nothing
     * opens it any more. */
    {
        auto* settings = new PlayerSettingsPanel();
        root->addView(settings);
        this->settingsPanel = settings;
        root->setSettingsPanel(settings);

        /* ---- ROLLED BACK, AND THE TREE IS NOT TOUCHED (build117) ----------------------------------------
         *
         * The two lines that stood here -- GONE + unfocusable (build116) -- were followed by an Atmosphere crash
         * on the console, and the user's instruction was explicit: "never mind the box, roll back to the previous
         * version, then finish the rest."
         *
         * So this is exactly the state that ran without crashing (build115): the panel is attached and otherwise
         * left alone. The box stays on screen for now and is accepted -- the box is cosmetic, a crash is not.
         *
         * The real removal, together with the null-string path that removing a panel exposes, belongs to the
         * player-UI rewrite (docs/待做清单.md M4/M5), where the whole layout is redone with nanovg and that path
         * can be found and fixed in the same pass. */
    }

    if (!hg::cfgIs("player.menu", "no")) {
        auto* panel = new PlayerMenuPanel();
        root->addView(panel);
        this->menuPanel = panel;
        root->setMenuPanel(panel);
        root->setMenuOverlayCallback(this);

    /* THE PICTURE MENU'S VALUES ARE STILL SEEDED, because the state is what the X key and the quality ladder read.
     * They no longer drive a panel (there is none), but keeping the seeding means the cfg keys keep working and
     * the values are ready if the panel ever comes back. */
    {
        HgPMenu& m = root->menuState();
        m.values[HG_PMENU_ROW_BRIGHTNESS] =
            cfgIntClamped("player.brightness", 0, HG_PMENU_VALUE_MIN, HG_PMENU_VALUE_MAX);
        m.values[HG_PMENU_ROW_CONTRAST] =
            cfgIntClamped("player.contrast", 0, HG_PMENU_VALUE_MIN, HG_PMENU_VALUE_MAX);
        m.values[HG_PMENU_ROW_SATURATION] =
            cfgIntClamped("player.saturation", 0, HG_PMENU_VALUE_MIN, HG_PMENU_VALUE_MAX);
        m.values[HG_PMENU_ROW_GAMMA] =
            cfgIntClamped("player.gamma", 0, HG_PMENU_VALUE_MIN, HG_PMENU_VALUE_MAX);
        m.values[HG_PMENU_ROW_HUE] =
            cfgIntClamped("player.hue", 0, HG_PMENU_VALUE_MIN, HG_PMENU_VALUE_MAX);

        int aspect = parseAspect(hg::cfgValue("player.aspect", "auto"));
        if (aspect < 0) {
            hg::logPrintf("PLAYERMENU: cfg player.aspect is not auto/crop/stretch -- using auto");
            aspect = HG_PMENU_ASPECT_AUTO;
        }
        m.aspect = aspect;
        m.mirror = hg::cfgIs("player.mirror", "yes") ? 1 : 0;

        hg::logPrintf("PLAYERMENU: seeded from cfg -- bright=%d contrast=%d sat=%d gamma=%d hue=%d "
                      "aspect=%d mirror=%d [build115]",
                      m.values[HG_PMENU_ROW_BRIGHTNESS], m.values[HG_PMENU_ROW_CONTRAST],
                      m.values[HG_PMENU_ROW_SATURATION], m.values[HG_PMENU_ROW_GAMMA],
                      m.values[HG_PMENU_ROW_HUE], m.aspect, m.mirror);
    }
    }   /* closes the "if (!player.menu)" block opened above */

    return root;
}

void PlayerActivity::onContentAvailable() {
    if (!this->playerView) return;

    /* A toggles pause. Registered on the player view so it fires only while
     * this activity is the top of the stack.
     *
     * THE MENU GUARD IS NOT OPTIONAL. While the picture menu is open, A closes it --
     * and A is also pause. Both handlers are live at once, so without this check a
     * single press would close the menu AND pause the drama. The menu is polled
     * rather than registered (see player_view.cpp), so the guard has to be here, on
     * the registered side.
     *
     * Returning false means "someone else handled it": it suppresses borealis' click
     * animation and click sound for a press that is really a menu-close. That is safe
     * because borealis registers NO default action for A on an activity --
     * Application::handleAction only acts when a listener returns true
     * (application.cpp:712-743), so an unconsumed press simply ends. */
    PlayerView* view = this->playerView;
    this->playerView->registerAction("Controls", brls::ControllerButton::BUTTON_A,
        [view](brls::View*) {
            /* THE SAME GUARD FOR THE SETTINGS PANEL, AND IT WAS MISSING (found by the round-24 audit).
             *
             * `tickSettings` -- the poll -- closes the panel when A or B is pressed; this registered action
             * runs on the SAME press and knew nothing about it. The consequence was visible: with the settings
             * panel open, A closed it and then this handler ran `activateControl()` with the bar still up, and
             * on the 设置 chip that OPENS THE PANEL AGAIN -- so A looked like it did nothing at all. B was
             * worse: it closed the panel and then fell through to `popActivity`, i.e. leaving playback when the
             * user only meant to close a panel.
             *
             * `settingsIsOpen()` had been written for exactly this guard in build47 and never called; it is
             * the round-24 audit that found the omission, by grepping for the accessor. */
            if (view->menuIsOpen() || view->settingsIsOpen() || view->chipMenuIsOpen()) {
                /* The poller sees this press too and will close whichever panel is open; this handler
                 * deliberately only reports it. */
                hg::logPrintf("PLAYER: A pressed while %s is open -> it closes (not controls) [build64]",
                              view->menuIsOpen() ? "the picture menu" : "the settings panel");
                return false;
            }
            /* A SHOWS THE CONTROL BAR BEFORE IT ACTS ON IT, which is the user's instruction for this
             * screen: "点画面或按 A 呼出/隐藏（A 从『暂停』改为『呼出控件』）".
             *
             * So the first A brings the bar up with its play/pause button highlighted, and the next A --
             * while the bar is still up -- presses that button, i.e. it pauses. Pause is therefore never
             * unreachable, and never happens by accident from someone who only wanted to see the controls.
             *
             * An earlier version of this screen paused on every A. That is what the user asked to change,
             * and the two-step is the smallest change that does it without adding a second input path to a
             * screen whose keys are polled from the controller state (see player_view.hpp). */
            if (!view->controlsVisible()) {
                hg::logPrintf("PLAYER: A pressed -> show the control bar");
                view->showControls();
                return true;
            }
            /* A ACTS ON THE SELECTED ITEM (see activateControl): play/pause for the play and progress items,
             * and Open-the-picture-menu-on-the-quality-row for the quality chip. */
            view->activateControl();
            /* Keep it up: a bar that vanishes the moment you use it hides the effect of the press. */
            view->showControls();
            return true;
        });

    /* B GOES BACK, EXPLICITLY.
     *
     * borealis registers B as a global "back" on activities, but on this screen
     * it produced no reaction at all -- and "no reaction" is indistinguishable
     * from a swallowed keypress unless something says which it was. Registering
     * B here makes the intent explicit and makes the log say whether the button
     * was even seen.
     *
     * CHECKED, NOT ASSUMED: borealis does NOT in fact register B anywhere by itself.
     * The only B actions in the library are in dialog.cpp (dismiss a dialog) and this
     * registration -- Activity itself offers registerExitAction(button) and nothing
     * else (activity.cpp:142-148). So the player's B never worked because nothing had
     * registered it, and returning false below cannot fall through to a hidden pop:
     * there is no other B action in the tree to fall through to. */
    this->playerView->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B,
        [view](brls::View*) {
            /* ONE GUARD, TWO PANELS. B must close whichever is open rather than fall through to the
             * popActivity below -- the round-24 audit found that the settings panel was missing from this
             * check, so B used to LEAVE PLAYBACK when the user only wanted to close it. */
            if (view->menuIsOpen() || view->settingsIsOpen() || view->chipMenuIsOpen()) {
                hg::logPrintf("PLAYER: B pressed while %s is open -> it closes (not back)",
                              view->menuIsOpen() ? "the picture menu" : "the settings panel");
                return false;
            }
            hg::logPrintf("PLAYER: B pressed -> popActivity (deferred one frame)");
            /* THE SAME USE-AFTER-FREE AS THE DETAIL SCREEN, and it was here all along.
             *
             * This callback is registered ON `view` and calls `menuIsOpen()` on it, so the player view is
             * alive and in use when this runs -- and popActivity destroys it. `handleAction` is still
             * walking this view's action list further down the stack.
             *
             * The deferral is not a workaround for one screen; it is the rule for every action that
             * destroys the view it was registered on. `brls::sync` is correct here because the main loop
             * runs `Application::frame()` (which dispatches actions) and only THEN
             * `Threading::performSyncTasks()` (application.cpp:201-204), so this body executes with the
             * dispatch fully unwound. */
            brls::sync([]() { brls::Application::popActivity(); });
            return true;
        });

    /* ZL / ZR = PREVIOUS / NEXT EPISODE (F1). THE NAMES ARE THE XBOX-GENERIC ONES: borealis' Switch backend maps
     * `HidNpadButton_ZL -> BUTTON_LT` and `HidNpadButton_ZR -> BUTTON_RT` (switch_input.cpp:26 and :48), so LT/RT
     * ARE the console's ZL/ZR. Using a literal `BUTTON_ZL` does not compile, and guessing that it did would have
     * cost a round.
     *
     * Registered actions rather than polled keys, because these are
     * discrete: one press, one episode, and borealis' own repeat is fine for that. The seek keys stay polled
     * because they need acceleration, and the two paths do not overlap.
     *
     * `this` is captured, not a strdup of the pointer: the action is registered ON the player view, which the
     * activity owns, so the activity outlives the action. Nothing here destroys the view, so there is no
     * deferral to do (compare the B handler above, which pops the activity and must be deferred). */
    this->playerView->registerAction("Prev episode", brls::ControllerButton::BUTTON_LT,
        [this](brls::View*) {
            hg::logPrintf("PLAYER: ZL pressed -> previous episode");
            this->switchEpisode(-1);
            return true;
        });
    this->playerView->registerAction("Next episode", brls::ControllerButton::BUTTON_RT,
        [this](brls::View*) {
            hg::logPrintf("PLAYER: ZR pressed -> next episode");
            this->switchEpisode(+1);
            return true;
        });
    /* L/R are the Switch player's physical shoulder buttons.  Keep their
     * binding in the action system so a short tap is not lost when the raw
     * controller poll is owned by the control bar; allowRepeating preserves
     * wiliwili's hold-to-seek behaviour. */
    this->playerView->registerAction("Rewind", brls::ControllerButton::BUTTON_LB,
        [this](brls::View*) {
            this->playerView->seekRelative(-10);
            return true;
        }, true, true);
    this->playerView->registerAction("Fast forward", brls::ControllerButton::BUTTON_RB,
        [this](brls::View*) {
            this->playerView->seekRelative(+10);
            return true;
        }, true, true);
    /* ---- seeking is NOT registered here, on purpose ---------------------------
     *
     * LEFT / RIGHT / LB / RB are handled by polling the raw controller state once
     * per frame inside PlayerView::draw(), following wiliwili's
     * VideoProgressSlider::buttonsProcessing. The reasons are in player_view.cpp
     * next to the curve, and the short version is:
     *
     *   - holding must ACCELERATE, and borealis' action repeat is a fixed 100 ms
     *     cadence with no acceleration (application.cpp:65-66)
     *   - registering them here AS WELL would make every tap seek twice
     *
     * So this file keeps only the discrete actions: A (play/pause), B (back), Y
     * (overlay). wiliwili's mapping for the seek keys is LEFT/RIGHT for the
     * progress slider and LB/RB for previous/next item (player_base_activity.cpp:
     * 226-253); this app has no item list to drive, so LB/RB carry a 6x larger
     * seek that accelerates on the same curve. */

    /* Y shows/hides the overlay, which is wiliwili's OSD toggle. Registered even
     * when the overlay is hidden by config: then it only logs, so the key is
     * accounted for rather than silently dead.
     *
     * The menu guard: while the picture menu is open, Y belongs to the menu (which
     * ignores it, as wiliwili does with the OSD key inside its settings panel). The
     * poller logs the ignored press, so this handler only declines it -- otherwise
     * one press would hide the overlay underneath the panel and the next would
     * bring it back, which looks like a stutter rather than a decision. */
    this->playerView->registerAction("Toggle info", brls::ControllerButton::BUTTON_Y,
        [this, view](brls::View*) {
            if (view->menuIsOpen()) {
                hg::logPrintf("PLAYER: Y pressed while the menu is open -> menu keeps it");
                return false;
            }
            this->toggleOverlay();
            return true;
        });

    /* X OPENS THE PICTURE MENU -- AND THIS HANDLER DOES NOT DO IT.
     *
     * The key itself is polled in PlayerView::draw together with the four direction
     * keys, because a menu needs held-key repeat with its own timing (400 ms then
     * 90 ms) and borealis' action repeat is a flat 250 ms / 100 ms. Registering X
     * here as well would toggle the menu twice per press.
     *
     * It is registered anyway, with hidden=false, for one concrete reason: borealis
     * builds the on-screen key hints from the actions registered on the focused view
     * (Application::frame walks hintParent->getActions()), so this line is what puts
     * "X Picture" on the bottom bar and makes the feature discoverable at all. The
     * listener returns false and says so.
     *
     * ONLY WHEN THE FEATURE IS ON: with player.menu=no in hongguo.cfg the key is not
     * advertised and the poller does not read it either, so the whole feature is off
     * without a rebuild. */
    if (!hg::cfgIs("player.menu", "no")) {
        this->playerView->registerAction(hg_tr(HG_T_PICTURE), brls::ControllerButton::BUTTON_X,
            [](brls::View*) {
                hg::logPrintf("PLAYER: X action fired (the menu is polled in draw())");
                return false;
            });
    }
}

/* THE TWO HOOKS THE PLAYER VIEW CALLS, now that there is no overlay row for them to hide.
 *
 * `controlBarVisible` used to stand the overlay row down whenever the bar came up, and `setMenuOverlay` used to
 * clear it out of the picture menu's way. Both existed because there were TWO places showing the same state --
 * a brls view tree drawn by the framework and the bar drawn with nanovg -- and they could disagree. The user saw
 * that as "you built two interfaces" and asked for one, so the row is deleted (see the note where it used to be
 * built) and these two only record the state that the rest of the screen reads. */
void PlayerActivity::controlBarVisible(bool visible) {
    this->controlBarUp = visible;
    hg::logPrintf("PLAYER: the control bar %s", visible ? "went up" : "went down");
}

void PlayerActivity::setMenuOverlay(int open) {
    this->menuOverlayHidden = open != 0;
    hg::logPrintf("PLAYER: the picture menu is %s", this->menuOverlayHidden ? "open" : "closed");
}

/* Y: show or hide the CONTROL BAR.
 *
 * It used to toggle the overlay row, and that row no longer exists. Leaving Y doing nothing would waste the one
 * spare button a player has, so it now pins the bar open -- the same thing A does, except that A also presses
 * whatever is selected, and Y only shows it. Both are recorded in the log, so a report about "the bar will not
 * stay" or "the bar will not go" can be read from it. */
void PlayerActivity::toggleOverlay() {
    if (!this->playerView) return;
    if (this->playerView->controlsVisible()) {
        this->playerView->hideControls();
        hg::logPrintf("PLAYER: Y pressed -> control bar hidden [build80]");
    } else {
        this->playerView->showControls();
        hg::logPrintf("PLAYER: Y pressed -> control bar shown [build80]");
    }
}

} // namespace hg
