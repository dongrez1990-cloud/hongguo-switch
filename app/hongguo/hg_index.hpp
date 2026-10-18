// hg_index.hpp -- a local index of the catalogue, so a search can return more than upstream's ten.
//
// GPL-3.0-only.
//
// WHY THIS EXISTS, in the user's words: "搜索栏显示有比如 AI 有 199 个结果，但是还是只显示 10 个".
// MEASURED: the search page hands over ONE page of ten results and offers no way to ask for the next
// (fourteen parameter spellings tried, all ignored -- see docs/搜索网格与分类页-实现.md §3), because the
// pagination that used to serve it lives in a signed API that now answers 200 with an empty body.
//
// THE CATEGORY PAGES DO PAGE, THOUGH. `/category/<slug>?page=N` was measured to return different lists per
// page and the page states its own `pagination` block (total 800, 24 per page, 34 pages for comic-drama).
// So the app can read a real slice of the catalogue and search THAT, locally, and show it alongside
// upstream's ten -- which is what this does.
//
// WHAT IT IS NOT, and the screen has to say so: it is NOT upstream's search. The index is whatever pages
// were read (a few hundred titles), so a keyword upstream would match in its other 19 pages is not here.
// The status line therefore reports upstream's own total separately from the local matches, and this
// header exists so nobody has to guess that.
//
// BUILT ONCE, IN THE BACKGROUND, LAZILY. Filling it costs one request per page, so it runs on a worker
// while the user is still typing (the console's keyboard is modal, which is free time). A search that
// happens before it finishes simply has fewer local matches -- `size()` reports what is there, and the
// screen shows it.
//
// ASCII-only source file.

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "hg_client.hpp"

namespace hg {

class SearchIndex {
  public:
    /* Fill the index if it has not been filled. BLOCKING and worker-thread ONLY (it fetches).
     *
     * `pagesPerCategory` bounds the cost: the measured category pages hold 24 series each and a full
     * category is 34 pages, so this fetches the first `pagesPerCategory` pages of each of the three
     * categories this app shows. It stops early when a page comes back short (that is the end).
     *
     * Safe to call more than once from any number of workers: the second caller waits for the first one's
     * result instead of fetching again. */
    static Result build(int pagesPerCategory);

    /* Have we finished? The screen uses this to say whether a thin result list is "nothing matched" or
     * "the index is still being read". */
    static bool ready(void);

    /* How many titles the index holds right now. Usable from the main thread at any time. */
    static size_t size(void);

    /* The indexed series whose TITLE contains `query` (see hg_api's hg_title_matches for the matching
     * rules), up to `cap`. Usable from the main thread; returns what is available, so a caller that runs
     * during the build gets a partial answer rather than a block. */
    static std::vector<Series> match(const std::string& query, size_t cap);

    /* A stable summary for the log and the status line. */
    static std::string describe(void);

  private:
    /* The build's own mutex: the index is written by one worker while the main thread reads it. Held for
     * the duration of the build as well, so two workers cannot fetch the same pages twice. */
    static std::mutex& mutex(void);
    static std::vector<Series>& items(void);
    static int& state(void);   /* 0 = not built, 1 = building, 2 = built */
};

} // namespace hg
