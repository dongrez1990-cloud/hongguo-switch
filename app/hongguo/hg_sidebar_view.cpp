// hg_sidebar_view.cpp -- the left sidebar, drawn with nanovg.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_sidebar_view.hpp"

#include <borealis/views/label.hpp>

#include "hg_log.hpp"
#include "hg_touch.hpp"
#include "hg_status_dot.hpp"   /* the play-chain status light at the end of the bar (build158) */

extern "C" {
#include "hg_lang.h"
}

namespace hg {

/* The icon's nominal box inside a row. The bar's width is derived from it, so the icon and the
 * narrow column cannot drift apart: 28 px icon + 2x2 px padding + room for the label is what makes
 * 72 px work. */
#define HG_SB_ICON_BOX 28.0f

/* A THEME LOOKUP THAT CANNOT KILL THE APP.
 *
 * MEASURED ON HARDWARE, ON THE FIRST CONSOLE RUN OF THIS VIEW (build34): the border colour was
 * looked up as "brls/separator", which THIS theme does not have. borealis' ThemeValues::getColor
 * calls `fatal()` for an unknown key (theme.cpp:155-156), fatal() throws, nothing catches it, and
 * std::terminate aborts the process -- so the app was GONE ONE FRAME IN, with nothing on screen at
 * all. The breadcrumb log had the reason; that is the only reason this was diagnosable.
 *
 * WHY THIS FILE HAD THAT BUG: it was written in round 17 and never instantiated until now -- the
 * code below had never executed once. A wrong theme key in dead code is invisible until the moment
 * the code goes live, and then it kills the app rather than misdrawing a line.
 *
 * So every colour this file reads goes through here, and the fallback is not decoration: it turns
 * "the process aborts" into "one border is the wrong shade", which is the correct failure mode for a
 * colour. It also NAMES the key in the log, so the next occurrence is a one-line fix.
 *
 * The keys that exist are fixed by borealis' own theme table (theme.cpp, 35 values, same names in
 * both variants); run.ps1 now cross-checks every key this app uses against that table, which is what
 * would have caught this before it shipped. */
static NVGcolor themeColor(const char* key, NVGcolor fallback) {
    try {
        return brls::Application::getTheme().getColor(key);
    } catch (...) {
        static int lines = 0;
        if (lines < 8) {
            hg::logPrintf("SIDEBAR: theme has no '%s' -- using the fallback colour (this would have "
                          "aborted the app without this guard)",
                          key);
            lines++;
        }
        return fallback;
    }
}

/* ---- the icons, drawn rather than loaded ---------------------------------
 *
 * wiliwili loads SVGs; this devkitPro has no lunasvg, so each shape is a few nanovg paths. All of
 * them are drawn inside a NOMINAL SQUARE of `size` pixels centred on (cx, cy), so a caller positions
 * an icon by giving it a centre and nothing else has to know the shape's own proportions.
 *
 * They are stroked, not filled: a stroked outline reads clearly against the bar's flat background at
 * the small sizes this bar uses, and it matches the thin-line look of the icon set in wiliwili's own
 * screenshots. `alpha` carries the focus/active state through so the caller does not have to draw
 * two variants. */
static void drawIcon(NVGcontext* vg, int icon, float cx, float cy, float size, NVGcolor col,
                     float alpha) {
    const float r = size * 0.5f;
    nvgSave(vg);
    nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, alpha));
    nvgStrokeWidth(vg, 2.0f);
    nvgLineJoin(vg, NVG_ROUND);
    nvgLineCap(vg, NVG_ROUND);

    switch (icon) {
        case HG_SB_ICON_HOME: {
            /* A house: a roof (two lines to an apex) over a body. Two paths, no fill. */
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - r, cy - r * 0.1f);
            nvgLineTo(vg, cx, cy - r * 0.95f);
            nvgLineTo(vg, cx + r, cy - r * 0.1f);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - r * 0.75f, cy - r * 0.15f);
            nvgLineTo(vg, cx - r * 0.75f, cy + r * 0.85f);
            nvgLineTo(vg, cx + r * 0.75f, cy + r * 0.85f);
            nvgLineTo(vg, cx + r * 0.75f, cy - r * 0.15f);
            nvgStroke(vg);
            break;
        }
        case HG_SB_ICON_STAR: {
            /* A five-point star, stroked. The points are computed rather than tabulated so the shape
             * scales with `size` instead of needing a table per size. */
            int i;
            nvgBeginPath(vg);
            for (i = 0; i < 10; i++) {
                /* Even indices are the outer points, odd the inner ones; starting at -90 degrees puts
                 * a point at the top, which is what makes it read as a star rather than a wheel. */
                float ang = -NVG_PI / 2.0f + (float)i * NVG_PI / 5.0f;
                float rad = (i % 2 == 0) ? r : r * 0.45f;
                float px = cx + cosf(ang) * rad;
                float py = cy + sinf(ang) * rad;
                if (i == 0) nvgMoveTo(vg, px, py);
                else nvgLineTo(vg, px, py);
            }
            nvgClosePath(vg);
            nvgStroke(vg);
            break;
        }
        case HG_SB_ICON_CLOCK: {
            /* A clock: the dial plus two hands. The hands are what make it unambiguous at this size --
             * a bare circle reads as a radio button. */
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, r);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, cy);
            nvgLineTo(vg, cx, cy - r * 0.6f);
            nvgMoveTo(vg, cx, cy);
            nvgLineTo(vg, cx + r * 0.45f, cy + r * 0.25f);
            nvgStroke(vg);
            break;
        }
        case HG_SB_ICON_GEAR: {
            /* A gear, simplified to a hub plus eight spokes. Drawing real teeth needs either a table
             * or a lot of trigonometry for a shape nobody looks at closely; the spokes read as a gear
             * at every size this bar uses and cost four lines. */
            int i;
            const float hub = r * 0.42f;
            const float inner = r * 0.62f;
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, hub);
            nvgStroke(vg);
            nvgBeginPath(vg);
            for (i = 0; i < 8; i++) {
                float ang = (float)i * NVG_PI / 4.0f;
                nvgMoveTo(vg, cx + cosf(ang) * inner, cy + sinf(ang) * inner);
                nvgLineTo(vg, cx + cosf(ang) * r, cy + sinf(ang) * r);
            }
            nvgStroke(vg);
            break;
        }
        case HG_SB_ICON_SEARCH: {
            /* A MAGNIFIER: a ring with a handle at four o'clock. Two paths, and the handle's angle is
             * what makes it read as a magnifier rather than as a lollipop. */
            const float rr = r * 0.62f;
            nvgBeginPath(vg);
            nvgCircle(vg, cx - r * 0.18f, cy - r * 0.18f, rr);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx + rr * 0.62f, cy + rr * 0.62f);
            nvgLineTo(vg, cx + r * 0.95f, cy + r * 0.95f);
            nvgStroke(vg);
            break;
        }
        default:
            break;
    }
    nvgRestore(vg);
}

