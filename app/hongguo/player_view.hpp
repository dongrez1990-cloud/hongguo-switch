/*
 * PlayerView: the view that owns mpv and renders it into borealis' deko3d
 * framebuffer.
 *
 * GPL-3.0-only.
 *
 * Why a View and not the Activity: brls::Activity has no virtual draw(), so
 * the only place a frame hook exists is a View. The deko3d sequence in draw()
 * is transcribed from upstream/wiliwili/wiliwili/source/view/mpv_core.cpp
 * (the BOREALIS_USE_DEKO3D branches) -- the configuration known to work on
 * this hardware -- including the fence pair, which is not optional: without
 * queueWaitFence the CPU can reissue the command list while the GPU is still
 * consuming it.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

extern "C" {
#include "hg_ctl.h"
}
#include <string>

/* OUTSIDE THE extern "C" BLOCK BELOW, and that is not a style choice: hg_alive.hpp includes <atomic> and
 * <memory>, and a C++ template inside `extern "C"` is a hard error ("template with C linkage") whose diagnostic
 * output buries the real message. */
#include "hg_alive.hpp"

extern "C" {
#include "hg_pmenu.h"
#include "hg_play.h"
#include "hg_quality.h"
#include "hg_scrub.h"
}

/* PlayerActivity is a COMPLETE type here rather than a forward declaration:
 * drawMenuTick() calls PlayerActivity::setMenuOverlay() directly, and a forward
 * declaration would make that ill-formed. There is no cycle -- player_activity.hpp
 * forward-declares PlayerView and includes nothing from this header. */
#include "player_activity.hpp"

namespace hg {

class PlayerMenuPanel;

class PlayerView : public brls::Box {
  public:
    PlayerView();
    ~PlayerView() override;

    /* Kick off the signed request, the plan derivation and mpv startup.
     * Split from the constructor so the Activity can build the view tree first
     * and have somewhere to report progress into. */
    void start(const std::string& episodeId);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

    /* A toggles pause; returns true when the key was consumed. */
    void togglePause();

    /* Seek by a relative amount in seconds. This is wiliwili's own seek path:
     *     void MPVCore::seekRelative(int64_t p) { command_async("seek", p, "relative"); }
     *     (upstream/wiliwili/wiliwili/source/view/mpv_core.cpp:1209)
     * ASYNC, deliberately: this is called from a button handler on the render
     * thread, and mpv_command() blocks until the command completes -- which for a
     * seek means blocking the thread that is presenting frames. wiliwili's
     * command_async wraps mpv_command_async for the same reason.
     *
     * Returns the mpv request id (>= 0) or a negative error; it is logged by the
     * caller, because a seek key that silently does nothing is indistinguishable
     * from a key that was never seen. */
    int seekRelative(int seconds);

    /* Seek to an ABSOLUTE position in seconds. This is what a drag commits to:
     *
     *     mpv "seek <seconds> absolute"
     *
     * The absolute form is not a stylistic choice. A drag produces a target POSITION, and
     * expressing it as a relative seek would require knowing the position the player was at
     * when the drag started and hoping nothing moved it since -- the position is live, so
     * it drifts. wiliwili reaches the same place with `requestSeeking()` on its slider
     * (video_view.cpp:234,242). */
    int seekAbsolute(double seconds);

    /* The overlay's text and the progress bar are hidden and shown together (Y on
     * the console). Set from the activity, read by draw() on the main thread. */
    bool progressBarVisible = true;

