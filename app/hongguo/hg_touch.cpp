// hg_touch.cpp -- see hg_touch.hpp for why a tap recognizer has to be installed by hand.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_touch.hpp"

#include "hg_log.hpp"

namespace hg {

void installTapSelect(brls::View* view) {
    if (!view) return;

    /* The one-argument constructor (`TapGestureRecognizer(View*)`) wires the recognizer to the view's
     * own primary action and fires it only on GestureState::END -- a release INSIDE the view's bounds.
     * That is exactly "tap to select":
     *
     *   * a drag that starts on the card and leaves it resolves to FAILED, so scrolling with a finger
     *     on a card does not open it -- which is the behaviour that matters most here, because the
     *     whole grid is covered in cards and a scroll gesture necessarily starts on one;
     *   * a drag that starts on the card and comes back would END inside, and would activate. That is
     *     the same rule the framework's own scroll-tap uses, and it matches what a touchscreen user
     *     expects from a "tap" that did not move.
     *
     * TAKING THE PRIMARY ACTION RATHER THAN A CALLBACK IS THE POINT. The card registers its click
     * action separately (the grid needs the item index, which the recognizer cannot know), so this
     * recognizer stays a pure "activate me" and there is exactly one activation path for both inputs.
     * A second parallel handler per cell is how pad and touch end up disagreeing. */
    view->addGestureRecognizer(new brls::TapGestureRecognizer(view));

    /* Logged once per PROCESS, not per cell: a 36-cell grid would otherwise emit 36 identical lines,
     * and the useful fact is "tap-select is installed in this build", not "N times". */
    static bool said = false;
    if (!said) {
        said = true;
        hg::logPrintf("TOUCH: tap-to-select installed (a tap runs the view's primary action; a drag "
                      "that leaves the view does not)");
    }
}

} // namespace hg
