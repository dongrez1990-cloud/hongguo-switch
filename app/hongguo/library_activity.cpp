// library_activity.cpp -- see the header for what this screen is and why it is one screen.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "library_activity.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "detail_activity.hpp"
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_store.hpp"
#include "hg_touch.hpp"
#include "home_activity.hpp"   /* makePosterGrid / setPosterGridRows: the same grid the home page uses */

extern "C" {
#include "hg_lang.h"
}

namespace hg {
namespace {

/* IS THE FOCUS INSIDE THIS SUBTREE?
 *
 * WHY THIS EXISTS -- A FRAMEWORK FACT FOUND BY THE ROUND-25 AUDIT, and it applies to every screen that rebuilds
 * itself: borealis' per-frame deletion-pool drain (`application.cpp:209-224`) deletes views that nobody has
 * ptr-locked and does NOT clear `Application::currentFocus`. So a rebuild that destroys the focused view leaves
 * the framework holding a DANGLING POINTER, and the next input walks freed memory.
 *
 * The rule that follows is: whoever destroys views must move the focus first. Most of this app's rebuilds already
 * do (the home grid re-focuses after populating, the detail screen right after setEpisodes, the search screen
 * when its results arrive) -- but the library's TAB SWITCH did not, and it is reachable with the focus inside the
 * grid: a TOUCH TAP on a tab does not move the focus to the tab first, which is what a controller press does.
 *
 * Asking "was the focus inside?" rather than "give the grid the focus" keeps this from STEALING focus: a tap on
 * the tab leaves the focus where it was whenever that place survives, and moves it into the grid only when the
 * place it was in is about to be deleted. */
bool focusIsInside(brls::View* root) {
    if (!root) return false;
    brls::View* v = brls::Application::getCurrentFocus();
    while (v != nullptr) {
        if (v == root) return true;
        v = v->getParent();
    }
    return false;
}

} // namespace
namespace {

/* THE LINE UNDER THE TITLE. For history it is everything the record knows -- which episode, how far in, and
 * how long ago -- because that is what makes a history list useful rather than a list of titles. For the
 * watchlist it is when it was added, which is all that file stores. */
std::string historyLine(const HgHistoryEntry& e) {
    char clock[24];
    char age[32];
    const time_t now = time(nullptr);
    hg_fmt_clock(e.position, clock, (int)sizeof clock);
    hg_fmt_age(e.when, (long long)now, age, (int)sizeof age);
    char line[120];
    if (e.episodeLabel[0] != '\0') {
        /* One sentence: the episode, how far in, and how long ago. Built with snprintf and then appended,
         * because the format string is a translation (see HG_T_EPISODE_N) and the numbers after it are not. */
        snprintf(line, sizeof line, hg_tr(HG_T_EPISODE_N), e.episodeLabel);
        strncat(line, "   ", sizeof line - strlen(line) - 1);
        strncat(line, clock, sizeof line - strlen(line) - 1);
        strncat(line, "   ", sizeof line - strlen(line) - 1);
        strncat(line, age, sizeof line - strlen(line) - 1);
    } else {
        snprintf(line, sizeof line, "%s   %s", clock, age);
    }
    line[sizeof line - 1] = '\0';
    return std::string(line);
}

std::string favLine(const HgFavEntry& e) {
    char age[32];
    hg_fmt_age(e.when, (long long)time(nullptr), age, (int)sizeof age);
    return std::string(age);
}

Series seriesFrom(const char* id, const char* title, const char* cover, const std::string& sub) {
    Series s;
    s.id = id ? id : "";
    s.title = title ? title : "";
    s.cover = cover ? cover : "";
    /* The card draws title + tags + badge, so the extra line goes in `tags`: it is the field the grid already
     * renders under the title, and inventing a second one for this screen would mean two card layouts. */
    s.tags = sub;
    return s;
}

} // namespace

LibraryActivity::LibraryActivity(bool startOnFavorites)
    : current(startOnFavorites ? TAB_FAVORITES : TAB_HISTORY) {}

LibraryActivity::~LibraryActivity() {
    /* Tell every callback in flight that this screen is gone -- see hg_alive.hpp. */
    this->alive_->kill();
}

brls::View* LibraryActivity::createContentView() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(24.0f);

    /* THE TWO TABS ARE REAL BUTTONS, not a drawn indicator: this screen's navigation is the framework's
     * (unlike the player, which polls the controller) so LEFT/RIGHT walks between them and A switches -- the
     * same interaction every other screen in this app has. */
    auto* tabs = new brls::Box();
    tabs->setAxis(brls::Axis::ROW);
    tabs->setMarginBottom(6.0f);

