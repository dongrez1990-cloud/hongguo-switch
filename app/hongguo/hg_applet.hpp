/*
 * hg_applet: notice when the console is docked or undocked, while the app runs.
 *
 * GPL-3.0-only.
 *
 * WHY THIS EXISTS. Reported: "playing video, insert the dock -- the picture becomes a small block in the
 * top-left corner, and only closing and reopening the app brings back the full-screen TV picture."
 *
 * THE LOG SAYS THE PROGRAM NEVER NOTICED. build168's SD log contains ZERO lines about docking, the
 * operation mode or a resolution change, across a session in which the user docked the console while the
 * player was open. So the app is not "handling docking badly" -- it is not handling it at all: nothing in
 * this program is subscribed to the event that says the mode changed.
 *
 * THE MECHANISM WAS ALREADY THERE AND WE WERE NOT CONSUMING IT. libnx raises `AppletHookType_OnOperationMode`
 * (applet.h), and the vendored borealis already registers a hook for it
 * (upstream/.../platforms/switch/switch_platform.cpp:32-65) which forwards to
 * `SwitchVideoContext::appletCallback` and resets the swapchain. That half fixes the FRAMEBUFFER; it does not
 * tell this app's own code that anything happened, and it does not touch this app's mpv render target, whose
 * dimensions were frozen when the player was set up. That frozen size is the small block in the corner.
 *
 * SO THIS MODULE IS THE MISSING SUBSCRIBER, and it does exactly two things:
 *
 *   1. it LOGS the change -- which is what makes the next console run answerable at all; today the
 *      transition is invisible;
 *   2. it re-asserts `appletSetAutoSleepDisabled(true)`, because that flag is believed to be dropped by the
 *      mode transition. build168's log shows the boot-time call SUCCEEDING
 *      ("boot: auto-sleep disabled for playback (rc=00000000)") while the console still slept in TV mode,
 *      so "the call failed" is excluded and "the flag did not survive the transition" is what is left.
 *
 * THREADING. libnx runs applet hooks on its own thread, NOT the app's main thread. Nothing in this file's
 * callback touches the UI, mpv or any borealis view: it writes a log line and pushes a deferred function onto
 * borealis' main-thread queue with brls::sync (thread.cpp:67 -- the queue is drained by
 * Threading::performSyncTasks from Application::mainLoop, application.cpp:204). See hg_applet.cpp for why
 * each piece is where it is.
 *
 * ASCII-only source file.
 */

#pragma once

namespace hg {

/* Registers the applet hook for operation-mode changes and records the mode the app started in.
 *
 * Call once, after borealis' window exists (borealis' own hook is installed when the Switch platform is
 * constructed, so this is a second, independent cookie -- see hg_applet.cpp). Safe to call more than once;
 * only the first call does anything, and the repeat is logged rather than silently ignored. */
void appletWatchStart(void);

/* 1 when the console is docked (TV / Console mode) RIGHT NOW, 0 in handheld mode.
 *
 * This asks libnx directly rather than returning a cached copy of the last event, so a caller that asks
 * without ever having received a hook still gets the truth. `tvIsDocked()` in hg_tv.cpp answers the same
 * question for the safe area; this one exists so a caller that is already reading THIS module's log does not
 * have to know about hg_tv. */
int appletIsDocked(void);

/* HOW MANY TIMES THE MODE HAS CHANGED since appletWatchStart(), and the mode the last event reported
 * (-2 = no event seen yet, 0 = handheld, 1 = docked).
 *
 * THE COUNTER IS THE POINT. The bug being fixed is that the program never noticed; a log line that only
 * appears on a change cannot be distinguished from a build where the hook never fires. A view can read the
 * counter every frame (cheap, no allocation, no view-tree change) and act on the frame where it moves -- the
 * "one-shot flag" pattern this project already uses elsewhere. */
unsigned appletModeChangeCount(void);
int appletLastReportedMode(void);

/* ---- KEEP THE CONSOLE AWAKE WHILE THIS APP RUNS (build172, the user's "主机模式下还是有睡眠状态") -------
 *
 * CALL THIS ONCE PER FRAME FROM THE MAIN LOOP. It does real work AT MOST ONCE A SECOND; every other call is
 * one integer comparison. (The gate assumes main-thread-only callers, which is how main.cpp uses it.)
 *
 * WHY NOT EVERY FRAME, WHICH WAS THE FIRST IDEA. Re-asserting the flag per frame is one IPC request to the
 * applet service per frame -- the same service appletMainLoop polls for messages -- and it is defending
 * against a MINUTE-SCALE timer, not a frame-scale one. The user describes the sequence themselves: the
 * screen dims first, and "roughly three to five minutes" later the console sleeps. A check once a second is
 * already 60-300x more often than the thing it is racing.
 *
 * THREE CALLS, AND EACH EARNS ITS PLACE:
 *
 *   1. `appletIsAutoSleepDisabled(&out)` -- THE READ-BACK, and it is what turns a belief into evidence. Until
 *      now the whole diagnosis rested on an unproven hypothesis: that the boot-time
 *      `appletSetAutoSleepDisabled(true)` (which returns rc=0, so "the call failed" is excluded) stops
 *      holding when the console changes operation mode. With the read-back, the log says which of the two it
 *      is: silence means the flag was never dropped and the sleep has another cause; a line means it WAS
 *      dropped and this build caught it.
 *
 *   2. `appletSetAutoSleepDisabled(true)` -- the re-assert, kept from build170's hook path so this module
 *      still repairs it at the moment of a mode change, not up to a second later.
 *
 *   3. `appletReportUserIsActive()` -- THE ONE THAT COVERS THE DIMMING, and the reason this is the right API
 *      for the sequence the user reported. Its own libnx documentation (applet.h:849) says it "reports that
 *      the user is active, for idle detection (screen dimming / auto-sleep)... equivalent to when the user
 *      uses HID input". `SetAutoSleepDisabled` names only the sleeping half; the dimming the user sees first
 *      is the other half of the same idle timer. It is also the oldest of the candidates (2.0.0+, with no
 *      applet-type restriction, where appletSetAutoSleepTimeAndDimmingTimeEnabled is OverlayApplet-only and
 *      therefore cannot be called by an Application applet like this one at all).
 *
 * THE RETURN CODES ARE LOGGED ONCE, at the first check, and then again only when something CHANGES. A
 * line per second would be 3600 lines an hour of "nothing happened", which is how a log stops being read --
 * the same reasoning build118 used for the cover walk's one-line-per-change rule. */
void appletKeepAwakeTick(void);

} // namespace hg