/* ---- one row -------------------------------------------------------------- */

class SidebarView::Item : public brls::Box {
  public:
    Item() {
        this->setFocusable(true);
        /* ROW: icon on the left, label beside it. This is a HORIZONTAL bar now, so the item is wide
         * and short rather than narrow and tall -- the shape wiliwili's own bottom bar uses. */
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setPadding(6.0f, 14.0f, 6.0f, 12.0f);
        this->setCornerRadius(8.0f);

        iconArea = new brls::Box();
        iconArea->setWidth(HG_SB_ICON_BOX);
        iconArea->setHeight(HG_SB_ICON_BOX);
        iconArea->setFocusable(false);
        /* The icon is painted by THIS class in draw(), so the area is a plain box used only for
         * spacing -- giving it its own view or image would mean fighting nanovg for a texture it does
         * not need. Its size is also what draw() uses to place the icon's centre. */
        this->addView(iconArea);

        /* A COLUMN inside the ROW so a subtitle can sit under the label without widening the item. */
        auto* textColumn = new brls::Box();
        textColumn->setAxis(brls::Axis::COLUMN);
        textColumn->setJustifyContent(brls::JustifyContent::CENTER);
        textColumn->setFocusable(false);
        textColumn->setMarginLeft(8.0f);

        label = new brls::Label();
        label->setFontSize(15.0f);
        label->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        textColumn->addView(label);

        /* The subtitle only appears when there is something to say: the live counts wiliwili puts under
         * its tabs, and this app's "not built yet" for a section whose screen does not exist. */
        subtitle = new brls::Label();
        subtitle->setFontSize(10.0f);
        subtitle->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        subtitle->setVisibility(brls::Visibility::GONE);
        textColumn->addView(subtitle);

        this->addView(textColumn);
    }

