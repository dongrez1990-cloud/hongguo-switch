/*
 * hg_boot: the one startup sequence, shared by the shipping main and the
 * diagnostic main.
 *
 * GPL-3.0-only.
 *
 * WHY SHARED. The crash this project is chasing happened BEFORE the first
 * frame, and a diagnostic build that runs a different startup sequence tests
 * the wrong program. Both entry points call this function, so a run of the
 * diagnostic build exercises exactly the code the shipping build runs and
 * differs only in what it does afterwards.
 *
 * STARTUP SEQUENCE. borealis on Switch requires userAppInit/userAppExit, which
 * scripts/build-hongguo.ps1 links from
 * upstream/wiliwili/library/borealis/library/lib/platforms/switch/switch_wrapper.c.
 * That file does the socket/nifm/romfs/pl/setsys/lbl initialisation AND the
 * appletLockExit pair, matching what app/play-test/main.c had to add by hand
 * after the first version crashed the console with 2168-0002. Reusing it means
 * this app cannot forget one of those steps.
 *
 * ASCII-only source file.
 */

#pragma once

namespace hg {

/* Runs borealis init, window creation, theme variant, global quit and the C
 * layer's curl init -- each step logged. Returns false if borealis failed to
 * initialise, in which case the caller must return EXIT_FAILURE without
 * entering the main loop. */
bool bootBorealis(int argc, char** argv);

} // namespace hg
