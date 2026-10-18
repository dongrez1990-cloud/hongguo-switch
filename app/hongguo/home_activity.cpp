/*
 * Home activity implementation.
 *
 * GPL-3.0-only. See home_activity.hpp for why covers are not drawn.
 *
 * THREADING. borealis' main loop owns the main thread, and hg::Client blocks
 * while it fetches (there is no async variant of hg_http_request). So the
 * fetch runs on a std::thread and the result is handed back with
 * brls::sync(), which is borealis' documented way to touch views from a
 * non-main thread. Doing the fetch inline would freeze focus navigation for
 * the multi-second duration of two HTTPS requests.
 *
 * ASCII-only source file.
 */

#include "home_activity.hpp"
#include "detail_activity.hpp"
#include "category_activity.hpp"
#include "search_activity.hpp"
#include "settings_activity.hpp"
#include "hg_cfg.hpp"
#include "hg_diag_view.hpp"
#include "hg_grid.h"
#include "hg_image.hpp"
#include "hg_sidebar_view.hpp"
#include "hg_touch.hpp"

extern "C" {
#include "hg_lang.h"
}
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_store.hpp"
#include "library_activity.hpp"
#include "hg_thread.hpp"

#include <borealis.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hg {

/* ---- THE COVER WINDOW (build102, and the design is the user's) -----------------------------------------
 *
 * PRELOAD: how many rows BELOW the viewport's row stay loaded. Their words: "when the 更多 page opens, detect
 * which row it is on and preload the NEXT FOUR ROWS ... and load those four rows one row at a time".
 *
 * KEEP ABOVE: how many rows above stay loaded before the eviction begins. Their number was 168 covers, which is 21
 * rows of 8 -- a window of five rows plus sixteen above them. Sixteen is generous on purpose: scrolling back up
 * should find the rows already there, and anything that was evicted comes back one row at a time through the
 * ordinary pipeline, which is the other thing they asked for. */
#define HG_COVER_PRELOAD_ROWS 4
#define HG_COVER_KEEP_ROWS_ABOVE 10

/* ---- fetch mode ---------------------------------------------------------
 *
 * The fetch runs on a worker thread (hg_thread / libnx threadCreate) so that
 * focus navigation stays live for the seconds two HTTPS requests take.
 *
 * The choice is a file, not a compile flag:
 *
 *   sdmc:/switch/hongguo.cfg   containing the line  fetch=sync
 *
 * No file, or no such line -> the worker-thread path. The file can be added or
 * removed over FTP between runs, so both arms of the experiment cost nothing
 * but a restart. See hg_cfg.hpp.
 *
 * The match is exact, not a substring: "async" contains "sync", and a
 * substring match would silently flip the variable being tested -- the one
 * mistake this kind of experiment cannot afford. */
static bool wantSyncFetch() {
    bool sync = hg::cfgIs("fetch", "sync");
    hg::logPrintf("cfg: fetch mode = %s", sync ? "SYNC (main thread)" : "worker thread (default)");
    return sync;
}

/* One row: an index, the title, and the cover URL as text so the populated
 * data path is visible on screen. */
/* One home card: a cover thumbnail, the running index, and the title.
 *
 * WHY THIS IS A CARD AND NOT A TEXT ROW. wiliwili's home is a grid of covers with
 * a title under each (RecyclingGrid + ImageHelper), and the request was to match
 * it. This is the first half of that: a horizontal card with the image on the
 * left. The card shape also solves a real problem -- a text-only list of 36
 * Chinese titles is hard to scan, and the covers are what the service itself uses
 * to identify a drama.
 *
 * THE IMAGE SLOT IS NOT OPTIONAL. This cell is RECYCLED: the same object shows
 * card 3 and later card 17. A cover fetch started for card 3 can finish after
 * that reuse, and without the slot it would install card 3's cover on card 17's
 * title -- a screen that looks completely normal and is simply wrong. See
 * hg_image.hpp. */
class HomeCell : public brls::RecyclerCell {
  public:
    HomeCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(104.0f);
        this->setMarginBottom(6.0f);
        this->setPadding(8.0f, 14.0f, 8.0f, 14.0f);
        this->setCornerRadius(8.0f);
        /* The default borealis focus highlight recolours the row; that plus
         * the gamepad cursor is the whole focus affordance here. */
        this->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));

        /* 60x84 is the cover's own aspect (2:3-ish) at a size that fits two
         * lines of title beside it without the row growing to a full poster. */
        cover = new brls::Image();
        cover->setWidth(60.0f);
        cover->setHeight(84.0f);
        cover->setMarginRight(14.0f);
        cover->setScalingType(brls::ImageScalingType::FIT);
        this->addView(cover);

        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        col->setGrow(1.0f);

        auto* head = new brls::Box();
        head->setAxis(brls::Axis::ROW);
        head->setAlignItems(brls::AlignItems::CENTER);

        index = new brls::Label();
        index->setFontSize(18.0f);
        index->setWidth(48.0f);
        index->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
        head->addView(index);

        title = new brls::Label();
        title->setFontSize(21.0f);
        title->setGrow(1.0f);
        head->addView(title);

        col->addView(head);

        /* The synopsis, one line. hg_api_parse_home already fills `desc` from
         * series_intro; showing a single line of it is what makes a card look
         * like a card rather than a menu row, and it is data that was being
         * fetched and thrown away. */
        desc = new brls::Label();
        desc->setFontSize(15.0f);
        desc->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
        desc->setMarginTop(4.0f);
        desc->setMarginLeft(48.0f);
        desc->setGrow(1.0f);
        col->addView(desc);

        this->addView(col);
    }

    void setData(size_t i, const Series& s) {
        char b[32];
        snprintf(b, sizeof b, "%u.", (unsigned)(i + 1));
        index->setText(b);
        /* The title comes from series_title on section cards and series_name
         * on banners; hg_api_parse_home already resolved that. If it is empty
         * that is upstream data, not a rendering choice, so say so rather
         * than drawing a blank line that looks like a bug. */
        title->setText(s.title.empty() ? "(untitled card)" : s.title);
        desc->setText(s.desc.empty() ? "" : s.desc);

        /* THE SLOT IS UPDATED BEFORE THE FETCH STARTS, and the check inside
         * loadImageAsync compares against it at install time. Order matters: if
         * it were updated after, a response arriving in between would be
         * installed for the previous card. */
        const std::string previous = slot.currentUrl();
        slot.want(s.cover);

        /* ALREADY SHOWING THIS CARD: DO NOTHING.
         *
         * cellForRow is called again whenever the recycler re-binds a row, and
         * every rebind would otherwise throw away this cell's texture and create a
         * fresh 1x1 placeholder -- churning GPU textures and restarting a fetch
         * that already completed. Comparing against the slot is exactly what the
         * slot is for; this is the cheap early-out that keeps the common case
         * (re-binding the same row) free. */
        if (previous == s.cover && !s.cover.empty() &&
            cover->getVisibility() != brls::Visibility::GONE) {
            return;
        }

        if (s.cover.empty()) {
            /* No URL upstream. borealis has no way to CLEAR a texture --
             * innerSetImage(0) just logs "Cannot set texture: 0" and returns --
             * so the stale cover of whatever card this cell showed before would
             * still be drawn. Hiding the view is the supported way to not draw
             * it, and it is the whole point of this branch: a wrong cover on the
             * right title looks perfectly normal. */
            cover->setVisibility(brls::Visibility::GONE);
            return;
        }
        cover->setVisibility(brls::Visibility::VISIBLE);
        /* A PLACEHOLDER BEFORE THE FETCH, so a reused cell never shows the
         * previous card's cover while the new bytes are in flight. borealis has no
         * "clear the texture" call (innerSetImage(0) only logs and returns), so
         * the way to show nothing is to show something neutral: a 1x1 texture in
         * the theme's dark grey.
         *
         * This does not leak. Image::freeTexture defaults to true and
         * innerSetImage() deletes the previous texture (image.cpp:379), so each
         * placeholder is freed when the real cover arrives. */
        {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg) {
                unsigned char px[4] = {0x28, 0x28, 0x28, 0xff};
                int id = nvgCreateImageRGBA(vg, 1, 1, 0, px);
                if (id > 0) cover->innerSetImage(id);
            }
        }
        /* `this` IS THE OWNER: the slot lives in this cell, so the cell must stay alive until the
         * callback has finished with it. See hg_image.hpp. */
        hg::loadImageAsync(cover, s.cover, &slot, HG_IMAGE_LIST_MAX_BYTES, nullptr, this);
    }

    static brls::RecyclerCell* create() { return new HomeCell(); }

  private:
    brls::Label* index = nullptr;
    brls::Label* title = nullptr;
    brls::Label* desc = nullptr;
    brls::Image* cover = nullptr;
    hg::ImageSlot slot;
};

/* A tier heading. NOT focusable: D-pad should move between cards, and a heading
 * that accepted focus would swallow a press and do nothing, which reads as a dead
 * button. It still derives from RecyclerCell so the recycler's dequeue/queue
 * machinery works unchanged. */
class HomeHeaderCell : public brls::RecyclerCell {
  public:
    HomeHeaderCell() {
        this->setFocusable(false);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(44.0f);
        this->setMarginTop(12.0f);
        this->setMarginBottom(4.0f);
        this->setPadding(0.0f, 6.0f, 0.0f, 6.0f);

        title = new brls::Label();
        title->setFontSize(22.0f);
        title->setGrow(1.0f);
        title->setTextColor(brls::Application::getTheme().getColor("brls/accent"));
        this->addView(title);
    }

    void setTitle(const std::string& s) {
        /* An empty heading is upstream data, not a rendering choice, so it says so
         * instead of drawing a blank band that looks like a bug. */
        title->setText(s.empty() ? "(untitled section)" : s);
    }

    static brls::RecyclerCell* create() { return new HomeHeaderCell(); }

  private:
    brls::Label* title = nullptr;
};

class HomeDataSource : public brls::RecyclerDataSource {
  public:
    explicit HomeDataSource(std::vector<HomeRow>* items) : items(items) {}

    int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }

    int numberOfRows(brls::RecyclerFrame* recycler, int section) override {
        return (int)items->size();
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        /* BOUNDS-CHECKED, AND CHECKED BEFORE ANY INDEXING.
         *
         * The old version did (*items)[index.row] with no bound. A stale row count
         * or an off-by-one would then read past the end of a std::vector, which on
         * a console is a crash with no message -- and the symptom would look like
         * "the list is broken", not like an index error. A heading cell is returned
         * as the fallback because it is registered and harmless.
         *
         * The row model itself is what the host suite checks (hg_api_home_rows),
         * so reaching this branch at all means something upstream disagrees with
         * what was tested. */
        if (index.row < 0 || (size_t)index.row >= items->size()) {
            hg::logPrintf("HOME list: row %d is out of range (%u rows) -- returning a blank row "
                          "instead of indexing past the end",
                          index.row, (unsigned)items->size());
            return (brls::RecyclerCell*)recycler->dequeueReusableCell("HomeHeader");
        }

        const HomeRow& row = (*items)[(size_t)index.row];
        if (row.header) {
            auto* cell = (HomeHeaderCell*)recycler->dequeueReusableCell("HomeHeader");
            cell->setTitle(row.sectionTitle);
            return cell;
        }

        auto* cell = (HomeCell*)recycler->dequeueReusableCell("HomeCell");
        /* The index shown on a card is its position among the CARDS, not among the
         * rows, so the numbering does not jump by one at every heading. */
        cell->setData(countCardsBefore((size_t)index.row), row.series);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (index.row < 0 || (size_t)index.row >= items->size()) return;
        const HomeRow& row = (*items)[(size_t)index.row];
        /* Headings are not selectable. RecyclerCell's base constructor registers a
         * click action, so a heading can reach here through a tap even though it is
         * not focusable; opening "detail for a heading" would push a DetailActivity
         * with an empty series. */
        if (row.header) {
            hg::logPrintf("HOME list: heading row %d selected; ignoring", index.row);
            return;
        }
        brls::Application::pushActivity(new DetailActivity(row.series));
    }

  private:
    /* How many cards precede `rowIndex`. Used only for the on-screen numbering. */
    size_t countCardsBefore(size_t rowIndex) const {
        size_t n = 0;
        for (size_t i = 0; i < rowIndex && i < items->size(); i++) {
            if (!(*items)[i].header) n++;
        }
        return n;
    }

    std::vector<HomeRow>* items;
};

/* ---- the poster grid (home.layout=grid, the default) ----------------------
 *
 * GEOMETRY CONSTANTS. They are named here rather than inlined because hg_grid.c takes
 * them as arguments and the tests exercise them: "the heading is 44 tall" is a fact
 * about this screen that three places would otherwise each have their own idea of. */
#define HG_GRID_HEADING_H 44.0f
#define HG_GRID_ROW_SPACE 18.0f
#define HG_GRID_ITEM_SPACE 16.0f

/* THE COMPACT DENSITY, as a percentage of the requested large poster width.
 *
 * MEASURED at the four ui.scale widths with the default large poster: 55% gives 8 columns at 544p,
 * 11 at 720p and 12 (the compact ceiling) at 1080p -- against 4, 6 and 8 in the large layout.
 * Derived from the large value rather than being a second cfg key: a user who retunes `home.cellW`
 * should not have to remember a second number that only matters while the toggle is on.
 *
 * The first version of this was 68% (136 pt), which gave 8 columns at EVERY resolution because
 * the ceiling was then hard-coded at 8 -- so the toggle did nothing visible above 544p. The
 * measurement is in hg_grid.h and the relationship is a check in hg_grid_test. */
#define HG_GRID_COMPACT_PERCENT 55

/* THE DEFAULT POSTER WIDTH, and the third value this has taken -- the history is short and every step
 * was a measurement:
 *
 *   200 -> six columns across at 720p, which the user read as "one row" because a whole tier fitted and
 *          the screen looked like a band rather than a grid.
 *   380 -> three columns with cards 400x656 tall, i.e. TALLER THAN THE VIEWPORT: one row filled the
 *          screen and the user reported "only 3 previews show". A card taller than the screen is not a
 *          card, it is a page.
 *   240 -> five columns, two-and-a-bit rows visible. Working, and the user could scroll.
 *   150 -> COMPACT IS NOW THE DEFAULT, at the user's request ("?????????????????): more posters on
 *          screen at once, which is what a browse-first grid wants. X still switches to the large
 *          layout (150 -> 150/0.55 = 273 wide), so the big-card view is one press away.
 *
 * The column count is always DERIVED from the width and this number, never fixed. */
#define HG_GRID_DEFAULT_CELL_W 150

/* THE METRICS HAVE TO BE AN OBJECT, NOT THE MACRO DIRECTLY.
 *
 * hg_grid.h defines HG_GRID_POSTER_METRICS as a C compound literal, which is fine in C
 * and is what hg_grid_test.c uses. In C++ a compound literal is a TEMPORARY, so
 * `&HG_GRID_POSTER_METRICS` is "taking address of rvalue" and does not compile -- which
 * is exactly what the first build of this file said. A named constant keeps one source
 * of truth (the numbers still match the macro, and hg_grid_test checks that they do)
 * while being addressable. */
static const HgGridMetrics POSTER_METRICS = {3.0f, 2.0f, 56.0f};

/* Read a positive integer out of hongguo.cfg, falling back to `fallback` when the key
 * is absent or is not a usable number. Same discipline as the player menu's reader:
 * this file is hand-edited over FTP, so a typo should leave a working screen. */
static int cfgGridInt(const char* key, int fallback) {
    std::string raw = hg::cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || v <= 0) {
        hg::logPrintf("HOMEGRID: cfg %s='%s' is not a positive number -- using %d", key,
                      raw.c_str(), fallback);
        return fallback;
    }
    return (int)v;
}

/*
 * WHY NOT A RecyclerFrame. brls::RecyclerFrame is a LIST: one cell per row, and the
 * only thing it recycles is cells. wiliwili's home is a grid, from its own
 * RecyclingGrid (upstream/wiliwili/wiliwili/include/view/recycling_grid.hpp), which
 * borealis does not have -- and the handover document records that fact explicitly:
 * "borealis' RecyclerFrame has no grid capability". So a grid has to come from
 * somewhere.
 *
 * WHAT WAS TAKEN FROM wiliwili'S RECYCLINGGRID AND WHAT WAS NOT. Taken: the layout
 * model (a column count, a uniform cell, gaps between items). Not taken: its scroll
 * and focus half. A brls::ScrollingFrame already scrolls to keep the focused view on
 * screen and Box::getNextFocus already walks children in order (box.cpp:409-443), so
 * reusing those means this grid inherits whatever borealis gets right instead of
 * standing up a second implementation that can disagree with it. The arithmetic that
 * is genuinely new -- where each cell goes -- is in hg_grid.c, where the host suite
 * checks it.
 *
 * NO CELL RECYCLING, DELIBERATELY, BECAUSE THIS PAGE DOES NOT NEED IT. wiliwili's
 * grid is built for an infinite feed; this home page is 36 cards plus 4 headings, all
 * known at once. The expensive part of a cell is its COVER FETCH, not its Box, and
 * recycling would fight that: a recycled cell has to re-fetch its cover, so scrolling
 * up and down would restart downloads. Forty cells that keep their covers is both
 * simpler and better behaved here, and it is the same argument the handover document
 * already makes for the image cap ("a dozen visible cells" is the budget that
 * matters).
 *
 * WHAT THIS DOES MEAN: all 40 cells are children of the content box, so all 40 are
 * drawn every frame and borealis culls them by the scroll frame's bounds. That is
 * acceptable at this size, and the log states the cell count so a future page with
 * hundreds of cards has the evidence to justify a pool. */

/* One poster card: the cover, a badge, and a two-line title underneath.
 *
 * The layout is 2:3 (portrait), because the covers are portraits -- see hg_grid.h for
 * why this is the one place the port deliberately departs from wiliwili's landscape
 * thumbnails. */