    void setItem(const HgSidebarItem* it) {
        this->icon = it->icon;
        this->enabled = it->enabled != 0;
        this->setFocusable(this->enabled);
        this->label->setText(hg_tr(it->labelKey));
        if (it->subtitle[0] != '\0') {
            this->subtitle->setText(it->subtitle);
            this->subtitle->setVisibility(brls::Visibility::VISIBLE);
        } else {
            this->subtitle->setVisibility(brls::Visibility::GONE);
        }
        this->applyColors();
    }

    void setSubtitleText(const std::string& text) {
        if (text.empty()) {
            this->subtitle->setVisibility(brls::Visibility::GONE);
        } else {
            this->subtitle->setText(text);
            this->subtitle->setVisibility(brls::Visibility::VISIBLE);
        }
    }

    void setActiveState(bool value) {
        this->active = value;
        this->applyColors();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        /* THE HIGHLIGHT IS DRAWN HERE rather than by setting a background colour, because it has to
         * carry three states at once (focused / active / enabled) and a single background colour can
         * only express one. Drawing it means the rules are all in one place -- `applyColors`. */
        const float a = this->enabled ? 1.0f : 0.35f;
        if (this->active || this->isFocused()) {
            nvgSave(vg);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 8.0f);
            /* The active section gets the accent tint; merely focused gets a neutral one. Both at low
             * alpha so the label stays readable on top. */
            NVGcolor tint = this->isFocused() ? themeColor("brls/accent", nvgRGB(0, 255, 204))
                                              : themeColor("brls/text", nvgRGB(255, 255, 255));
            nvgFillColor(vg, nvgRGBAf(tint.r, tint.g, tint.b, this->isFocused() ? 0.28f : 0.14f));
            nvgFill(vg);
            nvgRestore(vg);
        }

        NVGcolor fg = themeColor("brls/text", nvgRGB(255, 255, 255));
        /* The icon is centred on the ICON AREA, which is the item's first child in a ROW: 12 px of
         * left padding, then the box, and vertically centred by the item's own `AlignItems::CENTER`. */
        drawIcon(vg, this->icon, x + 12.0f + HG_SB_ICON_BOX * 0.5f, y + height * 0.5f, 24.0f, fg, a);

        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    void applyColors() {
        const float a = this->enabled ? 1.0f : 0.35f;
        const NVGcolor accent = themeColor("brls/accent", nvgRGB(0, 255, 204));
        const NVGcolor text = themeColor("brls/text", nvgRGB(255, 255, 255));
        /* The label is brighter when the row is active or focused, so the eye can find the current
         * section without reading every word. */
        NVGcolor base = (this->active || this->isFocused()) ? accent : text;
        base.a = a;
        this->label->setTextColor(base);
        NVGcolor dim = themeColor("brls/text_disabled", nvgRGB(80, 80, 80));
        dim.a = a;
        this->subtitle->setTextColor(dim);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        this->applyColors();
    }
    void onFocusLost() override {
        brls::Box::onFocusLost();
        this->applyColors();
    }

    brls::Box* iconArea = nullptr;
    brls::Label* label = nullptr;
    brls::Label* subtitle = nullptr;
    int icon = HG_SB_ICON_HOME;
    bool enabled = true;
    bool active = false;
};

/* ---- the bar -------------------------------------------------------------- */

SidebarView::SidebarView() {
    /* A HORIZONTAL STRIP ACROSS THE TOP OF THE HOME PAGE. See the note on HEIGHT in the header for
     * why it is not a left column any more: a vertical bar costs the poster grid columns, and the grid
     * needs all eight to keep a tier's "more" card on its first row. */
    this->setAxis(brls::Axis::ROW);
    this->setFocusable(false);   /* the ITEMS are focusable; the container is not a stop */
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(4.0f, 0.0f, 4.0f, 0.0f);
    this->setHeight(HEIGHT);
    this->setBackgroundColor(themeColor("brls/background", nvgRGB(45, 45, 45)));
    /* A hairline under the bar marks the edge of the content area, the way a separator does on a
     * desktop toolbar.
     *
     * THE KEY IS "brls/sidebar/separator", NOT "brls/separator" -- the latter does not exist in this
     * theme, and asking for it aborted the app on the first console run of this view. See themeColor
     * at the top of this file. */
    this->setBorderThickness(1.0f);
    this->setBorderColor(themeColor("brls/sidebar/separator", nvgRGB(81, 81, 81)));
}

