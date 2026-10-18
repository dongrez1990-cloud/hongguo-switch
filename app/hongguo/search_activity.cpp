// search_activity.cpp -- see the header for what this screen is and what it borrows.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "search_activity.hpp"

#include <borealis.hpp>
#include <cstdio>

#include "detail_activity.hpp"
#include "hg_image.hpp"
#include "hg_index.hpp"
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"
#include "hg_touch.hpp"

extern "C" {
#include "hg_lang.h"
}

namespace hg {

SearchActivity::SearchActivity() = default;
SearchActivity::~SearchActivity() {
    /* Tell every callback in flight that this screen is gone -- see hg_alive.hpp. */
    this->alive_->kill();
}

brls::View* SearchActivity::createContentView() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(24.0f);

    auto* title = new brls::Label();
    title->setText(hg_tr(HG_T_SEARCH));
    title->setFontSize(26.0f);
    title->setMarginBottom(4.0f);
    box->addView(title);

    /* THE KEYWORD BUTTON. See the note in the header: it is the screen's always-present focus stop,
     * and its label doubles as the hint before the first search. */
    queryButton = new brls::Button();
    queryButton->setText(hg_tr(HG_T_SEARCH_HINT));
    queryButton->setFontSize(18.0f);
    queryButton->setMarginTop(8.0f);
    queryButton->setMarginBottom(4.0f);
    queryButton->registerClickAction([this](brls::View*) {
        this->openKeyboard();
        return true;
    });
    hg::installTapSelect(queryButton);
    box->addView(queryButton);

    /* THE STATUS LINE CARRIES THE COUNT, THE ERROR OR "NO RESULTS" -- and nothing else, because the
     * hint now lives on the button above. */
    statusLabel = new brls::Label();
    statusLabel->setFontSize(16.0f);
    statusLabel->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    statusLabel->setText("");
    statusLabel->setMarginBottom(10.0f);
    box->addView(statusLabel);

    /* THE RESULTS ARE THE HOME PAGE'S OWN POSTER GRID -- same columns, same poster size, same
     * scrolling, same UP/DOWN-with-a-row-step. See makePosterGrid: it is the home grid, not a copy of
     * it, so "arranged the way the home page arranges things" is true by construction rather than by
     * my having matched two layouts by eye.
     *
     * It is created EMPTY, before any search, and filled by setRows on each result -- the same
     * lifecycle the home page uses, and the reason a second search cannot leave the previous query's
     * cells on screen. */
    resultsGrid = makePosterGrid(std::vector<HomeRow>(), [](const Series& s) {
        hg::logPrintf("SEARCH: opening detail for '%s' (id=%s)", s.title.c_str(), s.id.c_str());
        brls::Application::pushActivity(new DetailActivity(s));
    });
    box->addView(resultsGrid);

    hg::logPrintf("SEARCH: screen built (the keyboard opens on the first frame) [build39]");
    return hg::withHintBar(box);
}

void SearchActivity::onContentAvailable() {
    auto back = [](brls::View*) {
        hg::logPrintf("SEARCH: B pressed -> popActivity (deferred one frame)");
        /* DEFERRED, because the action is registered on this activity's own view: popping inside the
         * callback destroys the view the dispatcher is walking. Same rule and same reason as the
         * detail screen's B action. */
        brls::sync([]() { brls::Application::popActivity(); });
        return true;
    };
    auto keyboard = [this](brls::View*) {
        this->openKeyboard();
        return true;
    };

    if (this->statusLabel) {
        auto* view = this->statusLabel->getParent();
        if (view) {
            view->registerAction(hg_tr(HG_T_SEARCH), brls::ControllerButton::BUTTON_Y, keyboard);
            view->registerAction(hg_tr(HG_T_SEARCH), brls::ControllerButton::BUTTON_A, keyboard);
            view->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, back);
            hg::logPrintf("SEARCH: A/Y open the keyboard, B leaves");
        }
    }

    /* THE KEYBOARD OPENS BY ITSELF THE FIRST TIME, one frame after the screen is up. Opening it
     * directly from onContentAvailable would run it in the middle of the push transition, and the
     * screen behind it is not drawn yet -- a frame's delay costs nothing and the user sees the
     * search page with the keyboard already up, which is the whole point of pressing Y. */
    {
        const AliveToken alive = this->alive_;
        brls::sync([this, alive]() {
            /* THE KEYBOARD CANNOT OPEN ON A SCREEN THAT IS GONE: this task is posted for the NEXT frame, and B can
             * pop the screen in this one. Without the guard that is a freed-object call -- see hg_alive.hpp. */
            HG_GUARD_ALIVE(alive);
            this->openKeyboard();
        });
    }

    /* AND START READING THE LOCAL INDEX, ON A WORKER, WHILE THE USER TYPES.
     *
     * The console's keyboard is modal, so the seconds spent typing are free time; by the time the user
     * submits, the index is usually complete and a search has hundreds of titles to match against instead
     * of upstream's ten. See hg_index.hpp for why the index exists at all. A search submitted before the
     * build finishes still works -- it just has fewer local matches, and the status line says how many
     * titles it searched. */
    if (!hg::runDetached([]() { (void)SearchIndex::build(6); })) {
        hg::logPrintf("SEARCH: no worker for the index -- the first search will build it instead");
    }
}