/* THE MARK INSIDE THE "MORE" CELL (G3).
 *
 * WHY A VIEW AND NOT A PICTURE. The cell used to be an empty dark rectangle with the two characters 更多 under
 * it -- the same shape as a card whose cover failed to load, which is the one thing a card in a grid must not
 * look like. The reference layout gives that cell artwork; the handover's G3 offered two ways (a blurred copy of
 * the tier's first cover, or a drawn mark) and the drawn one wins on cost: a blur needs an offscreen render pass
 * and nanovg has no blur filter, while two chevrons are four lines and read the same at every size.
 *
 * A CHEVRON, NOT A PLUS: the cell does not add anything to the list, it takes you to the rest of the tier --
 * and ">>" is what every interface uses for that.
 *
 * It draws in the art area (the space a poster would fill) and is hidden on every other card, so a normal card
 * is byte-for-byte what it was before this existed. */
class MoreMarkView : public brls::View {
  public:
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        if (!vg || width <= 0.0f || height <= 0.0f) return;

        const NVGcolor accent = brls::Application::getTheme().getColor("brls/accent");
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;

        /* A RING FIRST, so the cell reads as a control rather than as a hole: it is sized off the shorter side
         * so it stays round in both cell densities (the compact cell is much shorter than it is wide). */
        const float r = (width < height ? width : height) * 0.26f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgStrokeColor(vg, nvgRGBA(accent.r * 255, accent.g * 255, accent.b * 255, 200));
        nvgStrokeWidth(vg, 3.0f);
        nvgStroke(vg);

        /* TWO CHEVRONS INSIDE IT: "there is more this way". They are drawn as strokes with round caps so they
         * keep their weight when the cell is scaled by ui.scale. */
        const float arm = r * 0.42f;
        const float off = r * 0.28f;
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        for (int i = 0; i < 2; i++) {
            const float ox = cx - off + (float)i * off;
            nvgBeginPath(vg);
            nvgMoveTo(vg, ox - arm * 0.35f, cy - arm);
            nvgLineTo(vg, ox + arm * 0.55f, cy);
            nvgLineTo(vg, ox - arm * 0.35f, cy + arm);
            nvgStrokeColor(vg, accent);
            nvgStrokeWidth(vg, 3.0f);
            nvgStroke(vg);
        }
    }
};
class HomeGridCard : public brls::Box {
  public:
    /* WHAT THIS CARD'S COVER FETCH ENDED UP AS (hg_image's three states). The retry tick reads it to find the
     * first row that still needs covers -- see tickCoverRetries and the user's "the second row has to be
     * finished first". */
    int coverState() const { return this->coverSettled.load(); }

    /* ---- GIVE THE COVER'S TEXTURE BACK, AND SAY "I HAVE NO PICTURE" AGAIN (build96) --------------------
     *
     * THE USER'S DIAGNOSIS, WHICH WAS RIGHT FROM THE START: "is it that a cache never gets reset?" It was.
     *
     * A decoded cover is a 640x~960 RGBA texture -- about 2.4 MB each. HG_IMAGE_LIST_MAX_BYTES bounds the
     * DOWNLOAD buffer (1.5 MiB) and nothing bounded the DECODED TEXTURES, so a grid that had ever loaded N
     * covers was holding ~2.4 * N MB for the rest of the session: 36 covers is ~85 MB at startup, and the 更多
     * screen's 90-120 cards is ~250-290 MB. That is why loading MANY covers at once was what killed it, and why
     * slowing the loading down only hid the problem -- exactly as the user said ("slowing it down avoids it, but
     * something else must be causing it").
     *
     * So a cell that is scrolled far away hands its texture back. `Image::clear()` (borealis image.cpp:393) is
     * the release: it calls nvgDeleteImage and sets the texture to 0, which `innerSetImage(0)` deliberately does
     * not do.
     *
     * `coverSettled` goes back to 0 as well, because that flag means "this cell has its picture" -- and it no
     * longer does. That is what makes the retry tick fetch it again when the user scrolls back, instead of the
     * cell staying blank for the rest of the session.
     */
    void releaseCoverTexture() {
        if (!this->cover) return;
        if (this->cover->getTexture() == 0) {
            this->coverSettled.store(0);
            return;
        }
        this->cover->clear();
        this->coverSettled.store(0);
    }

