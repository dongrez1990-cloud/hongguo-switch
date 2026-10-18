// hg_tv.cpp -- see hg_tv.hpp for why this reads the console's setting instead of assuming a margin.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_tv.hpp"

#include <switch.h>

#include <cstdlib>
#include <string>

#include "hg_cfg.hpp"
#include "hg_log.hpp"

namespace hg {

/* Read an integer cfg value. Local rather than shared: hg_cfg exposes strings (there is a private
 * cfgGridInt in home_activity.cpp with the same shape), and one more three-line parser here beats
 * widening hg_cfg's interface for one setting. */
static int tvCfgInt(const char* key, int fallback) {
    std::string raw = hg::cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    const long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || v <= 0) return fallback;
    return (int)v;
}

/* -1 = not read yet, -2 = read and failed. Distinguishing "not yet" from "failed" is what keeps the
 * failure from being logged once per frame. */
static int g_underscan = -1;

static int readUnderscan(void) {
    if (g_underscan != -1) return g_underscan;

    g_underscan = -2;
    /* set:sys IS NOT INITIALISED ELSEWHERE IN THIS APP. Checked, not assumed: nothing in app/ or in the
     * borealis Switch platform calls setsysInitialize, so this is the one place that does, and it exits
     * the service again immediately -- the setting cannot change while the app runs. */
    if (R_FAILED(setsysInitialize())) {
        hg::logPrintf("TV: setsysInitialize FAILED -- the safe area falls back to 100%% (no inset)");
        return g_underscan;
    }
    SetSysTvSettings tv;
    Result rc = setsysGetTvSettings(&tv);
    setsysExit();
    if (R_FAILED(rc)) {
        hg::logPrintf("TV: setsysGetTvSettings FAILED (0x%x) -- the safe area falls back to 100%%",
                      (unsigned)rc);
        return g_underscan;
    }
    g_underscan = (int)tv.underscan;
    return g_underscan;
}

int tvUnderscanRaw(void) { return readUnderscan(); }

int tvSafeOverride(void) {
    static int cached = -3;
    if (cached != -3) return cached > 0 ? cached : 0;
    const int v = tvCfgInt("player.tv.safe", 0);
    /* A percentage, and only a sane one: 50..100. Anything else is ignored rather than applied, because a
     * typo would otherwise shrink the picture to nothing. */
    cached = (v >= 50 && v <= 100) ? v : 0;
    if (v != 0 && cached == 0) {
        hg::logPrintf("TV: player.tv.safe=%d is not a percentage in 50..100 -- ignored", v);
    }
    return cached;
}

int tvIsDocked(void) { return appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0; }

int tvSafePercent(void) {
    int pct = 100;
    int raw = readUnderscan();

    /* 0 -> 100%, 1 -> 95%, 2 -> 90%: the three choices the console's own TV screen-size setting offers.
     * See the header for why the raw value is logged. */
    if (raw == 0) pct = 100;
    else if (raw == 1) pct = 95;
    else if (raw == 2) pct = 90;
    else pct = 100;

    const int over = tvSafeOverride();
    if (over > 0) pct = over;

    static bool said = false;
    if (!said) {
        said = true;
        hg::logPrintf("TV: docked=%d underscan=%d -> safe=%d%% (override=%d) [build41]", tvIsDocked(), raw,
                      pct, over);
        if (raw > 2) {
            hg::logPrintf("TV: underscan=%d is outside the 0/1/2 the console's TV screen-size setting uses "
                          "-- treated as 100%% (no inset); use player.tv.safe to force one",
                          raw);
        }
    }
    return pct;
}

void tvApplySafeArea(float* x, float* y, float* width, float* height) {
    if (!x || !y || !width || !height) return;
    if (!tvIsDocked()) return;
    const int pct = tvSafePercent();
    if (pct >= 100 || pct < 50) return;
    if (*width <= 0.0f || *height <= 0.0f) return;

    const float scale = (float)pct / 100.0f;
    const float w = *width * scale;
    const float h = *height * scale;
    /* CENTRED: an inset that hugged one corner would look like a bug of its own. */
    *x = *x + (*width - w) * 0.5f;
    *y = *y + (*height - h) * 0.5f;
    *width = w;
    *height = h;
}

} // namespace hg
