// hg_tv.hpp -- the TV's safe area, from the console's own setting.
//
// GPL-3.0-only.
//
// WHY THIS EXISTS. Reported: "在播放界面时，主机模式下会超出电视显示范围" -- the picture on the player screen
// goes past the edge of the TV. That is what a TV's OVERSCAN does, and the console already knows about
// it: the system setting "TV screen size" (100% / 95% / 90%) is what libnx calls `underscan` inside
// `SetSysTvSettings` (set.h:417-426, `setsysGetTvSettings`). An app that draws edge to edge -- and a
// full-screen video is the only thing in this app that does -- gets its outer few percent cut off by the
// television unless it insets.
//
// SO THIS IS NOT A GUESS ABOUT GEOMETRY. The project's own diagnostics ("SW surface vs view", "UI scale")
// say the frames and scales are exact; the missing piece is the one number only the console can supply,
// and this reads it instead of inventing a margin.
//
// TWO HONEST CAVEATS, both of which the log makes visible rather than silent:
//
//   1. libnx documents the field only as "Underscan", with no enum. The mapping used here is the one the
//      console's own setting offers -- 0 -> 100%, 1 -> 95%, 2 -> 90% -- and the RAW value is logged, so a
//      console that reports something else shows up in the log instead of quietly insetting by nothing.
//   2. Anything outside 0..2 is treated as "no inset" (100%) and logged as unknown: insetting by a value
//      we do not understand would shrink the picture for a reason nobody could explain.
//
// `player.tv.safe=<percent>` in hongguo.cfg overrides the whole thing, which is the way back if the
// mapping turns out to be wrong on hardware -- one FTP line, no rebuild, the same escape hatch this
// project ships for every settings-shaped decision.
//
// ASCII-only source file.

#pragma once

namespace hg {

/* The safe area as a percentage of the frame: 100 means "draw edge to edge".
 *
 * Ask for this ONCE per use (it is cached after the first call, and the console's setting cannot change
 * while the app runs). Returns 100 when the setting cannot be read -- an unreadable setting must not
 * shrink anything. */
int tvSafePercent(void);

/* The raw `underscan` value read from set:sys, or -1 when the service could not be asked. For the log:
 * it is the only way to tell "the console says 95%" from "we assumed 95%". */
int tvUnderscanRaw(void);

/* The configured override, or 0 when player.tv.safe is not set. For the log. */
int tvSafeOverride(void);

/* 1 when the console is docked right now (appletGetOperationMode == Console). The safe area is a TV
 * concept: applying it in handheld mode would shrink the picture against a screen that has no overscan. */
int tvIsDocked(void);

/* THE ONE CALL A VIEW NEEDS: inset a rectangle by the safe area, centred, and leave `*width`/`*height`
 * scaled to match (so the same picture is drawn smaller, not cropped). Does nothing when undocked, when
 * the safe area is 100%, or when the rectangle is empty. */
void tvApplySafeArea(float* x, float* y, float* width, float* height);

} // namespace hg