    HomeGridCard() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(6.0f, 6.0f, 6.0f, 6.0f);
        this->setCornerRadius(8.0f);
        this->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));

        auto* art = new brls::Box();
        art->setAxis(brls::Axis::COLUMN);
        art->setGrow(1.0f);
        art->setJustifyContent(brls::JustifyContent::FLEX_END);
        art->setAlignItems(brls::AlignItems::FLEX_END);

        cover = new brls::Image();
        cover->setWidthPercentage(100.0f);
        cover->setHeightPercentage(100.0f);
        cover->setScalingType(brls::ImageScalingType::FIT);
        art->addView(cover);

        /* The episode count, which the data layer has had all along: hg_api fills
         * `badge` from episode_cnt and it is present on 36/36 home cards (the literal
         * is upstream Chinese, "all N episodes" -- it is DATA and is rendered as-is,
         * but this source file must stay ASCII, so it is not quoted here; see
         * hg_api_parse_home for the field). wiliwili puts a duration badge on the
         * thumbnail corner, so this is the same affordance with the datum this service
         * actually has.
         *
         * THE BADGE IS A BOX CONTAINING A LABEL, NOT A PADDED LABEL. brls::Label has no
         * setPadding at all (views/label.hpp:60-193 -- it has font size, colours and
         * alignment, and nothing else), so the dark plate behind the text has to come
         * from a Box. That also gets the badge a background that hugs the text instead
         * of filling the whole cell width. */
        auto* badgePlate = new brls::Box();
        badgePlate->setAxis(brls::Axis::ROW);
        badgePlate->setBackgroundColor(nvgRGBA(0, 0, 0, 180));
        badgePlate->setCornerRadius(4.0f);
        badgePlate->setPadding(2.0f, 6.0f, 2.0f, 6.0f);
        badgePlate->setMarginRight(4.0f);
        badgePlate->setMarginBottom(4.0f);

        badge = new brls::Label();
        badge->setFontSize(15.0f);
        badge->setTextColor(nvgRGB(255, 255, 255));
        badgePlate->addView(badge);
        art->addView(badgePlate);
        this->badgePlate = badgePlate;

        /* THE MARK LIVES IN THE ART AREA, hidden until a caller says this is a "more" cell (see buildRowView).
         * It is added AFTER the badge so it draws over the plate if both were ever visible -- they are not, but
         * the order is what makes that true by construction rather than by luck. */
        moreMark = new MoreMarkView();
        moreMark->setWidthPercentage(100.0f);
        moreMark->setHeightPercentage(100.0f);
        moreMark->setVisibility(brls::Visibility::GONE);
        art->addView(moreMark);

        this->addView(art);

        /* Two lines of title. The label is SINGLE_LINE-off and has a fixed height so
         * a long title wraps onto the second line and is then truncated, rather than
         * growing the cell and breaking the row alignment hg_grid.c computed. */
        title = new brls::Label();
        title->setFontSize(16.0f);
        title->setHeight(38.0f);
        title->setMarginTop(4.0f);
        title->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        this->addView(title);

        /* THE SECONDARY LINE: the genre tags, on their own line under the title.
         *
         * This is the data the small/large toggle is worth having for. `tags` is present on
         * 36/36 home cards while `desc` (the synopsis) was present on 0/36 in one live capture
         * -- so the tags are the only descriptive text that is always there, and they are what
         * wiliwili puts under a card's title too.
         *
         * Shown only in the COMPACT layout. In the large layout the poster already fills the
         * cell and a third line of text would either be clipped or force the cell taller than
         * the grid computed, breaking the row alignment. So the toggle is not merely "more
         * columns" -- it is more cards AND less detail each, which is exactly the trade a small
         * window is. */
        tags = new brls::Label();
        tags->setFontSize(14.0f);
        tags->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
        tags->setHorizontalAlign(brls::HorizontalAlign::LEFT);

        /* ---- A FIXED HEIGHT, AND THIS IS THE FIX FOR "THE NEXT ROW COVERS THE EPISODE LINE" (build134) ---
         *
         * MEASURED ON HARDWARE, from the user: "on the history screen the lower part of the cover is stretched,
         * which makes the card taller, and then the 第 N 集 line at the bottom is covered by the cover of the card
         * under it."
         *
         * Exactly right, and this is the mechanism: the art box grows (art->setGrow(1.0f)) and the cover fills it
         * (cover->setHeightPercentage(100.0f)), while `title` has a fixed height (32 in this layout) and THIS label
         * had none. A growing box takes the space that is left over, but it cannot shrink below its own minimum --
         * so with the last line free to demand as much as it likes, the card's content added up to MORE than the
         * cell hg_grid.c allocated (measured: cell 140.0x266.0), the card overflowed its cell, and the next row --
         * drawn after it -- covered the overflow. The episode line was the thing that fell victim, because it sits
         * at the bottom.
         *
         * With a fixed height here the arithmetic is closed: padding + title + tags + art == the cell, so the art
         * (and the cover inside it) gets exactly what is left and nothing can overflow. The library screen's extra
         * line is a single short line ("第 N 集"), so one line's worth is the right number.
         *
         * Harmless in the large layout, where this label is GONE: a height on a hidden view is not laid out. */
        tags->setHeight(20.0f);
        tags->setVisibility(brls::Visibility::GONE);
        this->addView(tags);
    }

    /* Show or hide the secondary line, and shrink the title to match. Called by the grid once
     * per layout, not per frame: the density is a property of the layout, and asking each cell
     * to work it out from its own width would be a second source of truth for it. */
    void setCompact(bool compact) {
        if (this->isCompact == compact) return;
        this->isCompact = compact;
        if (compact) {
            /* The title keeps two lines (the height hg_grid.c allowed is unchanged) but at a
             * smaller size, because the column is narrower and a 16 pt title would be truncated
             * after four characters. */
            this->title->setFontSize(14.0f);
            this->title->setHeight(32.0f);
            this->tags->setText(tagText);
            this->tags->setVisibility(tagText.empty() ? brls::Visibility::GONE
                                                      : brls::Visibility::VISIBLE);
        } else {
            this->title->setFontSize(16.0f);
            this->title->setHeight(38.0f);
            this->tags->setVisibility(brls::Visibility::GONE);
        }
    }

    /* Point this cell at a series. The SLOT is the mechanism that makes a reused cell
     * safe -- see hg_image.hpp: a cover fetch started for one card can finish after
     * the cell has been pointed at another, and without the token it would install the
     * wrong cover on a screen that looks perfectly normal. */
    /* "THIS CELL IS THE WAY TO THE REST OF THE TIER" -- see buildRowView, which is the only caller. */
    void setMoreCard(bool isMore) {
        if (moreMark) moreMark->setVisibility(isMore ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    void setData(const Series& s) {
        const std::string previous = slot.currentUrl();
        slot.want(s.cover);
        /* Remembered for retryCoverIfMissing: if this fetch is refused for being over the budget,
         * the URL to ask for again has to outlive setData. */
        pendingCover = s.cover;
        /* Reset the "this card has its picture" signal. The loader zeroes it again on every attempt,
         * so a cell whose fetch failed and is retried is not retired on the earlier attempt -- which
         * is the bug the console found. */
        coverSettled.store(0);

        title->setText(s.title);
        /* Kept so setCompact can show it without the series being passed again -- the toggle
         * runs from the grid, which has no idea which series a cell is showing. */
        tagText = s.tags;
        if (this->isCompact) {
            tags->setText(tagText);
            tags->setVisibility(tagText.empty() ? brls::Visibility::GONE
                                                : brls::Visibility::VISIBLE);
        }

        if (!s.badge.empty()) {
            badge->setText(s.badge);
            badgePlate->setVisibility(brls::Visibility::VISIBLE);
        } else {
            /* Hide the PLATE, not the label: the plate is what draws the dark
             * rectangle, so hiding only the text would leave an empty black chip on
             * every card whose badge is missing. */
            badgePlate->setVisibility(brls::Visibility::GONE);
        }

        if (s.cover.empty()) {
            /* No cover at all: hide the image rather than leaving the previous one in
             * place. borealis has no "clear texture" path -- innerSetImage(0) just
             * logs "Cannot set texture: 0" and returns -- so hiding is the only way to
             * avoid showing a stale poster. */
            cover->setVisibility(brls::Visibility::GONE);
            return;
        }
        cover->setVisibility(brls::Visibility::VISIBLE);

        if (previous == s.cover && slot.stillWants(s.cover) && coverSettled.load() == 1) return;

        /* A 1x1 placeholder first: "the new cover is in flight" should look like a
         * dark rectangle, not like the previous drama's poster. */
        if (!(coverSettled.load() == 1)) {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (vg) {
                const unsigned char pixel[4] = {40, 40, 44, 255};
                int tex = nvgCreateImageRGBA(vg, 1, 1, 0, pixel);
                if (tex > 0) cover->innerSetImage(tex);
            }
        }
        /* THE OWNER IS THIS CARD, AND THAT IS THE FIX FOR THE INTERMITTENT CRASH. `slot` and
         * `coverSettled` are members of this object, and a grid rebuild (the ??? button) hands the old
         * cards to borealis' free queue while this fetch is still in flight -- locking only the Image
         * would leave the callback writing into freed memory. See hg_image.hpp. */
        if (hg::loadImageAsync(cover, s.cover, &slot, HG_IMAGE_LIST_MAX_BYTES, &coverSettled, this)) {
            /* A FETCH STARTED, WHICH IS NOT THE SAME AS "THIS CARD HAS A PICTURE" -- see
             * retryCoverIfMissing. `coverSettled` is what the retry reads. */
        }
    }

    /* START THE COVER FETCH AGAIN IF IT WAS EVER SKIPPED FOR BEING OVER THE BUDGET.
     *
     * MEASURED ON HARDWARE, and it is why only the first six cards in the screenshot have posters:
     * the layout binds all 36 cells at once, `loadImageAsync` refuses anything beyond
     * `image.threads` in flight, and NOTHING EVER RETRIED. So six covers arrived and thirty cards
     * stayed on their placeholder for the life of the screen -- which looks exactly like "the
     * covers are broken", and was reported as such.
     *
     * The budget itself is right (each fetch is a thread plus up to 1.5 MiB); what was missing is
     * the second half of the idea: a skipped cell has to come back later. The grid calls this once
     * per relayout, which the frame does on a heartbeat while anything is still missing, so the
     * covers arrive in waves of six instead of never. */
    void retryCoverIfMissing() {
        /* INSTALLED and FAILED both mean "never ask again", and they are different facts: one has the
         * pixels, the other has a definitive no. Only PENDING is worth another request. Retrying a
         * FAILED URL would re-request a 404 on every pass for the life of the screen. */
        const int state = this->coverSettled.load();
        if (state == HG_IMG_INSTALLED || state == HG_IMG_FAILED) return;
        /* ---- IN FLIGHT: DO NOT ASK AGAIN (build98, and it is the race the user worked out) --------------
         *
         * Their words: "it is download, load, transport, display -- one of those must be broken; 140 covers do
         * not crash it and 16 at a time does not crash it, so maybe it is the load or the transport: it has not
         * been shown yet, so it is like the load gets removed and then the load comes again, and that crashes it."
         *
         * They are right. This test used to be "ask again unless INSTALLED or FAILED", and PENDING meant BOTH "the
         * loader refused me" AND "a fetch is already running". So a cell whose fetch was already in flight was
         * asked again on every retry pass: several fetches for one view, arriving one after another, and every
         * arrival runs the LOAD (nvgCreateImageMem -> innerSetImage), which creates a texture and deletes the
         * previous one. deko3d does not necessarily return that memory in the same frame, so a new texture could
         * be created while the old one was still unreclaimed -- a load/transport collision, and about the only
         * thing on this path that a burst of arrivals can turn into a crash.
         *
         * IN_FLIGHT is set by the loader the moment a fetch really starts, so this is now one request at a time
         * per cell. */
        if (state == HG_IMG_IN_FLIGHT) return;
        if (pendingCover.empty()) return;
        /* The slot already wants this URL, so a response that arrives is installed normally; this
         * only re-issues a request that was refused or that failed. */
        hg::loadImageAsync(cover, pendingCover, &slot, HG_IMAGE_LIST_MAX_BYTES, &coverSettled, this);
    }

    /* ---- PUT THE PICTURE BACK AFTER THE TEXTURE WAS HANDED BACK (build97) -------------------------------
     *
     * MEASURED ON HARDWARE, from the user, and it is my own regression: "in the 更多 screen it showed two rows, I
     * scrolled down, and the THIRD ROW DID NOT SHOW -- then rows four and five appeared, sixteen of them."
     *
     * The cause was the texture release added in build96 being too eager AND its state reset. A cell one screen
     * below the visible area was released and `coverSettled` was set back to "no picture", and then:
     *   - the cell drew nothing, because clear() leaves texture 0 and nothing puts a placeholder back;
     *   - the retry tick saw "no picture" and fetched it again -- for a cell that was STILL off screen, so it was
     *     released again on the next frame and re-fetched again. A row just outside the window oscillated for ever
     *     and never appeared, which is exactly the missing third row they saw.
     *
     * The fix has two halves, and this is the second: when a released cell comes back, its URL is still known
     * (pendingCover), so the picture is simply asked for again -- and `coverSettled` goes to PENDING, which says
     * "a fetch is in flight" rather than "there is no picture", so the retry tick stops chasing it.
     *
     * The first half is in the cull: releasing now only happens well outside the drawn window (two screens), so a
     * cell that is merely a little off screen keeps its picture and this path is not entered during ordinary
     * scrolling at all. */
    void reloadReleasedCover() {
        if (!this->coverReleased) return;
        this->coverReleased = false;
        if (pendingCover.empty()) return;
        this->coverSettled.store(HG_IMG_PENDING);
        hg::loadImageAsync(cover, pendingCover, &slot, HG_IMAGE_LIST_MAX_BYTES, &coverSettled, this);
    }

    bool hasReleasedCover() const { return this->coverReleased; }

    static brls::View* create() { return new HomeGridCard(); }

  private:
    brls::Image* cover = nullptr;
    brls::Label* badge = nullptr;
    brls::Box* badgePlate = nullptr;
    brls::Label* title = nullptr;
    brls::Label* tags = nullptr;
    /* The current series' tags, kept so the density toggle can put the line back without the
     * grid having to know which series this cell holds. */
    std::string tagText;
    /* The cover URL this cell is showing, kept so a refused fetch can be re-issued. */
    std::string pendingCover;
    /* True while this cell's texture has been handed back for being far off screen. See reloadReleasedCover. */
    bool coverReleased = false;
    /* 0 = no picture yet, 1 = a picture was INSTALLED. Written on the main thread only (the loader's
     * brls::sync callback and the retry both run there); atomic so this signal can be read without
     * having to reason about which of the two wrote it last.
     *
     * THIS REPLACED A PLAIN `hasImage` BOOL AND THE DIFFERENCE IS A REAL BUG. That bool was set when
     * a fetch STARTED, so a card whose fetch then failed looked finished: its retry never fired and
     * it kept its placeholder for the life of the screen. The console log showed exactly three URLs
     * still being refused on every retry pass minutes after the grid had settled. */
    std::atomic<int> coverSettled{0};

    /* The "more" cell's drawn mark. Null-safe: setData runs for both kinds of cell. */
    MoreMarkView* moreMark = nullptr;
    bool isCompact = false;
    hg::ImageSlot slot;
};

/* The grid itself.
 *
 * It is a ScrollingFrame whose content box is sized to the computed total height and
 * whose children are absolutely positioned posters. The children are added in ITEM
 * ORDER, and Box::getNextFocus walks children in order (box.cpp:409-443), so borealis'
 * own focus walk already goes left-to-right, top-to-bottom without a single line of
 * custom navigation code. */
class HomeGridView : public brls::ScrollingFrame {
  public:
    HomeGridView() {
        this->setFocusable(true);
        this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

        contentBox = new brls::Box();
        /* ROW, NOT COLUMN -- AND THIS ONE LINE IS THE WHOLE D-PAD BUG.
         *
         * MEASURED ON HARDWARE (the first console run after these features landed): the grid drew
         * six columns across, and UP/DOWN moved between cards while LEFT/RIGHT did nothing. The
         * screenshot and the report agree, and the cause is here.
         *
         * Box::getNextFocus is an *insertion-order* walk, not a spatial one. Its first check is:
         *
         *     if ((axis == ROW    && direction != LEFT && direction != RIGHT) ||
         *         (axis == COLUMN && direction != UP   && direction != DOWN))
         *         -> hand the direction to the parent
         *
         * so the axis decides which DIRECTION traverses these children, and the other direction
         * escapes to the parent. With COLUMN, UP/DOWN stepped through my children -- which are
         * stored in reading order, so up/down walked the row -- and LEFT/RIGHT went to the
         * ScrollingFrame, which is a vertical scroller and answered for neither.
         *
         * The children ARE in reading order (a card's index grows to the right, then down), so the
         * axis that should traverse them is ROW: left/right steps between columns, and up/down
         * leaves the box for the frame to scroll with. The visual layout is unaffected either way,
         * because every child is absolutely positioned and therefore outside the flex flow the axis
         * would otherwise govern. */
        contentBox->setAxis(brls::Axis::ROW);
        contentBox->setPositionType(brls::PositionType::ABSOLUTE);
        this->setContentView(contentBox);
    }

    /* SCROLL SO THE FOCUSED CARD IS ON SCREEN. Called from onChildFocusGained -- the framework's own
     * "a child of this frame just took focus" hook -- which is the one place every focus change inside
     * this grid passes through.
     *
     * MEASURED ON HARDWARE: "only 3 previews show because they fill the screen; pressing right moves
     * the selection to others but you cannot see them". That is not a scrollbar problem -- it is that
     * NOTHING SCROLLS WHEN FOCUS MOVES.
     *
     * WHERE THE FIRST VERSION OF THIS NOTE WAS WRONG: it said the framework has no follow-focus at all
     * because `updateScrolling()` is only reached from `draw()` while `updateScrollingOnNextFrame` is
     * standing. `draw()` is one call site; `ScrollingFrame::onChildFocusGained`
     * (scrolling_frame.cpp:478-487) is the other, and it calls updateScrolling(true) on every focus
     * change inside the frame. The framework does follow the focus -- but it was asking THIS grid's
     * getDefaultFocus() where to scroll to, and that answered "the first card" every time, so it
     * scrolled to the top row. Both halves are fixed now: getDefaultFocus names the focused card, and
     * this function sets the same offset immediately.
     *
     * wiliwili has the same framework and relies on that same mechanism, and its own grids call
     * `setContentOffsetY(...)` directly for the cases the framework cannot know about (its
     * RecyclingGrid::selectRowAt). This does the same, from the placement data hg_grid_layout already
     * produced -- no arithmetic is added, and the geometry is the geometry the cards are drawn at.
     *
     * INSTANT, not animated: this runs from inside focus movement, and an animated scroll would still
     * be in flight when the next key arrives. The framework's animated version is left in place and
     * finds the offset already correct, so it does nothing; the animation the framework provides for
     * the touch pan has its own inertia and is not touched. */
    void followFocus(int index) {
        if (index < 0 || index >= (int)this->places.size()) return;
        if (this->cells.empty()) return;

        const float viewH = this->getHeight();
        const float contentH = this->getContentHeight();
        if (viewH <= 0.0f) return;
        if (contentH <= viewH) {
            /* Everything fits: there is nothing to scroll, and forcing 0 keeps the grid from sitting
             * at a stale offset after a density change made the content shorter. */
            this->setContentOffsetY(0.0f, false);
            return;
        }

        /* The focused card's centre, in content coordinates. */
        const HgGridPlace& p = this->places[(size_t)index];
        const float centre = p.y + p.cellH * 0.5f;

        float want = centre - viewH * 0.5f;
        const float bottomLimit = contentH - viewH;
        /* MEASURED, because "it snaps back to the first row from the fourth row on" is consistent with
         * several different arithmetic errors, and one run of this tells them apart: the viewport, the
         * content, where the focused cell sits, what the offset was asked to be, and what it was clamped
         * to. The threshold is printed too, since "contentH <= viewH" forces the offset to 0 and that is
         * exactly what "snaps back to the first row" would look like from the outside. */
        {
            static int scrolled = 0;
            if (scrolled < 40) {
                hg::logPrintf("HOMEGRID scroll: item=%d row=%d y=%.0f centre=%.0f viewH=%.0f "
                              "contentH=%.0f want=%.0f bottom=%.0f fits=%d",
                              index, p.row, (double)p.y, (double)centre, (double)viewH,
                              (double)contentH, (double)want, (double)bottomLimit,
                              contentH <= viewH ? 1 : 0);
                scrolled++;
            }
        }
        if (want < 0.0f) want = 0.0f;
        if (want > bottomLimit) want = bottomLimit;
        this->setContentOffsetY(want, false);
    }

    /* Rebuild from the row model. Cells are created here, once, and never recycled
     * (see the note above): this page is small and its covers are the expensive part.
     *
     * TAKES A COPY, AND THAT IS A REFRESH-SAFETY REQUIREMENT. The first version stored
     * a std::vector<HomeRow>* -- the activity's member -- which is fine for one load
     * and a dangling pointer the moment the page can be reloaded: the next fetch
     * replaces the member, and every cell's click action captures its index into a
     * vector that is being rebuilt underneath it. A copy costs a few kilobytes of
     * strings on a screen with 36 cards and removes a whole class of
     * use-after-free. */
    void setRows(const std::vector<HomeRow>& rows);

    /* Recompute positions for the current width. Called from onLayout, so a ui.scale
     * change (544p..1080p, hongguo.cfg) produces a different column count with no
     * other code involved. */
    void relayout();

    void onLayout() override;
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;
    /* Re-issues cover fetches that the image budget refused. See the implementation. */
    void tickCoverRetries();
    /* Releases every loaded cover while another activity covers this screen. See the definition. */
    void dropAllCoversIfCovered();
    /* Called when this screen becomes visible again (build131): the activity re-reads the store counts for the
     * sidebar, whose entries are dim exactly when they are empty. */
    std::function<void()> onScreenReturned;

    /* The first card, for focus when the page opens. */
    brls::View* getDefaultFocus() override;

    /* THE GRID ANSWERS UP/DOWN; THE FRAMEWORK'S OWN WALK DOES EVERYTHING ELSE, AND THE FRAMEWORK IS
     * THE ONLY THING THAT EVER MOVES FOCUS. This one paragraph is the fix for a bug that cost five
     * rounds, so it is written out in full.
     *
     * WHAT WAS WRONG BEFORE. UP/DOWN were handled by a PER-FRAME POLL of the pad from draw(), which
     * called a `moveFocus` of this class. The framework, meanwhile, was still dispatching the same
     * button press to its own navigation. TWO MOVERS, ONE KEY: one press moved two cells. Four
     * "gates" were added to try to make the poll and the framework agree about which press was
     * which (a dedupe window, a source flag, a release test, a gate window), and all four failed for
     * the same structural reason -- a gate cannot tell "the user pressed again" from "something
     * repeated", so it either lets a duplicate through or eats a real press.
     *
     * WHAT WILIWILI DOES, WHICH IS WHAT THIS NOW DOES. Its grid does NOT move anything. It ANSWERS
     * the framework's question: `RecyclingGrid::getNextCellFocus` (recycling_grid.cpp:561-638) is
     * reached through the content box's `getNextFocus`, and for UP/DOWN it computes the target from
     * the item index and the column count (`index +/- spanCount`), returning the view to focus. The
     * framework's `Application::navigate` (application.cpp:518-583) then does the focusing, the focus
     * sound, the repeat timing and the shake-on-nothing. One dispatch, one movement, per press.
     *
     * SO THIS CLASS HAS NO TIMERS, NO POLL, NO GATE AND NO giveFocus() CALL OF ITS OWN. Where a
     * direction needs grid arithmetic it returns the view; where the framework's own walk is already
     * right, it returns nullptr and lets the walk do it (that is LEFT/RIGHT -- see the
     * implementation). "One press moves one cell" is then a property of the FRAMEWORK, not something
     * this screen can break by accident.
     *
     * THE ARITHMETIC IS NOT HERE. Which item is one row away is hg_grid_row_step, in hg_grid.c,
     * where the host suite can check every case -- the short last row, the row a tier heading owns
     * with no card in it, and the two ends. This class only looks up the view for the index it is
     * given. */
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

    /* The index into `cells` of the view that HAS focus, or -1 when the focus is not on a card of
     * this grid. Needed because the framework's walk reaches `getNextFocus` from the CONTENT BOX and
     * not from the card (view.cpp:1471 passes the focused view along, but box.cpp:418 replaces it
     * with the box), so `currentView` is not the card by the time this screen is asked. The live
     * focus is the reliable answer, and it is the same answer the framework itself uses. */
    int cellIndexOf(brls::View* view) const;

    /* SCROLL THE FOCUSED CARD INTO VIEW, from the framework's own focus hook.
     *
     * Scrolling needs a second look at the framework, because the handover that preceded this change
     * recorded "the framework has no follow-focus at all, updateScrolling() is only reached from
     * draw() while a flag set in willAppear is still standing". That is HALF TRUE and the missing
     * half is the whole feature: `ScrollingFrame::draw` is one call site, and
     * `ScrollingFrame::onChildFocusGained` (scrolling_frame.cpp:478-487) is the other -- it calls
     * `updateScrolling(true)` on EVERY focus change inside the frame, whenever the input is the pad
     * and the behaviour is CENTERED, which is exactly this grid. So the framework does follow the
     * focus, and wiliwili's grid relies on it.
     *
     * It was scrolling to the wrong place, though, and that is this class's fault: updateScrolling
     * asks `getDefaultFocus()` where to scroll to, and this grid's answer used to be "the first card,
     * always" -- which is why the screen snapped back to the top row. getDefaultFocus below now names
     * the FOCUSED card, so the framework's target and this hook's target are the same offset computed
     * two ways. This hook sets it immediately (the formula this app has already had confirmed on
     * hardware) and the framework's animated version then finds it already correct and does nothing. */
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;

    int columnCount() const { return this->columns; }
    float cellWidth() const { return this->lastCellW; }
    size_t cardCount() const { return this->cards; }

    /* Swap between the large and small cell densities. Bound to X, and it does not re-fetch or
     * recreate anything -- see the implementation for what it deliberately leaves alone. */
    void toggleDensity();
    bool isCompactDensity() const { return this->compact; }

    /* See activate(): empty means the home page's own behaviour. */
    void setOnCard(std::function<void(const Series&)> cb) { this->onCard = std::move(cb); }

  private:
    brls::Box* contentBox = nullptr;
    /* One entry per row-model item. A heading entry is nullptr, so an index into this
     * vector is the same index into `places` -- the two never drift apart, which is
     * what makes the placement loop below a single pass instead of two counters that
     * could disagree. */
    std::vector<HomeGridCard*> cells;
    std::vector<brls::Label*> headings;
    /* OWNED, not pointed at: see setRows. */
    std::vector<HomeRow> rows;
    /* Bumped on every rebuild so a cell from a previous row list cannot act on the current one.
     * See the note in setRows -- without it, borealis' deferred deletion lets a stale cell's click
     * action fire against indices that now mean something else. */
    unsigned generation = 0;

    /* The layout is cached so onLayout does not redo the arithmetic on every frame:
     * borealis calls onLayout on resize, and the last width is what decides whether
     * anything actually changed. */
    std::vector<HgGridPlace> places;
    float laidOutWidth = -1.0f;
    /* The density the cached layout was computed for. The early-out in relayout compares BOTH
     * this and the width -- see the note there. */
    bool laidOutCompact = false;
    /* The current density. COMPACT IS THE DEFAULT, at the user's request ("?????????????????):
     * more posters on screen at once, which is what a browse-first grid wants. X still toggles the two,
     * and `home.compact=no` in the cfg starts in the large one instead. */
    bool compact = true;
    int columns = 0;
    float lastCellW = 0.0f;
    size_t cards = 0;
    /* Counted apart from `cards` so the build log can say how many slots went to "more" entries. */
    size_t moreCards = 0;
    int layoutCount = 0;
    /* When the next cover-retry pass may run. See tickCoverRetries. */
    double nextCoverRetryAt = 0.0;

    /* HOW MANY RETRY PASSES THIS GRID HAS MADE. Bounded on purpose: see tickCoverRetries -- an unbounded
     * retry loop was measured at 861 passes in one session. */
    int coverRetryPasses = 0;
    /* The row the viewport was on last pass: the window follows it, and this is what makes the walk ordered.
     * See the note on the pipeline in tickCoverRetries. */
    int coverWindowRow = 0;
    /* What the cover walk logged last, so a steady screen writes nothing (see build118: the per-frame log was
     * hammering the SD card). */
    int coverLoggedViewRow = -1;
    int coverLoggedFirstRow = -1;
    int coverLoggedReleased = -1;
    int coverLoggedAskedCells = -1;
    /* Whether another screen was on top last frame, so the drop/return is logged once rather than every frame. */
    bool screenWasCovered = false;

    /* Opens the detail page for item `index`. Bound as that cell's click action. */
    void openDetail(size_t index);

    /* Called by a cell's click action. Dispatches to `onCard` when the screen set one, and to
     * openDetail (this screen's own behaviour) when it did not. See makePosterGrid. */
    void activate(size_t index);

  public:
    /* THE APPEND AND NEAR-BOTTOM HOOKS ARE PUBLIC because the free functions that makePosterGrid and
     * its siblings expose call them: the grid class itself stays private to this file, so those wrappers
     * are the only door, and they report a wrong type rather than dereferencing it. */
    void appendRows(const std::vector<HomeRow>& extra);
    void setOnNearBottom(std::function<void()> cb);

  private:
    /* SET BY A SCREEN THAT IS NOT THE HOME PAGE, through makePosterGrid. Empty here means "this is the
     * home page: activating a poster opens its detail page". */
    std::function<void(const Series&)> onCard;

    /* Create the view for one row and add it to the content box. Shared by setRows and appendRows. */
    void buildRowView(size_t index);

    void tickNearBottom();

    /* The next moment tickNearBottom may fire. Throttled like the cover retry: a frame runs whether or
     * not anything changed. */
    double nextNearBottomAt = 0.0;
    std::function<void()> onNearBottom;
};

void HomeGridView::setRows(const std::vector<HomeRow>& incoming) {
    this->rows = incoming;
    this->cells.clear();
    /* THE GENERATION GUARD, AND IT IS NOT OPTIONAL.
     *
     * `clearViews(true)` looks like it frees the old cells and it does not free them yet: borealis
     * pushes them onto Application::deletionPool to be deleted later (box.cpp -> view.cpp ->
     * application.cpp:1064). Until that happens the old cells still hold their registered click
     * actions -- and a `brls::RecyclerCell`-style action fires when its view is activated. So after
     * a REFRESH, activating a stale cell would run `openDetail(i)` with an index into the NEW rows,
     * which is either the wrong series or out of range.
     *
     * A code review of this file found that, and it is a crash rather than a cosmetic problem. The
     * guard is a counter: every rebuild increments it, and each cell's callback captures the value
     * it was built under and refuses to act if it no longer matches. That is cheaper and more
     * obvious than trying to track which cells are still alive. */
    this->generation++;
    this->contentBox->clearViews(true);
    this->headings.clear();
    this->places.clear();
    this->laidOutWidth = -1.0f;
    this->cards = 0;

    if (this->rows.empty()) {
        hg::logPrintf("HOMEGRID: no rows -- nothing to build");
        return;
    }

    /* Reserve so the pointer container does not move while cells are added. The cells
     * themselves are new'd and owned by contentBox. */
    this->cells.reserve(this->rows.size());

    for (size_t i = 0; i < this->rows.size(); i++) this->buildRowView(i);

    hg::logPrintf("HOMEGRID: the more cell now carries a drawn mark (chevrons, G3) [build58]");
    hg::logPrintf("HOMEGRID: built %u items (%u cards, %u headings)", (unsigned)this->rows.size(),
                  (unsigned)this->cards, (unsigned)this->headings.size());
    this->relayout();
}

/* CREATE THE VIEW FOR ONE ROW and add it to the content box.
 *
 * Factored out of setRows so that APPENDING rows (the category screen's "keep scrolling" -- see
 * appendRows) builds a row exactly the same way. Two loops that both know how a heading and a "more"
 * card are drawn would be two places for them to drift apart, and this grid's rows are exactly where
 * the bugs have been (the "more" card's own duplicated-row bug included).
 *
 * `this->cells` stays PARALLEL TO `this->rows`: every call pushes exactly one entry, nullptr for a
 * heading (headings are not focusable and are never a focus target). */
void HomeGridView::buildRowView(size_t index) {
    if (index >= this->rows.size()) return;
    const HomeRow& row = this->rows[index];

    if (row.more) {
        /* THE "MORE" CARD, which fills the last slot of a full row so a tier does not spill one card
         * onto a row of its own.
         *
         * It is deliberately NOT a heading: a heading is not focusable and takes a whole row for its
         * text, which is the thing this replaces. This occupies one grid slot, like a poster. */
        auto* cell = new HomeGridCard();
        cell->setPositionType(brls::PositionType::ABSOLUTE);
        {
            Series stub;
            stub.title = hg_tr(HG_T_MORE);
            cell->setData(stub);
            /* THE DRAWN MARK, which is what stops this cell from looking like a card whose cover failed to load --
             * the one thing a cell in a grid must not look like. See MoreMarkView. */
            cell->setMoreCard(true);
        }
        hg::installTapSelect(cell);
        {
            const unsigned gen = this->generation;
            const std::string tier = row.sectionTitle;
            /* THE TIER'S UPSTREAM SLUG travels with the row (HomeRow::tabType), because the category
             * page's URL is not the tier's display name -- see Client::categorySlugForTier, whose
             * mapping is measured by id overlap rather than matched by wording. */
            const std::string tierSlug = Client::categorySlugForTier(row.tabType);
            cell->registerClickAction([gen, tier, tierSlug](brls::View*) {
                if (gen != 0) {
                    /* THIS CARD OPENS THE REAL CATEGORY PAGE. It used to notify "not built yet", which
                     * was the honest answer while there was no category screen. */
                    hg::logPrintf("HOMEGRID: the more card for tier '%s' (slug '%s') -> category page",
                                  tier.c_str(), tierSlug.c_str());
                    brls::Application::pushActivity(new CategoryActivity(tier, tierSlug));
                }
                return true;
            });
        }
        this->contentBox->addView(cell);
        this->cells.push_back(cell);
        this->moreCards++;
        return;
    }

    if (row.header) {
        auto* heading = new brls::Label();
        heading->setText(row.sectionTitle.empty() ? "(untitled section)" : row.sectionTitle);
        heading->setFontSize(24.0f);   /* matches HG_GRID_HEADING_H below */
        heading->setTextColor(brls::Application::getTheme().getColor("brls/accent"));
        heading->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        heading->setPositionType(brls::PositionType::ABSOLUTE);
        heading->setFocusable(false);
        this->contentBox->addView(heading);
        this->headings.push_back(heading);
        this->cells.push_back(nullptr);
        return;
    }

    auto* cell = new HomeGridCard();
    cell->setPositionType(brls::PositionType::ABSOLUTE);
    cell->setData(row.series);
    /* TOUCH TAP = SELECT. The framework gives a ScrollingFrame drag-and-scroll for free but nothing
     * for tapping a child, so a card with no recognizer is draggable and dead -- which is exactly
     * how it was reported ("touch scrolls but will not select"). See hg_touch.hpp. */
    hg::installTapSelect(cell);
    /* `gen` is the generation this cell was built under. See the note in setRows: the cell's
     * view is destroyed LATER than the moment it leaves the tree, so without this check a stale
     * cell can still fire with an index that no longer means what it did. */
    {
        const unsigned gen = this->generation;
        cell->registerClickAction([this, index, gen](brls::View*) {
            if (gen != this->generation) {
                hg::logPrintf("HOMEGRID: a stale cell (generation %u, now %u) was activated -- "
                              "ignored, because its item index refers to the previous row list",
                              gen, this->generation);
                return true;
            }
            this->activate(index);
            return true;
        });
    }
    this->contentBox->addView(cell);
    this->cells.push_back(cell);
    this->cards++;
}

/* APPEND ROWS TO WHAT IS ALREADY ON SCREEN.
 *
 * THIS IS THE DIFFERENCE BETWEEN "MORE" WORKING AND "MORE" RE-DOWNLOADING EVERYTHING. The obvious
 * implementation -- keep every row in one vector and call setRows(all of them) -- recreates every cell,
 * and a recreated cell has a fresh cover slot and a fresh settled flag, so page 2 would re-fetch page
 * 1's twenty-four covers, page 3 would re-fetch forty-eight, and so on: quadratic requests for pictures
 * that are already on screen.
 *
 * So the rows already on screen keep their views, their covers and their click actions, and only the new
 * rows are built. The layout must then be recomputed from scratch (the content is taller), and relayout's
 * early-out is keyed on (width, density) -- neither of which changed -- so the cache is invalidated
 * explicitly here. */
void HomeGridView::appendRows(const std::vector<HomeRow>& extra) {
    if (!this->contentBox || extra.empty()) return;

    const size_t first = this->rows.size();
    this->rows.insert(this->rows.end(), extra.begin(), extra.end());
    this->cells.reserve(this->rows.size());
    for (size_t i = first; i < this->rows.size(); i++) this->buildRowView(i);

    this->laidOutWidth = -1.0f;
    this->places.clear();
    this->relayout();

    hg::logPrintf("HOMEGRID: appended %u rows (now %u items, %u cards)",
                  (unsigned)extra.size(), (unsigned)this->rows.size(), (unsigned)this->cards);
}

/* FIRE A CALLBACK WHEN THE VIEW IS SCROLLED NEAR ITS BOTTOM.
 *
 * The category screen uses this to load the next page, which is how "keep scrolling" works without a
 * "load more" button. Checked from draw() -- the one per-frame hook this view has, the same one the
 * cover retry uses -- and throttled, because a frame runs whether or not anything changed.
 *
 * It fires REPEATEDLY while the user sits at the bottom, deliberately: the owner knows whether it is
 * already loading or has reached the end, and a fire-and-forget edge here would need its own state to
 * re-arm. The cost of a repeated call is one boolean test; the cost of a missed one is a list that stops
 * loading for no visible reason. */
void HomeGridView::setOnNearBottom(std::function<void()> cb) { this->onNearBottom = std::move(cb); }

void HomeGridView::tickNearBottom() {
    if (!this->onNearBottom) return;
    const double now = brls::getCPUTimeUsec() / 1000.0;
    if (now < this->nextNearBottomAt) return;
    this->nextNearBottomAt = now + 400.0;

    const float limit = this->getContentHeight() - this->getHeight();
    /* CONTENT THAT FITS HAS NO BOTTOM TO REACH, so nothing fires: a category with a single page of
     * results is complete and must not ask for more for ever. */
    if (limit <= 1.0f) return;
    if (this->getContentOffsetY() < limit - 80.0f) return;

    this->onNearBottom();
}

/* WHAT ACTIVATING A POSTER MEANS. The default is this screen's own behaviour -- open the detail page --
 * and it is what the home page gets by not setting anything. The search and category screens set their
 * own callback instead, which is why this is one function with one branch rather than the click action
 * reaching into the activity: those screens show posters that are NOT home rows, and they need to say
 * what a poster means on THEIR screen. See makePosterGrid. */
void HomeGridView::activate(size_t index) {
    if (this->onCard) {
        if (index < this->rows.size() && !this->rows[index].header) {
            const Series s = this->rows[index].series;
            hg::logPrintf("HOMEGRID: activate item %u -> the screen's own handler", (unsigned)index);
            this->onCard(s);
        }
        return;
    }
    this->openDetail(index);
}

void HomeGridView::openDetail(size_t index) {
    if (index >= this->rows.size()) {
        hg::logPrintf("HOMEGRID: select of item %u is out of range (%u rows) -- ignored",
                      (unsigned)index, (unsigned)this->rows.size());
        return;
    }
    const HomeRow& row = this->rows[index];
    if (row.header) {
        hg::logPrintf("HOMEGRID: heading %u selected; ignoring", (unsigned)index);
        return;
    }
    brls::Application::pushActivity(new DetailActivity(row.series));
}

void HomeGridView::relayout() {
    if (this->rows.empty()) return;

    const float width = this->getWidth();
    if (width <= 0.0f) {
        /* NO WIDTH YET, SO NO LAYOUT -- BUT NOT SILENTLY, AND NOT FOREVER.
         *
         * This returns before `onLayout` has ever run with a real width, so the content box is still
         * at whatever height it was created with (0). That matters because `ScrollingFrame` computes
         * its scroll range from `getContentHeight()`: a content box of height 0 means a scroll range
         * of 0, and every cell is then clipped away -- a blank screen that looks like a failed fetch.
         *
         * A code review of this file flagged it. Two things are done about it: the state is LEFT
         * LOGGED (so "the grid is blank" has a reason in the log rather than being a mystery), and
         * the content box is given a height of one screen's worth so that if a layout never arrives
         * the frame still has a sane range to work with rather than a degenerate one.
         *
         * A LATER width of 0 does NOT take this branch in a damaging way: the previous layout's
         * placements are still in the content box, so the grid keeps its last good shape instead of
         * collapsing. That is the right failure mode for a temporary zero during a resize. */
        if (this->laidOutWidth < 0.0f) {
            static bool said = false;
            if (!said) {
                said = true;
                hg::logPrintf("HOMEGRID: relayout called with no width yet (%.0f) -- deferring to "
                              "the first onLayout",
                              width);
            }
            if (this->contentBox->getHeight() <= 0.0f) {
                this->contentBox->setHeight(this->getHeight() > 0.0f ? this->getHeight() : 720.0f);
            }
        }
        return;
    }
    /* THE EARLY-OUT IS ON *WHICH* LAYOUT, NOT ONLY ON THE WIDTH.
     *
     * The first version returned whenever the width had not changed, which was correct while the
     * width was the only input. The density toggle adds a second input, and without this check a
     * toggle at a fixed width would compute nothing and move nothing -- a key that silently does
     * nothing, which is the failure this project keeps designing against. So the cache key is the
     * pair.
     *
     * The cache still exists and still matters: onLayout runs on resize and relayout runs after
     * every data rebuild, and neither should redo the arithmetic when nothing changed. */
    if (width == this->laidOutWidth && this->laidOutCompact == this->compact) return;

    /* The content box is as wide as the frame; its padding is zero so the poster
     * geometry in hg_grid.c is measured from the same edge the columns are laid out
     * from. The PAGE's margins live on the Activity's outer box. */
    const float contentW = width;

    /* THE REQUESTED POSTER WIDTH, and what "small/large cells" means: two values of it.
     *
     * 200 logical pixels at 1280x720 gives 6 columns, which is close to the 5-6 wiliwili shows
     * at the same logical size and keeps a poster big enough to recognise. The compact mode
     * asks for about two thirds of that, which at the same width gives 8 columns. The column
     * count is DERIVED from the width either way, never fixed -- the request was explicit about
     * that, and it is what makes the toggle behave sensibly at every ui.scale.
     *
     * `home.cellW` overrides the LARGE value only. The compact value is derived from it rather
     * than having its own key, so a user who retunes the poster size does not have to remember
     * to retune a second number that only exists when the toggle is on. */
    const int largeCellW = cfgGridInt("home.cellW", HG_GRID_DEFAULT_CELL_W);
    /* `compact` is the DEFAULT now, so the multiply flips: compact uses the configured width as-is and
     * the LARGE layout asks for more. Keeping the 55% relationship in one direction only would have made
     * X shrink a grid that is already small. */
    const int targetCell = this->compact ? largeCellW : (largeCellW * 100 / HG_GRID_COMPACT_PERCENT);

    /* THE COMPACT LAYOUT ASKS FOR A HIGHER COLUMN CEILING, and that is not a detail: with the
     * default ceiling of 8 the toggle changed the poster size and left the column count alone at
     * 720p and above, which is a key that looks like it works and does not. MEASURED at 720p
     * with a 110 pt poster the arithmetic gives 11 columns, so 11 is what the small layout is
     * meant to show. hg_grid.h carries the measurements and hg_grid_test pins the relationship. */
    const int maxCols = this->compact ? HG_GRID_MAX_COLUMNS_HARD : 0;

    std::vector<int> kinds(this->rows.size());
    for (size_t i = 0; i < this->rows.size(); i++) {
        kinds[i] = this->rows[i].header ? HG_GRID_ITEM_HEADING : HG_GRID_ITEM_CARD;
    }

    this->places.assign(this->rows.size(), HgGridPlace());
    float total = 0.0f;
    int placed = hg_grid_layout(kinds.data(), kinds.size(), contentW, (float)targetCell, maxCols,
                                HG_GRID_HEADING_H, HG_GRID_ROW_SPACE, HG_GRID_ITEM_SPACE,
                                &POSTER_METRICS, this->places.data(),
                                this->places.size(), &total);

    if (placed != (int)this->rows.size()) {
        /* The layout refused or truncated: show nothing rather than a half-drawn
         * grid. The log says which, because "the home page is empty" on its own does
         * not distinguish a failed fetch from a failed layout. */
        hg::logPrintf("HOMEGRID: layout placed %d of %u items at width %.0f -- NOT showing a "
                      "partial grid (see hg_grid_layout's contract)",
                      placed, (unsigned)this->rows.size(), contentW);
        for (size_t i = 0; i < this->cells.size(); i++) {
            if (this->cells[i]) this->cells[i]->setVisibility(brls::Visibility::GONE);
        }
        this->laidOutWidth = width;
        this->laidOutCompact = this->compact;
        return;
    }

    /* WHICH CELL WAS FOCUSED, so the toggle can put focus back where it was.
     *
     * This is the part of a relayout that is easy to forget and unpleasant to hit: every cell's
     * coordinates change, and borealis' focus is a plain View pointer. The views themselves are
     * NOT recreated -- the toggle only moves them -- so the focus pointer stays valid and simply
     * travels with its cell. Recording the INDEX as well lets the log say which card the user
     * was on, and gives the giveFocus() below something to aim at if focus was lost. */
    View* focused = brls::Application::getCurrentFocus();
    int focusedIndex = -1;
    for (size_t i = 0; i < this->cells.size(); i++) {
        if (this->cells[i] && focused && this->cells[i] == focused) {
            focusedIndex = (int)i;
            break;
        }
    }

    this->columns = hg_grid_columns_for(contentW, (float)targetCell, maxCols);
    this->lastCellW = placed > 0 ? this->places[0].cellW : 0.0f;
    this->laidOutWidth = width;
    this->laidOutCompact = this->compact;

    this->contentBox->setHeight(total);

    size_t headingIndex = 0;
    for (size_t i = 0; i < this->places.size(); i++) {
        const HgGridPlace& p = this->places[i];
        if (!this->cells[i]) {
            if (headingIndex < this->headings.size()) {
                auto* heading = this->headings[headingIndex++];
                heading->setPositionTop(p.y);
                heading->setPositionLeft(p.x);
                heading->setWidth(p.w);
                heading->setHeight(p.h);
            }
            continue;
        }
        auto* cell = this->cells[i];
        cell->setPositionTop(p.y);
        cell->setPositionLeft(p.x);
        cell->setWidth(p.w);
        cell->setHeight(p.h);
        cell->setVisibility(brls::Visibility::VISIBLE);
        /* The secondary line (the genre tags) only fits when the cell is tall enough to hold it
         * without crowding the title -- which is the same question as "is this the compact
         * layout", asked once here rather than at every cell bind. */
        cell->setCompact(this->compact);
    }

    this->layoutCount++;
    hg::logPrintf("HOMEGRID: layout #%d w=%.0f %s -> %d columns, cell %.1fx%.1f, contentH=%.0f "
                  "(%u cards, %u headings, focus was item %d)",
                  this->layoutCount, width, this->compact ? "COMPACT" : "large", this->columns,
                  this->lastCellW, this->places[0].cellH, total, (unsigned)this->cards,
                  (unsigned)this->headings.size(), focusedIndex);

    /* PUT FOCUS BACK. The pointer usually survived -- the views were moved, not replaced -- but
     * if the focused cell was somehow not in the list (a heading, or focus on the frame itself),
     * aiming at the first card is strictly better than leaving focus nowhere. */
    if (focusedIndex >= 0 && this->cells[(size_t)focusedIndex]) {
        auto* view = this->cells[(size_t)focusedIndex]->getDefaultFocus();
        if (view) brls::Application::giveFocus(view);
    }
}

/* THE SMALL/LARGE TOGGLE (X). wiliwili registers its own layout switch on X
 * (player_base_activity.cpp:212, and its home screen uses X the same way), so this keeps the
 * same key for the same job.
 *
 * WHAT IT CHANGES, AND WHAT IT DELIBERATELY DOES NOT. Only the requested poster width, from
 * which the column count falls out. It does NOT change what is fetched, does not clear the
 * covers already in flight, and does not recreate a single view: the cells are the same objects
 * at new coordinates. That is why the toggle is cheap enough to run on a keypress, and it is why
 * the covers the user has already seen stay on screen instead of being re-downloaded in a new
 * density. */
void HomeGridView::toggleDensity() {
    this->compact = !this->compact;
    hg::logPrintf("HOMEGRID: X -> %s cells", this->compact ? "SMALL" : "LARGE");
    this->relayout();
}

void HomeGridView::onLayout() {
    brls::ScrollingFrame::onLayout();
    this->relayout();
}

/* RETRY THE COVERS THAT WERE REFUSED FOR BEING OVER THE BUDGET.
 *
 * MEASURED ON HARDWARE: the layout binds all 36 cells at once, the image budget allows six in
 * flight, and the rest were refused -- with nothing to come back for them, so thirty cards kept
 * their placeholder for ever. That is the missing-poster screenshot.
 *
 * This runs from draw(), throttled, rather than from relayout(): relayout only fires when the width
 * or the density changes, and neither happens while the user is simply looking at the screen. One
 * pass every 400 ms keeps six fetches in flight until everything has arrived, and then stops asking
 * entirely -- each cell that has its picture no longer offers a retry, so the cost goes to zero
 * once the screen is complete. */
void HomeGridView::tickCoverRetries() {
    /* ---- NO TIMER GATE ANY MORE (build103, and the user is right about why) -----------------------------
     *
     * THEIR WORDS: "after a row finishes, the next row comes more than three seconds later. It should be: as soon
     * as a download completes and is handed to the next step, the slot that just freed up starts the next
     * download, with a check whether the next step has room; if it has room, keep passing it along."
     *
     * That is what a pipeline is, and the thing that broke it was this function's own timer: it ran on a 400 ms
     * heartbeat that GREW BY FOUR TIMES every pass (400, 1600, 6400, 10000...) because an earlier build was trying
     * to stop a retry storm. A row could finish in a second and the next row still waited six seconds for its turn.
     *
     * So the walk now runs EVERY FRAME and starts as much as the loader will accept. The loader's own in-flight cap
     * (image.threads = 8) is the throttle, so a slot that frees is filled on the very next frame -- which is exactly
     * the "hand it to the next step and keep going" the user described. The walk is a few dozen atomic reads, so
     * doing it per frame costs nothing. */

    /* ---- THE RETRIES ARE BOUNDED NOW, AND THAT IS A RESOURCE FIX (build87) ------------------------------
     *
     * MEASURED ON HARDWARE: the log carried `HOMEGRID: cover retry pass 861 (32 cells asked)` -- eight hundred
     * and sixty-one passes, every 400 ms, each asking all 32 cards to fetch again. That is ~27,000 fetch
     * attempts in one session, and every attempt is a thread, a DNS lookup and a TLS handshake for a picture the
     * service is refusing to hand over.
     *
     * The deko3d crash on this console (`Result 0x767`, `CmdBuf::requestCmdMem` out of command memory) has the
     * image-loading path in its stack, and the user's own summary of this whole batch of work was "the principle
     * is to use as few resources as possible and never crash". A retry loop that never gives up is exactly the
     * shape that violates it.
     *
     * THREE PASSES, then the grid stops asking. Whatever arrived has arrived; the cards that did not get a cover
     * keep the dark placeholder, and the log says how many were left over. A screen that keeps retrying for ever
     * is not more likely to succeed -- it is only more likely to be the frame that runs out of memory. */
    /* ---- BACKING OFF, NOT GIVING UP (build89) ------------------------------------------------------
     * The first version of this stopped after three passes, and the user immediately reported the
     * consequence: "many cards in the 更多 screen show no cover any more". They were refused by the image
     * budget while several fetches were already in flight, and with the retries gone they never got a
     * second chance.
     *
     * So the interval GROWS instead of the attempts stopping: 400 ms, then four times that each pass,
     * capped at ten seconds. After a minute the grid is asking about six times a minute rather than a
     * hundred and fifty -- which was the actual resource problem -- while a picture that was merely
     * unlucky still arrives. */
    this->coverRetryPasses++;

    /* ---- THE ORDERED COVER PIPELINE (build102, and the design is the user's) ----------------------------
     *
     * THEIR WORDS: "there is one change I want: it sometimes skips a row. It is exactly eight, so it should load
     * row by row, but it leaves one or two rows empty and loads the rows below instead. Make it so that when the
     * 更多 page opens, it detects which row it is on and PRELOADS THE NEXT FOUR ROWS -- and that preload still has
     * to go through the same load/unload pipeline, just without stopping: keep walking that pipeline and load
     * those four rows one row at a time. That way, while we scroll, some of it is always on screen. When it
     * reaches 168, start clearing the FIRST row: we cannot see the content above anyway. And when we scroll back
     * up, those disappeared parts are preloaded again, the same four rows at a time, so that something is always
     * there."
     *
     * That is a sliding window over the rows, and it is the right shape for this problem. Three parts:
     *
     *   1. THE WINDOW: the row the viewport is on, plus the next four. Only those rows are ever requested, in
     *      order, which is what stops the skipping -- the previous loop asked EVERY cell in the grid, so the
     *      loader's eight slots were filled by whichever eight cells came first and a row could be left behind
     *      while later rows went ahead.
     *   2. ONE ROW AT A TIME, AND "DONE" MEANS SETTLED: a row with a cell that is still PENDING is asked, and the
     *      walk stops there for this pass. Eight cells and eight slots is exactly one row in flight, which is the
     *      behaviour the user asked for in the sentence about "it is exactly eight".
     *   3. EVICTION, NOT DESTRUCTION: rows that fall out of the window are released through the ordinary
     *      load/unload path (releaseCoverTexture / reloadReleasedCover), so scrolling back re-requests them exactly
     *      the way the first load did. Nothing about the pipeline is special-cased for them -- which was the
     *      user's other requirement.
     *
     * The window is computed from the same absolute geometry the grid draws with (View::getY walks up the parents
     * and includes the scroll translation), so it follows the viewport whatever input moved it. */
    const int cols = this->columnCount() > 0 ? this->columnCount() : 8;
    const int total = (int)this->cells.size();
    if (total <= 0) return;

    /* WHICH ROW IS THE VIEWPORT ON? The first cell that intersects it, in absolute coordinates. */
    const float selfTop = this->getY();
    const float selfH = this->getHeight() > 0.0f ? this->getHeight() : 720.0f;
    int viewRow = 0;
    bool foundViewRow = false;
    for (int i = 0; i < total; i++) {
        HomeGridCard* cell = this->cells[i];
        if (!cell || cell->getHeight() <= 0.0f) continue;
        const float top = cell->getY();
        const float bottom = top + cell->getHeight();
        if (bottom >= selfTop && top <= selfTop + selfH) {
            viewRow = i / cols;
            foundViewRow = true;
            break;
        }
    }
    if (!foundViewRow) viewRow = this->coverWindowRow;
    this->coverWindowRow = viewRow;

    const int firstRow = (viewRow > 1) ? viewRow - 1 : 0;          /* one row above, so scrolling back is ready */
    const int lastRow = viewRow + HG_COVER_PRELOAD_ROWS;           /* and four below, as the user asked */
    const int lastRowIdx = (total - 1) / cols;

    /* ---- EVERY CELL IS ASKED AGAIN (build122, and it is the user's "later rows do not load") ------------
     *
     * MEASURED ON HARDWARE, from the user: "only the first and second rows load; the third and fourth never get
     * their covers. The earlier version was fine." They are right, and this loop is why.
     *
     * build102 replaced "ask every cell" with a window walk: only the rows between `firstRow` and
     * `lastRow` are ever requested, one row per pass. When the row the viewport is on is computed correctly that
     * is exactly the behaviour asked for -- but it is a single point of failure, and on a screen whose geometry it
     * misreads (a freshly built grid before layout has placed the cells, a scroll it does not follow, a heading
     * that shifts the row arithmetic) the rows BELOW the window are never asked at all. That is the symptom: the
     * first rows arrive, everything after them stays dark for ever.
     *
     * So the walk goes back to what the version they call "fine" did: every cell is asked, and the LOADER decides
     * -- its in-flight cap, its own state machine -- while the cells that were refused are simply asked again on
     * the next pass. The log line below stays rate limited (that was the "startup got slower" fix, and it is
     * unrelated to which cells are asked).
     *
     * THE WINDOW IS STILL USED FOR EVICTION ONLY, which is the part the user explicitly asked for ("at 168, start
     * clearing the front rows"): refusing to load is what broke the grid, but handing textures back when they are
     * far off screen is safe -- a released cell is re-requested the moment it is asked again. */
    /* ---- ASK THE WINDOW FIRST, THEN EVERYTHING ELSE (build162) ------------------------------------------
     *
     * MEASURED ON HARDWARE (the user's report "covers load up to about 190 and then stop while the rows below
     * keep refreshing", log 2026-10-14 09:27-09:32 on the 更多 page with 376 cards):
     *
     *   - 151 distinct covers arrived in the first two minutes, and then NOT ONE more for the remaining
     *     three and a half minutes, while the loader kept working at full rate (2454 installs);
     *   - the same covers were installed 200-260 times each (2454 installs for 151 URLs -- 16x);
     *   - `askedCells` sat pinned at 376, i.e. every cell in the grid, for the whole session.
     *
     * The loop below asked every cell IN LIST ORDER (build122's change, and the reason for it still stands).
     * What it collided with is the eviction a few lines further down: rows above and below the window hand
     * their textures back, which puts them to PENDING -- and the rows ABOVE the viewport are the FIRST cells
     * in the list, so they are always the first candidates and always win the loader's few slots. They are
     * installed, released on the next pass, asked again, and installed again, for ever, while the rows the
     * user is actually looking at never get a slot at all.
     *
     * So the order is inverted: the WINDOW is asked first, then the rest of the grid exactly as before. The
     * eviction is unchanged, build122's "ask everything, the loader decides" is unchanged, and the cells that
     * are on screen stop losing the race to the ones that are not. */
    int askedCells = 0;
    auto askCell = [&](size_t i) {
        if (i >= this->cells.size() || !this->cells[i]) return;
        HomeGridCard* cell = this->cells[i];
        if (cell->hasReleasedCover()) {
            cell->reloadReleasedCover();
            askedCells++;
            return;
        }
        const int st = cell->coverState();
        if (st == HG_IMG_INSTALLED || st == HG_IMG_FAILED || st == HG_IMG_IN_FLIGHT) return;
        cell->retryCoverIfMissing();
        askedCells++;
    };

    /* PASS 1: the rows the viewport is on (one above through four below), in order. */
    for (int row = firstRow; row <= lastRow; row++) {
        if (row > lastRowIdx) break;
        const int start = row * cols;
        for (int k = 0; k < cols; k++) askCell((size_t)(start + k));
    }
    /* PASS 2: every other cell, in list order -- the part that keeps a row from being left dark for ever. */
    for (size_t i = 0; i < this->cells.size(); i++) {
        const int row = (int)i / cols;
        if (row >= firstRow && row <= lastRow) continue;
        askCell(i);
    }

    /* ---- THE EVICTION: ROWS ABOVE THE WINDOW GIVE THEIR TEXTURES BACK ------------------------------------
     *
     * The user's number: "when it reaches 168, start clearing the first row" -- 168 is 21 rows of 8, so the rule
     * is "keep the window plus a margin, release what is further above than that". Eviction goes through
     * releaseCoverTexture, which is the same unload path everything else uses, and the cell is marked so the walk
     * above re-requests it the moment it comes back into the window. */
    int released = 0;
    const int keepFromRow = (viewRow > HG_COVER_KEEP_ROWS_ABOVE) ? viewRow - HG_COVER_KEEP_ROWS_ABOVE : 0;
    for (int row = 0; row < keepFromRow; row++) {
        const int start = row * cols;
        for (int k = 0; k < cols; k++) {
            const int i = start + k;
            if (i >= total || !this->cells[i]) continue;
            HomeGridCard* cell = this->cells[i];
            if (cell->coverState() == HG_IMG_INSTALLED && !cell->hasReleasedCover()) {
                cell->releaseCoverTexture();
                released++;
            }
        }
    }
    /* And the same above the window's bottom end: a long scroll down leaves rows below that are far away. */
    const int keepToRow = viewRow + HG_COVER_PRELOAD_ROWS + HG_COVER_KEEP_ROWS_ABOVE;
    for (int row = keepToRow; row <= lastRowIdx; row++) {
        const int start = row * cols;
        for (int k = 0; k < cols; k++) {
            const int i = start + k;
            if (i >= total || !this->cells[i]) continue;
            HomeGridCard* cell = this->cells[i];
            if (cell->coverState() == HG_IMG_INSTALLED && !cell->hasReleasedCover()) {
                cell->releaseCoverTexture();
                released++;
            }
        }
    }

    /* ---- ONE LINE PER CHANGE, NOT ONE PER FRAME (build118, and it is the "startup got slower" fix) --------
     *
     * MEASURED ON HARDWARE, from the user: "launching from the Switch home menu into the app feels slower than it
     * used to." The log shows why, and it is this line:
     *
     *     HOMEGRID covers: window=row 0..4 (view row 0 of 4) -- asked row 0 (6 cells), released 0; pass N
     *
     * repeated once per FRAME -- hundreds of times a second while the home screen comes up -- and every line in
     * this project's log is an open + write + fsync by design, so that a crash cannot lose the last breadcrumb. The
     * walk was therefore asking the SD card for a few hundred syncs per second. That is the slowdown, and it is my
     * own build103 change that exposed it (the walk moved from a 400 ms heartbeat to every frame, to remove a three
     * second gap between rows).
     *
     * The walk itself stays per frame -- that is what keeps the rows coming without gaps -- and only the LOG is
     * rate limited: it is written when the numbers it reports actually change. A steady screen now costs nothing,
     * and the SD card stops being hammered. */
    const bool changed = (viewRow != this->coverLoggedViewRow) || (firstRow != this->coverLoggedFirstRow) ||
                         (released != this->coverLoggedReleased) ||
                         (askedCells != this->coverLoggedAskedCells);
    if (changed) {
        this->coverLoggedViewRow = viewRow;
        this->coverLoggedFirstRow = firstRow;
        this->coverLoggedReleased = released;
        this->coverLoggedAskedCells = askedCells;
        hg::logPrintf("HOMEGRID covers: window=row %d..%d (view row %d of %d) -- asked row %d (%d cells), "
                      "released %d; pass %d [build118: on change, not per frame]",
                      firstRow, lastRow, viewRow, lastRowIdx, firstRow, askedCells, released,
                      this->coverRetryPasses);
    }
}

/* ---- EVERYTHING IS DROPPED WHEN THIS SCREEN IS COVERED (build102, the user's instruction) --------------
 *
 * THEIR WORDS: "and then when we go into the player, or the episode page, or any other screen, this screen is
 * cleared completely."
 *
 * That is the other half of the memory story, and it is the half a sliding window cannot do by itself: the home
 * page is NOT destroyed when the player is pushed on top of it -- it stays in the activity stack with every cover
 * it had loaded, while the player allocates a 3 MB surface and mpv's own buffers. The window keeps the GRID small
 * while it is being used; this keeps it empty while it is not.
 *
 * The test is the activity stack: the grid belongs to the bottom activity, so anything above one entry means
 * something is covering this screen. Coming back is free -- every release marks its cell as "hand me a new one",
 * so the ordinary walk in tickCoverRetries re-requests them one row at a time, which is exactly the pipeline the
 * user asked for on the way back up. */
void HomeGridView::dropAllCoversIfCovered() {
    const bool covered = brls::Application::getActivitiesStack().size() > 1;
    if (covered == this->screenWasCovered) return;
    this->screenWasCovered = covered;
    if (!covered) {
        hg::logPrintf("HOMEGRID covers: this screen is visible again -- the walk will reload its rows [build102]");

        /* ---- AND THE SIDEBAR IS RE-READ HERE, BECAUSE ITS FLAGS WERE COMPUTED ONCE (build131) ----------
         *
         * MEASURED ON HARDWARE, from the user: "Y shows the favourite is on, and the X twice cleared the
         * history -- but on the home screen the star next to 历史 is still grey and cannot be selected."
         *
         * They are right, and this is the reason: haveWatchlist / haveHistory are set in
         * HomeActivity::setupSidebar() from the store's counts, and that runs when the home screen is BUILT. Add a
         * favourite on the detail screen and the home screen still holds the count it read on the way in, so the
         * entry stays dim until the app restarts -- which reads as "the favourite did not work".
         *
         * This is the exact moment the screen becomes visible again (the activity stack went from >1 back to 1),
         * so the counts are re-read here, through the activity's own setupSidebar(), which rebuilds the bar and
         * writes the same log line. Nothing is polled: it happens once per return.
         *
         * ---- THE REFRESH IS GONE FROM HERE (build133, and the star problem goes with it) ----------------
         *
         * build131 re-read the sidebar counts at this point; build132 deferred it with brls::sync; the console
         * crashed BOTH times. The log stops immediately after "HOME: sidebar wired (5 items; ...)" with no
         * exception line -- a hard fault while the bar was rebuilt from inside the frame that was drawing it.
         * brls::sync is not enough because it still runs WITHIN the current frame (see the project's own note in
         * detail_activity.cpp), and rebuilding the bar replaces the view the framework is walking.
         *
         * AND THE UNDERLYING NEED IS GONE TOO. The user's instruction: "just remove that favourite entry from the
         * home screen; favourites are reached from the history screen, the way it was." The library screen this
         * home entry opened already carries both tabs, so the home does not need its own 收藏 item at all -- one
         * entry point, and no sidebar rebuild on the way back, which is exactly the code path that was crashing. */
        return;
    }
    int dropped = 0;
    for (size_t i = 0; i < this->cells.size(); i++) {
        if (!this->cells[i]) continue;
        if (this->cells[i]->coverState() == HG_IMG_INSTALLED && !this->cells[i]->hasReleasedCover()) {
            this->cells[i]->releaseCoverTexture();
            dropped++;
        }
    }
    hg::logPrintf("HOMEGRID covers: another screen is on top, so all %d loaded covers were released -- this is "
                  "the memory the player needs [build102]", dropped);
}

void HomeGridView::draw(NVGcontext* vg, float x, float y, float width, float height,
                        brls::Style style, brls::FrameContext* ctx) {
    this->dropAllCoversIfCovered();
    this->tickCoverRetries();
    /* AND THE "AM I AT THE BOTTOM" CHECK, which is how a screen loads the next page. Both live here
     * because draw() is the only per-frame hook this view has. */
    this->tickNearBottom();

    /* NO INPUT IS READ HERE ANY MORE, AND THAT IS THE FIX, NOT A TIDY-UP.
     *
     * This function used to poll the pad every frame and move the focus itself, because the input
     * that arrived as an event seemed not to reach the grid. It does reach it -- `Application::navigate`
     * has always asked the focused view where to go next -- and having BOTH the poll and the framework
     * move the cursor is what produced "one press moves two cells". The poll is gone rather than
     * disabled: a second mover that is only switched off is one `if` away from coming back, which is
     * exactly how this bug survived four rounds of fixes.
     *
     * What is left here is the cover retry tick, which is a per-frame job that has nothing to do with
     * navigation. */

    /* ---- OFF-SCREEN CELLS ARE NOT DRAWN (build90), AND IT IS THE FIX FOR THE CRASH ----------------------
     *
     * MEASURED, from the console's crash report and the log beside it:
     *
     *     Result 0x767
     *       dk::detail::RaiseError -> CmdBuf::requestCmdMem        <- deko3d ran out of command memory
     *         <- CmdBufWriter::reserve
     *         <- CmdBuf::draw <- nvgEndFrame <- Application::frame()
     *
     * and in the same session: "HOMEGRID: appended 24 rows (now 144 items, 144 cards)".
     *
     * So the frame that overflowed was a frame in which the grid held 144 cells -- and EVERY one of them was
     * drawn, including those hundreds of pixels above and below the visible area. A card is a rounded rectangle,
     * a cover image, a label and sometimes a heading: 144 of them is several hundred nanovg shapes per frame, and
     * that is what exhausted the command buffer. It is also why these screens feel slow -- the same work was
     * being done for cards nobody can see, on every frame.
     *
     * THE CULL IS `INVISIBLE`, NOT `GONE`: `GONE` removes the cell from the layout, and this grid places its
     * cells absolutely (their geometry comes from hg_grid_layout, which the host suite checks), so the geometry
     * must not move. `INVISIBLE` keeps the rectangle and skips the draw, which is exactly the trade wanted.
     *
     * A GENEROUS MARGIN, AND THE FOCUSED CELL IS ALWAYS KEPT. A cell scrolled off screen must stay reachable by
     * the D-pad, and borealis cannot focus an invisible view -- so the drawn window is the visible area plus a
     * full screen on each side, and whatever holds focus is forced visible wherever it is. Moving the cursor
     * towards an off-screen cell therefore reveals it long before the cursor gets there.
     *
     * ---- IT FAILS OPEN, AND THAT IS THE FIX FOR WHAT THE FIRST VERSION BROKE (build91) -------------------
     *
     * MEASURED ON HARDWARE, from the user: "in the 更多 screen, once it has shown 90 or 120 items, scrolling
     * further makes them ALL disappear."
     *
     * The first version culled on `cell->getY()` alone -- and a cell whose rectangle has not been placed yet, or
     * a frame in which the offset is momentarily stale (the base class animates it), both read as "outside", and
     * every visible cell was switched off at once. A cull that hides the WRONG things is worse than no cull at
     * all: the screen goes blank and looks like a crash.
     *
     * So three guards, all of which resolve towards DRAWING:
     *   1. no usable viewport height -> do nothing;
     *   2. a cell with no height yet (layout has not placed it) -> keep it;
     *   3. if this frame would hide more than 90% of the cells, that is not a window, that is a bad number:
     *      skip the cull for this frame and say so once. */
    /* ---- THE CULL IS OFF (build99) ---------------------------------------------------------------------
     *
     * The user hit this build's crash twice with the same log: the home screen attaches its 36 rows, eight covers
     * start fetching, and the process is killed with no crash report. Their instruction was explicit -- if it still
     * crashes, put the cover path back to exactly what it was and add improvements back one at a time, each one
     * verified on the console. So every mechanism I added to the render path is now switched off, and the grid
     * draws every cell exactly as it did before build90.
     *
     * What that costs: the cull was the fix for a REAL crash (a frame that drew 144 cells and exhausted the deko3d
     * command buffer), so that crash may come back on very long screens. It is cheaper to bring back with evidence
     * than to keep a mechanism that stops the app starting. */
    /* ---- THE CULL IS BACK ON (build104), AND THE LOG IS THE REASON ------------------------------------
     *
     * MEASURED, from the console: the crash the user reported at 168+ happened right after
     * "HOMEGRID: appended 24 rows (now 192 items, 192 cards)" -- and the eviction they asked about IS running
     * ("released 8" appears in the log), but it only hands back the TEXTURES. The CELLS keep accumulating: the
     * category screen appends 24 more every page, and with this switch off every one of those 192 cells was DRAWN
     * on every frame, which is what exhausts the deko3d command buffer (the crash build90 fixed by turning this
     * on, and which came back when build99 turned it off to isolate the cover-loading problem).
     *
     * Turning it on is safe now for a reason that did not hold back then: the "blank third row" that made me
     * distrust it came from the old release-and-refetch oscillation, and that mechanism has been replaced by the
     * user's ordered window walk, which reloads a released row deliberately, one row per pass, rather than
     * depending on the visibility flag. This switch now does one job only: do not draw what is off screen. */
    const bool cullEnabled = true;
    if (cullEnabled && height > 0.0f && !this->cells.empty()) {
        /* ---- THE WINDOW IS IN ABSOLUTE COORDINATES, AND THAT IS THE FIX (build91) ------------------------
         *
         * MEASURED ON HARDWARE, from the user: "scrolling back up with touch brings them back, but as soon as I
         * scroll down they all disappear."
         *
         * That is the signature of comparing two numbers from different spaces. `View::getY()` (view.cpp:1235)
         * WALKS UP THE PARENTS and adds `translation`, so a cell's y is an ABSOLUTE screen coordinate that
         * DECREASES as the content scrolls up. `ScrollingFrame::contentOffsetY` on the other hand is a positive
         * scroll AMOUNT clamped to [0, contentHeight] (scrolling_frame.cpp:424-431) that INCREASES as the user
         * scrolls down. The first version put them in one expression, so the window and the cells moved apart in
         * opposite directions -- and once they no longer overlapped, every cell was culled at once.
         *
         * So both sides are absolute now: the grid's own y is the top of the viewport, and a cell counts as
         * visible if its absolute rectangle overlaps the viewport plus a screen of margin. `contentOffsetY` is not
         * consulted at all, which is the point -- there is nothing left to get backwards. */
        const float selfTop = this->getY();
        const float selfH = this->getHeight();
        const float winTop = selfTop - selfH;
        const float winBottom = selfTop + selfH * 2.0f;   /* one screen of margin on each side */
        brls::View* focused = brls::Application::getCurrentFocus();

        int hidden = 0;
        int released = 0;   /* covers handed back this frame; see releaseCoverTexture */
        int reloaded = 0;   /* covers re-requested because their cell came back into view */
        int placeable = 0;
        for (size_t i = 0; i < this->cells.size(); i++) {
            HomeGridCard* cell = this->cells[i];
            if (!cell) continue;
            /* A cell the layout has not placed is NOT culled: freshly appended pages were being switched off
             * before their geometry existed, which is the second half of what the user saw. */
            if (cell->getHeight() <= 0.0f) {
                cell->setVisibility(brls::Visibility::VISIBLE);
                continue;
            }
            placeable++;
            const float top = cell->getY();
            const float bottom = top + cell->getHeight();
            const bool inside = (bottom >= winTop) && (top <= winBottom);
            const bool wantVisible = inside || cell == focused;

            /* ---- TWO SCREENS, NOT ONE, BEFORE A TEXTURE IS HANDED BACK (build97) -----------------------
             *
             * MEASURED ON HARDWARE, from the user: "the third row did show up after a while, and then it crashed
             * while I was scrolling and more rows appeared."
             *
             * One screen of margin was the first attempt and it produced exactly that: a cell just outside the
             * drawn window was released, the window followed the scroll, the cell came back, it was re-fetched --
             * and because a fetch takes a second or so, the cell was released again before its picture arrived. A
             * row just outside the window oscillated between released and pending for ever: that is the delayed
             * third row, and the oscillation is also a fetch storm, which is the crash.
             *
             * So releasing now happens only WELL outside (two screens beyond the drawn window). That leaves a wide
             * band in which a cell keeps its picture, and ordinary scrolling never enters this path at all. */
            const float cellTop = cell->getY();
            const float cellBottom = cellTop + cell->getHeight();
            const bool farAway = (cellBottom < winTop - selfH) || (cellTop > winBottom + selfH);
            /* ---- THE RELEASE IS OFF (build97) ----------------------------------------------------------
             *
             * The user proved memory is not the problem: "I remember it going up to 140-odd covers without
             * crashing", which is around 330 MB of decoded covers. So this mechanism is not needed for stability,
             * and it has cost two regressions in two builds -- a blank third row (released and re-fetched in a
             * loop) and a crash while scrolling. The honest move is to stop using it rather than to keep tuning a
             * mechanism that nothing requires.
             *
             * The code stays, behind this constant, because the memory it reclaims is real (2.4 MB per cover) and
             * a future device or a much larger grid may want it. What it must not do is sit in the render path of
             * a build that works. */
            const bool releaseEnabled = false;
            if (releaseEnabled && farAway && cell != focused && cell->coverState() == HG_IMG_INSTALLED) {
                cell->releaseCoverTexture();
                released++;
            } else if (cell->hasReleasedCover() && inside) {
                /* It is back in view: ask for its picture again. */
                cell->reloadReleasedCover();
                reloaded++;
            }

            /* ---- ONLY TOUCH THE VISIBILITY WHEN IT ACTUALLY CHANGES (build93) ----------------------------
             *
             * The first version called setVisibility on every cell every frame. borealis' setVisibility is not a
             * plain flag: it invalidates layout and can add or remove the view from the focus chain, and the
             * ScrollingFrame's own pan handler calls Application::giveFocus(this) while the user drags. The crash
             * that followed was an INSTRUCTION ABORT inside the touch-scroll path
             * (ScrollGestureRecognizer::recognitionLoop -> ScrollingFrame's pan lambda -> Event<View*>::fire),
             * with a string construction on the same stack -- the signature of a wild pointer, not of bad
             * arithmetic.
             *
             * So the state is read first and written only when it differs. A steady screen now performs ZERO
             * visibility writes per frame instead of one per cell. */
            /* UNCONDITIONAL, AS build92 SHIPPED IT (build96): the "only when it changes" version was my attempt at explaining a
             * touch-scroll crash, but that report came from the build92 era, which had this unconditional write -- so it
             * was never the cause. */
            cell->setVisibility(wantVisible ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
            if (!wantVisible) hidden++;
        }

        /* Guard 3: a window that would blank the screen is not trusted, and everything is drawn again. */
        if (placeable > 0 && hidden * 10 > placeable * 9) {
            static bool said = false;
            if (!said) {
                said = true;
                hg::logPrintf("HOMEGRID cull: this frame would have hidden %d of %d placed cells (grid y %.0f, "
                              "viewport %.0f) -- that is a bad window, so nothing is culled and everything is "
                              "drawn [build91]",
                              hidden, placeable, (double)selfTop, (double)selfH);
            }
            for (size_t i = 0; i < this->cells.size(); i++) {
                if (this->cells[i]) this->cells[i]->setVisibility(brls::Visibility::VISIBLE);
            }
            hidden = 0;
        }

        /* One line when the picture changes materially, so the next report carries numbers. */
        static int lastHidden = -1000;
        if (hidden / 24 != lastHidden / 24) {
            lastHidden = hidden;
            hg::logPrintf("HOMEGRID cull: %d of %u cells outside the drawn window, %d covers released, %d reloaded (grid y %.0f, "
                          "window %.0f..%.0f) -- they keep their place and are skipped [build91]",
                          hidden, (unsigned)this->cells.size(), released, reloaded, (double)selfTop, (double)selfH,
                          (double)winTop,
                          (double)winBottom);
        }
    }

    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
}

/* WHERE FOCUS GOES WHEN THE PAD IS PUSHED. See the class declaration for why this is the whole of
 * this grid's navigation: the framework asks the question, and this answers it.
 *
 * LEFT/RIGHT -- nullptr, "not my question". The framework's own walk already does this half
 * correctly (the content box is Axis::ROW, the cards are in reading order, and the headings are
 * skipped because they are not focusable), and it is the half the user confirmed on hardware as "one
 * cell per press". This function is only reached for the directions that axis cannot traverse
 * (box.cpp:414-419), which is UP/DOWN -- so leaving them alone costs nothing and keeps a working
 * thing working.
 *
 * UP/DOWN -- the row step, from hg_grid_row_step in hg_grid.c. The target INDEX is turned into the
 * view and returned; the framework focuses it, plays the focus sound, and shakes the highlight when
 * the answer is nothing. Nothing here calls giveFocus(), because a screen that moves the focus itself
 * is a second mover, and a second mover is what this change removed.
 *
 * WHY THE FOCUSED VIEW IS READ FROM THE APPLICATION rather than from `currentView`: by the time the
 * walk reaches this frame, `currentView` is the CONTENT BOX and not the card (Box::getNextFocus hands
 * `this` to its parent at box.cpp:418). wiliwili sidesteps the same trap by putting its row
 * arithmetic on the content box; reading the live focus here is the simpler equivalent and is the
 * same value the framework uses one line after this returns.
 *
 * A DEAD END INSIDE THE GRID IS NOT A DEAD END FOR THE SCREEN. The last thing this does is hand the
 * direction to the parent, which is what Box::getNextFocus does with a direction it cannot serve
 * (box.cpp:416-419) and what wiliwili's grid does as well. On this screen that is what makes UP from
 * the first row reach the header's Refresh button -- a control that, with the grid blocking every
 * vertical direction, the D-pad could not reach at all. */
brls::View* HomeGridView::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    (void)currentView;   /* the live focus is what matters here; see above */

    if (direction != brls::FocusDirection::UP && direction != brls::FocusDirection::DOWN) {
        /* LEFT/RIGHT STAY THE FRAMEWORK'S WALK, WITH ONE AMENDMENT. Reaching this function with one of
         * them means the content box found nothing -- the focus is on the FIRST or the LAST card --
         * and the amendment is to hand that dead end to the parent instead of stopping here.
         *
         * Stopping here is what would make the sidebar unreachable: it sits to the left of this grid
         * as a sibling in a ROW box, and the walk can only get there if this grid declines and lets
         * the row box traverse its children. This is exactly what Box::getNextFocus does with a
         * direction it cannot serve (box.cpp:416-419). */
        if (this->getParent()) return this->getParent()->getNextFocus(direction, this);
        return nullptr;
    }

    const int at = this->cellIndexOf(brls::Application::getCurrentFocus());
    if (at < 0) {
        /* Focus is not on a card of this grid, so a row step has no starting point. Saying nothing is
         * the honest reply, and it leaves the direction with whoever does hold the focus. */
        return nullptr;
    }

    const int delta = (direction == brls::FocusDirection::UP) ? -1 : 1;
    const int target = hg_grid_row_step(this->places.data(), this->places.size(), at, delta);

    /* THE ONE LOG LINE THAT ANSWERS "DID ONE PRESS MOVE ONE ROW?". The five rounds this bug survived
     * were all spent INFERRING what the input had done; this makes the navigation itself countable --
     * one line per movement, saying where it came from and where it went. Rate-limited, because a
     * held direction repeats about ten times a second and the interesting press is the first one. */
    {
        static int lines = 0;
        if (lines < 200) {
            if (target >= 0) {
                hg::logPrintf("NAVROW: %s cell %d (row %d col %d) -> cell %d (row %d col %d) [build32]",
                              delta < 0 ? "UP" : "DOWN", at, this->places[(size_t)at].row,
                              this->places[(size_t)at].column, target,
                              this->places[(size_t)target].row, this->places[(size_t)target].column);
            } else {
                hg::logPrintf("NAVROW: %s cell %d -- no row that way, handed to the parent [build32]",
                              delta < 0 ? "UP" : "DOWN", at);
            }
            lines++;
        }
    }

    if (target >= 0 && this->cells[(size_t)target]) {
        return this->cells[(size_t)target]->getDefaultFocus();
    }

    if (this->getParent()) return this->getParent()->getNextFocus(direction, this);
    return nullptr;
}

int HomeGridView::cellIndexOf(brls::View* view) const {
    if (!view) return -1;
    /* THE WALK UP IS FOR FOCUS THAT SITS INSIDE A CARD rather than on the card itself: the question is
     * "which slot of this grid", and a descendant of a card is that slot. */
    for (brls::View* v = view; v != nullptr; v = v->getParent()) {
        for (size_t i = 0; i < this->cells.size(); i++) {
            if (this->cells[i] && this->cells[i] == v) return (int)i;
        }
    }
    return -1;
}

void HomeGridView::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    /* SCROLL THE FOCUSED CARD INTO VIEW, then let the framework finish its own bookkeeping.
     *
     * This hook is the framework's own "a child of mine just took focus" notification
     * (view.cpp:1305-1306 -> box.cpp:685 -> scrolling_frame.cpp:478), and it is the ONE place that
     * sees every focus change inside this grid -- left/right, up/down and touch alike -- which is why
     * the scrolling is driven from here rather than sprinkled through the movement code. The old
     * arrangement called followFocus() from inside moveFocus(), i.e. only on the moves that went
     * through the custom path, which is a second reason none of it ran once that path was disabled.
     *
     * The base implementation is still called, and the order matters: `followFocus` sets the offset
     * immediately, and `ScrollingFrame::onChildFocusGained` then calls updateScrolling(true), which
     * asks getDefaultFocus() where to go -- that now answers "the focused card", i.e. the same target
     * this just set. `startScrolling` returns immediately when the wanted offset is already the
     * current one, so the two agree instead of fighting. */
    const int at = this->cellIndexOf(focusedView);
    if (at >= 0) this->followFocus(at);
    brls::ScrollingFrame::onChildFocusGained(directChild, focusedView);
}

