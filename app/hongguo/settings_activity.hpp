/*
 * Settings screen, following wiliwili's.
 *
 * GPL-3.0-only.
 *
 * WHAT IT IS. wiliwili's settings screen (source/activity/setting_activity.cpp,
 * resources/xml/activity/setting_activity.xml) is a tab frame whose tabs hold
 * grouped rows: a header, then SelectorCell rows, one per setting. That structure
 * is copied here, with borealis' own widgets -- TabFrame, ScrollingFrame,
 * SelectorCell and DetailCell all ship in borealis, so nothing had to be ported.
 *
 * WHAT IT DOES NOT HAVE, AND WHY. wiliwili's screen offers roughly a hundred
 * items. Everything dropped here was dropped for a reason that is written on the
 * About tab rather than left as an unexplained gap:
 *
 *   - the 15 DANMAKU_* items and the comment settings: MEASURED -- this service's
 *     payloads contain no danmaku or comment data at all ("danmaku", "comment",
 *     "reply", "barrage" occur 0 times in both the home and the detail blobs)
 *   - LIVE_*: this service has no live rooms
 *   - window-on-top, keyboard shortcuts, keyboard/gamepad icon sets, self-update
 *     and downloadable themes: desktop concepts with no counterpart on Switch
 *
 * ONLY SETTINGS THAT DO SOMETHING APPEAR. Every row writes a key in
 * sdmc:/switch/hongguo.cfg -- the same file the FTP experiment switches use, via
 * the same reader. There is deliberately no second store: a value this screen
 * wrote but cfgValue() did not see would be worse than having no screen at all.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>

#include "hg_alive.hpp"   /* the liveness token: see hg_alive.hpp for the crash it prevents */

namespace hg {

class SettingsActivity : public brls::Activity {
  public:
    SettingsActivity();
    ~SettingsActivity() override;

    brls::View* createContentView() override;

    /* OPAQUE. This screen covers the whole window; without this, borealis would
     * keep drawing whatever is underneath it into every frame (View::isTranslucent
     * is `fadeIn || inFadeAnimation`, so a page with a pending fade-in drags its
     * predecessor along). */
    bool isTranslucent() override { return false; }

    /* THE LIVENESS TOKEN, PUBLIC SO THE TAB BUILDERS CAN TAKE IT. See hg_alive.hpp: the network tab's probe
     * runs on a worker for several seconds and then writes into rows that this screen owns, so the callback has
     * to be able to ask whether the screen is still there. It is public because those builders are free functions
     * (makeNetworkGroup and friends), and the alternative -- a getter -- would say the same thing less directly. */
    AliveToken alive_ = makeAlive();
};

} // namespace hg
