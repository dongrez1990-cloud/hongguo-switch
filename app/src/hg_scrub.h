// hg_scrub.h -- the player's drag-to-seek model, as pure logic.
//
// GPL-3.0-only.
//
// WHAT THIS REPLACES, AND WHY IT IS BETTER. The player already had hold-to-accelerate
// seeking: hold LEFT and it issued a stream of mpv `seek N relative` commands on an
// accelerating curve, so the position changed while you held. That works, and it has
// two real problems:
//
//   1. IT CANNOT PREVIEW ANYTHING. A relative seek has no target -- the target is "wherever
//      we end up" -- so there is nothing to show on a preview. wiliwili's slider works
//      differently: dragging sets a POINT, the bar draws that point, and the seek happens
//      once when you let go (`video_progress_slider.hpp`: progressEvent while dragging,
//      progressSetEvent on release).
//   2. EVERY INTERMEDIATE POSITION IS DECODED. Holding RIGHT across a two-hour video at
//      60 s/s asks mpv to decode a GOP every frame for the whole drag. It is the most
//      expensive thing this player can be asked to do, and it is all thrown away except
//      the last position.
//
// So this models wiliwili's shape: BEGIN anchors at the current position, STEP moves the
// target along the same acceleration curve, END commits ONE seek to the target. Nothing
// is fetched or decoded during the drag except what the preview asks for.
//
// EVERYTHING IN SECONDS, NOT IN FRACTIONS.
//
// The first version of this file mixed the two -- it accumulated the step in seconds into
// a field that held a fraction -- and the host suite caught it on the third check. That
// is worth recording because the mistake is invisible in the arithmetic: 0.1 + 60 is a
// perfectly good double, it just means nothing. So the model's state is seconds
// throughout, and the only place a fraction exists is the two bar-geometry functions,
// which take a fraction because that is what a position on a bar IS.
//
// WHY IT IS C. Same reason as hg_pmenu and hg_grid: this is arithmetic whose mistakes are
// quiet. A target past the end seeks off the stream; a bar that maps x to the wrong
// position makes the preview disagree with where the seek lands; a commit that happens on
// every step is the expensive behaviour this was written to remove. The host suite checks
// all of it, including that the BAR and the COMMIT agree -- the one property that turns a
// wrong mapping into a user-visible lie.
//
// ASCII-only source file.

#ifndef HG_SCRUB_H
#define HG_SCRUB_H

#ifdef __cplusplus
extern "C" {
#endif

// The bar reserves a few pixels at each end so its cursor does not sit half off the
// screen. Fractions still run the full 0..1 -- the inset is applied to the PIXELS, not to
// the time, or the last few seconds of a video would be unreachable by dragging.
#define HG_SCRUB_INSET 8.0f

// Shortest gap between two preview updates, in milliseconds. A preview costs a real
// operation (a frame render, or in the future a thumbnail fetch), and a drag produces a
// step every frame; without a floor, holding RIGHT would start 60 previews a second.
#define HG_SCRUB_PREVIEW_MS 250

typedef struct {
  int active;            // 1 while a drag is in progress
  double anchorSeconds;  // playback position when the drag began
  double posSeconds;     // the TARGET position now, already clamped to [0, duration]
  double duration;       // the scale this drag was begun with; a drag never spans two
  double movedSeconds;   // how far this drag has travelled, in seconds (unsigned)
  int dir;               // +1 or -1 while active, 0 otherwise
  double lastPreviewAt;  // CPU milliseconds of the last preview request
} HgScrub;

// The bar geometry a caller is drawing into.
typedef struct {
  float barX;
  float barW;
  double duration;
} HgScrubBar;

void hg_scrub_init(HgScrub* s);

// Begin a drag, anchored at `playbackSeconds` on a video of `duration` seconds.
//
// Returns 1 when a drag actually started; 0 when the duration or the position is
// unusable, in which case nothing is active and STEP will do nothing. mpv reports
// duration 0 or negative for a while after a load, so refusing here is the normal
// "you cannot scrub yet" state rather than an error.
int hg_scrub_begin(HgScrub* s, double playbackSeconds, double duration, int dir, double now);

// Move the target by `deltaSeconds` (sign included; the caller has already applied the
// acceleration curve and the direction). Returns 1 when the target CHANGED -- a step that
// hits an end of the video returns 0, which is what stops a caller redrawing or
// re-requesting a preview for a drag that is no longer moving.
int hg_scrub_step(HgScrub* s, double deltaSeconds);

// End the drag. Writes the position to seek to, in SECONDS, into *outSeconds.
//
// Returns 1 when a seek should be issued and 0 when nothing changed -- releasing without
// moving, or a drag that was never active. That distinction is the point: seeking to where
// the video already is makes mpv reload the stream for no reason, and a caller that cannot
// tell the two apart will do exactly that on every tap.
int hg_scrub_end(HgScrub* s, double* outSeconds);

// The current target as a fraction of the duration, for drawing. 0 when the duration is
// unknown. This is the ONLY fraction the drag itself produces; everything else is seconds.
double hg_scrub_fraction(const HgScrub* s);

// The current target POSITION in seconds (0 when the duration is unknown). The companion to
// hg_scrub_fraction, and the value the preview readout is built from -- having it here means
// the readout and the cursor are derived from the same field rather than from two
// computations that can drift.
double hg_scrub_position_seconds(const HgScrub* s);

// --- preview scheduling -----------------------------------------------------
//
// The preview is rate-limited by the MODEL rather than by the caller so every caller gets
// the same floor. The first call always succeeds (so the preview appears the moment a drag
// starts), then no more often than HG_SCRUB_PREVIEW_MS.
int hg_scrub_preview_due(HgScrub* s, double now);
void hg_scrub_preview_done(HgScrub* s, double now);

// --- drawing helpers --------------------------------------------------------
// Both are pure: same inputs, same output, no state. They exist so the bar's cursor and
// the position a click maps to are computed ONE way, and the host suite can assert they
// are inverses. That is the check that stops "the preview shows one moment and the seek
// lands on another".

// Fraction -> x, clamped into the bar.
float hg_scrub_x_for(const HgScrubBar* bar, double fraction);

// x -> fraction. A NULL or degenerate bar returns 0. The result is clamped to 0..1, so a
// touch or a cursor past the end of the bar means "the end", not "beyond it".
double hg_scrub_fraction_at(const HgScrubBar* bar, float x);

// --- time formatting --------------------------------------------------------
//
// "H:MM:SS" once the duration reaches an hour, "M:SS" below that. Written here rather than
// with strftime because the format must match between the bar, the preview and the log, and
// one function is the only way to guarantee that. Negative input is treated as 0: mpv
// reports -1 for "unknown" and a bar reading "-1:-1" is worse than one reading 0:00.
//
// Always NUL-terminates. Returns the length of the FULL formatted text, which may be longer
// than the buffer; that is the question "does this fit" needs answered, and returning the
// truncated count would always say yes.
//
// It does NOT return snprintf's value. MEASURED under TinyCC -- this project's host test
// compiler -- a truncating snprintf returns -1 and leaves the buffer unterminated, where
// C99 requires the full length and a terminator. The implementation therefore formats into
// a private buffer and copies with its own bound.
int hg_scrub_format_time(double seconds, char* buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif // HG_SCRUB_H