    /* ---- the on-screen control bar (A, or a tap on the picture) ----------------
     *
     * WHY A NO LONGER PAUSES. The user's instruction for this screen was: "点画面或按 A 呼出/隐藏（A 从
     * 『暂停』改为『呼出控件』）". A TV player's model is that A brings the furniture up and the furniture
     * does the acting -- and a pause key that is also the "show me the controls" key cannot do both at once.
     *
     * SO THE TWO-STEP IS DELIBERATE: the first A shows the bar (its play/pause button highlighted), and
     * pressing A again while the bar is up activates that button -- i.e. it pauses. That is one key doing
     * the two things the user asked for, in the order the user described, and it never leaves the pause
     * action unreachable.
     *
     * THE BAR HIDES ITSELF after a few idle seconds, which is what every console player does and the reason
     * the picture is not permanently covered; any A or tap brings it back and restarts the timer. */
    bool controlsVisible() const;
    void showControls();
    void hideControls();

    /* ---- WHAT THE BAR CONTAINS, AND HOW IT IS OPERATED -------------------------
     *
     * The items are the reference layout's: the play/pause button, the progress, then the three values this
     * screen never had (volume, speed, zoom) -- quality lives in the X menu, where it already works.
     *
     * UP/DOWN moves the selection; LEFT/RIGHT adjusts the selected item; on PROGRESS, which is the default,
     * LEFT/RIGHT stays the hold-to-seek it has always been. That split is what lets the three new values be
     * operated without giving this screen a second input path (see the note in the class header). */
    enum ControlItem {
        CONTROL_PLAY = 0,
        CONTROL_PROGRESS,
        CONTROL_VOLUME,
        CONTROL_SPEED,
        CONTROL_ZOOM,
        /* THE QUALITY CHIP (A4 of the list). It is a DISPLAY plus a door: LEFT/RIGHT does not step it, because
         * changing a tier reloads the stream and build45's two-step rule says the cursor moves freely and A
         * commits. So A on this chip opens the picture menu ON the quality row, where that two-step lives --
         * one interaction model for quality, in one place, instead of a second one invented here. */
        CONTROL_QUALITY,
        /* THE SETTINGS CHIP (A7). A door: A on it opens the player settings panel. */
        CONTROL_SETTINGS,
        CONTROL_COUNT
    };

    int controlSelection() const { return this->controlsIndex; }
    void moveControlSelection(int delta);
    /* Step the selected value by `delta` (LEFT/RIGHT). Does nothing on the play, progress and quality items:
     * see CONTROL_QUALITY for why quality is not stepped here. */
    void adjustControl(int delta);
    /* A while the bar is up: activate the SELECTED item. Play and progress are play/pause (the primary
     * action); the quality chip opens the picture menu on its quality row. */
    void activateControl();

    /* ---- THE CHIP PULL-UP MENUS (K16, the user's request) --------------------------------------------
     *
     * WHAT THEY ASKED FOR, in their words: the volume chip should be an icon that opens a DRAG BAR, and the
     * speed, zoom and resolution chips should each open a PULL-UP MENU -- "and when it is collapsed it shows the
     * current resolution". That is the console-player shape, and it is why these four exist.
     *
     * `chipMenu` is 0 when no menu is open, otherwise the CONTROL_* item whose menu it is. Drawn by
     * drawControlBar, driven by tickControls -- the same polled input every other key on this screen uses, so
     * there is still exactly one mover.
     *
     * QUALITY IS DELIBERATELY NOT IN THIS LIST: pressing A on that chip already opens the picture menu ON ITS
     * QUALITY ROW, which IS a list of resolutions to pick from (see openPictureMenu). A second implementation of
     * the same list would be the "two interfaces" problem the user reported two rounds ago. */
    /* True while a chip menu is open. The ACTIVITY needs this: A and B are also registered actions there, and
     * a press that closes a menu must not then fall through to "open the settings panel" or "leave the player".
     * See the guards in player_activity.cpp. */
    bool chipMenuIsOpen() const { return this->chipMenu != 0; }