brls::View* HomeGridView::getDefaultFocus() {
    /* WHAT THIS ANSWER IS FOR, WHICH IS NOT ONLY "WHERE DOES FOCUS ENTER THIS GRID".
     *
     * `ScrollingFrame::updateScrolling` -- the framework's follow-focus -- asks THIS function where to
     * scroll to (`View* focusedView = getDefaultFocus();`, scrolling_frame.cpp:534), and it is called
     * on every focus change inside the frame. A plain Box answers with "the child that last had
     * focus" (Box::getDefaultFocus -> lastFocusedView, box.cpp:351-361), and that is what wiliwili's
     * grid relies on.
     *
     * This override used to answer "the first card, always", which is right for the entry case it was
     * written for and wrong for every other one: it told the framework to scroll back to the TOP ROW
     * on every single focus change. That is the recorded "it snaps back to the first row" report.
     * Answering with the focused card when the focus is inside the grid keeps the entry case (focus
     * from outside -> the first card) and fixes the scrolling case.
     *
     * The first CARD and not the first child: item 0 can be a heading, headings are not focusable, and
     * a walk that found nothing would leave the focus on the previous activity -- the regression the
     * handover records for the list version ("pressing A on the detail page kept re-opening detail
     * pages"). */
    const int at = this->cellIndexOf(brls::Application::getCurrentFocus());
    if (at >= 0) {
        brls::View* focused = this->cells[(size_t)at]->getDefaultFocus();
        if (focused) return focused;
    }

    for (size_t i = 0; i < this->cells.size(); i++) {
        if (this->cells[i]) {
            auto* view = this->cells[i]->getDefaultFocus();
            if (view) return view;
        }
    }
    return brls::ScrollingFrame::getDefaultFocus();
}

