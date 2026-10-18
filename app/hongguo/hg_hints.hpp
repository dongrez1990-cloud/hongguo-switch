// hg_hints.hpp -- the button hints along the bottom of a screen.
//
// GPL-3.0-only.
//
// WHY THIS IS THREE LINES OF CODE AND NOT A FEATURE. borealis already has everything: `brls::Hints` is a widget
// that walks up from the focused view collecting its registered gamepad actions and draws each one as the
// console's own button glyph plus the action's label (hint.cpp:185-219), and it SUBSCRIBES to
// `Application::getGlobalHintsUpdateEvent()` in its constructor (hint.cpp:131-137), so it refreshes itself when
// the focus moves. What was missing was that no screen ever put one in its tree -- which is why the handover
// records "the hint bar does not exist" rather than "the hints are wrong".
//
// SO THIS EXISTS TO MAKE ADDING ONE A ONE-LINER, and to hold the two decisions that are ours:
//
//   1. AT THE BOTTOM, not in the frame's header. borealis' own AppletFrame puts the hint box in the HEADER
//      (applet_frame.cpp:74-79); the reference layout this app follows puts it along the bottom, which is also
//      where a television viewer looks for it. The widget is the same one either way.
//   2. OFF IS ONE cfg LINE (`app.hints=no`), like every other layout change on a screen that already works.
//
// THE PLAYER SCREEN IS DELIBERATELY NOT WRAPPED: it draws its own control bar and overlay with nanovg because
// its keys are polled from the controller rather than registered, and a hint bar under a full-screen video would
// have to either cover it or shrink it. See docs/待做清单.md E1.
//
// ASCII-only source file.

#pragma once

#include <borealis.hpp>

namespace hg {

/* Wrap `content` in a column with a live hint bar underneath, and hand back the wrapper. Returns `content`
 * unchanged when `app.hints=no`, so a caller never has to check the switch itself.
 *
 * `content` must be a view that GROWS (the wrapper gives it grow=1), which every screen's root already is. */
brls::View* withHintBar(brls::View* content);

} // namespace hg