    /* ---- TOUCH (build109, the user's design: "tap the screen and the UI appears"). ---------------------
     *
     * The framework hands a GestureRecognizer's recognitionLoop the TouchState, which is the ONLY public route to
     * a finger position in this fork (Application::currentTouchState is private and InputManager only feeds it).
     * A subclass of brls::GestureRecognizer is therefore the way in, and this is what it calls.
     *
     * The hit areas were already being recorded while drawing (chipHit / menuHit / volumeHit), so this is the
     * consumer those were written for. A tap that hits nothing toggles the control bar -- the "tap to show, and
     * it hides itself again" the user asked for. */
    void onTouchAt(float x, float y);
    /* The touch panel state of the previous frame, so a hold counts as ONE tap. See the poll in tickControls. */
    bool hadTouchLastFrame = false;
    bool touchLogged = false;

    int chipMenuCount(int item) const;
    void chipMenuLabel(int item, int index, char* buf, int n) const;
    void chipMenuApply();
    /* The current values, for the log and for anything that needs to show them. */
    int controlVolume() const { return this->volume; }
    int controlSpeedIndex() const { return this->speedIndex; }
    int controlZoomIndex() const { return this->zoomIndex; }

    /* ---- the picture menu (opened with X) ----------------------------------
     *
     * The menu's state, its keys and the calls into mpv all live here rather than
     * in the Activity, because this view is the only object that can read the raw
     * controller state every frame and the only one holding the mpv handle. The
     * Activity owns the panel widget and decides where it sits in the tree.
     *
     * The keys are polled in draw() for the same reason the hold-to-seek keys are:
     * borealis' action repeat is a fixed 250 ms / 100 ms cadence with no
     * acceleration (application.cpp:65-66), which is too coarse to step a number by
     * five. Polling also means this feature adds no focusable view and no action to
     * the screen, so the seek keys and the A/B/Y actions keep behaving exactly as
     * they did before -- which is the property that matters most here. */
    void setMenuPanel(PlayerMenuPanel* panel) { this->menuPanel = panel; }

    /* THE PLAYER SETTINGS PANEL (the control bar's 设置 chip). Its box is owned by the activity and handed
     * in, exactly the way the picture menu's panel is. Public because the activity builds the panel. */
    void setSettingsPanel(class PlayerSettingsPanel* panel) { this->settingsPanel = panel; }
    bool settingsIsOpen() const { return this->settings.open != 0; }

    /* CALLED BY THE EVENT DRAIN WHEN mpv REPORTS EOF, i.e. an episode played to its end. It hands the decision
     * to the activity, which owns the episode list -- the view knows nothing about episodes and must not start
     * to. Public because the drain is a free function. */
    void notifyEpisodeEnded();

    /* ---- THE STREAM REPAIR: A NEW SIGNED ADDRESS FOR THE SAME EPISODE (build171) -------------------------
     *
     * WHY THIS IS A MEMBER AND NOT A FREE FUNCTION LIKE THE OTHER DRAIN HELPERS. The drain is a free
     * function that only has `Impl*`, and everything else it calls runs on PlayerView's PUBLIC surface
     * through `impl->owner` (notifyEpisodeEnded above is the model). This one cannot: the repair's whole
     * safety argument depends on re-reading `this->impl` at the moment its brls::sync callback runs and
     * returning if the screen is gone, and it needs `this->alive_` for the guard every other asynchronous
     * screen in this app uses. Both are private, and this header declares no friend -- so a free function
     * has no route to either, which is exactly how the first version of this failed to compile. It is
     * written the same way switchQuality is, for the same reason.
     *
     * WHAT IT DOES, in one line: mpv reported an error ending rather than EOF, so the (time-limited,
     * signed) stream address is treated as expired -- a fresh plan is fetched on a worker, and the reload
     * goes through setupMpv's build159 clean-stop path. At most ONE attempt per episode; see
     * Impl::repairAttempts in player_view.cpp, which also records why the reset-on-success cannot become a
     * loop.
     *
     * Public because the call site is the free function drainMpvEvents. It takes the two numbers from
     * mpv's own event so the log line can name them (reason=4 is an error ending; error is mpv's code). */
    void requestStreamRepair(int reason, int error);