/* THE HOME GRID FOR OTHER SCREENS. See home_activity.hpp for why this exists instead of a second
 * grid implementation. */
brls::View* makePosterGrid(std::vector<HomeRow> rows, std::function<void(const Series&)> onCard) {
    auto* grid = new HomeGridView();
    if (onCard) grid->setOnCard(std::move(onCard));
    grid->setGrow(1.0f);
    grid->setRows(rows);
    hg::logPrintf("POSTERGRID: built a shared grid with %u rows (own click handler: %d)",
                  (unsigned)rows.size(), onCard ? 1 : 0);
    return grid;
}

void setPosterGridRows(brls::View* grid, std::vector<HomeRow> rows) {
    /* The dynamic_cast is here, not at the call site: it is the one place that knows what a grid made
     * by makePosterGrid actually is, and a null result is reported instead of dereferenced. */
    auto* g = dynamic_cast<HomeGridView*>(grid);
    if (!g) {
        hg::logPrintf("POSTERGRID: setPosterGridRows called with something that is not a poster grid "
                      "(%p) -- ignored",
                      (void*)grid);
        return;
    }
    g->setRows(rows);
}

void appendPosterGridRows(brls::View* grid, std::vector<HomeRow> rows) {
    auto* g = dynamic_cast<HomeGridView*>(grid);
    if (!g) {
        hg::logPrintf("POSTERGRID: appendPosterGridRows called with something that is not a poster grid "
                      "(%p) -- ignored",
                      (void*)grid);
        return;
    }

    /* ---- A CEILING ON HOW MANY CELLS A GRID MAY EVER BUILD -----------------------------------------------
     *
     * THE USER'S INSTRUCTION, and it is a resource rule rather than a preference: "in the search results and the
     * 更多 pages too, if there are too many, then after however many pages make the earlier ones static -- the
     * principle is to use as few resources as possible and never crash."
     *
     * This is the one place that can enforce it for all four screens at once, because the home page, the search
     * results, the library tabs and the category pages all append through this function. A category can have 800
     * series over 34 pages: without a ceiling the grid would build 800 focusable cells, each with a cover slot, a
     * label and a click recogniser, and destroy all of them in one frame when the screen was popped -- which is
     * the same shape as the episode-page crash they reported.
     *
     * The cells that ARE built never change again: `appendRows` adds to them, and nothing rebuilds a cell that is
     * already there (see the note on setPosterGridRows, which is the only rebuild path). So the older pages are
     * already static; what this adds is that there is a bounded number of them.
     *
     * HG_POSTER_GRID_MAX_CELLS IS NOW A SANITY STOP, NOT A PAGE LIMIT (build89). The user asked for a bound
     * first and then asked for the endless scroll back -- the later instruction wins. What made the bound worth
     * having is dealt with where the real cost was: the cover fetches now BACK OFF instead of stopping (see
     * tickCoverRetries), so a screen can be scrolled for ever without a fetch storm behind it. The number is 4000
     * cells = 166 pages of 24: unreachable in practice, and if it is ever reached the log says so rather than the
     * grid silently refusing. */
    /* One HomeRow is one cell (see HomeRow: `Series series` is a single series, and a heading or a "more" card is
     * a row of its own), so the arithmetic is just "cells already there plus cells offered". */
    const size_t have = g->cardCount();
    const size_t incoming = rows.size();
    if (have + incoming > (size_t)HG_POSTER_GRID_MAX_CELLS) {
        const size_t room = ((size_t)HG_POSTER_GRID_MAX_CELLS > have) ? (size_t)HG_POSTER_GRID_MAX_CELLS - have : 0;
        hg::logPrintf("POSTERGRID: the ceiling is reached -- %u cells built, %u more offered, ceiling %d. What is "
                      "on screen is kept and stays static; nothing more is built. [build85]",
                      (unsigned)have, (unsigned)incoming, (int)HG_POSTER_GRID_MAX_CELLS);
        if (room == 0) return;
        /* Keep as much as fits, so the ceiling costs nothing until it is reached. */
        rows.resize(room);
    }

    g->appendRows(rows);
}

