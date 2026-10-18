// category_activity.cpp -- see the header for what this screen is and what it reuses.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "category_activity.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <utility>

#include "detail_activity.hpp"
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"
#include "home_activity.hpp"

extern "C" {
#include "hg_lang.h"
}

namespace hg {

CategoryActivity::CategoryActivity(std::string tier, std::string categorySlug)
    : tierTitle(std::move(tier)), slug(std::move(categorySlug)) {}

CategoryActivity::~CategoryActivity() {
    /* TELL EVERY CALLBACK IN FLIGHT THAT THIS SCREEN IS GONE. See hg_alive.hpp: without this line the next
     * fetch to finish would run brls::sync against freed memory -- the crash in the reports. */
    this->alive_->kill();
    /* SEVER THE GRID'S BACK-POINTER, the way the player severs its menu callback.
     *
     * The callback the grid holds captures `this`, and it is only reachable from the grid's own draw() -- which
     * walks the ACTIVITY's view tree, so a popped activity's grid is never drawn again and the callback could
     * not fire. That reasoning holds today; the one line makes it hold by construction, and the cost is a null
     * check. This project has paid for "it cannot happen" once already (the standing clearMenuOverlayCallback in
     * player_activity.cpp exists for the same reason). */
    if (this->grid) setPosterGridNearBottom(this->grid, nullptr);
}

brls::View* CategoryActivity::createContentView() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(24.0f);

    auto* title = new brls::Label();
    title->setText(this->tierTitle);
    title->setFontSize(26.0f);
    title->setMarginBottom(4.0f);
    box->addView(title);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(16.0f);
    statusLabel->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    statusLabel->setText(hg_tr(HG_T_LOADING));
    statusLabel->setMarginBottom(10.0f);
    box->addView(statusLabel);

    /* THE HOME PAGE'S GRID, EMPTY UNTIL THE FIRST PAGE LANDS. See makePosterGrid: same columns, same
     * poster size, same scrolling, same row-step navigation -- "arranged like the home page" by
     * construction. */
    grid = makePosterGrid(std::vector<HomeRow>(), [](const Series& s) {
        hg::logPrintf("CATEGORY: opening detail for '%s' (id=%s)", s.title.c_str(), s.id.c_str());
        brls::Application::pushActivity(new DetailActivity(s));
    });
    box->addView(grid);

    /* KEEP SCROLLING = KEEP LOADING. The grid fires this whenever its bottom is near; this screen decides
     * whether that means anything (already loading? out of pages?) -- see loadNextPage. */
    setPosterGridNearBottom(this->grid, [this]() { this->loadNextPage(); });

    hg::logPrintf("CATEGORY: screen built for tier '%s' (slug '%s'; paging on) [build41]",
                  this->tierTitle.c_str(), this->slug.c_str());
    return hg::withHintBar(box);
}

void CategoryActivity::onContentAvailable() {
    if (this->grid) {
        auto* owner = this->grid->getParent();
        if (owner) {
            owner->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, [](brls::View*) {
                hg::logPrintf("CATEGORY: B pressed -> popActivity (deferred one frame)");
                /* Deferred: the action is registered on this activity's own view, so popping inside the
                 * callback destroys the view the dispatcher is walking. Rule 5 of the handover. */
                brls::sync([]() { brls::Application::popActivity(); });
                return true;
            });
        }
    }
    this->loadPage(1);
}

void CategoryActivity::setStatus(const std::string& text) {
    if (this->statusLabel) this->statusLabel->setText(text);
}

void CategoryActivity::loadNextPage() {
    if (this->loading || this->exhausted) return;
    this->loadPage(this->nextPage);
}