    /* ---- THE PLAN, ON THE TIER WE WANT, BUILT BEFORE ANY LOADFILE (build143) -------------------------
     *
     * One ladder (cached per episode), one plan for the chosen source, one loadfile. 1080p when the episode
     * offers it, the best tier otherwise, and the old default path on any failure. See its definition in
     * player_view.cpp. Declared here because the plan worker is a lambda that reaches the view through `self`. */
    Result fetchPlanOnPreferredTier(const std::string& episodeId, HgPlayPlan& out);
    /* The single attempt behind it (build151): the retry wrapper calls this twice. */
    Result fetchPlanOnPreferredTierOnce(const std::string& episodeId, HgPlayPlan& out);

    /* The resume-after-quality-switch entry point (build135) is removed in build141: see the note in
     * player_view.cpp where switchQuality used to capture the position. */

    /* WHO RECORDS THE WATCH HISTORY: see hg_playback_sink.hpp, which exists so this header and
     * player_activity.hpp can both name the interface without including each other. */
    /* THE TV-STYLE TITLE (C14): what is playing, on the control bar. The activity knows the series and the episode
     * number, so it hands the finished line in; the view only draws it. Empty means "draw nothing". */
    void setOverlayTitle(std::string title) { this->overlayTitle = std::move(title); }

    void setHistorySink(PlaybackSink* sink) { this->historyOwner = sink; }
    void clearHistorySink() { this->historyOwner = nullptr; }

    /* ---- THE SERIES ID, WHICH THIS VIEW CANNOT OTHERWISE KNOW (build173) -------------------------
     *
     * WHY IT HAS TO BE HANDED IN. The play-info route needs BOTH ids: measured, the same vid answers
     * 200 under its own series_id and 400 with a wrong, empty or absent one. This view only ever had
     * the episode id -- hg_playback_sink.hpp says so in as many words ("the view knows the episode id
     * and the position, and nothing else") -- while the series lives in PlayerActivity. So the activity
     * passes it down once, the same way it already passes setOverlayTitle and setHistorySink.
     *
     * It is a SETTER rather than a constructor argument because the view is created by borealis'
     * XML/factory path (PlayerView::create) with no arguments, and the activity wires it up in
     * onContentAvailable. */
    void setSeriesId(std::string id) { this->seriesId = std::move(id); }
    const std::string& getSeriesId() const { return this->seriesId; }

    /* WHAT THE PLAYER DOES WHEN AN EPISODE ENDS (the settings panel's 播放方式 row). Read by the activity
     * when mpv reports EOF: it owns the episode list, so it is the one that can act on it. */
    bool playModeAdvances() const { return this->settings.playMode != 0; }
    bool menuIsOpen() const { return this->menu.open; }

    /* Called with 1 when the menu opens and 0 when it closes, on the main thread.
     * It exists so the Activity can take its overlay out of the panel's way without
     * this view needing to know what an overlay is.
     *
     * The Activity sets this to ITSELF and clears it in its destructor. That order
     * is the whole safety argument: borealis owns this view through the activity's
     * content view, so the view is destroyed before or with the activity, and the
     * only caller runs from draw() -- which cannot happen after the activity is
     * gone. A dangling pointer is therefore impossible, and no ownership is
     * implied. */
    void setMenuOverlayCallback(PlayerActivity* owner) { this->menuOwner = owner; }
    /* Called by the activity's destructor, so a stray frame can never reach a partly
     * destroyed activity. Cheap, and it makes the lifetime rule above enforceable
     * rather than merely true. */
    void clearMenuOverlayCallback() { this->menuOwner = nullptr; }

    /* Read or change the state from outside. The Activity seeds it from hongguo.cfg
     * at startup (so `player.brightness=20` in the file is an arm that needs no
     * rebuild) and clears it to "closed" before the first frame. */
    HgPMenu& menuState() { return this->menu; }
    const HgPMenu& menuState() const { return this->menu; }

