/*
 * PlayerMenuPanel: the on-screen overlay for the in-player picture menu.
 *
 * GPL-3.0-only.
 *
 * WHAT IT IS. The picture half of wiliwili's in-player settings fragment
 * (fragment/player_setting.cpp, resources/xml/fragment/player_setting.xml): its
 * "equalizer" group of five sliders, its aspect selector and its mirror switch,
 * drawn as an opaque panel on the right of the video. This application has no
 * second settings surface inside the player, so the panel carries exactly those
 * rows and nothing else -- the danmaku/subtitle/shader groups in that XML have no
 * counterpart here (this service sends no danmaku or subtitle tracks at all, and
 * the switch to shader profiles would need shader files this build does not ship).
 *
 * ---------------------------------------------------------------------------
 * THE ONE DESIGN DECISION THAT MATTERS: THIS PANEL IS NOT FOCUSABLE.
 *
 * Not one of its views can take focus, and nothing in it registers an action.
 * That is deliberate and it is the whole reason the panel is safe to put on this
 * screen. borealis' brls::Slider would have been the natural widget for the five
 * numeric rows, and it is precisely the wrong one here:
 *
 *     View* Slider::getDefaultFocus() { return pointer; }   // slider.cpp:119
 *
 * `pointer` is a focusable Rectangle created with the slider, and getDefaultFocus
 * returns it UNCONDITIONALLY -- so dropping a Slider anywhere in this screen's view
 * tree invites borealis' focus search to land on it, and Slider::buttonsProcessing
 * then eats LEFT and RIGHT, which are the two keys this player uses to seek. The
 * symptom is "the seek keys stopped working", which reads as a broken player and
 * has already cost this project one round of diagnosis.
 *
 * Instead: plain Boxes and Labels, and every keypress is read from the raw
 * controller state once per frame in PlayerView::draw -- the same mechanism the
 * hold-to-seek curve uses, and for the same reason (it also gets an acceleration
 * curve, which registered actions do not).
 *
 * Being non-focusable has a pleasant second consequence worth writing down:
 * because View::isFocusable() is `focusable && visibility == VISIBLE`, a hidden
 * panel is not merely invisible, it is structurally out of focus contention.
 *
 * ---------------------------------------------------------------------------
 * THREADING. show() / hide() / updateValues() are called from PlayerView::draw on
 * the main thread, and the strings they read out of HgPMenu were published by that
 * same thread. Nothing here is touched from the poll thread or from a brls::sync
 * callback, so there is no lock and no stale-read window.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>

extern "C" {
#include "hg_pmenu.h"
}

namespace hg {

class PlayerMenuPanel : public brls::Box {
  public:
    PlayerMenuPanel();
    ~PlayerMenuPanel() override;

    /* Show or hide the whole panel. Visibility is the only thing that changes: the
     * labels inside are reused, so opening the menu costs no allocation and cannot
     * fail halfway.
     *
     * NOT NAMED show()/hide(). brls::View has virtual show(..)/hide(..) overloads
     * (view.hpp:1306-1341), and a parameterless pair with those names HIDES them --
     * which g++ reports as -Woverloaded-virtual on every build. Keeping the names
     * distinct means the framework's animated show/hide stay reachable and the build
     * stays warning-free, which matters because these builds are read by eye. */
    void setPanelVisible(bool visible);
    bool isPanelVisible() const { return this->shown; }

    /* Pull the row labels and values out of the menu state. Called once per frame
     * while the panel is shown -- cheap (HG_PMENU_ROW_COUNT short strings) and
     * idempotent, and it means there is exactly ONE place that decides what a row
     * looks like, with no way for the display and the state to drift apart.
     *
     * `qualityLabels` and `qualityCount` are the ladder, which lives OUTSIDE HgPMenu because it
     * comes from the network -- see hg_pmenu.h. "2 of 5" is a position; "540p" is an answer, and
     * the answer is what a menu is for. Passing them in rather than storing them here keeps this
     * panel a function of what it is handed, with nothing to go stale. */
    void updateValues(const HgPMenu& menu, const char* const* qualityLabels = nullptr,
                      int qualityCount = 0);

    bool isTranslucent() override { return false; }

  private:
    /* The rows, in the order hg_pmenu.h defines them. Fixed-size arrays rather than
     * vectors because HG_PMENU_ROW_COUNT is a compile-time constant and an
     * out-of-range index here should be a compile error, not a silent resize. */
    brls::Label* nameLabels[HG_PMENU_ROW_COUNT] = {};
    brls::Label* valueLabels[HG_PMENU_ROW_COUNT] = {};
    brls::Box* rowBoxes[HG_PMENU_ROW_COUNT] = {};

    brls::Label* titleLabel = nullptr;
    brls::Label* hintLabel = nullptr;

    bool shown = false;

    /* The last highlight drawn, so the per-frame update only touches the two rows
     * whose appearance actually changed. -1 means "nothing highlighted yet". */
    int lastHighlighted = -1;

    void applyHighlight(int row);
};

} // namespace hg
