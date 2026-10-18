// hg_index.cpp -- see hg_index.hpp for why a local index exists at all.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_index.hpp"

#include <cstring>
#include <mutex>

#include "hg_log.hpp"

extern "C" {
#include "hg_api.h"
#include "hg_index.h"   /* the merge policy: duplicates, the cap, the order -- with host tests */
}

namespace hg {
namespace {

/* THE TWO CONVERSIONS BETWEEN THE C STRUCT THE POLICY FUNCTIONS TAKE AND THE C++ ONE THIS CLASS STORES.
 *
 * They are explicit field copies rather than a memcpy or a reinterpret_cast: the two types exist for different
 * reasons (the C one is fixed-size and testable on the host, the C++ one is what the UI passes around), and
 * treating them as the same layout is how a silent truncation gets in -- snprintf here at least truncates
 * visibly and predictably rather than reading past a different-sized field. */
void seriesToC(const Series& s, HgSeries* out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", s.id.c_str());
    snprintf(out->title, sizeof out->title, "%s", s.title.c_str());
    snprintf(out->cover, sizeof out->cover, "%s", s.cover.c_str());
    snprintf(out->desc, sizeof out->desc, "%s", s.desc.c_str());
    snprintf(out->badge, sizeof out->badge, "%s", s.badge.c_str());
    snprintf(out->tags, sizeof out->tags, "%s", s.tags.c_str());
}

Series seriesFromC(const HgSeries& in) {
    Series s;
    s.id = in.id;
    s.title = in.title;
    s.cover = in.cover;
    s.desc = in.desc;
    s.badge = in.badge;
    s.tags = in.tags;
    return s;
}

} // namespace

std::mutex& SearchIndex::mutex(void) {
    static std::mutex m;
    return m;
}

std::vector<Series>& SearchIndex::items(void) {
    static std::vector<Series> v;
    return v;
}

int& SearchIndex::state(void) {
    /* 0 = not built, 1 = a build is in progress, 2 = built. */
    static int s = 0;
    return s;
}

bool SearchIndex::ready(void) {
    std::lock_guard<std::mutex> lock(mutex());
    return state() == 2;
}

size_t SearchIndex::size(void) {
    std::lock_guard<std::mutex> lock(mutex());
    return items().size();
}

Result SearchIndex::build(int pagesPerCategory) {
    /* THE WHOLE BUILD HOLDS THE LOCK, deliberately: two workers asking at the same time (the screen
     * starts the build, and a search can start it too) must not fetch the same eighteen pages twice. The
     * cost is that a search waits for an in-flight build instead of reading a partial index -- which is
     * the right trade, because a partial index is exactly the thing the user complained about. */
    std::lock_guard<std::mutex> lock(mutex());

    Result r;
    if (state() == 2) {
        r.ok = true;
        return r;
    }
    if (state() == 1) {
        /* Cannot happen while the lock is held, but leaving the state machine total is cheaper than
         * reasoning about it. */
        r.ok = true;
        return r;
    }
    state() = 1;

    /* THE THREE CATEGORIES THIS APP SHOWS, in the order the home screen shows them. `/category` itself is
     * NOT here: it was measured to return exactly what /category/real-drama returns (the same 24 ids), so
     * fetching it would double that category's titles for nothing. */
    static const char* SLUGS[] = {"real-drama", "comic-drama", "ai-drama"};
    const int maxPages = pagesPerCategory < 1 ? 1 : (pagesPerCategory > 40 ? 40 : pagesPerCategory);

    items().clear();
    int pagesFetched = 0;
    for (size_t s = 0; s < sizeof SLUGS / sizeof SLUGS[0]; s++) {
        for (int page = 1; page <= maxPages; page++) {
            std::vector<Series> got;
            HgApiPagination info;
            Result one = Client::fetchCategory(SLUGS[s], page, got, &info);
            if (!one.ok) {
                /* A FAILED PAGE ENDS THIS CATEGORY, not the build: the other two are independent, and a
                 * half-built index is still worth having (the screen reports how many titles it holds). */
                hg::logPrintf("INDEX: %s page %d failed '%s' -- that category stops here", SLUGS[s], page,
                              one.error.c_str());
                break;
            }
            if (got.empty()) {
                hg::logPrintf("INDEX: %s page %d was empty -- that category ends here", SLUGS[s], page);
                break;
            }
            pagesFetched++;
            /* THE MERGE POLICY IS IN C WITH TESTS (hg_index.h): duplicate-by-id, a hard cap, and the order the
             * pages arrived in. It used to be a nested loop inside this member function, which talks to the
             * network and therefore cannot be reached from the host tests.
             *
             * THE TWO ARRAYS ARE STATIC, and that is deliberate: HG_INDEX_MAX is 512 and one HgSeries is about
             * 1.4 KB, so 1.4 MB of locals would sit on a worker thread's stack -- hg_thread's stacks are small and
             * this project has already had to reason about worker-thread lifetime once (see hg_thread.hpp). */
            static HgSeries buf[HG_INDEX_MAX];
            static HgSeries incoming[HG_INDEX_MAX];

            const size_t have = items().size() < (size_t)HG_INDEX_MAX ? items().size() : (size_t)HG_INDEX_MAX;
            for (size_t i = 0; i < have; i++) seriesToC(items()[i], &buf[i]);

            const size_t want = got.size() < (size_t)HG_INDEX_MAX ? got.size() : (size_t)HG_INDEX_MAX;
            for (size_t i = 0; i < want; i++) seriesToC(got[i], &incoming[i]);

            size_t count = have;
            HgIndexMerge merged;
            if (hg_index_append(buf, &count, HG_INDEX_MAX, incoming, want, &merged) != 0) {
                hg::logPrintf("INDEX: the merge refused page %d of %s -- keeping what is already there", page,
                              SLUGS[s]);
                break;
            }
            items().clear();
            items().reserve(count);
            for (size_t i = 0; i < count; i++) items().push_back(seriesFromC(buf[i]));

            hg::logPrintf("INDEX: %s page %d -> %u series (%d new, %d duplicate, %d over the cap, %u total) [build68]",
                          SLUGS[s], page, (unsigned)got.size(), merged.added, merged.duplicate,
                          merged.overflow, (unsigned)items().size());
            if (merged.overflow > 0) {
                /* A FULL INDEX STOPS THE BUILD rather than paging for ever: without this, an upstream paging bug
                 * would grow the list until the console runs out of memory -- a crash whose cause is nowhere
                 * near its symptom. */
                hg::logPrintf("INDEX: the index is full (%d entries) -- stopping here", HG_INDEX_MAX);
                break;
            }

            /* THE PAGE'S OWN COUNT STOPS US EARLY: no point asking for page 20 of a category that has 6. */
            if (info.total_pages > 0 && page >= info.total_pages) break;
            /* AND A SHORT PAGE IS THE OTHER END-OF-LIST SIGNAL (the last page is not always full). */
            if (info.page_size > 0 && (int)got.size() < info.page_size) break;
        }
    }

    state() = 2;
    hg::logPrintf("INDEX: ready -- %u series from %d pages across 3 categories [build42]",
                  (unsigned)items().size(), pagesFetched);
    r.ok = true;
    return r;
}

std::vector<Series> SearchIndex::match(const std::string& query, size_t cap) {
    std::vector<Series> out;
    std::lock_guard<std::mutex> lock(mutex());
    for (size_t i = 0; i < items().size() && out.size() < cap; i++) {
        if (hg_title_matches(items()[i].title.c_str(), query.c_str())) out.push_back(items()[i]);
    }
    return out;
}

std::string SearchIndex::describe(void) {
    std::lock_guard<std::mutex> lock(mutex());
    char b[64];
    snprintf(b, sizeof b, "%u titles", (unsigned)items().size());
    return std::string(b);
}

} // namespace hg