    /* Push the current state into mpv: the five picture properties, the aspect
     * triple and the mirror filter. `source` is one of the HG_PMENU_SRC_* values
     * and goes into the log, which is what makes "the property did not change" and
     * "the property was never meant to" distinguishable after the fact. */
    void applyPictureSettings(int source);

    /* True when any row differs from the neutral defaults. Used for the log line on
     * capture, so a run with a cfg-seeded value says so. */
    bool pictureSettingsDifferFromDefaults() const;

    static brls::View* create();

    /* Public because the file-local poll thread and the setup helpers in
     * player_view.cpp take an Impl*. It is still an incomplete type to every
     * other translation unit (mpv/client.h stays out of this header). */
    struct Impl;

  private:
    Impl* impl = nullptr;
    /* drawCount: every draw of this view. mpvDraws: only the draws that enter
     * the mpv path. They differ because the first frames arrive before mpv
     * exists, so one counter cannot cover both. */
    int drawCount = 0;
    int mpvDraws = 0;

    /* Last status text pushed into the label, so draw() only calls setText
     * when it actually changed. */
    std::string lastStatus;

    /* ---- picture-menu state, all touched only on the main thread -------------
     *
     * `menu` is the whole state (see hg_pmenu.h). `menuPanel` is the on-screen
     * panel, owned by the activity's view tree; this view only tells it what to
     * show. `menuRevision` counts state changes so the panel is refreshed only when
     * something actually changed rather than every frame, and the X button and the
     * four direction keys are edge-detected against `prevButtons` because draw()
     * runs 60 times a second and a held key must not act as 60 presses. */
    HgPMenu menu;
    PlayerMenuPanel* menuPanel = nullptr;
    unsigned menuRevision = 0;
    /* The revision the panel was last drawn from. Compared every frame, which is
     * what makes the panel correct on the very frame the menu opens instead of one
     * edit later -- the alternative, pushing the state only from the key handler,
     * had exactly that one-frame hole in its first draft. */
    unsigned panelRevision = 0;
    bool prevButtons[brls::_BUTTON_MAX] = {};
    double menuRepeatAt = 0.0;
    bool menuRepeatArmed = false;
    PlayerActivity* menuOwner = nullptr;

    /* Draw the panel and drive its keys. Called from draw(), on the main thread. */
    void drawMenuTick();

    /* THE DRAG-TO-SEEK STATE (hg_scrub.h).
     *
     * `scrub` is the model. The scale and position it needs are NOT mirrored here: they live
     * in PlayerView::Impl next to `progress`, published by the poll thread under the same
     * atomic discipline, because that is the thread that reads them from mpv. Copying them
     * into the view would be a second location for the same fact. */
    HgScrub scrub;
    /* Counts drags that could not start, so the refusal is logged on the first two and then
     * only every 600th -- this runs per frame and the log fsyncs per line. */
    int scrubRefusals = 0;

    /* The episode this view is playing, kept so a quality switch can re-request the video model
     * for the same episode with a different source index. */
    std::string episodeId;

    /* The series this episode belongs to, handed in by PlayerActivity (build173) -- see setSeriesId.
     * Empty means "the caller did not say", in which case the play-info route refuses the request
     * rather than guessing: a wrong series_id is a 400, not a fallback. */
    std::string seriesId;

    /* THE QUALITY LADDER. Fetched from the network AFTER playback starts, because a quality
     * menu is worth a second's wait and playback is not -- see startQualityLadder.
     *
     * BOTH of these are touched on the main thread only: the worker hands its result back
     * through brls::sync, and the menu's row index is read from the same place. */
    void startQualityLadder(const std::string& episodeId);
    void switchQuality(int tierIndex);

    /* Records the tier the menu was showing when the row changed, so a switch is issued once per
     * change rather than once per frame. -1 means "nothing pending". */
    int lastQualityIndex = -1;

