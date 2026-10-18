/*
 * hg_status_dot implementation. See the header for what it is and why it reads a global rather than
 * being told what to draw.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_status_dot.hpp"

#include "hg_selfcheck.hpp"

namespace hg {

namespace {

/* The dot is 14 logical pixels across inside a 26-pixel box, so it has room to sit away from a screen
 * edge without a separate margin. The box is what the parent layout sees. */
const float DOT_BOX = 26.0f;
const float DOT_RADIUS = 7.0f;

} // namespace

StatusDotView::StatusDotView() {
    this->setWidth(DOT_BOX);
    this->setHeight(DOT_BOX);
    /* NOT FOCUSABLE, and that is not cosmetic: the top bar is walked by the d-pad, and a status light
     * that the cursor can land on is a stop the user has to walk past to reach the items they want. */
    this->setFocusable(false);
}

void StatusDotView::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style style, brls::FrameContext* ctx) {
    (void)style;
    (void)ctx;
    if (!vg || width <= 0.0f || height <= 0.0f) return;

    const PlayChainStatus status = playChainStatus();

    NVGcolor fill;
    switch (status) {
        case PLAY_CHAIN_OK:
            fill = nvgRGB(64, 200, 96);      /* green: the whole chain answered */
            break;
        case PLAY_CHAIN_FAILED:
            fill = nvgRGB(228, 72, 72);      /* red: a stage failed */
            break;
        default:
            /* GREY, and dimmer than either: "not checked yet" must not read as either answer. */
            fill = nvgRGBA(150, 150, 150, 120);
            break;
    }

    const float cx = x + width * 0.5f;
    const float cy = y + height * 0.5f;

    /* A thin dark rim, so a green dot stays visible against a light poster as well as a dark bar. */
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, DOT_RADIUS + 1.0f);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 120));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, DOT_RADIUS);
    nvgFillColor(vg, fill);
    nvgFill(vg);
}

} // namespace hg
