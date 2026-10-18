/*
 * Player activity: mpv on deko3d, with audio.
 *
 * GPL-3.0-only.
 *
 * WHAT IS ALREADY PROVEN. The A/B on hardware established that
 * --demuxer-lavf-o=decryption_key=<hex> works: with the derived key mpv
 * decoded frames (time-pos advanced, picture-type appeared); with no key or a
 * wrong key it produced none. So the CENC path is measured, and this file only
 * has to hand mpv the right options and give it a surface.
 *
 * WHAT THIS FILE CHANGES, AND WHY THAT IS THE RISKY PART:
 *
 *   1. AUDIO. The verified test ran ao=null, i.e. sound has NEVER been
 *      exercised. It also ran vo=null. Both were right for a decoder test and
 *      neither is acceptable in a player. Here `ao` is left UNSET so mpv uses
 *      its platform default; naming one explicitly would be a guess about how
 *      this particular mpv was built, and repeating ao=null would repeat the
 *      exact gap that makes audio unverified.
 *
 *   2. VIDEO OUT. wiliwili's proven deko3d configuration is followed exactly
 *      (see player_view.cpp): MPV_RENDER_API_TYPE_DEKO3D with
 *      MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS carrying borealis' own dk::Device,
 *      ADVANCED_CONTROL on, and every frame rendered straight into borealis'
 *      current framebuffer behind a fence pair. That sequence is copied rather
 *      than re-derived because its failure mode is a black screen with no
 *      error to diagnose.
 *
 * ARCHITECTURE NOTE. Drawing needs a brls::View: brls::Activity has no
 * virtual draw() (only View does), so the framebuffer work lives in
 * PlayerView and this Activity is the container. That is also why the header
 * is thin -- mpv/client.h does not need to leak into every includer.
 *
 * NOT VERIFIED: none of this has run. The console screen is not visible from
 * the build machine, so "it plays" is an inference from the measured pieces,
 * not an observation.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "hg_playback_sink.hpp"
#include "hg_client.hpp"   /* Episode: the sibling list that makes 上一集/下一集 possible */

namespace hg {

class PlayerView;
class PlayerMenuPanel;
class PlayerSettingsPanel;

class PlayerActivity : public brls::Activity, public PlaybackSink {
  public:
    /* THE WHOLE EPISODE LIST, NOT JUST THE ONE ID.
     *
     * F1 (ZL/ZR = previous/next episode) and "what happens when an episode ends" both need the sibling ids, and
     * the detail page already HAS them -- it is the list the user just picked from. Passing them here costs a
     * few strings and saves a second fetch of the detail page; it also means the two screens cannot disagree
     * about which episode is "next" (the list is the same object, in the same order).
     *
     * `index` is 1-based for display ("第 3 集"), matching what the detail page passes today. */
    PlayerActivity(Series series, std::vector<Episode> episodes, int index);

    /* ---- what happens at the end, and the two keys that jump (F1) ----------------
     *
     * Called by the view when mpv reports EOF, i.e. the episode played to its end. Whether anything happens
     * depends on the play mode the user chose in the settings panel (停在结尾 / 自动下一集). */
    void onEpisodeEnded();

    /* PlaybackSink. The view reports the position; this class is what knows the SERIES, so it is
     * what can write a history record. */
    void recordPlayback(const std::string& episodeId, double position, double duration) override;

    /* THE EPISODE THAT COULD NOT BE STARTED (round-26 audit). `startEpisode` advanced the index BEFORE asking
     * the view to play, because the label and the ZL/ZR arithmetic read it -- so a failed play-plan request has
     * to be reported back, or this screen would keep calling the previous episode "episode N+1" and ZR would
     * skip one. Putting the index back is the whole fix. */
    void playbackFailed(const std::string& reason) override;

    /* THE CONTROL BAR IS UP, SO THE OVERLAY ROW STANDS DOWN. A user photographed both progress indicators and both
     * copies of the episode label on screen together; this is the fix, and it uses the same rule as the picture
     * menu (menuOverlayHidden) so there is one idea rather than two. */
    void controlBarVisible(bool visible) override;

    /* ZL/ZR: step to the previous or next episode. Refuses at the ends with a reason rather than wrapping --
     * an episode list is not a ring, and jumping from the last episode back to the first looks like a bug. */
    void switchEpisode(int delta);


    /* The label the overlay shows: the series title with the episode number after it. */
    std::string episodeLabel() const;
    ~PlayerActivity() override;

    brls::View* createContentView() override;

    /* Input mapping on this activity: A toggles pause (or closes the picture menu),
     * B goes back (or closes it), Y toggles the overlay, X opens the picture menu
     * (polled in PlayerView, registered here for the on-screen hint). */
    void onContentAvailable() override;

