// search_activity.hpp -- the search screen: the system keyboard, then the results.
//
// GPL-3.0-only.
//
// WHY THIS SCREEN IS SHORT. Everything hard is borrowed:
//
//   * the KEYBOARD is the console's own. borealis' Switch platform wraps libnx's software keyboard
//     (switch_ime.cpp: swkbdCreate/swkbdShow with SwkbdType_All), which is the keyboard the user
//     already knows and -- the reason it is the right choice here -- the one that has the system's
//     IME behind it, so typing Latin letters gives Chinese through PINYIN. Drawing our own on-screen
//     keyboard would mean reimplementing an input method to get a worse version of it.
//   * the DATA comes from the same server-rendered page every other screen uses, parsed by
//     hg_api_parse_search_page -- see hg_api.h for the measured shape (the card is nested under
//     `video_data`, the id is `keyword`).
//   * a result OPENS THE EXISTING DETAIL PAGE: a search hit is a Series like any other.
//
// WHAT THIS SCREEN HONESTLY CANNOT DO, and says on screen instead of pretending: the page carries one
// page of results (10) and offers no way to ask for the next one -- the signed search API that used to
// serve pagination answers 200 with an empty body. The page does report the total (e.g. 201), so the
// status line shows "显示 10 条（共 201 条）" rather than implying those ten are everything.
//
// ASCII-only source file.

#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

#include "hg_alive.hpp"
#include "hg_client.hpp"
#include "home_activity.hpp"   /* makePosterGrid: the home page's poster grid, reused here */

namespace hg {

class SearchActivity : public brls::Activity {
  public:
    SearchActivity();
    ~SearchActivity() override;

    brls::View* createContentView() override;

    /* B GOES BACK (registered explicitly -- borealis registers no back action of its own, which this
     * project has already paid for once on the detail screen), and Y/A opens the keyboard again. */
    void onContentAvailable() override;

  private:
    /* Open the console's keyboard with the current query as its initial text, and search with what
     * comes back. Called once when the screen appears and again on Y or A. */
    void openKeyboard();

    /* Fetch on a worker thread and hand the result to applyResults on the main thread. */
    void runSearch(const std::string& keyword);

    /* Replace the list with these results. Runs on the main thread in both fetch modes.
     *
     * `upstreamCount`/`indexSize` are what let the status line be honest about WHERE the results came from
     * -- upstream search hands over ten and no more (measured), the rest are local index matches. */
    void applyResults(std::vector<Series> got, const Result& r, long long total, size_t upstreamCount,
                      size_t indexSize);

    void setStatus(const std::string& text);

    brls::Label* statusLabel = nullptr;

    /* THE KEYWORD BUTTON, which is also why this screen always has somewhere to put the focus.
     *
     * MEASURED-INTO-THE-DESIGN rather than discovered later: with no results yet, the only views here
     * would be labels and a non-focusable scroller, so the framework would have nothing to focus and
     * the focus would stay on the HOME page's cell underneath -- the regression this project already
     * recorded once ("pressing A on the detail page kept re-opening detail pages"). A focusable button
     * showing the query ("按 A 输入关键词" before the first search) is a real control the user wants
     * anyway, and it makes the empty state impossible to get stuck in. */
    brls::Button* queryButton = nullptr;

    /* THE RESULTS, AS THE HOME PAGE'S POSTER GRID. Created empty in createContentView and re-filled by
     * setRows for every search -- the same lifecycle the home page uses, which is what makes "a second
     * search cannot show the first search's posters" true by construction. */
    brls::View* resultsGrid = nullptr;

    /* The last keyword that was searched, which is also the keyboard's initial text. */
    std::string query;

    /* 1 while a fetch is in flight: a second submit must not race the first (two workers writing the
     * same shared parse buffers is the kind of thing that looks like a parse bug for a week). */
    bool searching = false;

    /* See hg_alive.hpp: every async callback captures this and checks dead() before touching this screen. */
    AliveToken alive_ = makeAlive();

    /* How many results upstream says exist, or -1 when it did not say. */
    long long totalResults = -1;
};

} // namespace hg
