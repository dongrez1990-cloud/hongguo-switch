// hg_haptic.hpp -- one pulse, when something happened.
//
// GPL-3.0-only.
//
// WHY SO SMALL. The reference layout has ONE row for this ("手柄振动"), and the honest scope for it here is
// "the app can pulse the controller, and it does so at a few moments that mean something": the toggle in the
// settings screen (so the user can feel that it works) and adding something to the watchlist (a confirmation
// the console can deliver without looking up). Wiring it into every button press would be a different feature
// with a different switch, and pretending otherwise is how a settings row becomes a lie.
//
// The flag is read at call time, so the row applies immediately -- unlike the frame limit and the input swap,
// which borealis takes once at startup.
//
// ASCII-only source file.

#pragma once

namespace hg {

/* 1 when the setting is on. Read from hongguo.cfg at call time (cached after the first read). */
int hapticEnabled(void);

/* Pulse the first controller: `low`/`high` are the two motors' strengths, 0..65535, and the pulse stops by
 * itself after `ms`. Does nothing when the setting is off, when the platform has no input manager, or when
 * there is no controller attached -- each of those is logged ONCE, because a silently ignored pulse and a
 * working one look the same from the outside. */
void hapticPulse(unsigned short low, unsigned short high, int ms);

} // namespace hg