void SearchActivity::openKeyboard() {
    if (this->searching) {
        hg::logPrintf("SEARCH: a search is in flight -- the keyboard stays closed");
        return;
    }
    brls::ImeManager* ime = brls::Application::getImeManager();
    if (!ime) {
        /* NO KEYBOARD IS NOT A CRASH. Saying so on screen is the honest failure: a search screen that
         * silently ignores Y looks like a dead key. */
        this->setStatus("no keyboard available");
        hg::logPrintf("SEARCH: the platform has no IME manager -- Y does nothing");
        return;
    }
    hg::logPrintf("SEARCH: opening the system keyboard (initial text '%s')", this->query.c_str());
    ime->openForText(
        [this](std::string text) {
            hg::logPrintf("SEARCH: keyboard returned %u bytes", (unsigned)text.size());
            if (text.empty()) {
                hg::logPrintf("SEARCH: empty keyword -- nothing to search for");
                return;
            }
            this->runSearch(text);
        },
        hg_tr(HG_T_SEARCH), hg_tr(HG_T_SEARCH_SUB), 40, this->query);
}

void SearchActivity::runSearch(const std::string& keyword) {
    if (this->searching) {
        hg::logPrintf("SEARCH: already searching -- ignoring '%s'", keyword.c_str());
        return;
    }
    this->searching = true;
    this->query = keyword;
    this->totalResults = -1;
    this->setStatus(std::string(hg_tr(HG_T_SEARCH_BUSY)) + "  " + keyword);
    hg::logPrintf("SEARCH: searching for '%s'", keyword.c_str());

    SearchActivity* self = this;
    const AliveToken alive = this->alive_;
    bool started = hg::runDetached([self, alive, keyword]() {
        std::vector<Series> got;
        long long total = -1;
        Result r = Client::fetchSearch(keyword, got, total);
        const size_t upstreamCount = got.size();

        /* THE LOCAL INDEX, WHICH IS WHERE "MORE THAN TEN" COMES FROM. Read on the same worker: the user is
         * waiting anyway, and the index is usually already built (the screen starts it when it appears --
         * see onContentAvailable). A partial index reads as fewer matches, never as a wrong answer, and
         * `indexSize` is reported so the screen can say what it searched. */
        (void)SearchIndex::build(6);
        const size_t indexSize = SearchIndex::size();
        std::vector<Series> local = SearchIndex::match(keyword, 200);
        for (size_t i = 0; i < local.size(); i++) {
            /* UPSTREAM'S COPY WINS when a series is in both: its card fields came from the search page,
             * which is what the user actually asked about. */
            bool dup = false;
            for (size_t k = 0; k < got.size(); k++) {
                if (got[k].id == local[i].id) {
                    dup = true;
                    break;
                }
            }
            if (!dup) got.push_back(local[i]);
        }

        hg::logPrintf("SEARCH: '%s' -> upstream ok=%d gave %u; index of %u titles added %u (total %u, "
                      "upstreamTotal=%lld) [build76]",
                      keyword.c_str(), (int)r.ok, (unsigned)upstreamCount, (unsigned)indexSize,
                      (unsigned)(got.size() - upstreamCount), (unsigned)got.size(), total);
        brls::sync([self, alive, got, r, total, upstreamCount, indexSize]() {
            /* THE SEARCH RESULT IS THE MOST LIKELY OF ALL OF THESE TO ARRIVE AFTER THE SCREEN IS GONE: the user
             * types a keyword, submits, and can leave while the fetch runs. This guard was MISSING -- found by the
             * round-36 review that counted the guards against the callbacks in every file (the other five matched;
             * this one did not). */
            HG_GUARD_ALIVE(alive);
            self->applyResults(std::move(got), r, total, upstreamCount, indexSize);
        });
    });

    if (!started) {
        /* NO WORKER: search inline. A frozen screen for a second is far better than a status line
         * that says "searching" for ever -- the same choice the home screen makes. */
        hg::logPrintf("SEARCH: no worker available -- searching inline");
        std::vector<Series> got;
        long long total = -1;
        Result r = Client::fetchSearch(keyword, got, total);
        const size_t upstreamCount = got.size();
        (void)SearchIndex::build(6);
        const size_t indexSize = SearchIndex::size();
        std::vector<Series> local = SearchIndex::match(keyword, 200);
        for (size_t i = 0; i < local.size(); i++) {
            bool dup = false;
            for (size_t k = 0; k < got.size(); k++) {
                if (got[k].id == local[i].id) {
                    dup = true;
                    break;
                }
            }
            if (!dup) got.push_back(local[i]);
        }
        this->applyResults(std::move(got), r, total, upstreamCount, indexSize);
    }
}


