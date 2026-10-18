// hg_store.cpp -- see hg_store.hpp for why the files' location and lifetime live in their own class.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_store.hpp"

#include <cstring>
#include <ctime>   /* time(): the record's own timestamp */
#include <mutex>

#include "hg_log.hpp"
#include "hg_thread.hpp"

namespace hg {
namespace {

/* BESIDE hongguo.cfg, with the same prefix: sdmc:/switch/. A user who wants to back them up, inspect them or
 * delete one entry by hand looks in the same directory as everything else this app owns. */
const char* const HISTORY_PATH = "sdmc:/switch/hongguo-history.txt";
const char* const FAV_PATH = "sdmc:/switch/hongguo-fav.txt";

std::mutex& lock(void) {
    static std::mutex m;
    return m;
}

std::vector<HgHistoryEntry>& historyList(void) {
    static std::vector<HgHistoryEntry> v;
    return v;
}

std::vector<HgFavEntry>& favList(void) {
    static std::vector<HgFavEntry> v;
    return v;
}

bool& loaded(void) {
    static bool b = false;
    return b;
}

/* Copy a std::string into one of the fixed-size C fields, always NUL-terminated. Truncation is a real
 * possibility (a 512-byte cover URL, a 128-byte title) and is REFUSED rather than silently cut: a half URL is
 * a broken image with no explanation, and the caller can say so in the log. */
bool copyField(char* dst, size_t cap, const std::string& src) {
    if (cap == 0) return false;
    if (src.size() >= cap) return false;
    memcpy(dst, src.c_str(), src.size());
    dst[src.size()] = '\0';
    return true;
}

} // namespace

const char* Store::historyPath(void) { return HISTORY_PATH; }
const char* Store::favoritesPath(void) { return FAV_PATH; }

void Store::reloadFromDisk(void) {
    {
        std::lock_guard<std::mutex> lk(lock());
        loaded() = false;   /* the next ensureLoaded() re-reads; see the header */
    }
    hg::logPrintf("STORE: re-reading both files (the library screen asked)");
    ensureLoaded();
}

void Store::ensureLoaded(void) {
    std::lock_guard<std::mutex> lk(lock());
    if (loaded()) return;
    loaded() = true;

    HgHistoryEntry h[HG_HIST_MAX];
    int skippedH = 0;
    const int nh = hg_history_load(HISTORY_PATH, h, HG_HIST_MAX, &skippedH);
    historyList().assign(h, h + nh);

    /* ---- FILES WRITTEN BEFORE "ONE ROW PER SERIES" EXISTED ARE NORMALISED HERE (build128) ---------------
     *
     * MEASURED ON HARDWARE, from the user: after the write path was changed to merge by series (build127) the
     * history still listed one row per episode for the same drama. Merging on WRITE cannot clean rows that are
     * already in the file, and the whole point of the change is that "continue watching" holds one row per series.
     *
     * The merge runs HERE rather than inside the loader because the loader's contract is "give me the file's rows
     * in the file's order" -- hg_history_test checks exactly that ("the first good record is first"), and it caught
     * this when the merge was briefly put there. So the file is read faithfully and normalised afterwards, through
     * the same hg_history_put() the live recording uses: it matches on the series id, keeps the row naming the
     * episode watched last, and re-sorts newest first.
     *
     * The result is written back only when something actually merged, so a clean file is not rewritten on every
     * launch. */
    if (nh > 0) {
        HgHistoryEntry merged[HG_HIST_MAX];
        int nm = 0;
        for (int i = 0; i < nh; i++) nm = hg_history_put(merged, nm, HG_HIST_MAX, &h[i]);
        historyList().assign(merged, merged + nm);
        if (nm != nh) {
            hg::logPrintf("STORE: merged %d history rows into %d -- one row per series (older files wrote one row "
                          "per episode) [build128]",
                          nh, nm);
            saveLocked();
        }
    }

    HgFavEntry f[HG_FAV_MAX];
    int skippedF = 0;
    const int nf = hg_fav_load(FAV_PATH, f, HG_FAV_MAX, &skippedF);
    favList().assign(f, f + nf);

    /* THE SKIPPED COUNT IS LOGGED, not swallowed: a malformed line is a lost record, and "my history lost an
     * entry" is exactly the kind of report that needs an earlier log line to explain it. */
    hg::logPrintf("STORE: loaded %d history entries (%d skipped) from %s", nh, skippedH, HISTORY_PATH);
    hg::logPrintf("STORE: loaded %d favorites (%d skipped) from %s", nf, skippedF, FAV_PATH);
}

int Store::historyCount(void) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());
    return (int)historyList().size();
}