    /* The ladder's display labels, in menu order (best first), so the menu's quality row can
     * print "540p" instead of "3 of 5". Held as a fixed array of std::string rather than built
     * per frame: the panel's updateValues takes `const char* const*`, and a vector of C strings
     * has to own its storage somewhere. Rebuilt only when the ladder arrives. */
    std::string qualityLabels[HG_QUALITY_MAX];
    const char* qualityLabelPtrs[HG_QUALITY_MAX] = {};
    int qualityLabelCount = 0;

    /* Drive the drag and draw the bar. Called from draw(), on the main thread, and only
     * when the video has actually started. */
    void drawScrubTick(NVGcontext* vg, float x, float y, float width, float height);

    /* The preview panel: the target time, large, above the bar. */
    void drawScrubPreview(NVGcontext* vg, float x, float y, float width, float height,
                          const HgScrubBar& bar);

    /* True when the drag arm is in force (player.scrub=drag, the default). Read once. */
    bool dragSeekEnabled() const;

  private:

    /* THE SLEEP TIMER, checked once per frame. Zero minutes means off; a deadline in the past pauses playback
     * and switches itself off, which is the whole feature. */
    void tickSleepTimer();

    /* Open the picture menu, optionally highlighting `row`. Shared by X and by the quality chip: the two
     * entry points must not be able to leave the panel and the flag disagreeing. */
    void openPictureMenu(int row = -1, int haveRow = 0);

    /* THE CONTROL BAR'S OWN STATE. `controlsOn` is what A and a tap set; the idle timer is what clears it,
     * evaluated in draw() (the only per-frame hook this view has) rather than by a callback, because there
     * is nothing to schedule and a timer that has to be cancelled is a leak waiting to happen. */
    bool controlsOn = false;
    double controlsShownAt = 0.0;

    /* WHICH ITEM IS SELECTED, and the three values. The values live here (main thread) and reach mpv through
     * applyControlValue, which reads every write back -- a property write that is accepted and then does
     * nothing looks exactly like success, which is this project's most-repeated failure mode. */
    int controlsIndex = CONTROL_PROGRESS;

    /* SEE chipMenuCount: 0 = no pull-up menu, otherwise the CONTROL_* item it belongs to. */
    int chipMenu = 0;
    int chipMenuIndex = 0;

    /* A and B rising edges for the menu, tracked here for the same reason the D-pad flags are: this screen reads
     * the controller state itself rather than through borealis' focus system. */
    /* The remembered-tier member (build120) is removed in build142: "revert every change you made for the
     * default-to-1080p step." A quality switch now affects only the episode it was made on. */

    bool prevMenuA = false;
    bool prevMenuB = false;

    /* ---- THE TOUCH HIT AREAS (build87, the user's request: "there is no touch -- you can just do this sort of
     * thing along the way") ---------------------------------------------------------------------------------
     *
     * The bar and its pull-up menus are drawn with nanovg, so there is no brls view to tap: the geometry exists
     * only as the drawing code computes it. These rectangles are recorded WHILE DRAWING and hit-tested in
     * tickControls, which is the immediate-mode way to do this and keeps one source of truth for where a chip is
     * -- the alternative (a second copy of the layout arithmetic for hit-testing) is how a button ends up
     * responding somewhere it is not drawn. */
    struct HitRect {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        int item = 0;   /* the CONTROL_* item, or the option index inside the open menu */
        bool valid = false;
    };
    HitRect chipHit[5];
    HitRect menuHit[16];    /* enough for the longest ladder (speed has 7) */
    int menuHitCount = 0;
    HitRect volumeHit;
    /* THE PLAY BUTTON AND THE PROGRESS BAR ARE NOT CHIPS, and touch could not reach them because only the chips
     * recorded a rectangle (build111). See the note where they are filled in. */
    HitRect playHit;
    HitRect progressHit;
    /* True while a finger is dragging the volume bar, so a drag does not also become a tap. */
    bool touchingVolume = false;
    int volume = 100;
    int speedIndex = HG_CTL_SPEED_DEFAULT_INDEX;
    int zoomIndex = HG_CTL_ZOOM_DEFAULT_INDEX;
    /* Read the cfg seeds once, on the first time the bar is shown, so a cfg file with no player.* lines
     * behaves exactly as before this feature existed. */
    bool controlsSeeded = false;

