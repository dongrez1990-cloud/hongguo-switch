// library_activity.hpp -- the watch history and the watchlist, on one screen with two tabs.
//
// GPL-3.0-only.
//
// WHY ONE SCREEN WITH TWO TABS. The two lists hold the same kind of thing (a series with a picture) and are
// read the same way, so they share the grid, the card and the layout; what differs is the line under the title
// ("第 3 集 · 12:34 · 3 天前" versus when it was added). Two activities would be two copies of one layout.
//
// THE DATA IS LOCAL: hg_store reads sdmc:/switch/hongguo-history.txt and hongguo-fav.txt, so this screen draws
// with no network at all -- which is the point of storing titles and covers in those files (hg_history.h says
// so where the fields are declared).
//
// ASCII-only source file.

#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

#include "hg_alive.hpp"
#include "hg_client.hpp"

namespace hg {

class LibraryActivity : public brls::Activity {
  public:
    /* `startOnFavorites` is what the home screen's bar passes: it already knows which item was pressed, and
     * making the user switch tabs after choosing 收藏 would be asking twice. */
    explicit LibraryActivity(bool startOnFavorites);
    ~LibraryActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    /* Which list is showing. */
    enum Tab { TAB_HISTORY = 0, TAB_FAVORITES = 1, TAB_COUNT };

    void showTab(Tab tab);
    /* Re-read the two files and rebuild the current list. Bound to X, and also what a tab press does --
     * the files can change while this screen is open (the player writes history), so "refresh" is a real
     * action here rather than decoration. */
    void reload();
    /* Turn the store's entries into the grid's row model. The two lists differ only in the line under the
     * title, which is why this is one function with a branch. */
    void applyEntries();

    brls::Button* tabButtons[TAB_COUNT] = {nullptr, nullptr};
    brls::Label* statusLabel = nullptr;
    brls::View* grid = nullptr;

    Tab current = TAB_HISTORY;
    std::vector<Series> shown;
    std::vector<std::string> subtitles;

    /* See hg_alive.hpp: every async callback captures this and checks dead() before touching this screen. */
    AliveToken alive_ = makeAlive();
};

} // namespace hg
