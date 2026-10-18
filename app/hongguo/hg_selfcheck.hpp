/*
 * hg_selfcheck: when playing fails, walk the whole play chain and say which link broke.
 *
 * GPL-3.0-only.
 *
 * WHY THIS EXISTS, in the terms of the failure that produced it. On 2026-10-13 every attempt to play
 * answered "Playback failed: play_plan rc=-5". Detecting it was never the hard part -- the user saw the
 * message within seconds. DIAGNOSING it took a round: -5 only means "the response carried no playable
 * source", and it is produced by a server that refused the request, a server that changed its answer
 * shape, a parser that stopped matching, and a truncated response alike. The one thing that answered it
 * was the HTTP status and the first 512 bytes of the body (the build155 failure logging), which showed a
 * 125-byte {"Code":110001} envelope: the batched endpoint this app called had been retired and replaced
 * by a single-video one.
 *
 * So this module does not wait for the user to report a symptom and does not guess at one. When the
 * player fails to get a plan or a ladder, it walks THE WHOLE CHAIN -- home page, detail page, signed
 * video-model request, plan parse, quality ladder, and a Range request for the first bytes of the
 * stream -- using the SAME production code the player itself calls (Client::fetchPlayPlan and friends),
 * and reports each stage's measurement separately. A stage that was never reached says so rather than
 * showing a zero, because "0 bytes" and "we never got that far" are different findings.
 *
 * WHY ON FAILURE AND NOT AT STARTUP. Three reasons, and they are the reason this is not a boot check:
 *
 *   1. It costs nothing when everything works. A startup check would send a set of signed requests on
 *      every launch, and this service rate-limits (the reference project rotates device identities for
 *      exactly that reason). A diagnostic that invites the failure it is meant to catch is a bad one.
 *   2. It cannot raise a false alarm. A console that has just booted, or is behind a portal, fails a
 *      startup check for reasons that have nothing to do with playback -- and a warning people learn to
 *      ignore is worse than no warning.
 *   3. The failure moment has the most information: which episode, which stage, and the server's own
 *      words in the response body. That is exactly what was missing last round.
 *
 * A manual re-run lives on the settings screen's self-check tab (settings_activity.cpp), which is the
 * on-demand half of the same code.
 *
 * ASCII-only source file (the log and the on-screen strings are English for the same reason every other
 * string this app owns is: the build machine parses non-ASCII source as GBK).
 */

#pragma once

#include <string>
#include <vector>

namespace hg {

/* One link of the chain, with what it measured. `detail` is always a measurement -- "206, 64 bytes,
 * ftyp" or "HTTP 200 | 125 bytes | first 512: {...}" -- never just "failed". */
struct CheckStage {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct PlayChainReport {
    bool ok = false;
    int msElapsed = 0;
    std::string trigger;    /* why the check ran, in the words of the failure that asked for it */
    std::string episodeId;  /* the episode tested; empty when the chain broke before one was found */
    std::string headline;   /* one line: what to tell the user, and what it means */
    std::vector<CheckStage> stages;
};

/* ---- THE RESULT AS A LIGHT, NOT A SENTENCE (build158) ---------------------------------------------
 *
 * The user's instruction, after seeing the first version: "just show the check result as a green dot,
 * red if it fails -- no need for this text box." They are right, and not only for tidiness: the
 * notification this replaces appeared ON TOP of whatever was being watched and disappeared a few
 * seconds later, so it was both intrusive and easy to miss. A dot in the corner is neither.
 *
 * It is a STATUS, not an event: it says what the last check found and stays saying it, which is what
 * makes it useful on a screen someone is about to press Play from. The full per-stage detail -- the
 * part that takes reading -- is on the settings screen's Play check tab and in the log. */
enum PlayChainStatus {
    /* GREY until a check has run. "Not checked yet" is a different fact from "failed", and a red dot
     * that has never been earned is how a warning light teaches people to ignore it. */
    PLAY_CHAIN_UNKNOWN = 0,
    PLAY_CHAIN_OK = 1,        /* GREEN: every stage answered */
    PLAY_CHAIN_FAILED = 2     /* RED: a stage failed; the tab and the log say which */
};

/* The most recent check's outcome, safe to read from any thread (the drawing code calls it every
 * frame). Never resets to UNKNOWN once a check has run. */
PlayChainStatus playChainStatus();

/* ---- AND GREEN WHEN PLAYBACK ITSELF PROVES IT (build158) ------------------------------------------
 *
 * Without this the light would sit GREY through a perfectly normal session: the check only runs when
 * something fails, so a user whose app works would never see it turn green and could reasonably read the
 * grey as a fault.
 *
 * Called by the player at the one point where the real path has proven the same two stages the check
 * looks for -- the plan request answered and the quality ladder answered. That is better evidence than a
 * probe, because it is the actual playback that just worked. */
void notePlayChainWorked(const char* what);

/* Walk the chain once. BLOCKING: several HTTPS requests, so call it from a worker thread (hg_thread.hpp
 * -- std::thread throws ENOSYS on this platform). `trigger` is copied into the report; `episodeId` is
 * kept for the log's context, and the chain itself is walked from the top so that a break anywhere is
 * located rather than assumed. */
PlayChainReport runPlayChainCheck(const std::string& trigger, const std::string& episodeId);

/* Run the check on a thread of its own and report the result: one block in the log, and one on-screen
 * notification carrying the headline. Returns immediately, so the caller's own failure dialog is not
 * delayed by it.
 *
 * AT MOST ONE RUN PER MINUTE. A failing episode can produce a plan failure and a ladder failure within a
 * second of each other, and a user retrying a broken episode produces more; without the cooldown the
 * diagnostic would become a request storm against a service that already rate-limits. */
void startPlayChainCheck(const std::string& trigger, const std::string& episodeId);

/* The most recent report, for the settings screen. False until one has run. */
bool lastPlayChainReport(PlayChainReport& out);

} // namespace hg
