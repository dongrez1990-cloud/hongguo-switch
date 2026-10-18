// category_activity.hpp -- one category page's list, opened by a home tier's "more" card.
//
// GPL-3.0-only.
//
// WHY THIS IS SHORT. It is the same poster grid the home page uses (makePosterGrid), fed by the same
// parser the home page uses (Client::fetchCategory -> hg_api_parse_home), on a page whose items carry the
// same field names. What is here is the paging: keep loading while the user scrolls.
//
// IT PAGES, AND THE PAGE SAYS HOW FAR. MEASURED (app/src/tools/probe_category_paging.mjs):
// `/category/comic-drama?page=2` and `?page=3` return DIFFERENT lists (compared by first id, not by
// count), `?page=35` is a 404, and the page carries
// `loaderData.category_$.pagination = { total: 800, pageNum: 1, pageSize: 24, totalPages: 34 }`.
// So a full comic-drama category is 800 series over 34 pages, and this screen walks them.
//
// ASCII-only source file.

#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

#include "hg_alive.hpp"   /* the liveness token: see hg_alive.hpp for the crash it fixes */
#include "hg_client.hpp"

namespace hg {

class CategoryActivity : public brls::Activity {
  public:
    /* `tierTitle` is what the card said (the tier's heading), used for the screen's own title; `slug` is
     * the category page's path segment, from Client::categorySlugForTier. */
    CategoryActivity(std::string tierTitle, std::string slug);
    ~CategoryActivity() override;

    brls::View* createContentView() override;

    /* B goes back, registered explicitly -- borealis registers no back action of its own. */
    void onContentAvailable() override;

  private:
    /* Fetch one page on a worker and apply it on the main thread. */
    void loadPage(int page);
    void applyResults(std::vector<Series> got, const Result& r, const HgApiPagination& info, int page);
    /* Called by the grid when it is scrolled near its bottom -- see setPosterGridNearBottom. */
    void loadNextPage();
    void setStatus(const std::string& text);

    brls::Label* statusLabel = nullptr;
    brls::View* grid = nullptr;

    std::string tierTitle;
    std::string slug;

    /* HOW FAR WE HAVE GOT, from the page's own pagination block rather than from arithmetic on counts:
     * `total_pages` of 0 means "the page did not say", and then the only end-of-list signal is a page
     * that comes back with no series. */
    int nextPage = 1;
    int totalPages = 0;
    int totalItems = 0;
    size_t loadedItems = 0;

    /* 1 while a page is in flight. The near-bottom hook fires repeatedly by design, so this is what makes
     * it idempotent -- and `exhausted` is what stops it asking for a page that does not exist. */
    bool loading = false;
    bool exhausted = false;

    /* See hg_alive.hpp: every async callback captures this and checks dead() before touching this screen. */
    AliveToken alive_ = makeAlive();
};

} // namespace hg