    /* Called from PlayerView::draw -- which is on the main thread -- with 1 when the
     * picture menu opens and 0 when it closes. It hides the overlay text and the
     * progress bar while the panel is up: the panel is opaque and covers the right
     * 460 logical pixels, and the progress bar in particular is drawn by PlayerView
     * as nanovg rectangles in the same recorded frame as the panel's, so which of
     * the two wins would depend on record order. */
    void setMenuOverlay(int open);

    /* THIS SCREEN IS OPAQUE, AND SAYING SO IS LOAD-BEARING.
     *
     * borealis' Application::frame() walks the activity stack from the top and
     * keeps collecting views "until we find one that's not translucent"
     * (application.cpp:770-780), then draws the collected ones bottom-up. An
     * activity is translucent when its content view is, and
     * View::isTranslucent() is `fadeIn || inFadeAnimation` -- so a page that
     * still carries a pending fade-in drags the page underneath it into every
     * frame.
     *
     * That matters here more than anywhere else, because of HOW the two kinds of
     * drawing are submitted: nanovg records the whole UI during the frame and
     * flushes it in nvgEndFrame (application.cpp:811), while
     * mpv_render_context_render pushes its commands into the deko3d queue
     * immediately, from inside the view's draw. The GPU therefore executes:
     *
     *     clear -> mpv video -> all recorded nanovg UI
     *
     * so anything borealis draws -- including the episode list of the activity
     * underneath -- lands ON TOP of the video. On the console that looked like
     * "the player shows no picture; the detail page is still there with the
     * player's text over it, and the video flashes for an instant on exit". */
    bool isTranslucent() override { return false; }

  private:
    /* THE MEMBERS ARE DECLARED IN THE ORDER THE CONSTRUCTOR INITIALISES THEM (seriesTitle, episodes,
     * episodeIndex): any other order is a -Wreorder warning, and this project builds with zero warnings --
     * which is how a real mistake gets noticed at all. */
    Series series;
    std::string episodeId;
    int index;

    /* THE EPISODE LIST AND WHERE WE ARE IN IT. `episodeIndex` is 1-based for display; the vector index is
     * `episodeIndex - 1`. Both are kept because the display number is what the logs and the label use, and
     * mixing the two is the classic off-by-one. */
    std::vector<Episode> episodes;
    int episodeIndex = 1;

    /* FOR playbackFailed: where we were before the last jump, and whether there has been one. The FIRST
     * episode is started by the constructor, which has nothing to go back to. */
    int previousIndex = 0;
    bool lastStartWasAJump = false;

    /* The id of the episode we are on, or "" when the list is empty. */
    std::string currentEpisodeId() const;
    void startEpisode(int oneBasedIndex, const char* why);

    /* Y shows/hides the two overlay lines. This is wiliwili's OSD toggle
     * (video_view.cpp:466-496 registers A for the same job in its TV control
     * mode). Kept as members so the toggle can reach them. */
    void toggleOverlay();

    /* Owned by borealis once the content view is set; kept here only so the
     * activity can wire input to it. */
    PlayerView* playerView = nullptr;

    /* Also owned by the view tree (added to the player view's root, last, so it
     * draws on top). This pointer is only used to show/hide it; PlayerView drives it
     * from its own draw. */
    PlayerMenuPanel* menuPanel = nullptr;

    /* THE PLAYER SETTINGS PANEL (the control bar's 设置 chip). Owned by the view tree like the picture
     * menu's; this pointer is only for creating it and handing it over. */
    PlayerSettingsPanel* settingsPanel = nullptr;

    /* ZL/ZR can arrive in the same input batch (or repeat while the previous
     * stream is still being torn down).  Keep one switch per short window so
     * two loadfile workers cannot overlap and leave audio/video out of sync. */
    std::chrono::steady_clock::time_point lastEpisodeSwitch{};

    
    
    
    /* OFF BY DEFAULT, AND THAT IS THE USER'S INSTRUCTION RATHER THAN A PREFERENCE.

     * MEASURED (their own photographs + message, 2026-10-10): with the overlay on, the picture carried a title
     * row and a progress line whether or not Y had been pressed ("whether I press Y or not there is this line,
     * and I do not need that information"), and the instruction for the default state was explicit:
     * "normally only the playing content, so nothing is covered".
     *
     * So the row starts hidden and Y is the one key that asks for it. Whenever the control bar is up, this stays
     * hidden anyway (see controlBarVisible) because the bar carries the same title and its own progress bar. */
    bool overlayShown = false;

    /* True while the picture menu has forced the overlay out of the way, so that
     * syncMenuOverlay() is a no-op on every frame but the two that change it. */
    bool menuOverlayHidden = false;

    /* True while the control bar is up -- see controlBarVisible. */
    bool controlBarUp = false;
};

} // namespace hg