void SearchActivity::setStatus(const std::string& text) {
    if (this->statusLabel) this->statusLabel->setText(text);
}

void SearchActivity::applyResults(std::vector<Series> got, const Result& r, long long total,
                                  size_t upstreamCount, size_t indexSize) {
    this->searching = false;
    this->totalResults = total;

    /* The button shows what is being searched for, so it is also the answer to "what did I type".
     * Set before the early returns below: the query is a fact about the screen, not about whether the
     * fetch worked. */
    if (this->queryButton && !this->query.empty()) this->queryButton->setText(this->query);

    if (!this->resultsGrid) {
        hg::logPrintf("SEARCH: the view is gone; dropping the result");
        return;
    }

    if (!r.ok) {
        char b[256];
        snprintf(b, sizeof b, "%s: %s", hg_tr(HG_T_SEARCH), r.error.c_str());
        this->setStatus(b);
        setPosterGridRows(this->resultsGrid, std::vector<HomeRow>());
        hg::logPrintf("SEARCH: reported failure '%s'", r.error.c_str());
        return;
    }

    if (got.empty()) {
        /* A SEARCH THAT MATCHED NOTHING IS NOT AN ERROR, and it is not a blank screen either. The total is
         * shown when the page stated one, because "没有找到" with 0 known results and "没有找到" after the page
         * said 201 are different facts. */
        this->setStatus(hg_tr(HG_T_SEARCH_EMPTY));
        setPosterGridRows(this->resultsGrid, std::vector<HomeRow>());
        hg::logPrintf("SEARCH: no results for '%s' (total=%lld, index=%u)", this->query.c_str(), total,
                      (unsigned)indexSize);
        return;
    }

    /* THE COUNT LINE, AND IT NAMES BOTH SOURCES. This is the answer to "it says 199 results but shows ten":
     * upstream's own ten are separated from the local index's matches, because a list that silently mixes
     * "everything the site has" with "what our few hundred indexed titles matched" would be a lie about
     * either one. When there are no local matches the line is the old one. */
    char b[224];
    const size_t localCount = got.size() > upstreamCount ? got.size() - upstreamCount : 0;
    snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)got.size());
    std::string status(b);
    if (localCount > 0) {
        char mix[128];
        snprintf(mix, sizeof mix, hg_tr(HG_T_SEARCH_MIXED), (unsigned)upstreamCount,
                 (unsigned)localCount);
        status += mix;
    }
    if (total > 0) {
        char tail[64];
        snprintf(tail, sizeof tail, hg_tr(HG_T_SEARCH_TOTAL), total);
        status += tail;
    }
    this->setStatus(status);

    /* THE RESULT LIST BECOMES THE GRID'S ROW MODEL: one card row per hit, no headings. That is exactly
     * the shape the home page feeds the grid, which is what makes the two screens look the same. */
    std::vector<HomeRow> rows;
    rows.reserve(got.size());
    for (size_t i = 0; i < got.size(); i++) {
        HomeRow row;
        row.header = false;
        row.series = got[i];
        rows.push_back(std::move(row));
    }
    setPosterGridRows(this->resultsGrid, rows);

    /* FOCUS THE GRID, so A opens the first result and the D-pad moves through the posters without a
     * click first. When there are no rows the button above keeps the focus, which is why this screen
     * cannot end up with the focus stranded on the page underneath. */
    if (!rows.empty()) brls::Application::giveFocus(this->resultsGrid);
    else if (this->queryButton) brls::Application::giveFocus(this->queryButton);

    hg::logPrintf("SEARCH: showing %u results (total=%lld)", (unsigned)got.size(), total);
}

} // namespace hg