void setPosterGridNearBottom(brls::View* grid, std::function<void()> cb) {
    auto* g = dynamic_cast<HomeGridView*>(grid);
    if (!g) {
        hg::logPrintf("POSTERGRID: setPosterGridNearBottom called with something that is not a poster "
                      "grid (%p) -- ignored",
                      (void*)grid);
        return;
    }
    g->setOnNearBottom(std::move(cb));
}

HomeActivity::HomeActivity() = default;
HomeActivity::~HomeActivity() {
    /* Tell every callback in flight that this screen is gone -- see hg_alive.hpp. */
    this->alive_->kill();
}

void HomeActivity::onContentAvailable() {
    /* Y OPENS THE SEARCH SCREEN -- THE KEY wiliwili USES FOR IT.
     *
     * Button choice, since borealis has no MINUS: the enum offers A/B/X/Y/START/
     * BACK/LB/RB/LT/RT/sticks (core/input.hpp). A and B are taken here (open /
     * leave) and + quits, so Y and X were the free pair. wiliwili's home uses Y for
     * search (docs/wiliwili按键与触屏规格.md: "Y -> wiliwili/search/tab, 进搜索页") and X for
     * its layout switch, which is the same division of labour this screen now has.
     *
     * SETTINGS DID NOT MOVE OUT OF REACH: it is an item in the bar above the grid, which is where
     * wiliwili keeps it too (pinned rather than a tab). Y used to open settings here because there
     * was no search screen and no bar; now both exist and the keys match wiliwili's.
     *
     * Registered on the frame, not on the activity: a frame-level action only fires
     * while that frame has focus, which is exactly while this screen is in use. */
    if (this->frame) {
        this->frame->registerAction(hg_tr(HG_T_SEARCH), brls::ControllerButton::BUTTON_Y,
            [](brls::View*) {
                hg::logPrintf("HOME: Y pressed -> search");
                brls::Application::pushActivity(new SearchActivity());
                return true;
            });
    }
    /* THE SAME ACTION ON THE GRID, and it is not redundant: actions are looked up
     * starting at the FOCUSED view and walking up its parents
     * (Application::handleAction, application.cpp:712-743). In the grid arm the frame
     * is not in the tree at all, so registering only on the frame would leave Y dead
     * on the grid -- a key that silently does nothing, which is the failure mode this
     * project keeps designing against. */
    if (this->grid) {
        this->grid->registerAction(hg_tr(HG_T_SEARCH), brls::ControllerButton::BUTTON_Y,
            [](brls::View*) {
                hg::logPrintf("HOME: Y pressed (grid) -> search");
                brls::Application::pushActivity(new SearchActivity());
                return true;
            });

        /* LB REFRESHES. The user asked for this: "把刷新整合到按键上" -- the button that used to sit in the
         * header row is gone, and a key is the better home for it anyway, because a refresh wants to be
         * reachable from anywhere on the screen instead of being navigated TO.
         *
         * LB because it is the only free button on this screen: A opens, B is unused here, X is the
         * density toggle, Y is search, + quits, and the bar consumes the D-pad. The PLAYER screen's own
         * LB is its seek action -- the two screens do not share a binding, which is the same division
         * wiliwili makes.
         *
         * `home.refresh=no` in hongguo.cfg removes the key, which is the way back if a refresh key turns
         * out to be unwanted -- the same escape hatch this screen has always shipped for this feature. */
        if (!hg::cfgIs("home.refresh", "no")) {
            this->grid->registerAction(hg_tr(HG_T_REFRESH), brls::ControllerButton::BUTTON_LB,
                [this](brls::View*) {
                    hg::logPrintf("HOME: LB pressed -> refresh");
                    this->reloadHome();
                    return true;
                });
            hg::logPrintf("HOME: refresh bound to LB (home.refresh=no removes it) [build40]");
        } else {
            hg::logPrintf("HOME: home.refresh=no -- no refresh key");
        }
        /* X SWITCHES BETWEEN LARGE AND SMALL CELLS -- wiliwili's own layout switch, on the same
         * key it uses (player_base_activity.cpp:212).
         *
         * REGISTERED ON THE GRID, which works because borealis looks an action up starting at
         * the FOCUSED view and walking up its parents (application.cpp:712-743). When the user
         * presses X, focus is on a CELL, whose parent chain runs through the content box to this
         * grid -- so the action fires. Registering it on the cells instead would mean one
         * registration per card, 36 of them, for an action that is identical on every one.
         *
         * `player.quality=no`-style escape hatch: this screen's is the whole `home.layout=list`
         * line, which replaces the grid entirely. There is no separate switch for the toggle
         * because with the grid present there is nothing for it to break -- it changes a number
         * and re-runs an arithmetic function this project tests on the host. */
        this->grid->registerAction("Size", brls::ControllerButton::BUTTON_X,
            [this](brls::View*) {
                hg::logPrintf("HOME: X pressed -> toggle cell size");
                if (this->grid) this->grid->toggleDensity();
                return true;
            });
    }
    hg::logPrintf("HOME: Y registered for SEARCH, X for the cell-size toggle (frame=%p grid=%p)",
                  (void*)this->frame, (void*)this->grid);

    this->setupSidebar();
}

