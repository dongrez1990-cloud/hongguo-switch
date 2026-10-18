/*
 * hg_status_dot: the play-chain status light.
 *
 * GPL-3.0-only.
 *
 * WHAT IT IS. A small filled circle: GREEN when the last play-chain check passed every stage, RED when a
 * stage failed, GREY when no check has run yet. The user's instruction was exactly this -- "just show the
 * check result as a green dot, red if it fails, no need for this text box" -- after the first version put
 * the same answer in a notification that appeared over the video and vanished after a few seconds.
 *
 * WHY IT READS THE STATUS INSTEAD OF BEING TOLD IT. The check finishes on a worker thread, seconds after
 * the failure that started it. The alternative would be for that thread to reach into the view tree and
 * change this view -- and this project has paid for that class of mistake more than once (a freed cell
 * written from a brls::sync callback is an Atmosphere Data Abort; see hg_alive.hpp). Drawing reads a
 * global that the worker sets, so the view is never touched from anywhere but the render loop, there is
 * no callback to go stale, and there is no view-tree mutation at any point in the check's life.
 *
 * WHY A VIEW AND NOT A LABEL. A glyph or a word would have to change with the status, which means the
 * view tree changes with it. A circle is painted from whatever the status happens to be that frame, so
 * the status can change at any moment and nothing has to be rebuilt.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>

namespace hg {

class StatusDotView : public brls::View {
  public:
    StatusDotView();

    /* Paints the circle for the CURRENT status -- see hg_selfcheck.hpp for what the three mean. */
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;
};

} // namespace hg
