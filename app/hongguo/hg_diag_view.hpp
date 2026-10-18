/*
 * hg_diag_view: thin subclasses that bracket borealis' own code with
 * breadcrumbs.
 *
 * GPL-3.0-only.
 *
 * WHY. The app crashed after the home screen's view had been built, laid out
 * and drawn at least once ("Loading home page..." was visible on the console's
 * screen), and it produced no message at all. Upstream borealis is read-only
 * here, so the next best thing is to subclass the view and record when control
 * enters and leaves the two virtuals that do the work: onLayout() and draw().
 *
 * A log that ends at "RF: ENTER draw" without a matching "RF: OK draw" says the
 * fault is inside RecyclerFrame::draw -- and, because draw's first statement is
 * cellsRecyclingLoop(), inside the recycling loop specifically. That is a claim
 * about borealis' code that no amount of reading it can settle, and one line of
 * output can.
 *
 * It brackets only the first few calls: the log is fsync'd per line on purpose
 * (a crash does not flush), so logging every frame of a 60 Hz loop would turn
 * the diagnostic into a performance problem of its own.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>

#include "hg_log.hpp"

namespace hg {

class LoggingRecyclerFrame : public brls::RecyclerFrame {
  public:
    void onLayout() override {
        if (layoutLogs < 4) {
            layoutLogs++;
            hg::logPrintf("RF: ENTER onLayout #%d (w=%.0f h=%.0f)", layoutLogs, this->getWidth(),
                          this->getHeight());
            brls::RecyclerFrame::onLayout();
            hg::logPrintf("RF: OK    onLayout #%d", layoutLogs);
        } else {
            brls::RecyclerFrame::onLayout();
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        if (drawLogs < 4) {
            drawLogs++;
            hg::logPrintf("RF: ENTER draw #%d", drawLogs);
            brls::RecyclerFrame::draw(vg, x, y, width, height, style, ctx);
            hg::logPrintf("RF: OK    draw #%d", drawLogs);
        } else {
            brls::RecyclerFrame::draw(vg, x, y, width, height, style, ctx);
        }
    }

  private:
    int layoutLogs = 0;
    int drawLogs = 0;
};

} // namespace hg