/* THE SIDEBAR'S CONTENTS, ONCE, AT THE POINT THE SCREEN IS USABLE.
 *
 * Built here rather than in the constructor because `rebuild` creates views and registers click
 * actions, and borealis wants the tree to exist before actions are hung on it -- the same order the
 * Y/X actions above follow.
 *
 * WHY FAVORITES AND HISTORY ARE DIM. `hg_sidebar_build` is given the truth: this build has no
 * favorites and no history, because the data layer for them (hg_history.c / hg_fav_*, 92 host checks)
 * is NOT WIRED INTO THE APP YET -- nothing writes those files today, so there is nothing behind either
 * entry. The model's rule for that case is to build the item DISABLED rather than hide it: a dim entry
 * the user can read and understand beats walking into an empty page to find out. The subtitle says
 * which part is missing, so the dimming is not a mystery.
 *
 * When the History and Favorites screens land, the two flags become real counts read from
 * sdmc:/switch/ and the entries light up with no change to the bar. */
void HomeActivity::setupSidebar() {
    /* ---- THE HOME NO LONGER CARRIES A 收藏 ENTRY, SO NOTHING HERE HAS TO BE RE-READ (build133) ----------
     *
     * The user's instruction: "just remove that favourite entry from the home screen; favourites are reached from
     * the history screen, the way it was." The library screen already carries both tabs, so there is one entry
     * point instead of two -- and the sidebar is therefore built once, when this screen is created, with no
     * rebuild on the way back from another screen (which is what crashed twice: see the note in the grid's
     * dropAllCoversIfCovered). */

    if (!this->sidebar) return;

    SidebarContent content;
    content.haveHome = true;
    /* THE TRUTH, FROM THE FILES. Until this round these were hardcoded false with a comment saying "nothing
     * writes the watchlist yet", and that was accurate: hg_history.c had 92 host checks and no caller. Now the
     * player records history and the detail screen toggles the watchlist, so the flags come from the store's
     * counts -- an entry is dim exactly when there is nothing behind it, which is the sidebar model's rule. */
    content.haveWatchlist = Store::favoritesCount() > 0;
    content.haveHistory = Store::historyCount() > 0;
    hg::logPrintf("HOME: the bar's content flags -- watchlist=%d (%d entries), history=%d (%d entries)",
                  content.haveWatchlist ? 1 : 0, Store::favoritesCount(), content.haveHistory ? 1 : 0,
                  Store::historyCount());
    this->sidebar->rebuild(content);

    /* WHAT EACH ITEM'S SUBTITLE SAYS. Found by language key rather than by position, so inserting an item
     * cannot silently mislabel another one.
     *
     * THESE TWO USED TO SAY "NOT BUILT YET", and that string survived build50 for a reason worth recording: the
     * edit that was supposed to replace it did not apply, and nothing checked. The 2026-10-10 stale-claim sweep
     * found it by grepping for the phrase -- which is exactly what that sweep is for, and why it ran before this
     * round's build rather than after the user reported a bar item lying about itself. */
    for (int i = 0; i < this->sidebar->itemCount(); i++) {
        const int key = this->sidebar->labelKeyOf(i);
        if (key == HG_T_FAVORITES) {
            char b[48];
            snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)Store::favoritesCount());
            this->sidebar->setSubtitle(i, b);
        } else if (key == HG_T_HISTORY) {
            char b[48];
            snprintf(b, sizeof b, hg_tr(HG_T_SEARCH_SHOWING), (unsigned)Store::historyCount());
            this->sidebar->setSubtitle(i, b);
        }
    }

    this->sidebar->setOnActivate([this](int index) { this->onSidebarActivate(index); });
    hg::logPrintf("HOME: sidebar wired (%d items; the settings screen and both library tabs open their own "
                  "screens) [build62]",
                  this->sidebar->itemCount());
}

void HomeActivity::onSidebarActivate(int index) {
    if (!this->sidebar) return;
    const int key = this->sidebar->labelKeyOf(index);
    hg::logPrintf("HOME: sidebar item %d activated (label key %d)", index, key);

    if (key == HG_T_SETTINGS) {
        /* Pushed, not popped -- so the callback can run from the click action without the
         * use-after-free that popActivity from a registered action caused (see
         * detail_activity.cpp's note and rule 5 in the handover). */
        brls::Application::pushActivity(new SettingsActivity());
        return;
    }

    if (key == HG_T_SEARCH) {
        /* THE SAME SCREEN Y OPENS. The bar is where this app's screens are, so search belongs on it
         * too: a user who has learned that the bar holds the sections should not have to know a
         * function key to find the search. */
        brls::Application::pushActivity(new SearchActivity());
        return;
    }

    if (key == HG_T_HOME) {
        /* Already on this screen. What "Home" can usefully mean from here is "put the focus back in
         * the content", which is what a user pressing it while the bar has focus is asking for. */
        if (this->grid) brls::Application::giveFocus(this->grid);
        else if (this->frame) brls::Application::giveFocus(this->frame);
        return;
    }

    /* FAVORITES AND HISTORY NOW HAVE A SCREEN (D2/D3). The library holds both lists behind two tabs, and the
     * bar already knows which one was pressed -- so it says so, and the user does not have to switch tabs after
     * choosing 收藏. This used to notify "not built yet", which was the honest answer while the data layer had
     * no callers; hg_store (build49) and this screen are what changed that. */
    if (key == HG_T_FAVORITES) {
        brls::Application::pushActivity(new LibraryActivity(true));
        return;
    }
    if (key == HG_T_HISTORY) {
        brls::Application::pushActivity(new LibraryActivity(false));
        return;
    }

    /* Anything else in the bar still has no screen behind it. SAYING SO is the honest answer -- a button
     * that silently does nothing reads as a bug, and this project's rule is that an unfinished thing says so. */
    brls::Application::notify(std::string(hg_tr(HG_T_NOT_BUILT_YET)));
}

/* WHICH LAYOUT, DECIDED IN EXACTLY ONE PLACE.
 *
 * The grid is the default because matching wiliwili is the point of this work. The
 * list is kept as the way back: it is the shape that HAS been verified on hardware,
 * and this project's rule is that an unverified change to a working screen ships with
 * a route back that costs no rebuild. `home.layout=list` in hongguo.cfg is that route.
 *
 * Read once, at startup, because the layout is chosen while the view tree is built and
 * a mid-session change would have to rebuild the tree. It is logged below so a run
 * always states which layout it drew -- the same discipline as every other arm in this
 * project.
 *
 * `home.tiers=no` (the older switch) still means "no headings" and is orthogonal: it
 * changes the ROW MODEL, this changes how rows are drawn. Asking for list+tiers=no
 * therefore gives the flat list that was the original screen. */
static bool homeLayoutIsGrid() {
    static const bool grid = !hg::cfgIs("home.layout", "list");
    static bool said = false;
    if (!said) {
        said = true;
        hg::logPrintf("HOME: layout = %s (cfg home.layout; grid is the default, list is the "
                      "way back)",
                      grid ? "GRID" : "LIST");
    }
    return grid;
}