SidebarView::~SidebarView() = default;

void SidebarView::rebuild(const SidebarContent& content) {
    this->clearViews(true);
    for (int i = 0; i < HG_SB_MAX; i++) this->views[i] = nullptr;
    this->count = hg_sidebar_build(this->items, HG_SB_MAX, content.haveHistory ? 1 : 0,
                                   content.haveWatchlist ? 1 : 0);

    /* ---- THE 收藏 ROW IS DROPPED FROM THIS BAR (build133, the user's instruction) ----------------------
     *
     * THEIR WORDS: "just remove that favourite entry from the home screen; favourites are reached from the history
     * screen, the way it was."
     *
     * The screen behind that entry is the library, and the library already carries BOTH tabs -- its own log line
     * says "switched to the favorites tab" when the entry is used. So the two home rows were two doors into one
     * room, and the user asked for one door. History stays; favourites are reached from inside it.
     *
     * Filtered here, in the view, rather than by teaching the model to skip it: hg_sidebar_build is host-tested
     * against the exact list it builds, and that list is still correct for any bar that wants it. This bar simply
     * does not show that row.
     *
     * IT ALSO REMOVES THE CRASH PATH. The 收藏 row was the only reason the counts had to be re-read when this
     * screen came back, and rebuilding this bar from inside a frame is what crashed the console twice (build131
     * inline, build132 with brls::sync). With no row to refresh, the bar is built once and never rebuilt. */
    {
        int w = 0;
        for (int i = 0; i < this->count; i++) {
            if (this->items[i].group == HG_SB_GROUP_PINNED) {
                this->items[w++] = this->items[i];
                continue;
            }
            if (this->items[i].icon == HG_SB_ICON_STAR) {
                hg::logPrintf("SIDEBAR: the 收藏 row is not shown (favourites are reached from the history "
                              "screen) [build133]");
                continue;
            }
            this->items[w++] = this->items[i];
        }
        this->count = w;
    }

    /* ONE PLACE BUILDS A ROW, so the two groups cannot drift apart -- and, more importantly, so the
     * pinned group cannot be added TWICE. It was, until this was factored out: the first loop below
     * ran over EVERY model item (including the pinned ones) and the third loop added the pinned ones
     * again after the spacer, so the bar drew a 设置 row under 历史 AND another one at the bottom.
     * MEASURED ON HARDWARE by the user: "现在在历史记录的下面有一个设置界面，然后在底部也有一个设置界面".
     *
     * It also has to skip... nothing else: the spacer is what separates the groups, and it is added
     * between the two loops. */
    auto addRow = [this](int i) {
        auto* item = new Item();
        item->setItem(&this->items[i]);
        /* Touch: tapping a sidebar entry activates it, as A does. */
        hg::installTapSelect(item);
        const int index = i;
        item->registerClickAction([this, index](brls::View*) {
            /* Activation goes through the callback. Which item is active is the bar's business; what
             * an item MEANS belongs to the screen that owns the bar. */
            this->setActive(index);
            if (this->onActivate) this->onActivate(index);
            return true;
        });
        this->addView(item);
        this->views[i] = item;
    };

    /* THE SECTIONS, and only the sections: the pinned group has to come after the spacer. */
    for (int i = 0; i < this->count; i++) {
        if (this->items[i].group == HG_SB_GROUP_PINNED) continue;
        addRow(i);
    }

    /* The gap that separates the pinned group from the sections, in the same way wiliwili's bottom
     * buttons are pinned to the foot of the bar rather than following the tabs. A growing spacer is
     * what pushes them down, and it is not focusable so it is never a stop. */
    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    spacer->setFocusable(false);
    this->addView(spacer);

    /* THE PINNED GROUP, appended last so the spacer pushes it to the bottom. */
    for (int i = 0; i < this->count; i++) {
        if (this->items[i].group != HG_SB_GROUP_PINNED) continue;
        addRow(i);
    }

    /* ---- THE PLAY-CHAIN STATUS LIGHT (build158) ---------------------------------------------------
     *
     * The user asked for the self-check's answer as a dot rather than as text: green when the whole play
     * chain answered, red when a stage failed, grey before any check has run. It goes at the END of this
     * row, which -- because the spacer above grows -- is the top-right corner of the home screen, and the
     * home screen is where someone stands before pressing Play, which is exactly when the answer is
     * worth having without asking for it.
     *
     * It is added ONCE, here, at construction. Nothing about the check ever adds, removes or replaces a
     * view: the dot reads the status as it draws (see hg_status_dot.hpp for why that is the design and
     * not a shortcut). */
    this->addView(new StatusDotView());

    this->activeIndex = hg_sidebar_first(this->items, this->count);
    if (this->activeIndex < 0) this->activeIndex = 0;
    this->layoutItems();

    hg::logPrintf("SIDEBAR: built %d items (%d sections, %d pinned; watchlist=%d history=%d)",
                  this->count, hg_sidebar_count_group(this->items, this->count, HG_SB_GROUP_SECTIONS),
                  hg_sidebar_count_group(this->items, this->count, HG_SB_GROUP_PINNED),
                  content.haveWatchlist ? 1 : 0, content.haveHistory ? 1 : 0);
}