int Store::favoritesCount(void) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());
    return (int)favList().size();
}

bool Store::isFavorite(const std::string& seriesId) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());
    return hg_fav_find(favList().data(), (int)favList().size(), seriesId.c_str()) >= 0;
}

std::vector<HgHistoryEntry> Store::history(void) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());
    return historyList();
}

std::vector<HgFavEntry> Store::favorites(void) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());
    return favList();
}

void Store::saveLocked(void) {
    const int n = hg_history_save(HISTORY_PATH, historyList().data(), (int)historyList().size());
    hg::logPrintf("STORE: saved %d history entries (rc=%d) to %s", (int)historyList().size(), n, HISTORY_PATH);
}

/* ---- DROP ONE SERIES FROM THE CONTINUE-WATCHING LIST (build129, the user's request) --------------------
 *
 * THEIR WORDS: "for this one series I need a way to clear the history, because I never really watched 222
 * episodes of it, but now it says 222."
 *
 * A row is written once an episode has been watched past the thirty second mark (see the player), and a series
 * opened at its end -- or a stream that resumes somewhere unexpected -- can leave a row claiming the last episode.
 * Until now there was no way to remove it short of editing the file by hand.
 *
 * Filtering in place and saving once, which is what toggleFavorite does for its own file. */
int Store::clearHistoryFor(const std::string& seriesId) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());

    std::vector<HgHistoryEntry> keep;
    keep.reserve(historyList().size());
    for (size_t i = 0; i < historyList().size(); i++) {
        if (seriesId == historyList()[i].seriesId) continue;
        keep.push_back(historyList()[i]);
    }

    const int removed = (int)historyList().size() - (int)keep.size();
    if (removed > 0) {
        historyList() = keep;
        saveLocked();
        hg::logPrintf("STORE: cleared %d history row(s) for series %s -- %d remain [build129]", removed,
                      seriesId.c_str(), (int)keep.size());
    }
    return removed;
}
bool Store::toggleFavorite(const Series& series) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());

    const int at = hg_fav_find(favList().data(), (int)favList().size(), series.id.c_str());
    std::vector<HgFavEntry> out(favList());
    bool nowFavorite;

    if (at >= 0) {
        const int n = hg_fav_remove(out.data(), (int)out.size(), series.id.c_str());
        out.resize(n > 0 ? (size_t)n : 0);
        nowFavorite = false;
        hg::logPrintf("STORE: removed '%s' from the watchlist (%d left)", series.title.c_str(),
                      (int)out.size());
    } else {
        HgFavEntry e;
        memset(&e, 0, sizeof e);
        if (!copyField(e.seriesId, sizeof e.seriesId, series.id) ||
            !copyField(e.title, sizeof e.title, series.title) ||
            !copyField(e.cover, sizeof e.cover, series.cover)) {
            /* REFUSE RATHER THAN TRUNCATE: a record with half a URL is worse than no record, because it looks
             * like it worked. */
            hg::logPrintf("STORE: refusing to add '%s' -- a field is too long for the file format", series.title.c_str());
            return false;
        }
        HgFavEntry tmp[HG_FAV_MAX];
        const size_t copy = out.size() < HG_FAV_MAX ? out.size() : HG_FAV_MAX;
        memcpy(tmp, out.data(), copy * sizeof(HgFavEntry));
        const int n = hg_fav_add(tmp, (int)copy, HG_FAV_MAX, &e);
        out.assign(tmp, tmp + n);
        nowFavorite = true;
        hg::logPrintf("STORE: added '%s' (id=%s) to the watchlist (%d total)", series.title.c_str(),
                      series.id.c_str(), n);
    }

    favList() = out;
    const int rc = hg_fav_save(FAV_PATH, favList().data(), (int)favList().size());
    hg::logPrintf("STORE: saved %d favorites (rc=%d) to %s", (int)favList().size(), rc, FAV_PATH);
    return nowFavorite;
}

