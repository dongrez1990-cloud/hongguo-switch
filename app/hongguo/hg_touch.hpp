// hg_touch.hpp -- making a view selectable by touch.
//
// GPL-3.0-only.
//
// WHY THIS IS NEEDED AT ALL. The console report was "touch only scrolls, it does not select", and it
// is a property of the framework rather than a bug in the grid:
//
//   * `ScrollingFrame` INSTALLS ITS OWN gestures in its constructor -- a scroll recognizer and a
//     tap-to-stop-scrolling recognizer (scrolling_frame.cpp:42-84). Those are what make dragging work.
//   * NOTHING installs a tap-to-ACTIVATE recognizer. wiliwili's own flow is that each cell carries a
//     `TapGestureRecognizer` whose END state runs the view's primary action -- so in wiliwili, tapping
//     a card selects it, and that behaviour comes from the cells, not from the frame.
//
// Ours were built without one, so a card could be dragged around and never opened. This installs the
// missing half.
//
// THE STICKY DETAIL THAT MATTERS: the recognizer is created with the view's own primary action, so a
// tap runs exactly the callback the card already registered for the A button. That is deliberate --
// there is one activation path, and touch and pad both go through it. A second, parallel "touch
// handler" per cell is how the two inputs end up disagreeing about what a card does.
//
// ASCII-only source file.

#ifndef HG_TOUCH_HPP
#define HG_TOUCH_HPP

#include <borealis.hpp>

namespace hg {

/* Make `view` respond to a touch tap by running its primary action -- the same action the A button
 * runs. Safe to call once per cell, right after it is created and before its action is registered
 * (the recognizer holds the VIEW, not the callback, so the later registration is picked up).
 *
 * A view with no primary action does nothing on tap, which is the correct behaviour for a heading. */
void installTapSelect(brls::View* view);

} // namespace hg

#endif