    static const int TAB_KEYS[TAB_COUNT] = {HG_T_LIB_HISTORY, HG_T_LIB_FAVORITES};
    for (int i = 0; i < TAB_COUNT; i++) {
        auto* b = new brls::Button();
        b->setText(hg_tr(TAB_KEYS[i]));
        b->setFontSize(18.0f);
        b->setMarginRight(10.0f);
        const Tab which = (Tab)i;
        b->registerClickAction([this, which](brls::View*) {
            this->showTab(which);
            return true;
        });
        hg::installTapSelect(b);
        tabs->addView(b);
        this->tabButtons[i] = b;
    }
    box->addView(tabs);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(15.0f);
    statusLabel->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    statusLabel->setMarginBottom(8.0f);
    box->addView(statusLabel);

    /* THE HOME PAGE'S GRID, again: same columns, same posters, same scrolling and same row-step navigation.
     * A second grid for this screen would be a second layout to keep in step. */
    grid = makePosterGrid(std::vector<HomeRow>(), [](const Series& s) {
        hg::logPrintf("LIBRARY: opening detail for '%s' (id=%s)", s.title.c_str(), s.id.c_str());
        brls::Application::pushActivity(new DetailActivity(s));
    });
    box->addView(grid);

    hg::logPrintf("LIBRARY: screen built (starting on the %s tab) [build50]",
                  this->current == TAB_FAVORITES ? "favorites" : "history");
    return hg::withHintBar(box);
}

void LibraryActivity::onContentAvailable() {
    if (this->grid) {
        this->grid->registerAction(hg_tr(HG_T_REFRESH), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
            hg::logPrintf("LIBRARY: X pressed -> re-reading the files");
            this->reload();
            return true;
        });
        auto* owner = this->grid->getParent();
        if (owner) {
            owner->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, [](brls::View*) {
                hg::logPrintf("LIBRARY: B pressed -> popActivity (deferred one frame)");
                brls::sync([]() { brls::Application::popActivity(); });
                return true;
            });
        }
    }

    this->reload();
    if (this->grid) brls::Application::giveFocus(this->grid);
}

void LibraryActivity::showTab(Tab tab) {
    if (tab == this->current) return;
    this->current = tab;
    hg::logPrintf("LIBRARY: switched to the %s tab", tab == TAB_FAVORITES ? "favorites" : "history");
    /* NOT reload() -- the files are already in memory (hg_store keeps the one copy); switching tabs is a
     * redraw, not a disk read. */
    this->applyEntries();
}

void LibraryActivity::reload() {
    /* THE FILES ARE RE-READ BY REBUILDING THE STORE'S COPY. hg_store loads once per process, and this screen
     * cannot see what the player wrote a minute ago otherwise -- which is exactly the case X exists for ("I
     * watched something, came back, and the list did not change"). The format's own loader skips malformed
     * lines and reports how many, so a hand-edited file cannot take the screen down. */
    Store::reloadFromDisk();
    this->applyEntries();
}

void LibraryActivity::applyEntries() {
    if (!this->grid) return;

    this->shown.clear();
    this->subtitles.clear();

    if (this->current == TAB_FAVORITES) {
        const std::vector<HgFavEntry> favs = Store::favorites();
        for (size_t i = 0; i < favs.size(); i++) {
            this->subtitles.push_back(favLine(favs[i]));
            this->shown.push_back(seriesFrom(favs[i].seriesId, favs[i].title, favs[i].cover, this->subtitles.back()));
        }
    } else {
        const std::vector<HgHistoryEntry> hist = Store::history();
        for (size_t i = 0; i < hist.size(); i++) {
            this->subtitles.push_back(historyLine(hist[i]));
            this->shown.push_back(
                seriesFrom(hist[i].seriesId, hist[i].title, hist[i].cover, this->subtitles.back()));
        }
    }

    std::vector<HomeRow> rows;
    rows.reserve(this->shown.size());
    for (size_t i = 0; i < this->shown.size(); i++) {
        HomeRow row;
        row.header = false;
        row.series = this->shown[i];
        rows.push_back(std::move(row));
    }
    /* SEE focusIsInside: the rebuild destroys every cell, so a focus that was inside the grid has to be moved
     * BEFORE the old cells are deleted. Doing it after is too late -- the pool drain may already have run. */
    const bool hadGridFocus = focusIsInside(this->grid);
    setPosterGridRows(this->grid, rows);
    if (hadGridFocus) {
        brls::Application::giveFocus(this->grid);
        hg::logPrintf("LIBRARY: the focus was inside the grid, which the rebuild destroys -- moved back into it [build65]");
    }

    char b[96];
    if (this->shown.empty()) {
        snprintf(b, sizeof b, "%s", hg_tr(this->current == TAB_FAVORITES ? HG_T_LIB_NO_FAVORITES : HG_T_LIB_NO_HISTORY));
    } else {
        snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)this->shown.size());
    }
    b[sizeof b - 1] = '\0';
    if (this->statusLabel) this->statusLabel->setText(b);

    hg::logPrintf("LIBRARY: %s tab shows %u entries", this->current == TAB_FAVORITES ? "favorites" : "history",
                  (unsigned)this->shown.size());
}

} // namespace hg