void Store::recordPlayback(const Series& series, const std::string& episodeId,
                           const std::string& episodeLabel, double position, double duration) {
    ensureLoaded();
    std::lock_guard<std::mutex> lk(lock());

    HgHistoryEntry e;
    memset(&e, 0, sizeof e);
    if (!copyField(e.seriesId, sizeof e.seriesId, series.id) ||
        !copyField(e.title, sizeof e.title, series.title) ||
        !copyField(e.cover, sizeof e.cover, series.cover) ||
        !copyField(e.episodeId, sizeof e.episodeId, episodeId) ||
        !copyField(e.episodeLabel, sizeof e.episodeLabel, episodeLabel)) {
        hg::logPrintf("STORE: not recording '%s' -- a field is too long for the file format",
                      series.title.c_str());
        return;
    }
    e.position = position;
    e.duration = duration;
    e.when = (long long)time(nullptr);

    HgHistoryEntry tmp[HG_HIST_MAX];
    const size_t copy = historyList().size() < HG_HIST_MAX ? historyList().size() : HG_HIST_MAX;
    memcpy(tmp, historyList().data(), copy * sizeof(HgHistoryEntry));
    /* hg_history_put moves an existing series to the front and stamps it with the new time, so the list stays
     * newest-first with one entry per SERIES (not per episode) -- the model's own rule, tested there. */
    const int n = hg_history_put(tmp, (int)copy, HG_HIST_MAX, &e);
    historyList().assign(tmp, tmp + n);

    /* ---- THE SAVE GOES TO A WORKER, NOT TO THE FRAME LOOP (build147, the user's report) ----------------
     *
     * THEIR WORDS: "episode four played to twelve seconds fine; switching to episode five, it happened at
     * seventeen." Ten, twelve, seventeen -- not a fixed cadence, which is what a PERIODIC writer looks like, and
     * this is that writer: the player ticks this every fifteen seconds (see player_view.cpp), and this function
     * used to end in saveLocked() -- a whole-file write to a temporary file, a rename, and an fsync, ON THE THREAD
     * THAT DRAWS FRAMES. On an SD card that is tens to hundreds of milliseconds, so the picture hitched, mpv's
     * audio clock ran ahead, and mpv caught up in one garbled burst. Same class of fault as the per-line log
     * fsync fixed in build145; this is simply the other writer.
     *
     * NOTHING ABOUT CRASH-SAFETY CHANGES: the file is still written every fifteen seconds, with the same format,
     * the same atomic rename and the same fsync -- just not while a frame is waiting to be drawn. The snapshot is
     * taken here, under the store's lock, and the worker writes its own copy, so no lock is held across I/O and
     * there is no data race. A small mutex serialises two saves that happen to overlap (a favourite toggle can
     * save at the same moment). */
    {
        static std::mutex saveMutex;
        std::vector<HgHistoryEntry> snapshot(tmp, tmp + n);
        const std::string path = HISTORY_PATH;
        if (!hg::runDetached([snapshot, path]() {
                std::lock_guard<std::mutex> saveLock(saveMutex);
                HgHistoryEntry arr[HG_HIST_MAX];
                const size_t k = snapshot.size() < HG_HIST_MAX ? snapshot.size() : HG_HIST_MAX;
                if (k) memcpy(arr, snapshot.data(), k * sizeof(HgHistoryEntry));
                hg_history_save(path.c_str(), arr, (int)k);
            })) {
            /* No thread available: the record still lives in memory and the next one will try again. Losing one
             * save is better than stalling the frame loop to keep it. */
            hg::logPrintf("STORE: no worker for the history save -- the record stays in memory until the next one");
        }
    }
}

} // namespace hg