    /* The rising-edge state of the four directions, for tickControls. The seek poll reads the same
     * buttons as LEVELS (hold to accelerate), so the bar cannot use tapped() -- see tickControls. */
    /* ---- the player settings panel's own state ---------------------------------
     *
     * Kept here rather than in a C model because there is nothing to compute: two values whose steps and
     * ranges live in hg_ctl (unit-tested), and a row index. */
    struct SettingsState {
        int open = 0;
        int row = 0;
        /* An index into hg_ctl's sleep ladder: 0 is off. */
        int sleepIndex = HG_CTL_SLEEP_DEFAULT_INDEX;
        /* 1 = keep the progress bar on screen even when the controls are hidden. */
        int barAlways = 0;
        /* ---- AUTO-NEXT IS THE DEFAULT NOW (build112, the user''s instruction) --------------------------------
         *
         * It used to default to 0 (stop at the end), and the switch that changed it lived in the player settings
         * panel -- which the user reports opens, shows nothing, and cannot be closed. Their instruction for this
         * round was "the settings that are not there, do not do them", and their earlier complaint was "episode 20
         * did not play the next one automatically", so the default is what should have carried this: a player
         * that stops at the end of every episode by default is the surprising behaviour, not the expected one.
         *
         * 0 = stop at the end, 1 = auto-next. */
        int playMode = 1;
        /* The moment the sleep timer fires (ms), or 0 when it is off. */
        double sleepDeadline = 0.0;
    };
    SettingsState settings;
    PlayerSettingsPanel* settingsPanel = nullptr;

    /* Poll the panel's keys. Called from draw() with the menu's own poll. */
    void tickSettings();

    /* A MENU COPY WITH ONLY THE MIRROR SET, for the `player.eq.mode=props` escape hatch: hg_pmenu_vf() is the
     * single place that builds the vf chain, so "the old behaviour" has to be expressed through it rather than by
     * a second writer. Cleared (all five zero) whenever the hatch is used. */
    HgPMenu mirrorOnly;

    /* ---- WHERE THE FRAME TIME GOES (I2) ---------------------------------------
     *
     * Accumulated inside draw() and reported once a second: mpv's software render (CPU colour conversion and
     * scaling), the texture upload, and the rest of the frame. See the note at the render call for why this
     * exists: "1080p is slow" does not say which of the three is slow. */
    double frameRenderMs = 0.0;
    double frameUploadMs = 0.0;
    double frameWindowStartMs = 0.0;
    int frameWindowCount = 0;

    /* The line the control bar draws at its top left. See setOverlayTitle. */
    std::string overlayTitle;

    PlaybackSink* historyOwner = nullptr;

    /* See hg_alive.hpp: the three brls::sync callbacks below capture this and must not touch a destroyed player. */
    AliveToken alive_ = makeAlive();
    /* When the next history record may be written. See the note in draw(). */
    double nextHistoryAt = 0.0;

    bool prevControlsUp = false;
    bool prevControlsDown = false;
    bool prevControlsLeft = false;
    bool prevControlsRight = false;

    void seedControlsFromCfg();
    /* Write volume/speed/zoom to mpv and log what mpv says back. */
    void applyControlValue(const char* why);
    /* Poll UP/DOWN/LEFT/RIGHT for the bar. Called from draw(), like the menu's own poll. */
    void tickControls();

    /* Draw the bar: the play/pause button, the progress bar and the clock. */
    void drawControlBar(NVGcontext* vg, float x, float y, float width, float height);

  public:
    brls::Label* statusLabel = nullptr;
};

} // namespace hg
