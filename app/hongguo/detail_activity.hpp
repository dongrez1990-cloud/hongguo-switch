/*
 * Detail activity: the selected series' synopsis and episode picker.
 *
 * GPL-3.0-only.
 *
 * MEASURED: the detail page is a 509 KB server-rendered HTML document whose
 * _ROUTER_DATA carries seriesDetail.vid_list[], an array of BARE episode-id
 * strings. The series this client's home page yielded first had 76 of them.
 * There are no per-episode titles upstream, so episodes are labelled by
 * position and the id is kept in the log rather than on every button -- inventing
 * a title would be fabricating data the API does not provide.
 *
 * TWO LAYOUTS, AND WHY BOTH STILL EXIST. The episode list shipped first (one row per
 * episode, 76 rows) and that version is the one verified on hardware. wiliwili's
 * detail page uses a grid for its episode picker, and 76 rows is a lot of scrolling
 * for a list whose entries are just numbers. So the picker is now a grid of numbered
 * buttons by DEFAULT, and the list is kept as the way back:
 *
 *   sdmc:/switch/hongguo.cfg    detail.episodes=list
 *
 * Selecting an episode (A) starts playback.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

#include "hg_alive.hpp"
#include "hg_client.hpp"

namespace hg {

class EpisodeGridView;

class DetailActivity : public brls::Activity {
  public:
    explicit DetailActivity(const Series& series);
    ~DetailActivity() override;

    brls::View* createContentView() override;

    /* B GOES BACK, REGISTERED EXPLICITLY.
     *
     * MEASURED ON HARDWARE: "in the episode picker I cannot get back to the preview, but from the
     * player I can get to the episode picker". So forward navigation worked and the way back did
     * not, on a screen that had no B action of its own.
     *
     * borealis does not register a back action for B anywhere by itself -- checked, not assumed: the
     * only B actions in the library are in dialog.cpp and in this app's own player. So a screen that
     * wants B to leave has to say so. The list version of this screen never had one either; it went
     * unnoticed because B appeared to work as long as HomeActivity's own registrations were in play,
     * and with a grid the focus sits deeper in the tree.
     *
     * Returning true means "handled", so the press is consumed here rather than reaching whatever
     * else might claim it. */
    void onContentAvailable() override;

  private:
    /* Applies a finished episode fetch: header text, then the grid or the list. Runs
     * on the main thread in both fetch modes. Builds the cells only when there are
     * rows -- an empty RecyclerFrame is the out-of-bounds path documented in
     * home_activity.cpp. */
    void applyEpisodes(std::vector<Episode> got, const Result& r);

    brls::Label* headerLabel = nullptr;
    /* The watchlist state line (build124). See createContentView: it names the key and shows whether this series
     * is already in the list. */
    brls::Label* favLabel = nullptr;
    /* "Watched to episode N, mm:ss / mm:ss" from the history store (build125). */
    brls::Label* histLabel = nullptr;
    /* When a "press X again to confirm" window closes (ms). See the X action: clearing the history is
     * destructive, so it takes two presses (build130). */
    double clearArmedUntil = 0.0;

    /* The cover thumbnail and the line that states its state. Both live as
     * members so the lifetime is the activity's, not a lambda's -- see
     * hg_image.hpp for the ptrLock discipline that keeps them valid across the
     * asynchronous fetch. */
    brls::Image* coverImage = nullptr;
    brls::Label* coverStatus = nullptr;

    /* THE LIST (detail.episodes=list). Created in createContentView with a NULL data
     * source: it has to exist when the activity is pushed so borealis gives it focus,
     * and it must not be laid out with zero rows. See home_activity.cpp's
     * createContentView for the full reasoning -- both conditions are met by borealis'
     * `if (dataSource)` guard around the whole of reloadData. */
    brls::RecyclerFrame* frame = nullptr;

    /* THE GRID (the default). Same reasoning about existing before the data: it is a
     * focusable ScrollingFrame from the start, so focus has somewhere to land. Exactly
     * one of `frame` and `episodeGrid` is non-null. */
    EpisodeGridView* episodeGrid = nullptr;

    /* The synopsis, if upstream sent one.
     *
     * MEASURED AND UNRELIABLE: `desc` (from series_intro) is present on some captures
     * and absent on others -- one capture had 0 of 36 cards with it, another had two
     * over 500 bytes. `tags` (from category_list) is present on 36/36. So the synopsis
     * falls back to the tags, and when BOTH are empty the block is hidden entirely
     * rather than leaving an empty bordered box that looks like a loading failure. */
    brls::Label* synopsis = nullptr;

    Series series;
    std::vector<Episode> episodes;
    std::string status;

    /* ---- THE EPISODE GROUPS (the user's design: 20 at a time, one open at a time) ----------------------
     *
     * A 200-episode series used to build 200 focusable cells and destroy them all in one frame on the way out,
     * which crashed the console every time. Now only one group of HG_EPISODE_GROUP is built, `groupItems` holds
     * that slice (the grid keeps a POINTER to what it is given, so it cannot be a temporary), and `groupButtons`
     * is the selector row that switches between them. */
    int groupIndex = 0;
    std::vector<Episode> groupItems;
    brls::Box* groupRow = nullptr;
    std::vector<brls::Button*> groupButtons;

    void buildGroupSelector();
    void showGroup(int index, bool initial);

    /* See hg_alive.hpp: every async callback captures this and checks dead() before touching this screen. */
    AliveToken alive_ = makeAlive();
};

} // namespace hg