SidebarView::Item* SidebarView::itemAt(int index) {
    if (index < 0 || index >= this->count) return nullptr;
    /* Straight out of the table rebuild() filled. This used to count focusable children and assume
     * their order matched the model's -- which stopped being true the moment the pinned group was
     * added after the spacer, and is what let a duplicate row through unnoticed. */
    return this->views[index];
}

void SidebarView::layoutItems() {
    /* Nothing to compute: the items are in a column Box, so borealis' own flex layout does the
     * vertical stacking and the spacer does the pinning. This function exists as the one place a
     * future change would go if the bar ever needed per-item geometry -- and as a name to call from
     * onLayout, where the width is finally known. */
}

void SidebarView::onLayout() {
    brls::Box::onLayout();
    this->layoutItems();
}

void SidebarView::moveFocus(int delta) {
    const int next = hg_sidebar_step(this->items, this->count, this->activeIndex, delta);
    if (next < 0 || next == this->activeIndex) return;
    /* THE MODEL MOVES FIRST, then focus follows it. Doing it the other way round (ask the framework
     * what is focused, then derive the index) breaks as soon as focus is somewhere the model does not
     * know about -- and the model is the thing the drawing is based on. */
    this->activeIndex = next;
    if (auto* item = this->itemAt(next)) brls::Application::giveFocus(item);
    hg::logPrintf("SIDEBAR: moving %+d -> item %d", delta, next);
}

void SidebarView::focusGroup(HgSidebarGroup group) {
    const int next = hg_sidebar_first_of_group(this->items, this->count, group);
    if (next < 0) return;
    this->activeIndex = next;
    if (auto* item = this->itemAt(next)) brls::Application::giveFocus(item);
}

void SidebarView::setActive(int index) {
    if (!hg_sidebar_valid(this->items, this->count, index)) return;
    this->activeIndex = index;
    for (int i = 0; i < this->count; i++) {
        if (auto* item = this->itemAt(i)) item->setActiveState(i == index);
    }
}

std::string SidebarView::labelOf(int index) const {
    if (index < 0 || index >= this->count) return "";
    return hg_tr(this->items[index].labelKey);
}

int SidebarView::labelKeyOf(int index) const {
    if (index < 0 || index >= this->count) return -1;
    return this->items[index].labelKey;
}

int SidebarView::setSubtitle(int index, const std::string& text) {
    if (index < 0 || index >= this->count) return 0;
    const int changed = hg_sidebar_set_subtitle(&this->items[index], text.c_str());
    if (changed) {
        if (auto* item = this->itemAt(index)) item->setSubtitleText(this->items[index].subtitle);
    }
    return changed;
}

void SidebarView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                       brls::FrameContext* ctx) {
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace hg