/* One place decides whether the home page keeps its tiers.
 *
 * There are three call sites -- the synchronous arm, the worker thread and the
 * no-worker inline fallback -- and they must not be able to disagree about the
 * shape of the list, so none of them calls Client directly.
 *
 * `home.tiers=no` in hongguo.cfg falls back to the FLAT list: exactly the screen
 * that was already verified on hardware, with no headings. That exists because
 * this change is deployed without a console run: if the tiered list misbehaves,
 * one FTP line reverts it and no rebuild is needed. An unverified change to a
 * screen that is known to work should always ship with a way back. */
static Result fetchHomeAsRows(std::vector<HomeRow>& out) {
    static const bool tiers = !hg::cfgIs("home.tiers", "no");
    if (tiers) return Client::fetchHomeRows(out);

    std::vector<Series> cards;
    Result r = Client::fetchHome(cards);
    if (!r.ok) return r;

    out.clear();
    out.reserve(cards.size());
    for (size_t i = 0; i < cards.size(); i++) {
        HomeRow row;
        row.header = false;
        row.series = cards[i];
        out.push_back(std::move(row));
    }
    hg::logPrintf("HOME: home.tiers=no -- flat fallback, %u cards and no headings",
                  (unsigned)out.size());
    return r;
}

/* REFRESH. Re-runs the same fetch the screen started with, on a worker thread, and
 * hands the result to the same applyHome.
 *
 * WHY IT IS WORTH HAVING AT ALL, given a restart would do the same thing: the home
 * page is the screen where "the network came back" or "the page has moved on" is
 * noticed first, and on a console the alternative is quitting the app. wiliwili has
 * the same button for the same reason.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: it does not clear the screen first. Leaving the
 * current cards up until the new ones arrive means a failed refresh shows an error in
 * the header while the user keeps a usable page -- versus an empty screen that makes a
 * transient failure look like the app broke. */
void HomeActivity::reloadHome() {
    if (this->reloading) {
        hg::logPrintf("HOME reload: already in flight -- ignoring the second press");
        return;
    }
    this->reloading = true;
    /* THE STATUS IS SAID THROUGH THE BAR NOW: the small line under 主页 (see applyHome). While a refresh
     * is in flight it says so, which is the same place the result lands. */
    this->setHomeSubtitle(hg_tr(HG_T_LB_REFRESH));

    HomeActivity* self = this;
    const AliveToken alive = this->alive_;
    bool started = hg::runDetached([self, alive]() {
        std::vector<HomeRow> got;
        Result r = fetchHomeAsRows(got);
        hg::logPrintf("HOME reload: fetch returned ok=%d n=%u err='%s'", (int)r.ok,
                      (unsigned)got.size(), r.error.c_str());
        brls::sync([self, alive, got, r]() {
            HG_GUARD_ALIVE(alive);
            self->reloading = false;
            self->applyHome(got, r);
        });
    });

    if (!started) {
        /* No worker: do it inline rather than leave "Refreshing..." on screen forever.
         * A frozen screen for a few seconds is far better than a lie. */
        hg::logPrintf("HOME reload: no worker -- fetching inline");
        std::vector<HomeRow> got;
        Result r = fetchHomeAsRows(got);
        this->reloading = false;
        this->applyHome(std::move(got), r);
    }
}

brls::View* HomeActivity::createContentView() {
    hg::logPrintf("HOME: ENTER createContentView");

    /* WHY THE LIST VIEW EXISTS BEFORE THERE IS ANY DATA.
     *
     * Two requirements pull in opposite directions:
     *
     *  (a) A RecyclerFrame must never be LAID OUT with zero rows. borealis'
     *      reloadData calls selectRowAt(defaultCellFocus, false) unconditionally
     *      (recycler.cpp:270) and that loop
     *      `for (int i = -1; i <= indexPath.row; i++) offset +=
     *      cacheFramesData[count++].height` (recycler.cpp:319-320) then indexes
     *      cacheFramesData[1] while cacheCellFrames pushed exactly ONE entry for
     *      a zero-row list (recycler.cpp:341-367). selectRowAt even carries the
     *      comment "TODO: Implement it normally" (recycler.cpp:307). On
     *      hardware a zero-row first layout is what killed the app.
     *
     *  (b) Something focusable must be in the content view AT THE MOMENT the
     *      activity is pushed, because that is when borealis assigns focus.
     *      Building the frame late (data first, then addView) satisfies (a) and
     *      breaks (b): focus stayed on the previous activity's cell, and on the
     *      detail page pressing A re-opened detail pages instead of playing.
     *
     * Both hold at once because of an existing guard in borealis: reloadData's
     * whole body -- including selectRowAt -- is inside `if (dataSource)`. So a
     * RecyclerFrame with a NULL data source lays out as an empty frame and
     * never reaches the out-of-bounds path. The frame is therefore created and
     * attached here with no data source, and applyHome attaches one when rows
     * exist. */
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(24.0f);

    /* THE HEADER ROW IS GONE, AND REFRESH IS A KEY NOW.
     *
     * The user asked for exactly this: "把最上面一排的 32 个卡片在 4 个里面消除，和那个刷新那一栏整合，
     * 把刷新整合到按键上". That row carried the count ("32 cards in 4 sections") and a focusable Refresh
     * button, and it cost a whole line of height on a screen whose grid needs every pixel it can get
     * (the barrier it draws is 8 posters wide and several rows tall).
     *
     * WHAT HAPPENS TO THE TWO THINGS IT CARRIED:
     *   * the COUNT moved to the bar item's subtitle -- the small live line under 主页 that the sidebar
     *     model was built to carry ("subtitle is what makes the bar informative instead of decorative",
     *     hg_sidebar.h). It is still on screen, still updated on every fetch, and costs no row.
     *   * REFRESH moved to the LB BUTTON (see onContentAvailable). A key is reachable from anywhere on the
     *     screen without moving the focus, which is what a refresh wants to be; the button also had to be
     *     navigated TO, and it was the only reason the screen had a focusable header at all.
     *   * a FAILURE is reported with brls::Application::notify (a toast) rather than a header line, because
     *     a failure must be visible even though the header no longer exists.
     *
     * `home.refresh=no` still means "no refresh": it now decides whether the LB action is registered. */
    hg::logPrintf("HOME: no header row (count moved to the bar's subtitle, refresh moved to LB) [build40]");

    /* THE BAR SITS ABOVE THE GRID, AND THAT IS THE WHOLE OF ITS NAVIGATION INTEGRATION.
     *
     * The layout is a COLUMN: [bar] [grid]. borealis' own walk does the rest --
     *
     *   LEFT/RIGHT   inside the bar it steps between the bar's items (the bar is a ROW). From the grid
     *                it is the grid's own content-box walk, unchanged.
     *   UP/DOWN      from the grid, UP reaches the bar (the outer COLUMN box traverses its children);
     *                from the bar, DOWN goes into the grid, and UP from the bar is the top of the screen
     *                (it shakes, which is the honest answer now that there is no header row above it).
     *
     * No key is intercepted, nothing is polled, and neither the bar nor the grid moves the focus
     * itself: the last two rounds were spent removing a SECOND mover from this very screen.
     *
     * WHY A TOP BAR RATHER THAN A LEFT COLUMN -- the user reported this twice. A left column takes its
     * width OUT OF THE GRID: at 120 px the grid lost a column and every tier's eighth card (the "more"
     * card) landed on a second row, and at 72 px it still did not fit. Across the top the bar costs the
     * grid NOTHING horizontally, so the posters keep their original size and all eight stay on one row:
     * the grid gets the full 1232 px again.
     *
     * `home.sidebar=no` in hongguo.cfg removes the bar -- the same escape hatch the other layout
     * changes on this screen ship with, because an unverified change to a working screen must have a
     * way back that costs no rebuild. */
    if (!hg::cfgIs("home.sidebar", "no")) {
        this->sidebar = new SidebarView();
        box->addView(this->sidebar);
        hg::logPrintf("HOME: sidebar bar added above the grid (home.sidebar=no removes it) [build37]");
    } else {
        hg::logPrintf("HOME: home.sidebar=no -- no sidebar bar");
    }

    /* The grid is built with no cells; applyHome fills it. */
    auto* grid = new HomeGridView();
    const bool useGrid = homeLayoutIsGrid();

    if (useGrid) {
        grid->setGrow(1.0f);
        box->addView(grid);
        this->grid = grid;
        hg::logPrintf("HOME: grid view built (home.layout=grid, ui logical width decides columns)");
    }

    if (!useGrid) {
        auto* frame = new hg::LoggingRecyclerFrame();
        frame->registerCell("HomeCell", []() { return HomeCell::create(); });
        /* THE HEADING CELL MUST BE REGISTERED BEFORE ANY LAYOUT.
         *
         * dequeueReusableCell("HomeHeader") does allocationMap.at(identifier), which
         * THROWS std::out_of_range for an unregistered name -- and an uncaught throw
         * reaches std::terminate, i.e. a console crash with no message. Registering it
         * unconditionally, right here, is what makes that impossible rather than
         * unlikely. */
        frame->registerCell("HomeHeader", []() { return HomeHeaderCell::create(); });
        frame->setGrow(1.0f);
        box->addView(frame);
        this->frame = frame;
        hg::logPrintf("HOME: Box + header + empty list frame built (no data source yet)");
    }

    if (wantSyncFetch()) {
        /* SYNC ARM: the fetch happens on this thread, before the box is ever
         * laid out. Kept because it is the one lever that changes the fetch
         * strategy without a rebuild -- see wantSyncFetch(). */
        hg::logPrintf("HOME: ENTER synchronous fetch (main thread)");
        std::vector<HomeRow> got;
        Result r = fetchHomeAsRows(got);
        hg::logPrintf("HOME: sync fetch returned ok=%d n=%u err='%s'", (int)r.ok,
                      (unsigned)got.size(), r.error.c_str());
        this->applyHome(std::move(got), r);
        hg::logPrintf("HOME: OK synchronous arm complete, returning view");
        return box;
    }

    /* ASYNC ARM. The worker comes from hg_thread (libnx threadCreate), not
     * std::thread: on this toolchain std::thread's constructor throws ENOSYS
     * and an uncaught throw aborts the process. Measured on hardware, see
     * hg_thread.hpp.
     *
     * The lambdas capture `self` and nothing else. The previous version
     * captured the raw view pointers, which is the lifetime defect recorded in
     * the handover: if the screen were destroyed before the fetch returned,
     * brls::sync would run against a destroyed view. Going through members
     * removes that window for these two views. */
    HomeActivity* self = this;
    const AliveToken alive = this->alive_;
    hg::logPrintf("HOME: ENTER hg::runDetached(fetch)");
    bool started = hg::runDetached([self, alive]() {
        hg::logPrintf("TH: fetch thread started");
        std::vector<HomeRow> got;
        Result r = fetchHomeAsRows(got);
        hg::logPrintf("TH: fetch returned ok=%d n=%u err='%s'", (int)r.ok,
                      (unsigned)got.size(), r.error.c_str());
        hg::logPrintf("TH: ENTER brls::sync(apply)");
        brls::sync([self, alive, got, r]() {
            HG_GUARD_ALIVE(alive);
            hg::logPrintf("SYNC: ENTER apply on main thread");
            self->applyHome(got, r);
            hg::logPrintf("SYNC: OK apply done");
        });
        hg::logPrintf("TH: brls::sync queued, thread returning");
    });

    if (!started) {
        /* NO THREAD AVAILABLE. Doing nothing would leave "Loading home page..."
        * on screen forever, so the work is done inline instead: a frozen screen
        * for a few seconds is worse UX and far better than a lie. */
        hg::logPrintf("HOME: no worker available -- fetching inline instead");
        std::vector<HomeRow> got;
        Result r = fetchHomeAsRows(got);
        hg::logPrintf("HOME: inline fetch returned ok=%d n=%u err='%s'", (int)r.ok,
                      (unsigned)got.size(), r.error.c_str());
        this->applyHome(std::move(got), r);
    }
    hg::logPrintf("HOME: OK createContentView returning view");

    /* ---- + OPENS THE SETTINGS SCREEN, HERE AND NOWHERE ELSE (build167, the user's instruction) -----
     *
     * THEIR WORDS: "把加号变成只是主界面设置" -- make + mean the settings screen, on the home screen
     * only. It used to be the app's exit button, and pressing it CRASHED the console: borealis' exit
     * action calls Application::quit() -> exit() -> libnx tearing down its services, while a cover
     * fetch was still inside libcurl's socket layer. The full evidence (two Atmosphere crash reports
     * and their stacks) is on setGlobalQuit(false) in hg_boot.cpp, and the exit action itself is gone
     * from this build -- so this button is now free to mean something useful.
     *
     * REGISTERED ON `box` RATHER THAN ON A CHILD: borealis' handleAction walks up from the FOCUSED
     * view, so a registration on the screen's root catches the press wherever the focus happens to be
     * -- the sidebar, the grid, or the hint bar. Any activity pushed on top (detail, player, settings)
     * has its own root and gets no + binding from here, which is the "only on the home screen" part.
     *
     * `hidden=false`, so the hint bar advertises it: a key nobody can see is a key nobody uses. The
     * label is the existing HG_T_SETTINGS string, so it follows the language table like the rest of
     * this screen rather than adding an eighth untranslated English label (see the handover's 6). */
    box->registerAction(hg_tr(HG_T_SETTINGS), brls::ControllerButton::BUTTON_START, [](brls::View*) {
        hg::logPrintf("HOME: + pressed -> settings screen [build167]");
        /* PUSHED, not popped, for the same reason the sidebar's settings item is (see onSidebarActivate):
         * the callback runs from an action dispatch, and tearing this activity down inside it is the
         * use-after-free that rule 5 in the handover exists to prevent. */
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });

    return hg::withHintBar(box);
}

void HomeActivity::setHomeSubtitle(const std::string& text) {
    if (!this->sidebar) return;
    /* FOUND BY LANGUAGE KEY, not by index: the bar's order is the model's business, and an item
     * inserted ahead of 主页 must not make the home page write its status onto something else. */
    for (int i = 0; i < this->sidebar->itemCount(); i++) {
        if (this->sidebar->labelKeyOf(i) == HG_T_HOME) {
            this->sidebar->setSubtitle(i, text);
            return;
        }
    }
}

void HomeActivity::applyHome(std::vector<HomeRow> got, const Result& r) {
    /* "IS THE VIEW STILL THERE" IS ASKED OF THE GRID, NOT OF THE BAR: with home.sidebar=no there is no
     * bar, and returning early on that would leave the grid empty. The grid (or the list frame) is what
     * this function fills, so it is what must exist. */
    if (!this->grid && !this->frame) {
        hg::logPrintf("HOME apply: the view is gone; nothing to do");
        return;
    }

    if (!r.ok) {
        /* REFUSE, DO NOT GUESS: a failure is reported with its reason and
         * leaves the list empty, rather than silently rendering a shorter home
         * page. */
        /* A FAILURE MUST STAY VISIBLE. It used to be written into the header label; with the header gone
         * the toast is the visible half and the bar subtitle carries the short version. */
        this->setHomeSubtitle(std::string("! ") + r.error);
        brls::Application::notify("Home page failed: " + r.error);
        hg::logPrintf("HOME apply: reported failure '%s'", r.error.c_str());
        return;
    }

    this->rows = std::move(got);
    this->loaded = true;

    /* The count is stated on screen because "36 cards" is the measured property
     * of this endpoint; seeing a different number is the fastest signal that
     * the page shape moved. Cards and headings are counted separately because
     * both numbers are meaningful: 36 cards in 4 tiers is the measured shape. */
    size_t cards = 0, headings = 0;
    for (size_t i = 0; i < this->rows.size(); i++) {
        if (this->rows[i].header) headings++;
        else cards++;
    }
    char b[128];
    snprintf(b, sizeof b, "%u cards in %u sections", (unsigned)cards, (unsigned)headings);
    this->setHomeSubtitle(b);
    hg::logPrintf("HOME apply: %u cards in %u sections (%u rows)", (unsigned)cards,
                  (unsigned)headings, (unsigned)this->rows.size());

    if (this->rows.empty()) {
        /* A genuinely empty page leaves the frame with no data source, which is
         * the safe empty state -- see createContentView. fetchHomeRows refuses
         * zero rows outright, so reaching here means the flat fallback path was
         * used and the page really was empty. */
        hg::logPrintf("HOME apply: 0 rows -- data source NOT attached");
        this->setHomeSubtitle(std::string("0 ") + hg_tr(HG_T_HOME_EMPTY));
        return;
    }

    hg::logPrintf("HOME apply: ENTER attach %u rows", (unsigned)this->rows.size());

    if (this->grid) {
        /* THE GRID ARM. setRows builds the cells and places them, and it also computes
         * the column count from the frame's own width -- so a different ui.scale gives
         * a different grid with nothing else involved. */
        this->grid->setRows(this->rows);
        /* Focus is not optional: the grid was empty when the activity was pushed, so
         * whatever had focus then may still have it. Without this the detail page's A
         * kept re-opening detail pages -- the regression the handover document
         * records. */
        brls::Application::giveFocus(this->grid);
        hg::logPrintf("HOME apply: grid populated (%u cards, %d columns), focus given to the grid",
                      (unsigned)this->grid->cardCount(), this->grid->columnCount());
        return;
    }

    if (!this->frame) {
        hg::logPrintf("HOME apply: no frame; nothing to attach to");
        return;
    }
    this->frame->setDataSource(new HomeDataSource(&this->rows));
    /* setDataSource only reloads when the frame is already laid out. It is, so
     * this is belt and braces rather than required. */
    this->frame->reloadData();

    /* FOCUS IS NOT OPTIONAL HERE. The frame was empty when the activity was
     * pushed, so whatever had focus then may still have it; re-giving focus now
     * that cells exist makes the D-pad and A land on the list. Without this the
     * detail page's A kept re-opening detail pages. */
    brls::Application::giveFocus(this->frame);
    hg::logPrintf("HOME apply: rows attached, focus re-given to the list");
}

} // namespace hg