void CategoryActivity::loadPage(int page) {
    if (this->loading) {
        hg::logPrintf("CATEGORY: page %d asked for while page %d is in flight -- ignored", page,
                      this->nextPage);
        return;
    }
    /* THE PAGE'S OWN COUNT IS THE END-OF-LIST GUARD. totalPages is 0 when the page did not say, and then
     * the only signal left is an empty page (handled in applyResults). */
    if (this->totalPages > 0 && page > this->totalPages) {
        this->exhausted = true;
        hg::logPrintf("CATEGORY: page %d is past totalPages=%d -- nothing more to load", page,
                      this->totalPages);
        return;
    }

    this->loading = true;
    this->nextPage = page;
    if (this->loadedItems == 0) this->setStatus(hg_tr(HG_T_LOADING));

    CategoryActivity* self = this;
    const AliveToken alive = this->alive_;
    const std::string slug = this->slug;
    bool started = hg::runDetached([self, alive, slug, page]() {
        std::vector<Series> got;
        HgApiPagination info;
        Result r = Client::fetchCategory(slug, page, got, &info);
        hg::logPrintf("CATEGORY: page %d ok=%d n=%u total=%d pages=%d err='%s'", page, (int)r.ok,
                      (unsigned)got.size(), info.total, info.total_pages, r.error.c_str());
        brls::sync([self, alive, got, r, info, page]() {
            HG_GUARD_ALIVE(alive);
            self->applyResults(got, r, info, page);
        });
    });

    if (!started) {
        hg::logPrintf("CATEGORY: no worker available -- fetching page %d inline", page);
        std::vector<Series> got;
        HgApiPagination info;
        Result r = Client::fetchCategory(slug, page, got, &info);
        /* THE INLINE PATH TOO: it cannot race (there is no callback), but the guard costs nothing and keeps the
         * two paths the same shape. */
        if (!alive->dead()) this->applyResults(std::move(got), r, info, page);
    }
}

void CategoryActivity::applyResults(std::vector<Series> got, const Result& r,
                                    const HgApiPagination& info, int page) {
    this->loading = false;

    if (!this->grid) {
        hg::logPrintf("CATEGORY: the view is gone; dropping page %d", page);
        return;
    }

    if (!r.ok) {
        /* A FAILED PAGE DOES NOT CLEAR THE LIST. The pages already loaded are still good, so the status
         * line reports the failure and the user keeps what is on screen -- and the next scroll tries
         * again, because `exhausted` is left alone. */
        this->setStatus(r.error);
        brls::Application::notify(std::string("Category page failed: ") + r.error);
        hg::logPrintf("CATEGORY: page %d failed '%s' (the %u series already loaded stay on screen)", page,
                      r.error.c_str(), (unsigned)this->loadedItems);
        return;
    }

    if (info.total_pages > 0) this->totalPages = info.total_pages;
    if (info.total > 0) this->totalItems = info.total;

    if (got.empty()) {
        /* AN EMPTY PAGE IS THE END, even when the count said otherwise: the last page of a category whose
         * total is not a multiple of the page size is short, and a page past the end is empty. Either way
         * there is nothing more to ask for. */
        this->exhausted = true;
        this->setStatus(this->loadedItems ? std::string(hg_tr(HG_T_SEARCH_SHOWING)) +
                                                " " + std::to_string(this->loadedItems)
                                          : std::string(hg_tr(HG_T_SEARCH_EMPTY)));
        hg::logPrintf("CATEGORY: page %d came back with no series -- treating that as the end", page);
        return;
    }

    std::vector<HomeRow> rows;
    rows.reserve(got.size());
    for (size_t i = 0; i < got.size(); i++) {
        HomeRow row;
        row.header = false;
        row.series = got[i];
        rows.push_back(std::move(row));
    }

    /* PAGE 1 REPLACES, EVERY LATER PAGE APPENDS. Appending is not an optimisation here: recreating the
     * cells already on screen would empty their cover slots and re-fetch every picture (see
     * appendPosterGridRows). */
    if (page <= 1) {
        setPosterGridRows(this->grid, rows);
    } else {
        appendPosterGridRows(this->grid, rows);
    }
    this->loadedItems += got.size();
    this->nextPage = page + 1;

    char b[192];
    if (this->totalItems > 0) {
        snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)this->loadedItems);
        char tail[64];
        snprintf(tail, sizeof tail, hg_tr(HG_T_SEARCH_TOTAL), (long long)this->totalItems);
        this->setStatus(std::string(b) + tail);
    } else {
        snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)this->loadedItems);
        this->setStatus(b);
    }

    /* FOCUS THE GRID ON THE FIRST PAGE ONLY: later pages arrive while the user is looking at the bottom
     * of the list, and moving their focus then would be the app fighting the hand. */
    if (page <= 1) brls::Application::giveFocus(this->grid);

    hg::logPrintf("CATEGORY: page %d showed %u series (%u loaded so far, %d pages total)", page,
                  (unsigned)got.size(), (unsigned)this->loadedItems, this->totalPages);
}

} // namespace hg
