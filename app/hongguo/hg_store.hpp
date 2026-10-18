// hg_store.hpp -- the two local files this app owns: watch history and the watchlist.
//
// GPL-3.0-only.
//
// WHY THIS EXISTS AS A CLASS AND NOT AS CALLS TO hg_history. hg_history.c has the FORMAT (parse, emit, insert,
// find, remove -- 92 host checks), and it is deliberately file-agnostic: it takes a path. What was missing is
// the part that knows WHERE the files live, when to load them, and where the one in-memory copy lives.
//
// ONE COPY, IN ONE PLACE. The console has no file watcher, and the two writers -- the player (history) and the
// detail screen (the watchlist) -- plus the readers (the home screen's bar, and the two screens that do not
// exist yet) must all see the same list. Two copies is how "I added it to my list and it did not appear"
// happens, and it is the reason this is a single process-wide store rather than a struct passed around.
//
// WHAT IT DOES NOT DO: it does not draw anything, and it does not decide what a button means. Each screen asks
// for a count or toggles one entry; docs/待做清单.md D2 has the screens themselves.
//
// ASCII-only source file.

#pragma once

#include <string>
#include <vector>

#include "hg_client.hpp"

extern "C" {
#include "hg_history.h"
}

namespace hg {

class Store {
  public:
    /* Load both files once, on first use. Safe to call from any thread; the load happens under the store's
     * own mutex, and a second caller waits rather than loading twice. */
    static void ensureLoaded(void);

    /* THROW AWAY THE IN-MEMORY COPY AND READ THE FILES AGAIN.
     *
     * The library screen's X key. hg_store loads once per process, so without this a screen opened after the
     * player wrote a history record would show the list from before it -- the exact "I watched something and
     * the list did not change" report. Safe to call at any time; the load itself is the same code path as the
     * first one, including the skipped-line count it logs. */
    static void reloadFromDisk(void);

    /* How many entries each file holds. This is what the home screen's bar needs to stop dimming 收藏 and
     * 历史 -- the exact question the sidebar model's `haveWatchlist`/`haveHistory` flags were built to answer. */
    static int historyCount(void);
    static int favoritesCount(void);

    static bool isFavorite(const std::string& seriesId);
    /* Add or remove, and SAVE. Returns true when the series is now a favorite. */
    static bool toggleFavorite(const Series& series);

    /* ---- DROP ONE SERIES FROM THE CONTINUE-WATCHING LIST (build129, the user's request) ------------------
     *
     * THEIR WORDS: "for this one series I need a way to clear the history, because I never really watched 222
     * episodes of it, but now it says 222."
     *
     * They are right that it can happen: a record is written once an episode has been watched past the thirty
     * second mark (see the player), and a series opened at its end -- or a stream that resumes at the wrong
     * position -- can leave a row claiming the last episode. There was no way to remove it, so the only fix was to
     * edit the file by hand.
     *
     * Returns how many rows were removed, and saves when that is not zero. */
    static int clearHistoryFor(const std::string& seriesId);

    /* Record that this series' episode was playing, at `position` seconds. Called on start and then
     * periodically, so the newest record wins and the list stays newest-first. Saves every time: a console
     * session can end with the power button, and a position that only reached memory is a position lost. */
    static void recordPlayback(const Series& series, const std::string& episodeId,
                               const std::string& episodeLabel, double position, double duration);

    /* Read-only copies, newest first, for the screens that will show them (D2). */
    static std::vector<HgHistoryEntry> history(void);
    static std::vector<HgFavEntry> favorites(void);

    /* The paths, for the log and for the About screen. */
    static const char* historyPath(void);
    static const char* favoritesPath(void);

  private:
    static void saveLocked(void);
};

} // namespace hg
