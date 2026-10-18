// hg_hints.cpp -- see the header for why this is a wrapper and not a widget of our own.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_hints.hpp"

#include <borealis/views/hint.hpp>

#include "hg_cfg.hpp"
#include "hg_log.hpp"

extern "C" {
#include "hg_lang.h"   /* HG_T_OK / HG_T_QUIT: see rewriteRawI18nKeys */
}

namespace hg {
namespace {

/* How tall the strip is. Enough for the button glyph plus its label at borealis' default hint font, and small
 * enough that no screen loses a row of content to it: the home grid's rows are ~250 px, the settings rows 40. */
const float HINT_BAR_HEIGHT = 34.0f;

/* ---- THE RAW i18n KEYS, AND WHY THEY HAVE TO BE REWRITTEN HERE -------------------------------
 *
 * MEASURED ON HARDWARE (the console's own crash screenshots, 2026-10-10): the hint bar drew
 *
 *     + hints/exit      B Back      A hints/ok
 *
 * -- two RAW LOOKUP KEYS, shown to the user as if they were words.
 *
 * They come from borealis' own AppletFrame, which registers its actions as `"hints/ok"_i18n`. `_i18n` resolves
 * through brls::i18n, and that reads its catalogue from `romfs:/i18n/<locale>/` (lib/core/i18n.cpp:46-60) --
 * this app has no such resources, so the lookup misses and `getStr` returns the key unchanged. Every other hint
 * on that bar is one of OUR strings ("Back", "Search", "Refresh") and is therefore fine, which is why only
 * those two looked broken.
 *
 * THE FIX IS A REWRITE, NOT A FILTER: Action::setHintText() is public (actions.hpp:149), so the action itself
 * can be given a translated label. Hiding the two hints would have been simpler and would have thrown away a
 * useful one -- "A activates the thing you are on" is exactly what a user needs to know on a settings row.
 *
 * It runs when the FOCUS CHANGES rather than every frame: the walk is short, but refilling a Box's children
 * sixty times a second would not be. */

void rewriteRawI18nKeys() {
    brls::View* v = brls::Application::getCurrentFocus();
    int rewritten = 0;

    while (v != nullptr) {
        for (auto& action : v->getActions()) {
            if (!action) continue;
            const std::string text = action->getHintText();
            /* ONLY KEYS. Our own labels never contain a slash, and neither does any real English label, so this
             * cannot eat a legitimate hint by accident. */
            if (text.empty() || text.find('/') == std::string::npos) continue;

            if (text == "hints/ok") {
                action->setHintText(hg_tr(HG_T_OK));
            } else if (text == "hints/back") {
                action->setHintText(hg_tr(HG_T_BACK));
            } else if (text == "hints/exit") {
                action->setHintText(hg_tr(HG_T_QUIT));
            } else if (text == "hints/cancel") {
                action->setHintText(hg_tr(HG_T_CANCEL));
            } else {
                /* An unknown key is left alone rather than guessed at, but it is NAMED, so the next report of a
                 * raw key on this bar comes with the key in the log. */
                hg::logPrintf("HINTS: leaving unknown key '%s' alone (no translation for it)", text.c_str());
                continue;
            }
            rewritten++;
        }
        v = v->getParent();
    }

    if (rewritten > 0) {
        hg::logPrintf("HINTS: rewrote %d raw i18n key(s) on the hint bar (borealis has no catalogue in this "
                      "build) [build73]",
                      rewritten);
    }
}

/* THE BAR'S CONTAINER, WHICH RE-READS THE SWITCH AS THE USER CHANGES IT.
 *
 * The alternative was to decide once, while the screen is built, and mark the settings row "needs a restart" --
 * which for this row would have been the wrong sentence: the hint bar belongs to screens that are rebuilt every
 * time they are opened, so applying it at once is the honest behaviour. One cached cfg read and one visibility
 * compare per frame is the whole cost (hg_cfg only re-reads the file when it has changed). */
class HintBarBox : public brls::Box {
  public:
    void setHints(brls::View* h) { this->hints = h; }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        const bool want = !hg::cfgIs("app.hints", "no");
        if (this->hints && want != this->shown) {
            this->shown = want;
            this->hints->setVisibility(want ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            hg::logPrintf("HINTS: the settings row turned the hint bar %s", want ? "on" : "off");
        }

        /* SEE rewriteRawI18nKeys. Once per focus change: the framework refills the bar on its own event, which
         * fires after this, so the fixed labels are what gets drawn from the next frame onward -- and since the
         * bar redraws every frame, that is not visible. */
        brls::View* focus = brls::Application::getCurrentFocus();
        if (focus != this->lastFocus) {
            this->lastFocus = focus;
            rewriteRawI18nKeys();
        }

        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    brls::View* hints = nullptr;
    bool shown = true;
    /* Only to notice a change; the pointer is never dereferenced. */
    brls::View* lastFocus = nullptr;
};

} // namespace

brls::View* withHintBar(brls::View* content) {
    if (!content) return nullptr;

    if (hg::cfgIs("app.hints", "no")) {
        hg::logPrintf("HINTS: app.hints=no -- no hint bar on this screen");
        return content;
    }

    auto* wrapper = new brls::Box();
    wrapper->setAxis(brls::Axis::COLUMN);
    wrapper->setFocusable(false);

    /* grow=1 so the screen keeps every pixel it had apart from the strip. */
    content->setGrow(1.0f);
    wrapper->addView(content);

    /* THE BAR ITSELF. borealis' own widget: it collects the focused view's actions up the parent chain, drawing
     * each as the console's button glyph plus the action's label (which is the string this app passes to
     * registerAction -- "Back", "Search", "Refresh" ...). It refreshes on the global hints event, so nothing here
     * has to poll or listen. */
    /* A WRAPPER THAT RE-CHECKS THE SWITCH EVERY FRAME, so the settings row for this shows or hides the bar as
     * soon as it is changed instead of "next time you open the screen". The check is one cached cfg read and a
     * visibility compare: hg_cfg re-reads the file only when it has changed, and setting the same visibility
     * twice costs nothing. See HintBarBox::draw. */
    auto* box = new HintBarBox();
    box->setAxis(brls::Axis::COLUMN);
    box->setFocusable(false);
    auto* hints = new brls::Hints();
    hints->setHeight(HINT_BAR_HEIGHT);
    hints->setFocusable(false);
    box->setHints(hints);
    box->addView(hints);
    wrapper->addView(box);

    hg::logPrintf("HINTS: hint bar added UNDER the content (%.0f px; app.hints=no removes it live) [build59]", HINT_BAR_HEIGHT);
    return wrapper;
}

} // namespace hg
